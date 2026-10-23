#!/usr/bin/env python3
"""Independently check the Arabic-first console map against installed XKB."""

from __future__ import annotations

import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
# An explicit path lets the duplicate-scan proof run against a tampered copy.
MAP = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "out/arabic-console.map"
XKB_SYMBOLS = Path("/usr/share/X11/xkb/symbols/ara")
XKB_KEYCODES = Path("/usr/share/X11/xkb/keycodes/evdev")
KEYSYMDEF = Path("/usr/include/X11/keysymdef.h")


def fail(message: str) -> None:
    print(f"FAIL: {message}", file=sys.stderr)
    raise SystemExit(1)


def keysyms() -> dict[str, str]:
    result: dict[str, str] = {}
    pattern = re.compile(r"^#define XK_(\w+)\s+\S+\s+/\*\s*U\+([0-9A-Fa-f]+)")
    for line in KEYSYMDEF.read_text().splitlines():
        match = pattern.match(line)
        if match:
            result[match.group(1)] = f"U+{int(match.group(2), 16):04X}"
    return result


def keycodes() -> dict[str, int]:
    result: dict[str, int] = {}
    for line in XKB_KEYCODES.read_text().splitlines():
        match = re.match(r"^\s*<(\w+)>\s*=\s*(\d+)", line)
        if match:
            result[match.group(1)] = int(match.group(2)) - 8
    return result


def ara_levels() -> list[tuple[str, list[str]]]:
    text = XKB_SYMBOLS.read_text()
    section = re.search(r'xkb_symbols\s+"basic"\s*\{(.*?)^\};', text, re.S | re.M)
    if not section:
        fail("ara basic section missing")
    return [(name, [value.strip() for value in values.split(",")])
            for name, values in re.findall(
                r"key\s+<(\w+)>\s+\{\s*\[\s*(.*?)\s*\]\s*\};", section.group(1))]


def unicode_token(token: str, names: dict[str, str]) -> str:
    if re.fullmatch(r"U[0-9A-Fa-f]{4,6}", token):
        return f"U+{int(token[1:], 16):04X}"
    if len(token) == 1:
        return f"U+{ord(token):04X}"
    return names[token]


def map_entries(text: str) -> dict[tuple[str, int], str]:
    entries: dict[tuple[str, int], str] = {}
    pattern = re.compile(r"^(plain|shift|altgr|shift altgr|control) keycode (\d+) = (\S+)$")
    for line in text.splitlines():
        match = pattern.match(line)
        if match:
            entries[(match.group(1), int(match.group(2)))] = match.group(3)
    return entries


def function_strings(text: str) -> dict[str, str]:
    result: dict[str, str] = {}
    pattern = re.compile(r'^string (F\d+) = "((?:\\[0-7]{3})+)"$')
    for line in text.splitlines():
        match = pattern.match(line)
        if match:
            raw = bytes(int(octal, 8) for octal in re.findall(r"\\([0-7]{3})", match.group(2)))
            result[match.group(1)] = f"U+{ord(raw.decode('utf-8')):04X}"
    return result


# kbd indexes the columns of a `keycode N = a b c ...' line by modifier
# bitmask: bit0 shift, bit1 altgr, bit2 control, bit3 alt, bit4 shiftl,
# bit5 shiftr, bit6 capslock, bit7 numlock.
_MOD_BITS = ("shift", "altgr", "control", "alt",
             "shiftl", "shiftr", "capslock", "numlock")


def binding_lines(text: str) -> tuple[dict[tuple[tuple[str, ...], int], list[int]],
                                      dict[tuple[tuple[str, ...], int], list[int]]]:
    """Parse every keymap binding line with its line numbers.

    Returns (singles, multis): single-level lines (exactly one keysym, with
    or without modifier words) and multi-column base lines (two or more
    keysyms), each keyed by (canonical modifier set, keycode) -> line numbers.
    """
    singles: dict[tuple[tuple[str, ...], int], list[int]] = {}
    multis: dict[tuple[tuple[str, ...], int], list[int]] = {}

    def column_level(index: int) -> tuple[str, ...]:
        return tuple(bit for pos, bit in enumerate(_MOD_BITS)
                     if index & (1 << pos))

    for lineno, line in enumerate(text.splitlines(), 1):
        match = re.match(r"^((?:\w+(?:\s+\w+)*)\s+)?keycode (\d+) = (.+)$", line)
        if not match:
            continue
        words = match.group(1).split() if match.group(1) else []
        words = ["control" if word == "ctrl" else word for word in words]
        keycode = int(match.group(2))
        values = match.group(3).split()
        if len(values) == 1:
            singles.setdefault((tuple(sorted(words)), keycode), []).append(lineno)
        else:
            for column in range(len(values)):
                multis.setdefault((column_level(column), keycode), []).append(lineno)
    return singles, multis


