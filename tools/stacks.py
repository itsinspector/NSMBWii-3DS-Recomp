"""Symbolize the watchdog stack samples in a PC run log.

    python tools/stacks.py build/run.log

Host frames f_XXXXXXXX are guest functions; each is shown with the nearest
known guest symbol from build/gen/symbols.txt.
"""
import bisect
import re
import sys

names = {}
for line in open("build/gen/symbols.txt"):
    a, n = line.split(None, 1)
    names[int(a, 16)] = n.strip()
keys = sorted(names)


def nm(a):
    i = bisect.bisect_right(keys, a) - 1
    if i < 0 or a - keys[i] > 0x2000:
        return "?"
    return names[keys[i]] + ("" if keys[i] == a else f"+{a - keys[i]:x}")


for line in open(sys.argv[1]):
    if "host thread" not in line:
        continue
    head, _, rest = line.partition(":")
    frames = []
    for tok in rest.split():
        m = re.fullmatch(r"f_([0-9A-F]{8})", tok)
        frames.append(f"{m.group(1)}:{nm(int(m.group(1), 16))[:44]}" if m else tok)
    print(head.strip() + ": " + " <- ".join(frames[:10]))
