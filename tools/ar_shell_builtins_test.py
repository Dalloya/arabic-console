#!/usr/bin/env python3
r"""D3F: ar-shell built-in rows end with CR LF and are right-aligned RTL.

`tools/ar_type.c`'s `sh_emit_row()` - the AR_TYPE_SHELL built-in path - separates
the committed line and every built-in row with `bytes("\\r\\n")`.  Written that
way in C it is the FOUR characters `\ r \ n`, not CR LF, and the console draws
them as four cells (measured on the guest: `review-d3/b1-cd-usr.cells.txt` row 23
cols 64..67 and row 24 cols 1..4, and every `شغّل` / `مساعدة` row the same way).

This suite drives `out/ar-shell` on a pty through the work-order-65 harness
(`tools/ar_shell_test.py`'s console model) and asserts the contract:

  B1  the raw byte stream contains no `5c 72 5c 6e` (the literal escape text)
      and no cell anywhere on the screen is U+005C;
  B2  each built-in result occupies its own physical row, with its last drawn
      cell at the right edge; it is not appended to the transcript or prompt;
  B3  the row's cells equal the project's visual RTL model: the shaped Arabic
      label is rightmost and any Latin run (such as a path) stays to its left;
  B4  all 27 help-table rows occupy distinct physical rows at 33x120;
  B5  اخرج ends the session.

Owner decision 2026-10-14: "writing answers in the right is the goal, it's an
Arabic console."  The expected rows are the measured ones: `المسار` draws the cwd
alone (`sh_builtin`), `انتقل` draws `انتقل: <path>`, `شغّل` draws
`حالة الخروج: <n>`.  The historical B1-B3 rows stay under their 80-column
model width.  B4 uses the mini's measured 33x120 geometry for the help table.

Exit 0 only with VERIFY PASS.
"""
import importlib.util
import fcntl
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import termios
from pathlib import Path
from help_entries import expected_row, full_cells, read_entries

ROOT = Path(__file__).resolve().parents[1]
ARSHELL = ROOT / "out" / "ar-shell"
HELP_ROWS = 27  # Owner decision 2026-10-13, round 5; mini: 33x120, banner: 6 rows.

spec = importlib.util.spec_from_file_location("ar_shell_test", ROOT / "tools" / "ar_shell_test.py")
H = importlib.util.module_from_spec(spec)
spec.loader.exec_module(H)          # main() is guarded, importing is side-effect free

# `python3 tools/ar_shell_builtins_test.py [BINARY]` - the optional argument lets
# the pre-fix build be run as a control without touching out/ar-shell.
if len(sys.argv) > 1 and not sys.argv[1].startswith("--"):
    H.ARSHELL = sys.argv[1]
ARSHELL = Path(H.ARSHELL)

failures = []


def check(name, good, detail=""):
    print(f"{name} {'PASS' if good else 'FAIL'} {detail}")
    if not good:
        failures.append(name)


def shape_codes(text):
    """The project's own shaper: the cell stream the editor must draw."""
    out = subprocess.run(["python3", str(ROOT / "tools" / "shape.py"), text],
                         capture_output=True, text=True, cwd=str(ROOT))
    line = [l for l in out.stdout.splitlines() if l.startswith("CODES")]
    return [int(t.split("+")[1], 16) for t in line[0].split(":", 1)[1].split()] if line else []


# The editor draws a transparent mark (shadda, fathatan, ...) as the font's own
# private-use glyph cell; tools/shape.py reports the mark codepoint itself.  One
# cell either way, so the comparison folds both to one sentinel.  This is the
# mark-cell representation the inherited suites already carry; D3F is about the
# CR LF, not the mark notation.
MARK_LO, MARK_HI = 0x064B, 0x0652
PRIVATE_LO, PRIVATE_HI = 0xE000, 0xF8FF
SENTINEL = 0xFFFD


def fold(cps):
    out = []
    for cp in cps:
        if MARK_LO <= cp <= MARK_HI or PRIVATE_LO <= cp <= PRIVATE_HI:
            out.append(SENTINEL)
        else:
            out.append(cp)
    return out


