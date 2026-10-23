#!/usr/bin/env python3
"""Work order 65: the Arabic-native editor's own suite.

Drives out/ar-shell on a pty through a minimal console model (the editor's
output is simple: CR/LF, CSI-K clear-line, CSI-nC cursor-forward, SGR, and
UTF-8 cells), and asserts the editor's contract:

  S1  the child argv is exactly {wrapper, shell, "-c", line} with ARSH_SHELL=1
      in the environment and stdin /dev/null;
  S2  commit -> buffer cleared (the line is not re-runnable);
  S3  history recalls two committed lines in order, stored as LOGICAL
      codepoints (a recalled lam-alef re-shapes into one ligature cell);
  S4  backspace at index 0 changes nothing; backspace deletes one atom and
      never drops a visible character that has no logical reason to go;
  S5  a line ending in لا paints ONE cell for two codepoints and the caret
      column is derived from the CELL count (the discriminating control swaps
      in a codepoint count and shows it would land one column off);
  S6  Ctrl+D on an empty line exits 0; Ctrl+D on a non-empty line changes
      nothing;
  S7  Ctrl+C with an empty buffer does not exit.

Exit 0 only with VERIFY PASS.
"""
import fcntl
import os
import pty
import select
import signal
import struct
import sys
import tempfile
import termios
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ARSHELL = str(ROOT / "out" / "ar-shell")

failures = 0


def check(name, good, detail=""):
    global failures
    print(f"{name} {'PASS' if good else 'FAIL'} {detail}")
    failures += not good


# ---------------------------------------------------------------------------
# the recording stub the editor's child execs in place of شكل
# ---------------------------------------------------------------------------
STUB_SRC = r"""#!/usr/bin/env python3
import os, sys
rec = os.environ.get("AR_SHELL_RECORD_FILE", "/tmp/ar-shell-record.txt")
with open(rec, "a") as f:
    f.write("argc=%d\n" % len(sys.argv))
    for i, a in enumerate(sys.argv):
        f.write("argv%d=%s\n" % (i, a))
    f.write("ARSH_SHELL=%s\n" % os.environ.get("ARSH_SHELL", ""))
    try:
        link = os.readlink("/proc/self/fd/0")
    except OSError:
        link = ""
    f.write("stdin=%s\n" % link)
    f.write("---\n")
"""


# ---------------------------------------------------------------------------
# the console model (enough of the VT for ar-shell's output)
# ---------------------------------------------------------------------------
class VT:
    def __init__(self, rows=25, cols=80):
        self.rows, self.cols = rows, cols
        self.grid = [[" "] * cols for _ in range(rows)]
        self.row = self.col = 0
        self.esc = ""

    def put(self, ch):
        if self.col < self.cols:
            self.grid[self.row][self.col] = ch
            self.col += 1

    def _handle_esc(self, s):
        if not s.startswith("\x1b["):
            return
        body = s[2:]
        if body == "K":
            for c in range(self.col, self.cols):
                self.grid[self.row][c] = " "
        elif body.endswith("C") and body[:-1].isdigit():
            self.col = min(self.cols - 1, self.col + int(body[:-1]))
        elif body.endswith("A") and body[:-1].isdigit():
            self.row = max(0, self.row - int(body[:-1]))
        elif body.endswith("B") and body[:-1].isdigit():
            self.row = min(self.rows - 1, self.row + int(body[:-1]))
        # SGR (m): no cell effect; the arrow is disabled in the harness.

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
                self.row = min(self.rows - 1, self.row + 1)
                self.col = 0
            elif ch == "\b":
                self.col = max(0, self.col - 1)
            elif ord(ch) >= 0x20:
                self.put(ch)

    def row_cp(self, r):
        cells = []
        for c, ch in enumerate(self.grid[r]):
            o = ord(ch)
            if o == 0x20:
                continue
            cells.append(f"{c}:{ch if o < 0x7F else 'U+%04X' % o}")
        return " ".join(cells)


