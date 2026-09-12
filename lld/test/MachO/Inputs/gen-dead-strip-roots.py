# Generates an object with enough globals to cross MarkLive's parallel
# root-selection threshold. Half the leaves match the export-list glob and stay
# live along with the cstring and literal8 pieces they reference; the other half
# are unreferenced and must be stripped along with their literal pieces.
N = 1600
out = []
out.append(".text")
out.append(".globl _main")
out.append("_main:")
out.append("  retq")
for tag in ("E", "D"):
    for i in range(N):
        out.append(".globl _leaf%s%d" % (tag, i))
        out.append("_leaf%s%d:" % (tag, i))
        out.append("  leaq L_str%s%d(%%rip), %%rax" % (tag, i))
        out.append("  movq L_lit%s%d(%%rip), %%rcx" % (tag, i))
        out.append("  retq")
out.append(".section __TEXT,__cstring")
for tag, word in (("E", "live"), ("D", "dead")):
    for i in range(N):
        out.append("L_str%s%d:" % (tag, i))
        out.append('  .asciz "%s%s%d"' % (word, tag, i))
out.append(".section __TEXT,__literal8")
out.append(".p2align 3")
for tag in ("E", "D"):
    for i in range(N):
        out.append("L_lit%s%d:" % (tag, i))
        out.append("  .quad %d" % (i if tag == "E" else i + N))
out.append(".subsections_via_symbols")
print("\n".join(out))
