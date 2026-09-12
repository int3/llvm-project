#!/usr/bin/env python3
"""Check the __text layout produced by section-partition-order-cold.s."""
import re
import sys

count = 9000
addrs = {}
for line in open(sys.argv[1]):
    m = re.match(r"^([0-9a-f]+)\s+g\s+F __TEXT,__text (_f[0-9]{5})$", line.strip())
    if m:
        addrs[m.group(2)] = int(m.group(1), 16)


def name(i):
    return "_f%05d" % i


ordered = [name(i) for i in reversed(range(count)) if i % 11 == 5]
cold = [name(i) for i in range(count) if i % 7 == 3 and i % 11 != 5]
ordinary = [name(i) for i in range(count) if i % 7 != 3 and i % 11 != 5]

missing = [n for n in ordered + cold + ordinary if n not in addrs]
assert not missing, "missing symbols: %s" % missing[:5]

# The order-file run comes first, in order-file order.
prev = -1
for n in ordered:
    assert addrs[n] > prev, "order-file run out of order at %s" % n
    prev = addrs[n]
# Then the ordinary run, in input order, entirely after it.
for n in ordinary:
    assert addrs[n] > prev, "ordinary section %s is not after the ordered run" % n
prev2 = prev
for n in ordinary:
    assert addrs[n] > prev2, "ordinary run out of order at %s" % n
    prev2 = addrs[n]
# Then the cold run, in input order, entirely after that.
for n in cold:
    assert addrs[n] > prev2, "cold section %s is not after the ordinary run" % n
prev3 = prev2
for n in cold:
    assert addrs[n] > prev3, "cold run out of order at %s" % n
    prev3 = addrs[n]

# A section that is both cold and in the order file is prioritized, not cold.
both = [name(i) for i in range(count) if i % 7 == 3 and i % 11 == 5]
assert both, "the input should contain cold order-file sections"
for n in both:
    assert addrs[n] <= prev, "%s should be in the prioritized run" % n

print("ok: %d ordered, %d ordinary, %d cold, %d both"
      % (len(ordered), len(ordinary), len(cold), len(both)))
