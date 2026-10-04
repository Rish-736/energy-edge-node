#!/usr/bin/env python3
"""
check_device_model.py -- keep the register map's three copies in agreement.

The map exists in three places, each for a different audience:

    common/modbus/meter_regs.h   the firmware compiles against it
    docs/device_model.json       tools and the gateway read it
    docs/MODBUS_REGISTERS.md     humans integrating from outside read it

Three copies of the same truth will drift apart eventually. This script fails
loudly when they do, which is cheaper than discovering it as a wrong reading
on a live bus.

Usage:  python tools/check_device_model.py
Exit:   0 = consistent, 1 = mismatch
"""

import json
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent

HEADER = ROOT / "common" / "modbus" / "meter_regs.h"
MODEL  = ROOT / "docs" / "device_model.json"
DOC    = ROOT / "docs" / "MODBUS_REGISTERS.md"

# json point name -> C symbol it must match
POINT_SYMBOLS = {
    "vendor_id":    "MREG_VENDOR_ID",
    "product_id":   "MREG_PRODUCT_ID",
    "fw_version":   "MREG_FW_VERSION",
    "map_version":  "MREG_MAP_VERSION",
    "capabilities": "MREG_CAPABILITIES",
    "voltage":      "MREG_VOLTAGE_F32",
    "current":      "MREG_CURRENT_F32",
    "power":        "MREG_POWER_F32",
    "energy":       "MREG_ENERGY_F32",
    "voltage_x100": "MREG_VOLTAGE_X100",
    "current_ma":   "MREG_CURRENT_MA",
    "power_x10":    "MREG_POWER_X10",
    "energy_mwh":   "MREG_ENERGY_MWH",
    "status_flags": "MREG_STATUS_FLAGS",
    "truth_mask":   "MREG_TRUTH_MASK",
    "sample_seq":   "MREG_SAMPLE_SEQ",
    "uptime_s":     "MREG_UPTIME_S",
    "overruns":     "MREG_OVERRUNS",
    "cmd":          "MREG_CMD",
}


def parse_header(text):
    """Collect MREG_* defines, resolving one level of symbolic aliasing."""
    defs = {
        m.group(1): int(m.group(2), 0)
        for m in re.finditer(r"#define\s+(MREG_\w+)\s+(0x[0-9A-Fa-f]+|\d+)\b", text)
    }
    for m in re.finditer(r"#define\s+(MREG_\w+)\s+(MREG_\w+)\s*$", text, re.M):
        if m.group(2) in defs:
            defs[m.group(1)] = defs[m.group(2)]
    return defs


def main():
    problems = []

    model = json.loads(MODEL.read_text())
    defs  = parse_header(HEADER.read_text())
    doc   = DOC.read_text()

    # 1. every json point matches its C symbol
    for point in model["points"]:
        sym = POINT_SYMBOLS.get(point["name"])
        if sym is None:
            problems.append(f"json point '{point['name']}' has no known C symbol")
        elif sym not in defs:
            problems.append(f"C symbol {sym} not found in the header")
        elif defs[sym] != point["addr"]:
            problems.append(
                f"{point['name']}: json addr {point['addr']} != header "
                f"{defs[sym]} ({sym})"
            )

    # 2. no two points overlap
    claimed = {}
    for point in model["points"]:
        for addr in range(point["addr"], point["addr"] + point["qty"]):
            if addr in claimed:
                problems.append(
                    f"address {addr} claimed by both '{claimed[addr]}' "
                    f"and '{point['name']}'"
                )
            claimed[addr] = point["name"]

    # 3. the poll block agrees with the header and covers everything a poll needs
    block = model["poll_block"]
    if (block["base"] != defs.get("MREG_POLL_BASE")
            or block["count"] != defs.get("MREG_POLL_COUNT")):
        problems.append(
            f"poll_block json ({block['base']},{block['count']}) != header "
            f"({defs.get('MREG_POLL_BASE')},{defs.get('MREG_POLL_COUNT')})"
        )

    covered = range(block["base"], block["base"] + block["count"])
    for point in model["points"]:
        if point["class"] in ("measurement", "status", "diagnostic"):
            for addr in range(point["addr"], point["addr"] + point["qty"]):
                if addr not in covered:
                    problems.append(
                        f"'{point['name']}' addr {addr} is outside the poll block, "
                        f"so one poll cannot fetch it"
                    )

    # 4. identity values
    if model["vendor_id"] != 0x4545:
        problems.append(f"vendor_id {model['vendor_id']} != 0x4545")
    if model["model_version"] != defs.get("MREG_MAP_VERSION_VALUE"):
        problems.append(
            f"model_version {model['model_version']} != header "
            f"MREG_MAP_VERSION_VALUE {defs.get('MREG_MAP_VERSION_VALUE')}"
        )

    # 5. the human-facing doc mentions every address, in hex
    for point in model["points"]:
        token = f"0x{point['addr']:04X}"
        if token not in doc:
            problems.append(
                f"MODBUS_REGISTERS.md never mentions {token} "
                f"('{point['name']}')"
            )

    # --- report ---
    print(f"header : {len(defs)} MREG_* symbols")
    print(f"model  : {len(model['points'])} points, "
          f"{len(claimed)} registers claimed")
    print(f"poll   : {block['base']}..{block['base'] + block['count'] - 1} "
          f"({block['count']} registers)")

    if problems:
        print(f"\nFAIL -- {len(problems)} problem(s):")
        for p in problems:
            print(f"  - {p}")
        return 1

    print("\nOK -- header, device model and documentation agree")
    return 0


if __name__ == "__main__":
    sys.exit(main())
