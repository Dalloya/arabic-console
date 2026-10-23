"""Sakhr AX-170 four-form records, decoded from the frozen ROM JSON.

The Unicode cell, ROM glyph slot, and connector/width attribute travel as
one record. A form's availability determines joining on that side; attribute
0x80 describes the ROM glyph, not whether an initial/medial form may exist.
"""

import hashlib
import json
from pathlib import Path

SOURCE = Path(__file__).resolve().parents[1] / "oracle/ax170/four_form_table.json"
TABLE = json.loads(SOURCE.read_text(encoding="utf-8"))
FORM_ORDER = ("initial", "isolated", "medial", "final")
POS_TO_ROM = {"init": "initial", "isol": "isolated", "medi": "medial", "fina": "final"}

if len(TABLE["groups"]) != 63 or tuple(TABLE["record_order"]) != FORM_ORDER:
    raise ValueError("AX-170 four-form table geometry changed")
raw = bytearray()
for index, group in enumerate(TABLE["groups"]):
    if group["index"] != index or int(group["file_offset"], 16) != 0x1AF7 + 8 * index:
        raise ValueError(f"AX-170 ROM group {index} offset changed")
    for position in FORM_ORDER:
        form = group["forms"][position]
        raw.extend((int(form["slot"], 16), int(form["attribute"], 16)))
if hashlib.sha256(raw).hexdigest() != TABLE["raw_table_sha256"]:
    raise ValueError("AX-170 ROM table hash mismatch")

GROUPS = tuple(TABLE["groups"])
BY_LETTER = {g["letter"]: g for g in GROUPS if g["letter"] and g["letter"] != "ـ"}


def record(letter, position):
    """Return the ROM's complete record for a logical letter/form, or None."""
    group = BY_LETTER.get(letter)
    return group["forms"][POS_TO_ROM.get(position, position)] if group else None


def can_join_before(char):
    """Can this character take a joining neighbour on its logical right?"""
    if char == "ـ":
        return True
    form = record(char, "final")
    return bool(form and form["unicode"])


def can_join_after(char):
    """Can this character take a joining neighbour on its logical left?"""
    if char == "ـ":
        return True
    form = record(char, "initial")
    return bool(form and form["unicode"])