class ScrollingVT(H.VT):
    """The console scrolls on LF at its bottom row."""

    def feed(self, data):
        text = data.decode("utf-8", "replace")
        for ch in text:
            if self.esc:
                self.esc += ch
                if self.esc.startswith("\x1b[") and len(self.esc) >= 3 \
                        and 0x40 <= ord(ch) <= 0x7E:
                    self._handle_esc(self.esc)
                    self.esc = ""
                continue
            if ch == "\x1b":
                self.esc = ch
            elif ch == "\r":
                self.col = 0
            elif ch == "\n":
                if self.row == self.rows - 1:
                    self.grid.pop(0)
                    self.grid.append([" "] * self.cols)
                else:
                    self.row += 1
                self.col = 0
            elif ch == "\b":
                self.col = max(0, self.col - 1)
            elif ord(ch) >= 0x20:
                self.put(ch)


class RecordingSession(H.Session):
    """ar_shell_test.Session + the raw output bytes, for the B1 byte assertion."""

    def __init__(self, **kw):
        super().__init__(**kw)
        self.vt = ScrollingVT(self.vt.rows, self.vt.cols)
        self.raw = bytearray()
        inner = self.vt.feed

        def feed(data):
            self.raw += data
            inner(data)

        self.vt.feed = feed

    def raw_bytes(self, quiet=0.4):
        self.run(quiet)
        return bytes(self.raw)


def screen_rows(s):
    """row index -> [(col, codepoint), ...] for every non-blank cell."""
    rows = {}
    for r, line in enumerate(s.vt.grid):
        cells = [(c, ord(ch)) for c, ch in enumerate(line) if ord(ch) != 0x20]
        if cells:
            rows[r] = cells
    return rows


def backslash_cells(rows):
    return [(r, c) for r in sorted(rows) for c, cp in rows[r] if cp == 0x5C]


def literal_runs(rows):
    """rows carrying the 4-cell literal \\ r \\ n run."""
    hit = []
    for r in sorted(rows):
        seq = [cp for _, cp in rows[r]]
        for i in range(len(seq) - 3):
            if seq[i:i + 4] == [0x5C, 0x72, 0x5C, 0x6E]:
                hit.append((r, rows[r][i][0]))
    return hit


def builtin_row(rows, logical, cols):
    """Find one complete physical row in the independent RTL cell model."""
    assert len(full_cells(logical)) < cols, f"the harness cannot match a wrapped row ({logical!r})"
    want = fold(expected_row(logical))
    return next(((r, cells) for r, cells in rows.items()
                 if fold([cp for _, cp in cells]) == want), None)


def check_builtin_row(name, rows, logical, cols, label=None, latin=None):
    """Check content, physical isolation, right edge, and visual run order."""
    hit = builtin_row(rows, logical, cols)
    detail = (f"row={hit[0]} first={hit[1][0][0]} last={hit[1][-1][0]} cols={cols}"
              if hit else "missing complete row")
    check(f"{name} matches the shaper's RTL cell stream on its own physical row",
          hit is not None, detail)
    check(f"{name} is not left-anchored", hit is not None and hit[1][0][0] > 0, detail)
    check(f"{name} ends at the terminal's last column",
          hit is not None and hit[1][-1][0] == cols - 1, detail)
    if label is not None:
        seq = fold([cp for _, cp in hit[1]]) if hit else []
        label_cells = fold(expected_row(label))
        label_start = len(seq) - len(label_cells)
        label_right = label_start >= 0 and seq[label_start:] == label_cells
        latin_cells = [ord(ch) for ch in latin] if latin else []
        latin_left = (not latin_cells or any(
            seq[i:i + len(latin_cells)] == latin_cells
            for i in range(max(0, label_start - len(latin_cells) + 1))))
        check(f"{name} shaped label is rightmost and Latin run is to its left",
              hit is not None and label_right and latin_left, detail)
    return hit[0] if hit else None


