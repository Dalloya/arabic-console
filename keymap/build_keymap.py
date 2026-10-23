#!/usr/bin/env python3
"""Build the single-group Arabic-first Linux console keymap."""

from __future__ import annotations

import re
import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
XKB_SYMBOLS = Path("/usr/share/X11/xkb/symbols/ara")
XKB_KEYCODES = Path("/usr/share/X11/xkb/keycodes/evdev")
KEYSYMDEF = Path("/usr/include/X11/keysymdef.h")
OUT = ROOT / "out/arabic-console.map"


def unicode_keysyms() -> dict[str, str]:
    """Read X11's own keysym-to-Unicode annotations."""
    result: dict[str, str] = {}
    pattern = re.compile(r"^#define XK_(\w+)\s+\S+\s+/\*\s*U\+([0-9A-Fa-f]+)")
    for line in KEYSYMDEF.read_text().splitlines():
        match = pattern.match(line)
        if match:
            result[match.group(1)] = f"U+{int(match.group(2), 16):04X}"
    return result


def xkb_keycodes() -> dict[str, int]:
    result: dict[str, int] = {}
    pattern = re.compile(r"^\s*<(\w+)>\s*=\s*(\d+)")
    for line in XKB_KEYCODES.read_text().splitlines():
        match = pattern.match(line)
        if match:
            # XKB evdev keycodes are Linux input keycodes plus eight.
            result[match.group(1)] = int(match.group(2)) - 8
    return result


def ara_basic() -> list[tuple[str, list[str]]]:
    text = XKB_SYMBOLS.read_text()
    section = re.search(r'xkb_symbols\s+"basic"\s*\{(.*?)^\};', text, re.S | re.M)
    if not section:
        raise RuntimeError("could not find ara basic section")
    pattern = re.compile(r"key\s+<(\w+)>\s+\{\s*\[\s*(.*?)\s*\]\s*\};")
    return [(name, [item.strip() for item in values.split(",")])
            for name, values in pattern.findall(section.group(1))]


def as_unicode(token: str, names: dict[str, str]) -> str | None:
    if token == "any":
        return None
    if re.fullmatch(r"U[0-9A-Fa-f]{4,6}", token):
        return f"U+{int(token[1:], 16):04X}"
    if len(token) == 1:
        return f"U+{ord(token):04X}"
    try:
        return names[token]
    except KeyError as exc:
        raise RuntimeError(f"no Unicode conversion for XKB keysym {token!r}") from exc


def us_base() -> list[str]:
    completed = subprocess.run(
        ["ckbcomp", "-compact", "-layout", "us"], check=True,
        text=True, stdout=subprocess.PIPE,
    ).stdout
    # ckbcomp appends instructional comments mentioning XKB group modifiers.
    # They are neither part of the map nor permitted in this single-group file.
    return completed.split("strings as usual", 1)[0].rstrip().splitlines()


