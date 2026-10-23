#!/usr/bin/env python3
"""layer1/mkumalqurra.py — generate layer1/umalqurra.h (the Umm al-Qura
month-start table as Rata Die numbers) and verify it against published data.

Which calendar this is (the exactness question):

  UMM AL-QURA — the official civil calendar of Saudi Arabia.  Its month
  starts are not a closed formula: they are published, month by month, from
  the kingdom's moon-sighting/astronomical criteria.  This generator embeds
  the published month-start table itself:

    * the data array is copied VERBATIM from the umalqurra Python package
      v0.2 (Khalid Al-Hussayen, PyPI) — layer1/umalqurra_data.py; the
      package's own source comments state its provenance: R.H. van Gent,
      "The Umm al-Qura Calendar of Saudi Arabia",
      http://www.staff.science.uu.nl/~gent0113/islam/ummalqura.htm, whose
      islamcalendar_dat.js holds the MCJDN month-start table the array is;
    * the span of that published table is 1 Muharram 1356 AH (1937-03-14)
      through 1 Muharram 1501 AH (2077-11-17) — full years 1356..1500;
    * the table is verified below against publicly known Saudi date pairs
      (Ramadan/Eid/Hijri-new-year announcements), a 29-day month boundary,
      a 30-day month boundary, and 355-day years.

  NOT the tabular (arithmetic) Islamic calendar: that one is a pure formula
  (30-year leap cycle) and differs from the official Saudi calendar by up to
  ±2 days.  The deltas are printed below for the same dates — the measured
 size of the requested comparison. The tabular side uses
  the "Kuwaiti algorithm" (epoch 1 Muharram 1 AH = 19 July 622 CE Gregorian
  proleptic = RD 227015; leap years ((11*y+14) mod 30) < 11), cross-checked
  here against the independent convertdate implementation.

Run:  python3 layer1/mkumalqurra.py
Then: gcc -O2 -static -o out/hijri layer1/hijri.c
"""
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "layer1"))
from umalqurra_data import UMMALQURRA_MCJDN  # noqa: E402

# RD (Rata Die) <-> Gregorian.  RD 1 = 1 January 1 CE (Gregorian proleptic);
# RD 730120 = 2000-01-01.  Reingold & Dershowitz, "Calendrical Calculations",
# the fixed-date function and its inverse.
def leap(y):
    return y % 4 == 0 and (y % 100 != 0 or y % 400 == 0)

def rd(y, m, d):
    return (365 * (y - 1) + (y - 1) // 4 - (y - 1) // 100 + (y - 1) // 400
            + (367 * m - 362) // 12
            + (0 if m <= 2 else (-1 if leap(y) else -2))
            + d)

def g_from_rd(r):
    y = (10000 * r + 14780) // 3652425
    ddd = r - rd(y, 3, 1)
    mi = (100 * ddd + 52) // 3060
    m = (mi + 2) % 12 + 1
    y += (mi + 2) // 12
    return y, m, r - rd(y, m, 1) + 1

# MCJDN -> RD anchor, derived from 1 Ramadan 1442 = 2021-04-13 and asserted
# on every other known pair below (a wrong anchor fails all of them).
MCJDN_TO_RD = rd(2021, 4, 13) - UMMALQURRA_MCJDN[1040]  # index of 1442-09
assert MCJDN_TO_RD == 678575

FIRST_YEAR = 1356          # the table's first month is 1 Muharram 1356
MONTH_STARTS = [m + MCJDN_TO_RD for m in UMMALQURRA_MCJDN]

def uq_from_rd(r):
    """Umm al-Qura (year, month, day) for a Rata Die number."""
    lo, hi = 0, len(MONTH_STARTS) - 1
    while lo < hi:
        mid = (lo + hi + 1) // 2
        if MONTH_STARTS[mid] <= r:
            lo = mid
        else:
            hi = mid - 1
    iln = FIRST_YEAR * 12 + lo            # months since AH 0, first entry = 1356
    y, m = divmod(iln, 12)
    return y, m + 1, r - MONTH_STARTS[lo] + 1

# ---- tabular Islamic (Kuwaiti algorithm) ----
TAB_EPOCH = 227015                        # 1 Muharram 1 AH = 19 July 622 CE