def type_row(s, text, quiet=0.7):
    s.send(text.encode("utf-8") + b"\r")
    s.run(quiet)


def help_geometry(s):
    rows, cols, _, _ = struct.unpack("HHHH", fcntl.ioctl(s.master, termios.TIOCGWINSZ, b"\0" * 8))
    check("B4 pty geometry is 33x120", (rows, cols) == (33, 120),
          f"rows={rows} cols={cols}")


def b4_only():
    """Focused table and lookup case; the historical B1/B2 checks remain in main."""
    print("--- B4: مساعدة - owner decision 2026-10-13 ---")
    table = read_entries(ROOT / "tools" / "help_entries.txt")
    s = RecordingSession(cols=120, rows=33)
    try:
        help_geometry(s)
        s.run(0.5)
        type_row(s, "مساعدة", quiet=2.0)
        rows = screen_rows(s)
        hits = []
        for entry in table:
            want = fold(expected_row(entry))
            hit = next((r for r, cells in rows.items()
                        if fold([cp for _, cp in cells]) == want), None)
            hits.append(hit)
            check(f"B4 row {entry}", hit is not None,
                  f"row={hit}" if hit is not None else "missing")
        check("B4 all 27 complete rows visible together at 33x120", len(table) == HELP_ROWS
              and None not in hits and len(set(hits)) == HELP_ROWS,
              f"table_rows={len(table)} drawn={len(set(hits) - {None})} missing={hits.count(None)} geometry=33x120")
    finally:
        s.kill()

    for query, answer in [
        ('"أعد التسمية"', "انقل / أعد التسمية - انقل بين الأدلة أو غيّر الاسم"),
        ('"اضبط الأذونات"', "اعرض الأذونات / اضبط الأذونات - اعرض أذونات ملف أو دليل أو اضبطها"),
        ("متصفح", "متصفح - افتح متصفح الصفحات لملف أو خرج برنامج"),
        ("شكل", "شكل - شكّل خرج برنامج عربي في الطرفية"),
        ("تاريخ", "تاريخ - اعرض التاريخ الهجري"),
        ("الطرفية", "شكل - شكّل خرج برنامج عربي في الطرفية"),
        ("مجهول", "مساعدة: ليس أمراً معروفاً: "),
    ]:
        s = RecordingSession(cols=120, rows=33)
        try:
            s.run(0.5)
            type_row(s, "مساعدة " + query, quiet=1.0)
            rows = screen_rows(s)
            want = fold(expected_row(answer))
            hit = next((r for r, cells in rows.items()
                        if fold([cp for _, cp in cells]) == want), None)
            check(f"B4 lookup {query}", hit is not None,
                  f"row={hit}" if hit is not None else "missing")
        finally:
            s.kill()
    print(f"=== B4 {'PASS' if not failures else 'FAIL'}: {len(failures)} failures ===")
    return 0 if not failures else 1