def duplicate_scan(text: str) -> None:
    """Fail on any (modifier, keycode) level assigned by more than one line.

    This is the durable fix for the earlier bug: kbd applies map entries
    in file order, so a second line for the same level silently overwrites the
    first (that is how `shift keycode 49 = U+0670` ate the alef-madda).
    Single-level lines are checked against each other, and the columns of the
    multi-column base lines against each other. The one deliberate exception
    is the map's own structure: the override section rewrites individual
    columns of the ckbcomp base block (base map first, then the Arabic
    levels) - those cross-kind rewrites are the design, not duplicates.
    """
    singles, multis = binding_lines(text)
    problems: list[tuple[tuple[str, ...], int, list[int]]] = []
    for (level, keycode), linenos in singles.items():
        if len(linenos) > 1:
            problems.append((level, keycode, linenos))
    for (level, keycode), linenos in multis.items():
        if len(linenos) > 1:
            problems.append((level, keycode, linenos))
    if problems:
        for level, keycode, linenos in sorted(problems):
            print(f"FAIL: duplicate binding {'+'.join(level) or 'plain'} "
                  f"keycode {keycode}: lines {', '.join(map(str, linenos))}",
                  file=sys.stderr)
        raise SystemExit(1)
    print(f"duplicate scan: 0 duplicate (modifier, keycode) assignments across "
          f"{len(singles)} single-level and {len(multis)} multi-column entries: PASS")


