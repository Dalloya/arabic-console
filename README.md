# Arabic console

An Arabic-native command layer for the Linux kernel text console (VT).

The kernel console paints one codepoint per cell, left to right, with no shaping and no
bidi. It also drops combining marks. This project is what it takes to make that surface
work in Arabic: a font with the right cells, a joining engine, a renderer that puts the
line in visual order, and a session whose commands are Arabic words.

It is not a distribution and not a translation layer. Nothing here converts an Arabic
word into a Latin one.

## What is in this repository

| path | what it is |
|---|---|
| `src/ar_type.c` | one translation unit, two binaries: `ar-type` (the typing window) and `ar-shell` (the console session, built with `-DAR_TYPE_SHELL=1`) |
| `src/arsh.c` | `شكل` — runs a program on a pty and shapes its Arabic output for the console |
| `src/ar_*.c` | the command binaries, one per command (see the table below) |
| `src/ar_type_tables.h`, `src/arsh_marks.h` | generated joining and mark-cell data (see the `arabic-console-shaper` repo) |
| `src/ar_type_latin.h` | the Latin run table used inside Arabic lines |
| `calendars/` | Hijri (Umm al-Qura) date line: `تاريخ` |
| `keymap/` | keymap build and check |
| `fonts/` | the 8x16 and 16x32 console font this layer draws with (same files as `arabic-console-font`) |
| `oracle/ax170/four_form_table.json` | the joining table the shaper follows |
| `tools/` | python reference tools (shaper, help table) **and** the test suite |
| `docs/ARCHITECTURE.md` | why the design is what it is, decision by decision |

## How a typed line is executed

The session has its own parser and its own execution path. It is not bash with Arabic
labels.

1. **Tokenize.** `sh_parse` (`src/ar_type.c`) splits the line into stages and words with
   its own quoting rules, and records redirections (`<`, `>`, `>>`, `2>`, `2>>`).
2. **Refuse early.** A malformed line — unmatched quote, too many words, too many
   redirections — is refused as a whole, in Arabic, before anything is forked. No partial
   execution.
3. **Resolve the name.** `sh_resolve_stage_name` maps the typed word to a command, folding
   the tashkeel marks so `حرر` and `حرّر` are the same name, and joining two-word names
   such as `اضبط الأذونات`.
4. **Builtins stay in the process.** `اخرج`, `المسار`, `انتقل`, `حرّر`, `مساعدة`, `شغّل`,
   `متصفح`, `البيئة` are handled inside the session itself — the file editor, the pager,
   the help table, the environment, the exit.
5. **Everything else is a program.** Pipelines and redirections run through the session's
   own fork/exec/reap executor. The last hop leaves through
   `شكل /bin/bash -c '<quoted words>'` so the child's output comes back shaped.

## The commands

Each command is a real executable installed in `/usr/local/bin`, under its Arabic name.
None of them wraps `ls`, `cp`, `mv` or `rm`: they do the work themselves.

| command | source | what it does |
|---|---|---|
| `اعرض` | `ar_list.c` | list a directory (`opendir`/`readdir`, no shell-out) |
| `اطبع` | `ar_print.c` | print a text file |
| `أنشئ` | `ar_mkdir.c` | create a directory |
| `انسخ` | `ar_copy.c` | copy |
| `انقل` | `ar_move.c` | move / rename |
| `أعد التسمية` | `ar_move.c` | rename |
| `احذف` | `ar_remove.c` | remove |
| `ابحث` | `ar_find.c` | find by name (exact basename, optional type) |
| `فتّش` | `ar_search.c` | search text inside files, line numbers, one match per line |
| `اعرض الأذونات` | `ar_perm.c` | show permissions |
| `اضبط الأذونات` | `ar_perm.c` | set permissions |
| `حدّث الحزم` | `ar_pkg_update.c` | update packages |
| `ثبّت حزمة` | `ar_pkg_install.c` | install a package |
| `أزل حزمة` | `ar_pkg_remove.c` | remove a package |
| `تاريخ` | `calendars/hijri.c` | Hijri + Gregorian date line |

`مساعدة <name>` answers from a fixed table; it never invents a row.

## Dependencies

- `cc` (C99), POSIX. No third-party libraries.
- A font that carries the Arabic cells — get it from the `arabic-console-font` repo and
  load it with `setfont`.
- The joining tables — generated, and vendored here as `src/ar_type_tables.h`.

## Build

```
./build.sh          # binaries land in ./out/
```

`build.sh` compiles every binary, stages the font and the source where the test harness
expects them, and runs the suite. It prints one line per test and ends with
`BUILD PASS` or `BUILD FAIL`.

## Tests

`build.sh` runs four things and prints one line each:

| test | state |
|---|---|
| `ar_type_safety_test.c` | passes — the editor's safety invariants |
| `ar_shell_builtins_test.py` | passes — builtin rows end in a real CR LF and are right-aligned RTL |
| `command_marks_test.py` | passes — command names resolve with or without marks |
| `ar_shell_test.py` | **4 known failures**, named below |

The pty suite's four failures are false against the current session build, and they fail
exactly the same way in the private tree this code was extracted from. They are named here
instead of being quietly deleted:

| assertion | what it expects |
|---|---|
| S1 "the child stdin is /dev/null" | the child of an executed command sees closed stdin |
| S3 "Down past the newest recalls an empty line" | history navigation past the newest entry |
| S4 "backspace deletes one atom and keeps the lam-alef ligature" | one backspace removes one atom |
| S5 "the caret column is the CELL-derived column" | caret placed by cell count, not codepoint count |

`build.sh` reports those four as `KNOWN-FAIL` and fails on anything else. If you fix one,
remove it from that list in `build.sh`, not from the test.

**Withheld, on purpose:** the project's `ar_type_test.py` is not shipped. It asserts the
pre-lock keyboard contract — SHIFT plus a mark key producing Latin `Q`/`W`/`~` — while the
fixed contract puts the eight tashkeel marks on SHIFT, and it byte-compares output against
fixtures captured from an older binary. A test that demands the superseded behaviour is the
thing that is wrong, so it stays out rather than being shipped red or quietly rewritten.

## Marks on the keyboard

Eight tashkeel marks on SHIFT, the same eight the Sakhr AX-170 ROM carries:
`16`=U+064E `17`=U+064B `18`=U+064F `19`=U+064C `30`=U+0650 `31`=U+064D `41`=U+0651
`45`=U+0652. `U+0670` superscript alef is in the font but bound to no key.

## Known limits, stated plainly

- Target is the kernel VT. Under a terminal emulator the session runs in pass-through and
  is not useful.
- Tested on Arch Linux on real hardware and on an MSX-era font budget of 512 cells.
- The child of a command still leaves through `bash -c`. The parser and the executor are
  ours; the last exec hop is not.
- The two help tables proposal (an English twin of `مساعدة`) was considered and dropped.
  There is one help table, in Arabic.

## Licence

MIT — see `LICENSE`.
