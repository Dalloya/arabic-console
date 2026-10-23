# Architecture: why the console looks like this

The Linux kernel text console is a grid of cells. It paints one codepoint per cell, from
left to right, in one pass. It does no shaping, it has no bidi algorithm, and it suppresses
combining marks. Arabic needs all three.

Everything below is a consequence of that constraint, not a preference.

## 1. The cell is the unit, so the shaper produces cells

Since the console cannot join letters, the joining has to happen **before** the bytes reach
it. The shaper turns a word of Arabic letters into a sequence of presentation forms
(`U+FE70`–`U+FEFC`), one per cell, already joined.

This is why the shaper is a separate concern from any text engine: it is not transforming
text for a reader, it is compiling text into a cell stream.

## 2. Visual order is produced, not delegated

A cell stream is painted left to right. An Arabic line reads right to left. So the shaper
emits the line **reversed at both levels**: the words, and the joined forms inside each
word. What the console receives is the screen, not the sentence.

Consequence for anything reading the screen: cell order equals screen order, so a cell dump
compares directly against the shaper's output with no bidi step in between.

## 3. Joining follows one table, and the table wins

Where an algorithm and the joining table disagree, the table is right. The table is the
four-form table of the Sakhr AX-170 ROM (`arabic-console-shaper`). Two visible results:

- Hamza has no final form there, so the letter before it stays isolated: `بء` →
  `U+FE80 U+FE8F`.
- Lam-alef is one ligature with its own two forms, not two letters choosing forms.

## 4. Marks need their own cells because the console drops them

The console suppresses combining marks — a mark codepoint in the stream produces no cell.
So the font carries each mark twice: once at its true codepoint for anything that maps text
properly, and once at a private-use codepoint (`U+E000`–`U+E008`) that is an ordinary
printable cell. The shaper emits the private-use cell when it wants the mark to appear.

`U+0670` superscript alef has a glyph and no key: it is reachable from text, not from the
keyboard.

## 5. One full-width join stroke instead of every combination

A cell cannot hold a half-width stroke plus a letter. Rather than bake every possible join
into the font, the font provides a full-width stroke on its own cells (`U+E010`–`U+E018`)
whose join rows are byte-identical to `U+0640` tatweel and which reaches both cell edges.
The renderer places it where a connection is needed.

This is the single most load-bearing agreement between font and shaper, so it is asserted
by a machine: the font check verifies the stroke's rows are byte-identical to tatweel and
that every connector reaches both edges.

## 6. A 512-cell budget forces aliasing, and aliasing is deliberate

The kernel accepts 512 glyph slots. The font maps 544 codepoints into them. Several
codepoints intentionally share one glyph: a mark's true codepoint and its private-use cell
select the same record, and the presentation forms of the letters that do not change shape
share records too.

The 16x32 face is an exact doubling of the 8x16 face, so the two sizes can never drift
apart; the font check asserts it glyph by glyph.

## 7. Command names fold marks; operands do not

A command typed with or without tashkeel is the same command: `حرر` and `حرّر` resolve
identically. The fold strips `U+064B`–`U+0652`, `U+0670` and a trailing tatweel **during
name comparison only**. Operands, paths and patterns stay byte-exact, because a filename is
not a command name and a mark in a path is content.

Two-word names (`اضبط الأذونات`) are joined the same way before lookup.

## 8. The session parses; bash only loads

The session owns the line: its own tokenizer, its own word and quoting rules, its own
redirection set, its own composition executor, its own builtins. A malformed line is
refused as a whole, in Arabic, before anything forks — there is no partial execution.

The last hop of a non-builtin command is `شكل /bin/bash -c '<requoted words>'`. That is a
loader, not the brain: `شكل` puts the child on a pty and shapes its output, bash resolves
and execs the program. The decision about what runs was made before bash saw the line.

## 9. Built-in rows are right-aligned, because this is an Arabic console

A built-in answer (a help row, a listing, an error) ends at the terminal's last column with
the shaped label rightmost and any Latin run to its left. A test that demands column 0 for
an Arabic row is testing the wrong contract.

## 10. What this design refuses to do

- It does not translate Arabic commands into English ones. Command words are the programs'
  own names in `/usr/local/bin`.
- It does not wrap `ls`, `cp`, `mv` or `rm`. Each command is its own program doing the work
  through syscalls.
- It does not maintain an English twin of the help table. There is one help table, in
  Arabic.
- It does not ask a missing glyph to be invented. A help lookup that does not match says so
  and returns nothing.