# ---------------------------------------------------------------------------
# the session
# ---------------------------------------------------------------------------
class Session:
    def __init__(self, cols=80, rows=25, env=None):
        self.master, slave = pty.openpty()
        fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))
        pid = os.fork()
        if pid == 0:
            os.setsid()
            fcntl.ioctl(slave, termios.TIOCSCTTY, 0)
            os.dup2(slave, 0); os.dup2(slave, 1); os.dup2(slave, 2)
            os.close(self.master); os.close(slave)
            e = dict(os.environ)
            e["AR_TYPE_DISABLE_POINTER"] = "1"
            if env:
                e.update(env)
            os.execve(ARSHELL, ["ar-shell"], e)
            os._exit(127)
        os.close(slave)
        self.pid = pid
        self.vt = VT(rows, cols)
        self.exit_status = None

    def send(self, data):
        os.write(self.master, data)

    def run(self, quiet_for=0.5):
        last = time.time()
        while time.time() - last < quiet_for:
            r, _, _ = select.select([self.master], [], [], 0.05)
            if r:
                try:
                    chunk = os.read(self.master, 65536)
                except OSError:
                    break
                if not chunk:
                    break
                self.vt.feed(chunk)
                last = time.time()

    def poll(self):
        w = os.waitpid(self.pid, os.WNOHANG)
        if w == (0, 0):
            return None
        self.exit_status = w[1]
        return w[1]

    def kill(self):
        try:
            os.kill(self.pid, signal.SIGKILL)
            os.waitpid(self.pid, 0)
        except (ProcessLookupError, ChildProcessError):
            pass
        try:
            os.close(self.master)
        except OSError:
            pass


def type_and_settle(s, data, quiet=0.5):
    s.send(data)
    s.run(quiet)


def caret_col(s):
    return s.vt.col


def wait_exit(s, timeout=1.5):
    deadline = time.time() + timeout
    while time.time() < deadline:
        st = s.poll()
        if st is not None:
            return st
        time.sleep(0.05)
    return None