def main():
    if not ARSHELL.exists():
        raise SystemExit(f"missing {ARSHELL}")
    print("=== D3F: ar-shell built-in rows end in a real CR LF ===")
    print(f"    binary: {ARSHELL}  {os.path.getsize(ARSHELL)} bytes")

    base = Path(tempfile.mkdtemp(prefix="d3f-"))
    work = base / "cwd"
    work.mkdir()
    (work / "sub").mkdir()
    oldcwd = os.getcwd()
    os.chdir(work)

    try:
        # -- B1: المسار -------------------------------------------------------
        print("\n--- B1: المسار (draws the bare session cwd) ---")
        s = RecordingSession()
        s.run(0.5)
        type_row(s, "المسار")
        rows = screen_rows(s)
        raw = s.raw_bytes()
        lit = raw.count(b"\\r\\n")
        check("B1 the raw stream holds no literal \\r\\n (5c 72 5c 6e)",
              lit == 0, f"occurrences={lit}")
        check("B1 no literal \\r\\n cell run on the screen",
              not literal_runs(rows), f"runs={literal_runs(rows)}")
        check("B1 no U+005C cell anywhere on the screen",
              not backslash_cells(rows), f"cells={backslash_cells(rows)}")
        check_builtin_row("B1 المسار cwd", rows, str(work), s.vt.cols)
        s.kill()

        # -- B2: انتقل, and the session cwd persists --------------------------
        print("\n--- B2: انتقل - the directory changes and the session keeps it ---")
        s = RecordingSession()
        s.run(0.5)
        type_row(s, "انتقل sub")
        rows = screen_rows(s)
        first = check_builtin_row("B2 انتقل sub", rows, f"انتقل: {work / 'sub'}",
                                  s.vt.cols, label="انتقل", latin=str(work / "sub"))
        type_row(s, "المسار")
        rows = screen_rows(s)
        path_row = check_builtin_row("B2 المسار after انتقل", rows, str(work / "sub"),
                                     s.vt.cols)
        check("B2 المسار after the change reports the new cwd (session cwd persists)",
              path_row is not None, f"row={path_row}" if path_row is not None else "missing")
        check("B2 the المسار row is its own row below the انتقل row (the transcript holds both)",
              first is not None and path_row is not None and path_row > first,
              f"انتقل row={first} cwd row={path_row}")
        type_row(s, "انتقل zz")
        rows = screen_rows(s)
        check_builtin_row("B2 انتقل missing path", rows,
                          "انتقل: تعذر الوصول إلى 'zz': لا يوجد ملف أو دليل بهذا الاسم",
                          s.vt.cols, label="انتقل", latin="zz")
        type_row(s, "انتقل")
        rows = screen_rows(s)
        check_builtin_row("B2 انتقل no operand", rows, "انتقل: معامل مفقود",
                          s.vt.cols, label="انتقل")
        raw = s.raw_bytes(0.3)
        rn = raw.count(b"\\r\\n")
        check("B2 the raw stream still holds no literal \\r\\n",
              rn == 0 and not literal_runs(rows),
              f"bytes={rn} cells={literal_runs(rows)}")
        s.kill()

        # -- B3: شغّل ---------------------------------------------------------
        print("\n--- B3: شغّل - runs a program and reports the exit status ---")
        # 73: the built-in routes its operand through the شكل wrapper; the host
        # has no /usr/local/bin/شكل, so point the session at the repo's real
        # wrapper (the same routing the guest uses).
        s = RecordingSession(env={"AR_SHELL_WRAPPER": str(ROOT / "out" / "شكل")})
        s.run(0.5)
        type_row(s, "شغّل true")
        rows = screen_rows(s)
        check_builtin_row("B3 شغّل true exit status", rows, "حالة الخروج: 0",
                          s.vt.cols, label="حالة الخروج", latin="0")
        type_row(s, "شغّل false", quiet=0.9)
        rows = screen_rows(s)
        check_builtin_row("B3 شغّل false nonzero exit status", rows, "حالة الخروج: 1",
                          s.vt.cols, label="حالة الخروج", latin="1")
        # 73: the operand's output must arrive SHAPED - presentation-form
        # bytes, not base letters.  A direct /bin/sh child (the pre-73 path)
        # would emit the base codepoints.  The operand must be ONE token (the
        # built-in's argument handling keeps only the first token, unchanged
        # by 73), so a nonexistent Arabic command is used: the wrapper's own
        # error row carries the shaped word.  The console model clips cells
        # past column 79, so the RAW byte stream is the instrument here.
        type_row(s, "شغّل مرحبا", quiet=0.9)
        raw = s.raw_bytes(0.3)
        shaped = "".join(chr(c) for c in shape_codes("مرحبا")).encode("utf-8")
        base = "مرحبا".encode("utf-8")
        check("B3 شغّل output is shaped by the wrapper (presentation bytes, no base letters)",
              shaped in raw and base not in raw,
              f"shaped_bytes={shaped in raw} base_bytes={base in raw}")
        type_row(s, "شغّل")
        rows = screen_rows(s)
        check_builtin_row("B3 شغّل no operand", rows, "شغّل: معامل مفقود",
                          s.vt.cols, label="شغّل")
        raw = s.raw_bytes(0.3)
        rn = raw.count(b"\\r\\n")
        check("B3 the raw stream holds no literal \\r\\n",
              rn == 0 and not literal_runs(rows),
              f"bytes={rn} cells={literal_runs(rows)}")
        s.kill()

        # -- B4: مساعدة -------------------------------------------------------
        print("\n--- B4: مساعدة - every row of the table is drawn ---")
        s = RecordingSession(cols=120, rows=33)
        help_geometry(s)
        s.run(0.5)
        type_row(s, "مساعدة", quiet=1.2)
        rows = screen_rows(s)
        # Owner decision 2026-10-13: the expectation is a separate file, never
        # parsed from sh_help_rows[].  Match complete inked rows so a removed
        # row, a partial row, or two entries on one physical row all fail.
        table = read_entries(ROOT / "tools" / "help_entries.txt")
        found = {}
        for entry in table:
            want = fold(expected_row(entry))
            hit = next((r for r, cells in rows.items()
                        if fold([cp for _, cp in cells]) == want), None)
            found[entry] = hit
            check(f"B4 row {entry}", hit is not None,
                  f"row={hit}" if hit is not None else "missing")
        check("B4 all 27 independent entries occupy distinct screen rows at 33x120",
              len(table) == HELP_ROWS and all(r is not None for r in found.values())
              and len(set(found.values())) == len(table),
              f"table_rows={len(table)} drawn={len(set(found.values()) - {None})} missing={sum(r is None for r in found.values())} geometry=33x120")
        check("B4 all 27 table rows end at the terminal's last column",
              all(r is not None and rows[r][-1][0] == s.vt.cols - 1
                  for r in found.values()),
              f"right_edge={s.vt.cols - 1} rows={len(found)}")
        check("B4 no literal \\r\\n cell run in the table's rows",
              not literal_runs(rows), f"runs={literal_runs(rows)}")
        check("B4 no U+005C cell anywhere on the screen",
              not backslash_cells(rows), f"cells={backslash_cells(rows)}")
        type_row(s, "مساعدة اعرض")
        rows = screen_rows(s)
        check_builtin_row("B4 مساعدة اعرض usage", rows, "اعرض: الاستخدام: اعرض مسار",
                          s.vt.cols, label="اعرض")
        raw = s.raw_bytes(0.3)
        rn = raw.count(b"\\r\\n")
        check("B4 the raw stream holds no literal \\r\\n after the whole table",
              rn == 0, f"occurrences={rn}")
        s.kill()

        # a fresh session: the console model has no scrollback, and this row
        # lands past its last row in the session above.
        s = RecordingSession()
        s.run(0.5)
        type_row(s, "مساعدة مجهول")
        rows = screen_rows(s)
        check_builtin_row("B4 مساعدة unknown argument notice", rows,
                          "مساعدة: ليس أمراً معروفاً: ", s.vt.cols, label="مساعدة")
        s.kill()

        # -- B5: اخرج ---------------------------------------------------------
        print("\n--- B5: اخرج - the session ends ---")
        s = RecordingSession()
        s.run(0.5)
        type_row(s, "اخرج", quiet=0.6)
        st = H.wait_exit(s, timeout=2.0)
        check("B5 اخرج ends the session with status 0", st == 0, f"status={st}")
        s.kill()
    finally:
        os.chdir(oldcwd)
        shutil.rmtree(base, ignore_errors=True)

    print()
    print(f"=== VERIFY {'PASS' if not failures else f'FAIL ({len(failures)} failures)'} ===")
    for f in failures:
        print(f"    failed: {f}")
    sys.exit(0 if not failures else 1)


if __name__ == "__main__":
    if "--b4" in sys.argv:
        sys.exit(b4_only())
    main()
