"""The undefined flags of a SingleStepTests metadata.json, for ssts.c:
"F6.4 65323" per line (the flags to compare), from opcodes and their reg
forms that have a "flags-mask".

    python3 -I masks.py data/8088/metadata.json > build/masks_8088.txt
"""
import json
import sys


def main():
    with open(sys.argv[1]) as f:
        opcodes = json.load(f)["opcodes"]
    for op, info in opcodes.items():
        if "flags-mask" in info:
            print(op, info["flags-mask"])
        for reg, sub in info.get("reg", {}).items():
            if "flags-mask" in sub:
                print("%s.%s %d" % (op, reg, sub["flags-mask"]))


main()