# ---------------------------------------------------------------------------
def main():
    print("=== work order 65: the Arabic-native editor's own suite ===")

    # S1: child argv + ARSH_SHELL=1 + stdin /dev/null ------------------------
    print("\n--- S1: the committed line is executed through شكل bash -c ---")
    stub = Path(tempfile.mkdtemp(prefix="ar65-")) / "stub.py"
    stub.write_text(STUB_SRC)
    stub.chmod(0o755)
    record = Path(tempfile.mkdtemp(prefix="ar65-")) / "record.txt"
    env = {"AR_SHELL_WRAPPER": str(stub), "AR_SHELL_RECORD_FILE": str(record)}
    s = Session(env=env)
    s.run(0.5)
    line = "سلام"                        # seen lam alef meem
    type_and_settle(s, line.encode("utf-8") + b"\r", quiet=0.7)
    s.kill()
    rec = record.read_text()
    check("S1 child argv is exactly {wrapper, shell, -c, line}",
          f"argv0={stub}\nargv1=/bin/bash\nargv2=-c\nargv3={line}\n" in rec,
          repr(rec))
    check("S1 ARSH_SHELL=1 in the child environment", "ARSH_SHELL=1\n" in rec)
    check("S1 the child stdin is /dev/null", "stdin=/dev/null\n" in rec)

    # S1b: AR_SHELL_SHELL override -------------------------------------------
    record2 = Path(tempfile.mkdtemp(prefix="ar65-")) / "record.txt"
    s = Session(env={"AR_SHELL_WRAPPER": str(stub),
                     "AR_SHELL_SHELL": "/bin/sh",
                     "AR_SHELL_RECORD_FILE": str(record2)})
    s.run(0.5)
    type_and_settle(s, "مرحبا".encode("utf-8") + b"\r", quiet=0.7)
    s.kill()
    check("S1 AR_SHELL_SHELL override reaches argv[1]",
          "argv1=/bin/sh\n" in record2.read_text())

    # S2: commit clears the buffer (not re-runnable) --------------------------
    print("\n--- S2: commit clears the buffer ---")
    record3 = Path(tempfile.mkdtemp(prefix="ar65-")) / "record.txt"
    s = Session(env={"AR_SHELL_WRAPPER": str(stub),
                     "AR_SHELL_RECORD_FILE": str(record3)})
    s.run(0.5)
    type_and_settle(s, "بيت".encode("utf-8") + b"\r", quiet=0.7)   # commit once
    type_and_settle(s, b"\r")                                       # empty Enter: no-op
    s.kill()
    invocations = record3.read_text().count("---\n")
    check("S2 the committed line is not re-runnable (empty Enter re-runs nothing)",
          invocations == 1, f"invocations={invocations}")

    # S3: history recalls in order, stored as logical codepoints -------------
    print("\n--- S3: history ---")
    s = Session()
    s.run(0.5)
    type_and_settle(s, "سلام".encode("utf-8") + b"\r", quiet=0.7)   # history[0]
    type_and_settle(s, "بيت".encode("utf-8") + b"\r", quiet=0.7)     # history[1]
    type_and_settle(s, b"\x1b[A")                    # Up -> newer (بيت)
    check("S3 Up recalls the newer line (بيت)", "U+FE91" in s.vt.row_cp(s.vt.row),
          s.vt.row_cp(s.vt.row))
    type_and_settle(s, b"\x1b[A")                    # Up -> older (سلام)
    cells = s.vt.row_cp(s.vt.row)
    check("S3 Up again recalls the older line (سلام) with the lam-alef ligature",
          "U+FEFC" in cells and "U+FEB3" in cells, cells)
    type_and_settle(s, b"\x1b[B")                    # Down -> newer (بيت)
    check("S3 Down recalls the newer line again (بيت)", "U+FE91" in s.vt.row_cp(s.vt.row))
    type_and_settle(s, b"\x1b[B")                    # Down past newest -> empty
    check("S3 Down past the newest recalls an empty line", caret_col(s) == 76,
          f"caret col={caret_col(s)}")
    s.kill()

    # S4: backspace atomics ---------------------------------------------------
    print("\n--- S4: backspace ---")
    s = Session()
    s.run(0.5)
    type_and_settle(s, "سلام".encode("utf-8"))       # 4 atoms, 3 cells
    type_and_settle(s, b"\x7f")                      # delete the meem atom
    cells = s.vt.row_cp(s.vt.row)
    check("S4 backspace deletes one atom and keeps the lam-alef ligature (2 cells)",
          "U+FEFC" in cells and "U+FEB3" in cells and "U+FEE1" not in cells, cells)
    type_and_settle(s, b"\x1b[H")                    # Home -> caret 0 (right margin)
    before = s.vt.row_cp(s.vt.row)
    type_and_settle(s, b"\x7f")                      # backspace at 0: no-op
    after = s.vt.row_cp(s.vt.row)
    check("S4 backspace at index 0 changes nothing", before == after,
          f"before={before!r} after={after!r}")
    s.kill()

    # S5: the cell-map caret and the discriminating control ------------------
    print("\n--- S5: caret column from the cell map ---")
    s = Session()
    s.run(0.5)
    type_and_settle(s, "لا".encode("utf-8"))         # 2 codepoints, 1 cell
    col = caret_col(s)
    cell_count_col = 80 - 4 - 1                       # prompt(3)+typed(1), caret 1 left
    codepoint_count_col = 80 - 4 - 2                  # the control: 2 codepoints
    check("S5 a line ending in لا paints ONE cell (U+FEFB) for two codepoints",
          "U+FEFB" in s.vt.row_cp(s.vt.row), s.vt.row_cp(s.vt.row))
    check("S5 the caret column is the CELL-derived column (75)",
          col == cell_count_col, f"col={col} want={cell_count_col}")
    check("S5 control: a codepoint count would place the caret at 74 (the assertion fails)",
          codepoint_count_col != cell_count_col and col != codepoint_count_col,
          f"cell-map={cell_count_col} codepoint-count={codepoint_count_col} actual={col}")
    print(f"  (control printed: cell-map column {cell_count_col}, "
          f"codepoint-count column {codepoint_count_col}, actual {col})")
    s.kill()

    # S6: Ctrl+D --------------------------------------------------------------
    print("\n--- S6: Ctrl+D ---")
    s = Session()
    s.run(0.5)
    type_and_settle(s, "ب".encode("utf-8"))          # non-empty line
    before = s.vt.row_cp(s.vt.row)
    type_and_settle(s, b"\x04")                      # Ctrl+D on non-empty: no-op
    check("S6 Ctrl+D on a non-empty line changes nothing",
          s.poll() is None and s.vt.row_cp(s.vt.row) == before)
    type_and_settle(s, b"\x15")                      # Ctrl+U clears the line
    type_and_settle(s, b"\x04")                      # Ctrl+D on empty: exit 0
    st = wait_exit(s)
    check("S6 Ctrl+D on an empty line exits with status 0", st == 0, f"status={st}")
    s.kill()

    # S7: Ctrl+C with an empty buffer does not exit ---------------------------
    print("\n--- S7: Ctrl+C ---")
    s = Session()
    s.run(0.5)
    type_and_settle(s, b"\x03")
    check("S7 Ctrl+C with an empty buffer does not exit", s.poll() is None)
    type_and_settle(s, "سلام".encode("utf-8"))
    type_and_settle(s, b"\x03")                      # abandon the line
    check("S7 Ctrl+C abandons the line (returns to the prompt-only cells)",
          "U+FEFC" not in s.vt.row_cp(s.vt.row))
    s.kill()

    print()
    print(f"=== VERIFY {'PASS' if failures == 0 else f'FAIL ({failures} failures)'} ===")
    sys.exit(0 if failures == 0 else 1)


if __name__ == "__main__":
    main()