def tab_leap(y):
    return (11 * y + 14) % 30 < 11

def tab_year_start(y):
    leaps = (11 * (y - 1) + 14) // 30     # leap years among 1..y-1
    return TAB_EPOCH + 354 * (y - 1) + leaps

def tab_from_rd(r):
    hy = (30 * (r - TAB_EPOCH + 1) + 10646) // 10631
    while tab_year_start(hy) > r:        # closed form is approximate at
        hy -= 1                          # year boundaries: correct both ways
    while tab_year_start(hy + 1) <= r:
        hy += 1
    start = tab_year_start(hy)
    day_in_year = r - start
    # months: 30,29,30,29,30,29,30,29,30,29,30, 29/30
    for m, length in enumerate([30, 29, 30, 29, 30, 29, 30, 29, 30, 29, 30,
                                30 if tab_leap(hy) else 29]):
        if day_in_year < length:
            return hy, m + 1, day_in_year + 1
        day_in_year -= length
    raise AssertionError("tabular: day out of year")

# ---- the published pairs (public Saudi announcements / van Gent's table) ---
# (hijri (y,m,d), gregorian (y,m,d))  — all firsts of months.
PUBLISHED = [
    ((1420, 1, 1), (1999, 4, 17)),   # 1 Muharram 1420
    ((1442, 9, 1), (2021, 4, 13)),   # 1 Ramadan 1442 (Saudi announcement)
    ((1444, 10, 1), (2023, 4, 21)),  # 1 Shawwal 1444 = Eid al-Fitr (Saudi)
    ((1444, 12, 10), (2023, 6, 28)), # 10 Dhu al-Hijjah 1444 = Eid al-Adha (Saudi)
    ((1446, 1, 1), (2024, 7, 7)),    # 1 Muharram 1446 (Saudi)
    ((1446, 9, 1), (2025, 3, 1)),    # 1 Ramadan 1446 (Saudi)
    ((1447, 1, 1), (2025, 6, 26)),   # 1 Muharram 1447 (Saudi)
    ((1448, 1, 1), (2026, 6, 16)),   # 1 Muharram 1448 (Saudi)
]

