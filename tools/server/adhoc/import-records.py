#!/usr/bin/env python3
"""Carry the old dedicated server's lap records over to the relay.

    import-records.py <records.xml> <records.tsv>

records.xml is what the dedicated server kept in its home directory
(~/.speed-dreams-2/records.xml). The lines are appended to records.tsv; the
relay keeps each driver's best per track when it loads the file, so importing
twice, or into a table that already has times, does no harm.
"""
import sys
import xml.etree.ElementTree as ET


def section(parent, name):
    for s in parent.findall("section"):
        if s.get("name") == name:
            return s
    return None


def attr(sec, name):
    for a in sec:
        if a.tag in ("attstr", "attnum") and a.get("name") == name:
            return a.get("val", "")
    return ""


def clean(s, allow_space):
    s = "".join(c for c in s if c >= " " and c != "\x7f")[:32].strip()
    if not allow_space:
        s = s.replace(" ", "_")
    return s or "?"


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    root = ET.parse(sys.argv[1]).getroot()
    recs = section(root, "records")
    lines = []
    for track in (recs if recs is not None else []):
        if track.tag != "section":
            continue
        name, cat = attr(track, "track"), attr(track, "category")
        times = section(track, "times")
        if not name or times is None:
            continue
        for t in times:
            if t.tag != "section":
                continue
            try:
                secs = float(attr(t, "time"))
            except ValueError:
                continue
            driver, car = attr(t, "name"), attr(t, "car")
            if driver and secs > 0:
                lines.append(f"{clean(name, False)}\t{clean(cat, False) if cat else '-'}\t{secs:.3f}"
                             f"\t{clean(car, False)}\t{clean(driver, True)}\n")
    with open(sys.argv[2], "a", encoding="utf-8") as f:
        f.writelines(lines)
    print(f"imported {len(lines)} lap times")


if __name__ == "__main__":
    main()
