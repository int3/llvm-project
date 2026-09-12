#!/usr/bin/env python3
"""Generate an input for dead-strip-zero-walk-roots.s.

The locals marked .no_dead_strip are emitted two at a time into two different
sections, so their nlist order (interleaved) differs from the per-section,
address-sorted order in which ObjFile::parseSymbols() creates them. markLive()
must still enqueue them in nlist order. The file also carries the other kinds of
root that used to be found by walking all of inputSections: a section marked
no_dead_strip, mod_init_funcs and mod_term_funcs sections, and -- for
createAbsolute()'s path -- a local absolute symbol marked .no_dead_strip.

It also carries the shouldIgnoreLabel() case: an N_EXT label in a __DATA,__cfstring
section marked .no_dead_strip. createDefined() makes that a *local* Defined, so it
is a root even though its nlist entry has N_EXT set, and the nlist-only scope test
in parseSymbols() has to reproduce the exception to find it.
"""
import sys

tag = sys.argv[1]
count = int(sys.argv[2])

out = []
w = out.append
w(".subsections_via_symbols")
if tag == "a":
    w(".text")
    w(".globl _main")
    w("_main:")
    w("  retq")

for i in range(count):
    n = "%s%03d" % (tag, i)
    w(".section __DATA,__seca")
    w(".p2align 3")
    w("_keepP%s:" % n)
    w("  .quad _targetP%s" % n)
    w(".no_dead_strip _keepP%s" % n)
    w(".section __DATA,__secb")
    w(".p2align 3")
    w("_keepQ%s:" % n)
    w("  .quad _targetQ%s" % n)
    w(".no_dead_strip _keepQ%s" % n)
    w(".text")
    w(".globl _targetP%s, _targetQ%s, _unrefR%s" % (n, n, n))
    w("_targetP%s:" % n)
    w("  movl $%d, %%eax" % (3 * i))
    w("  retq")
    w("_targetQ%s:" % n)
    w("  movl $%d, %%eax" % (3 * i + 1))
    w("  retq")
    w("_unrefR%s:" % n)
    w("  movl $%d, %%eax" % (3 * i + 2))
    w("  retq")

# An N_EXT label in a cfstring section: shouldIgnoreLabel() sends it down
# createDefined()'s local path, so it is a dead-strip root despite N_EXT. A scope
# test of just `not (n_type & N_EXT)` would miss it and strip _targetIgnored.
w(".section __DATA,__cfstring")
w(".p2align 3")
w(".globl _extLabel%s" % tag)
w("_extLabel%s:" % tag)
w("  .quad _targetIgnored%s" % tag)
w(".no_dead_strip _extLabel%s" % tag)

w(".section __DATA,__rootsec%s,regular,no_dead_strip" % tag)
w(".p2align 3")
w("  .quad _targetSection%s" % tag)

w(".section __DATA,__mod_init_func,mod_init_funcs")
w(".p2align 3")
w("  .quad _initFn%s" % tag)

w(".section __DATA,__mod_term_func,mod_term_funcs")
w(".p2align 3")
w("  .quad _termFn%s" % tag)

w(".text")
w(".globl _targetSection%s, _initFn%s, _termFn%s" % (tag, tag, tag))
w(".globl _targetIgnored%s" % tag)
w("_targetIgnored%s:" % tag)
w("  movl $14, %eax")
w("  retq")
w("_targetSection%s:" % tag)
w("  movl $11, %eax")
w("  retq")
w("_initFn%s:" % tag)
w("  movl $12, %eax")
w("  retq")
w("_termFn%s:" % tag)
w("  movl $13, %eax")
w("  retq")

w("_absLocal%s = 0x%d0000" % (tag, 1 if tag == "a" else 2))
w(".no_dead_strip _absLocal%s" % tag)

print("\n".join(out))
