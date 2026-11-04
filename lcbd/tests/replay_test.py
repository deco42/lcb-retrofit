#!/usr/bin/env python3
import csv
import struct
import subprocess
import sys

FLOATS = {'leg_angle', 'die_angle', 'line_angle', 'station_angle'}
NOT_COMPARED = {'n', 't'}
LISTED_MAX = 10


def norm(key, value):
    if value == '':
        return None
    if key in FLOATS:
        return struct.pack('<f', float(value))
    return int(value)


def main():
    tool, pcap, expected = sys.argv[1:4]
    out = subprocess.run([tool, pcap, '--csv'], check=True, capture_output=True, text=True).stdout
    got = list(csv.DictReader(out.splitlines()))
    with open(expected) as f:
        reader = csv.DictReader(f)
        want = list(reader)
    fields = [k for k in reader.fieldnames if k not in NOT_COMPARED]
    bad = 0
    for i, (g, w) in enumerate(zip(got, want)):
        diff = [k for k in fields if norm(k, g[k]) != norm(k, w[k])]
        if diff:
            bad += 1
            if bad <= LISTED_MAX:
                print('frame %d differs in %s' % (i, ', '.join(diff)))
    print('%s: %d frames replayed, %d expected, %d differ' % (pcap, len(got), len(want), bad))
    return 0 if bad == 0 and len(got) == len(want) else 1


if __name__ == '__main__':
    sys.exit(main())
