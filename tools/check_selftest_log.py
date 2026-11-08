#!/usr/bin/env python3
"""Check the boot decisions made by the Basecamp bootloader in a self-test log."""
import os
import re
import sys

# boots per slot: select + after esp_restart + after panic (+ after deep sleep)
BOOTS = 4 if os.environ.get("SELFTEST_DEEP_SLEEP") == "1" else 3

log = open(sys.argv[1], errors="replace").read()
decisions = []
for m in re.finditer(r"basecamp: (cold reset|no slot selected|booting ota_(\d)|ota_(\d) @0x[0-9a-f]+ is empty or invalid)", log):
    if m.group(1).startswith("cold"):
        decisions.append("cold->launcher")
    elif m.group(1).startswith("no slot"):
        decisions.append("none->launcher")
    elif m.group(2) is not None:
        decisions.append(f"ota_{m.group(2)}")
    else:
        decisions.append(f"bad ota_{m.group(3)}->launcher")

expected = ["cold->launcher"]
for s in range(3):
    expected += [f"ota_{s}"] * BOOTS + ["none->launcher"]
expected += ["bad ota_2->launcher", "none->launcher"]

ok = decisions == expected and "SELFTEST PASS" in log and "SELFTEST FAIL" not in log
print("bootloader decisions:")
for i, d in enumerate(decisions):
    mark = "ok " if i < len(expected) and expected[i] == d else "XX "
    print(f"  {mark}{d}")
if not ok:
    print("expected:", expected)
print("RESULT:", "PASS" if ok else "FAIL")
sys.exit(0 if ok else 1)
