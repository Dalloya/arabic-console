#!/usr/bin/env python3
"""Round 7: marked and bare command names, with literal operands."""

from __future__ import annotations

import os
import tempfile
from pathlib import Path

from ar_shell_builtins_test import ROOT, RecordingSession, builtin_row, screen_rows


PAIRS = [
    ("حرّر", "حرر", "حرّر - افتح ملفًا نصيًا في المحرر"),
    ("شغّل", "شغل", "شغّل - شغّل برنامجاً واعرض حالة الخروج"),
    ("فتّش", "فتش", "فتّش - فتّش عن نص داخل الملفات"),
    ("حدّث الحزم", "حدث الحزم", "حدّث الحزم - زامن المستودعات وحدّث الحزم المثبتة"),
    ("ثبّت حزمة", "ثبت حزمة", "ثبّت حزمة - ثبّت حزمة أو أكثر"),
]


def run(line: str, env: dict[str, str] | None = None, quiet: float = 0.7):
    s = RecordingSession(cols=120, rows=33, env=env)
    s.run(0.3)
    s.send(line.encode() + b"\r")
    s.run(quiet)
    return s


def check(name: str, good: bool) -> None:
    print(f"{name}: {'PASS' if good else 'FAIL'}")
    if not good:
        raise AssertionError(name)


def main() -> None:
    with tempfile.TemporaryDirectory(prefix="round7-commands-") as tmp:
        root = Path(tmp)
        fake = root / "bin"
        fake.mkdir()
        rec = root / "args.txt"
        operand = "نَصّـ.txt"   # all bytes must survive command-name resolution
        for marked, bare, _ in PAIRS[2:]:
            script = fake / marked
            script.write_text('#!/bin/sh\nprintf "%s\\n" "$0" "$@" > "$ROUND7_RECORD"\n')
            script.chmod(0o755)
            for spelling in (marked, bare):
                rec.unlink(missing_ok=True)
                s = run(f"{spelling} '{operand}'", {
                    "PATH": str(fake), "ROUND7_RECORD": str(rec),
                    "AR_SHELL_WRAPPER": str(ROOT / "out" / "شكل"),
                }, quiet=1.0)
                try:
                    check(f"command {spelling}", rec.exists() and
                          rec.read_text().splitlines() == [str(script), operand])
                finally:
                    s.kill()

        edited = root / "مَلَف.txt"
        edited.write_text("سطر\n")
        for spelling in ("حرّر", "حرر"):
            s = run(f"{spelling} '{edited}'")
            try:
                s.send(b"\x13")  # Ctrl+S exists only inside the editor
                s.run(0.3)
                check(f"editor {spelling} opens marked path", builtin_row(
                      screen_rows(s), f"حفظ: {edited}", 120) is not None)
                s.send(b"\x11")
                s.run(0.3)
            finally:
                s.kill()
            check(f"editor {spelling} saves to the exact marked path", edited.exists()
                  and not (root / "ملف.txt").exists())

        for spelling in ("شغّل", "شغل"):
            s = run(spelling)
            try:
                check(f"builtin {spelling}", builtin_row(screen_rows(s),
                      "شغّل: معامل مفقود", 120) is not None)
            finally:
                s.kill()

        for marked, bare, row in PAIRS:
            for spelling in (marked, bare):
                q = f"\"{spelling}\"" if " " in spelling else spelling
                s = run("مساعدة " + q)
                try:
                    check(f"help {spelling}", builtin_row(screen_rows(s), row, 120) is not None)
                finally:
                    s.kill()

        s = run("مساعدة حررـ")
        try:
            check("help trailing tatweel", builtin_row(screen_rows(s), PAIRS[0][2], 120) is not None)
        finally:
            s.kill()
        s = run("مساعدة مجّهول")
        try:
            check("unknown marked query stays unknown", builtin_row(screen_rows(s),
                  "مساعدة: ليس أمراً معروفاً: ", 120) is not None)
        finally:
            s.kill()
        s = run("مجهّول | اعرض")
        try:
            check("unknown marked command stays unknown", builtin_row(screen_rows(s),
                  "مجهّول: الأمر غير موجود", 120) is not None)
        finally:
            s.kill()
    print("VERIFY PASS: five marked command pairs, help, literal operand, unknown")


if __name__ == "__main__":
    main()