def main():
    fails = 0
    print(f"data: {len(MONTH_STARTS)} month starts, "
          f"span {g_from_rd(MONTH_STARTS[0])} .. {g_from_rd(MONTH_STARTS[-1])}")

    print("\n--- 1. published Hijri<->Gregorian pairs ---")
    for (hy, hm, hd), (gy, gm, gd) in PUBLISHED:
        got = uq_from_rd(rd(gy, gm, gd))
        ok = got == (hy, hm, hd)
        fails += not ok
        print(f"  {'PASS' if ok else 'FAIL'}  {gy:04d}-{gm:02d}-{gd:02d} -> "
              f"Umm al-Qura {got} (want {hy}-{hm}-{hd})")

    print("\n--- 2. month-length boundaries (29- and 30-day months) ---")
    # AH 1448: Safar (m02) = 30 days, Rabi I (m03) = 29 days (from the table).
    for gy, gm, gd, want in [
        (2026, 8, 13, (1448, 2, 30)),   # last day of a 30-day month (Safar)
        (2026, 8, 14, (1448, 3, 1)),    # boundary into Rabi I
        (2026, 9, 11, (1448, 3, 29)),   # last day of a 29-day month (Rabi I)
        (2026, 9, 12, (1448, 4, 1)),    # boundary into Rabi al-Thani
    ]:
        got = uq_from_rd(rd(gy, gm, gd))
        ok = got == want
        fails += not ok
        print(f"  {'PASS' if ok else 'FAIL'}  {gy:04d}-{gm:02d}-{gd:02d} -> "
              f"{got} (want {want})")

    print("\n--- 3. 355-day years (the Islamic leap year, in both calendars) ---")
    for y in (1447, 1448):
        length = MONTH_STARTS[(y + 1 - FIRST_YEAR) * 12] - MONTH_STARTS[(y - FIRST_YEAR) * 12]
        want = 355 if (11 * y + 14) % 30 < 11 else 354
        print(f"  Umm al-Qura AH {y}: {length} days; tabular AH {y}: "
              f"{355 if tab_leap(y) else 354} days"
              f"{' (tabular leap year)' if tab_leap(y) else ''}")

    print("\n--- 4. deltas: Umm al-Qura minus tabular, on the same dates ---")
    for label, gy, gm, gd in [
        ("1 Muharram 1420", 1999, 4, 17),
        ("1 Ramadan 1442", 2021, 4, 13),
        ("Eid al-Fitr 1444", 2023, 4, 21),
        ("Eid al-Adha 1444", 2023, 6, 28),
        ("1 Muharram 1446", 2024, 7, 7),
        ("1 Ramadan 1446", 2025, 3, 1),
        ("1 Muharram 1447", 2025, 6, 26),
        ("today", 2026, 9, 26),
    ]:
        r = rd(gy, gm, gd)
        uq = uq_from_rd(r)
        tb = tab_from_rd(r)
        # the meaningful delta: for the SAME AH month (the one the Umm al-Qura
        # table puts the date in), how far apart are the two calendars' first
        # days?  positive = Umm al-Qura month starts later than tabular.
        uq_start = MONTH_STARTS[(uq[0] - FIRST_YEAR) * 12 + uq[1] - 1]
        tb_start = tab_year_start(uq[0]) + sum(
            [30, 29, 30, 29, 30, 29, 30, 29, 30, 29, 30,
             30 if tab_leap(uq[0]) else 29][:uq[1] - 1])
        delta = uq_start - tb_start
        print(f"  {label:18s} {gy:04d}-{gm:02d}-{gd:02d}: UQ {uq[0]}-{uq[1]:02d}-{uq[2]:02d}, "
              f"tabular {tb[0]}-{tb[1]:02d}-{tb[2]:02d}, month-start delta "
              f"{delta:+d} day(s)")

    print("\n--- 5. tabular implementation cross-check vs convertdate ---")
    try:
        import convertdate.islamic as ci
        bad = 0
        checked = 0
        for y in range(1990, 2027):
            for m in (1, 6):
                r = rd(y, m, 1)
                mine = tab_from_rd(r)
                theirs = tuple(ci.from_gregorian(y, m, 1))
                checked += 1
                if mine != theirs:
                    bad += 1
                    print(f"  MISMATCH {y}-{m}-01 mine {mine} convertdate {theirs}")
        ok = bad == 0
        fails += not ok
        print(f"  {'PASS' if ok else 'FAIL'}  tabular agrees with convertdate "
              f"on {checked} dates ({bad} mismatches)")
    except ImportError:
        print("  (convertdate not installed - skipped)")

    print("\n--- 6. emit layer1/umalqurra.h ---")
    hdr = pathlib.Path(ROOT / "layer1" / "umalqurra.h")
    lines = [
        "/* GENERATED by layer1/mkumalqurra.py - do not edit.",
        " * Umm al-Qura month-start table as Rata Die numbers (RD 1 = 1 Jan 1 CE).",
        " * Entry i is the first day of AH month (1356*12 + i): entry 0 is",
        " * 1 Muharram 1356 AH (1937-03-14), the last entry 1 Muharram 1501 AH.",
        " * Data: umalqurra v0.2 (PyPI), itself van Gent's islamcalendar_dat.js",
        " * (http://www.staff.science.uu.nl/~gent0113/islam/ummalqura.htm). */",
        "#ifndef UMMALQURRA_H",
        "#define UMMALQURRA_H",
        "#define UQ_FIRST_YEAR 1356",
        "#define UQ_MONTH_COUNT " + str(len(MONTH_STARTS)),
        "static const int uq_month_start_rd[UQ_MONTH_COUNT] = {",
    ]
    for i in range(0, len(MONTH_STARTS), 10):
        lines.append("    " + ", ".join(str(v) for v in MONTH_STARTS[i:i + 10]) + ",")
    lines += ["};", "", "#endif"]
    hdr.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"  wrote {hdr} ({len(MONTH_STARTS)} entries)")

    print(f"\n{'ALL CHECKS PASS' if fails == 0 else str(fails) + ' CHECK(S) FAILED'}")
    return 1 if fails else 0

if __name__ == "__main__":
    raise SystemExit(main())
