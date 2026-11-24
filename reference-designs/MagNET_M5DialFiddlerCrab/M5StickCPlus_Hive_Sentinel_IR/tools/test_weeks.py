#!/usr/bin/env python3
"""Check the firmware's ISO week key against Python's isocalendar().

    cc -I../src -o /tmp/test_logic test_logic.c ../src/presence.c ../src/stats.c ../src/activity.c ../src/portal.c
    /tmp/test_logic weeks | python3 test_weeks.py
"""
import datetime
import sys

bad = n = 0
for line in sys.stdin:
    day, key = line.split()
    y, w, _ = datetime.date.fromisoformat(day).isocalendar()
    n += 1
    if int(key) != y * 100 + w:
        bad += 1
        print(f"MISMATCH {day}: firmware {key}, iso {y * 100 + w}")
print(f"{n} days checked, {bad} mismatches")
sys.exit(1 if bad or not n else 0)
