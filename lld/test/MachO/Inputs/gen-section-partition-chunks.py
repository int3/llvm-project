#!/usr/bin/env python3
"""Generate an input for section-partition-order-cold.s.

createOutputSections() scatters the section table in chunks of 4096 and now lays
each output section's inputs down already split into the three runs that
sortSegmentsAndSections() used to produce with two stable_partition()s: sections
with a negative explicit priority, ordinary sections, then cold sections. This
input has well over three chunks' worth of text sections so that every class has
members in several chunks, and mixes:

  * ordinary functions,
  * cold functions (N_COLD_FUNC via .desc),
  * functions named by the order file, which get negative priorities, and
  * functions that are both cold and named by the order file -- the order file
    clears isCold, so these must end up in the prioritized run, not the cold one.

It also emits data sections so that a second output section exercises the
per-output-section class bases.
"""
import sys

count = int(sys.argv[1])
mode = sys.argv[2]  # "asm" or "order"


def name(i):
    return "_f%05d" % i


def is_cold(i):
    return i % 7 == 3


def is_ordered(i):
    return i % 11 == 5


if mode == "order":
    # Reverse index order, so the order file's priorities do not agree with the
    # input order and the stable sort over the negative run is observable.
    for i in reversed(range(count)):
        if is_ordered(i):
            print(name(i))
    sys.exit(0)

out = []
w = out.append
w(".subsections_via_symbols")
w(".text")
w(".globl _main")
w("_main:")
w("  retq")
for i in range(count):
    n = name(i)
    w(".text")
    w(".globl %s" % n)
    if is_cold(i):
        w(".desc %s, 0x400" % n)
    w("%s:" % n)
    w("  movl $%d, %%eax" % i)
    w("  retq")
    w(".section __DATA,__data")
    w(".p2align 2")
    w("_d%05d:" % i)
    w("  .long %d" % i)
print("\n".join(out))