def main() -> None:
    text = MAP.read_text()
    entries = map_entries(text)
    strings = function_strings(text)
    names, codes = keysyms(), keycodes()

    # The duplicate scan runs before everything else so a
    # silently-overwritten level is reported first, with its line numbers.
    duplicate_scan(text)

    expected_letters: list[tuple[int, str]] = []
    expected_marks: list[tuple[int, str]] = []
    ara_levels_dict = dict(ara_levels())
    for name, levels in ara_levels_dict.items():
        plain = unicode_token(levels[0], names)
        shifted = unicode_token(levels[1], names)
        code = codes[name]
        value = int(plain[2:], 16)
        # The 28 standard alphabet letters: exclude hamza variants, teh
        # marbuta, alef maksura, the lam-alef ligature, and punctuation.
        if ((0x0627 <= value <= 0x063A and value != 0x0629) or
                (0x0641 <= value <= 0x064A and value != 0x0649)):
            expected_letters.append((code, plain))
        if 0x064B <= int(shifted[2:], 16) <= 0x0652:
            expected_marks.append((code, shifted))

    # Owner decision 2026-10-14: exactly Sakhr's eight marks on Shift.
    sakhr_eight = [(16, "U+064E"), (17, "U+064B"),
                   (18, "U+064F"), (19, "U+064C"),
                   (30, "U+0650"), (31, "U+064D"),
                   (41, "U+0651"), (45, "U+0652")]
    if sorted(expected_marks) != sorted(sakhr_eight):
        fail(f"XKB Shift marks differ from Sakhr's eight: {expected_marks}")

    for code, value in expected_letters:
        actual = entries.get(("plain", code))
        if strings.get(actual or "", actual) != value:
            fail(f"letter keycode {code}: expected {value}, got {actual}")
    for code, value in expected_marks:
        actual = entries.get(("shift", code))
        if actual != value:
            fail(f"shift keycode {code}: expected mark {value}, got {actual}")
    marks = {f"U+{cp:04X}" for cp in range(0x064B, 0x0653)} | {"U+0670"}
    for lineno, line in enumerate(text.splitlines(), 1):
        if not re.match(r"^(?:\w+(?:\s+\w+)*\s+)?keycode \d+ = ", line):
            continue
        for token in line.split("=", 1)[1].split():
            resolved = strings.get(token, token)
            if resolved == "U+0670":
                fail(f"U+0670 forbidden on every level by owner decision 2026-10-14, line {lineno}: {line}")
            if resolved in marks:
                match = re.match(r"^shift keycode (\d+) = (\S+)$", line)
                if not match or (int(match.group(1)), strings.get(token, token)) not in expected_marks:
                    fail(f"unexpected mark at line {lineno}: {line}")

    controls = {30: "a", 46: "c", 32: "d", 44: "z", 38: "l", 22: "u"}
    for code, letter in controls.items():
        if entries.get(("control", code)) != f"Control_{letter}":
            fail(f"control keycode {code} is not Control_{letter}")

    latin = "qwertyuiopasdfghjklzxcvbnm"
    latin_codes = [codes[f"AD{i:02d}"] for i in range(1, 11)]
    latin_codes += [codes[f"AC{i:02d}"] for i in range(1, 10)]
    latin_codes += [codes[f"AB{i:02d}"] for i in range(1, 8)]
    for code, letter in zip(latin_codes, latin):
        if entries.get(("altgr", code)) != f"U+{ord(letter):04X}":
            fail(f"AltGr keycode {code} is not {letter}")
        if entries.get(("shift altgr", code)) != f"U+{ord(letter.upper()):04X}":
            fail(f"Shift+AltGr keycode {code} is not {letter.upper()}")

    for number in range(1, 11):
        code = codes[f"AE{number:02d}"]
        digit = number % 10
        expected = f"U+{0x0660 + digit:04X}"
        if entries.get(("altgr", code)) != expected:
            fail(f"AltGr number-row keycode {code}: expected {expected}")

    if re.search(r"(?m)^.*\b(?:shiftl|shiftr)\b.*$", text):
        fail("group modifier line found")
    forbidden = ("U+200E", "U+200F", "U+202A", "U+202B", "U+202C", "U+202D",
                 "U+202E", "U+2066", "U+2067", "U+2068", "U+2069")
    for codepoint in forbidden:
        if codepoint in text:
            fail(f"bidi control {codepoint} found")

    # The Super keys must be Meta, not Alt, so the console
    # ESC-prefixes them (Super+Space becomes an ESC chord for mode toggling).
    super_meta: dict[int, str] = {}
    for line in text.splitlines():
        match = re.match(r"^keycode (125|126) = (\S+)", line)
        if match:
            super_meta[int(match.group(1))] = match.group(2)
    if super_meta.get(125) != "Meta_L" or super_meta.get(126) != "Meta_R":
        fail(f"Super keys are not Meta_L/Meta_R: {super_meta}")

    # The precomposed ligature levels are removed. The map keeps
    # the two physical b levels as editor-private sentinels: Arabic mode drops
    # them, while Latin mode turns them into b/B.
    ligature_levels = (("AD05", "shift"), ("AC05", "shift"),
                       ("AB05", "plain"), ("AB05", "shift"))
    for name, modifier in ligature_levels:
        token = unicode_token(ara_levels_dict[name][0 if modifier == "plain" else 1],
                              names)
        if token not in {"U+FEF5", "U+FEF7", "U+FEF9", "U+FEFB"}:
            fail(f"ara(basic) {name} {modifier} is {token}, not a lam-alef ligature")
        actual = entries.get((modifier, codes[name]))
        expected = "U+E100" if (name, modifier) == ("AB05", "plain") else \
                   "U+E101" if (name, modifier) == ("AB05", "shift") else "VoidSymbol"
        if actual != expected:
            fail(f"{modifier} keycode {codes[name]} ({name}): expected {expected}, got {actual}")
    for function in ("F242", "F243", "F245", "F246"):
        if function in strings or re.search(rf"(?m)^string {function} =", text):
            fail(f"ligature string {function} still defined")
    if strings.get("F244") != "U+2026":
        fail(f"F244 is {strings.get('F244')}, expected U+2026 (ellipsis kept)")
    if entries.get(("shift", codes["LSGT"])) != "F244":
        fail(f"ellipsis not bound to shift keycode {codes['LSGT']}: "
             f"{entries.get(('shift', codes['LSGT']))}")
    # The freed keys keep every other level: their US base rows' control and
    # meta columns must be untouched.
    for code, ctrl, meta in ((20, "Control_t", "Meta_t"),
                             (34, "Control_g", "Meta_g"),
                             (48, "Control_b", "Meta_b")):
        base_line = next(line for line in text.splitlines()
                         if re.match(rf"^keycode {code} = ", line))
        columns = base_line.split("=", 1)[1].split()
        if len(columns) < 10 or columns[4] != ctrl or columns[6] != meta:
            fail(f"keycode {code} control/meta columns damaged: {base_line}")

  # Shift keycode 49 carries the alef-madda U+0622 alone -
    # the duplicate scan above guarantees no second line binds that level -
    # The base SysRq row remains VoidSymbol on both levels, with no override.
    if entries.get(("shift", 49)) != "U+0622":
        fail(f"shift keycode 49 is {entries.get(('shift', 49))}, "
             f"expected U+0622 (alef madda)")
    base99 = next(line for line in text.splitlines()
                  if re.match(r"^keycode 99 = ", line))
    columns99 = base99.split("=", 1)[1].split()
    if (len(columns99) < 2 or columns99[0] != "VoidSymbol"
            or columns99[1] != "VoidSymbol"):
        fail(f"keycode 99 base line no longer VoidSymbol on plain/shift: {base99}")
    if ("plain", 99) in entries or ("shift", 99) in entries:
        fail("keycode 99 has an unexpected plain/shift override")

    print(f"letters {len(expected_letters)}/28")
    print(f"marks bound {len(expected_marks)}/8 on Shift: PASS")
    print("letter keycodes " + " ".join(str(code) for code, _ in expected_letters))
    print("mark bindings " + " ".join(f"{code}={value}" for code, value in expected_marks))
    print("control physical positions a c d z l u: PASS")
    print("Latin AltGr 26/26; Shift+AltGr 26/26")
    print("Arabic-Indic AltGr digits 10/10")
    print("single group and bidi exclusion: PASS")
    print("super keys keycode 125=Meta_L keycode 126=Meta_R: PASS")
    print(f"ligature levels VoidSymbol (plain/shift {codes['AB05']}, "
          f"shift {codes['AC05']}, shift {codes['AD05']}): PASS")
    print("ligature strings F242/F243/F245/F246 absent: PASS")
    print(f"ellipsis F244 = U+2026 on shift keycode {codes['LSGT']}: PASS")
    print("freed keys keep control/meta columns: PASS")
    print("shift keycode 49 = U+0622 sole binding; keycode 99 plain/shift VoidSymbol; U+0670 unbound: PASS")


if __name__ == "__main__":
    main()