def main() -> None:
    names = unicode_keysyms()
    keycodes = xkb_keycodes()
    ara = ara_basic()
    function_strings: list[tuple[str, str]] = []
    next_function = 246

    def emit_value(value: str) -> str:
        """Use a function-string for Unicode values kbd cannot encode as a keysym."""
        nonlocal next_function
        codepoint = int(value[2:], 16)
        # The kernel keyboard ABI reserves U+F800..U+FFFF for keysyms, so
        # loadkeys 2.6.4 rejects Arabic Presentation Forms-B as direct U+ keys.
        if codepoint <= 0xF7FF and codepoint != 0x2026:
            return value
        if codepoint == 0x2026:
            # The ellipsis is kept and pinned to F244, the
            # function number it has always carried.
            function = "F244"
        else:
            function = f"F{next_function}"
            next_function -= 1
            if next_function == 244:  # 244 is reserved for the ellipsis
                next_function -= 1
        utf8_octal = "".join(f"\\{byte:03o}" for byte in chr(codepoint).encode())
        function_strings.append((function, utf8_octal))
        return function

    # ckbcomp binds the Super keys to Alt.  A bare Alt is only a shift bit:
    # the kernel sends nothing extra, so Super+Space arrives as a plain space.
    # Rebind them to Meta_L/Meta_R instead: the kernel's k_meta handler
    # ESC-prefixes the Meta key's own press, so the shaper sees Super+Space as
    # an ESC chord and can use it as the Arabic<->Latin toggle.
    us_lines = [line for line in us_base()
                if not re.match(r"^keycode 12[56]\s", line)]
    lines = [
        "# Arabic-first one-group console keymap.",
        "# Sources: /usr/share/X11/xkb/symbols/ara (basic),",
        "# /usr/share/X11/xkb/keycodes/evdev, and ckbcomp -compact -layout us.",
        "# plain: Arabic letters, Western digits, Arabic-layout punctuation;",
        "# shift: Sakhr's eight tashkeel marks on XKB Arabic level 2.",
        "#   Owner decision 2026-10-14: U+0670 has no keyboard binding.",
        "#   Precomposed lam-alef ligatures remain Void; keycode 49 is U+0622;",
        "# altgr: Latin a-z plus Arabic-Indic digits;",
        "# shift+altgr: Latin A-Z. Bidi controls are deliberately omitted.",
        *us_lines,
        "",
        "# Right Alt is the sole level-three selector in this map.",
        "keycode 100 = AltGr",
        "",
        "# Super as Meta: the console ESC-prefixes the Meta key press, turning",
        "# Super+Space into an ESC chord instead of a bare space.",
        "keycode 125 = Meta_L",
        "keycode 126 = Meta_R",
    ]

    # Level 1 and 2 follow ara(basic), including its punctuation. The map
    # omits the four precomposed lam-alef ligature keys (the shaper forms the
    # ligature from lam+alef): their levels are VoidSymbol so they emit
    # nothing, instead of falling back to the US base row's Latin.
    # `any` intentionally leaves the US map's unused value alone rather than
    # inventing a character.
    removed_ligatures = {"U+FEF5", "U+FEF7", "U+FEF9", "U+FEFB"}
    # The physical b levels are private sentinels rather
    # than VoidSymbol.  The editor drops them in Arabic mode and translates
    # them to b/B in Latin mode; this preserves the freed-level behavior while
    # making the physical key usable when explicitly typing Latin.
    freed_b_code = keycodes["AB05"]
    freed_b_levels = {("plain", freed_b_code): "U+E100",
                      ("shift", freed_b_code): "U+E101"}
    for name, levels in ara:
        code = keycodes[name]
        for modifier, token in (("plain", levels[0]), ("shift", levels[1])):
            value = as_unicode(token, names)
            if value:
                if (modifier, code) in freed_b_levels:
                    lines.append(f"{modifier} keycode {code} = {freed_b_levels[(modifier, code)]}")
                elif value in removed_ligatures:
                    lines.append(f"{modifier} keycode {code} = VoidSymbol")
                else:
                    lines.append(f"{modifier} keycode {code} = {emit_value(value)}")

    # The ten number-row positions retain Western digits at plain level, while
    # AltGr yields Arabic-Indic digits from ara(basic)'s third level.
    for name, levels in ara:
        if name in {f"AE{i:02d}" for i in range(1, 11)}:
            value = as_unicode(levels[2], names)
            if value:
                lines.append(f"altgr keycode {keycodes[name]} = {value}")

    # Latin letters are one AltGr away on their ordinary physical US positions.
    us_letters = {
        "AD01": "qwertyuiop", "AC01": "asdfghjkl", "AB01": "zxcvbnm",
    }
    for first_key, letters in us_letters.items():
        prefix = first_key[:2]
        start = int(first_key[2:])
        for offset, letter in enumerate(letters):
            name = f"{prefix}{start + offset:02d}"
            code = keycodes[name]
            lines.append(f"altgr keycode {code} = U+{ord(letter):04X}")
            lines.append(f"shift altgr keycode {code} = U+{ord(letter.upper()):04X}")

    # These physical control bindings are asserted by the independent checker.
    for keycode, letter in ((30, "a"), (46, "c"), (32, "d"),
                            (44, "z"), (38, "l"), (22, "u")):
        lines.append(f"control keycode {keycode} = Control_{letter}")

    if function_strings:
        lines.append("")
        lines.append("# UTF-8 input strings for values rejected as direct U+ keysyms by kbd.")
        for function, utf8_octal in function_strings:
            lines.append(f'string {function} = "{utf8_octal}"')

    OUT.parent.mkdir(exist_ok=True)
    OUT.write_text("\n".join(lines) + "\n")
    print(OUT)


if __name__ == "__main__":
    main()
