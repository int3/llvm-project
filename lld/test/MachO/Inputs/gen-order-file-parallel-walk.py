# Generates the inputs for order-file-parallel-walk.s: enough object files to
# cross buildInputSectionPriorities' 64-file chunk boundary, an archive member
# whose __text is a single input section holding two ordered symbols, and an
# order file that names the symbols in reverse file order.
#
# Usage: gen-order-file-parallel-walk.py <output dir> <llvm-mc> <triple>
import os
import subprocess
import sys

outDir, mc, triple = sys.argv[1], sys.argv[2], sys.argv[3]
NUM_FILES = 66


def assemble(name, text):
    asmPath = os.path.join(outDir, name + ".s")
    with open(asmPath, "w") as f:
        f.write(text)
    subprocess.check_call(
        [mc, "-filetype=obj", "-triple=" + triple, asmPath,
         "-o", os.path.join(outDir, name + ".o")])


for i in range(NUM_FILES):
    body = [".subsections_via_symbols", ".text"]
    if i == 0:
        # The entry point is not in the order file, so it sorts last.
        body += [".globl Entry", "Entry:", "  retq"]
    body += [".globl sym%02d" % i, "sym%02d:" % i, "  retq"]
    assemble("f%02d" % i, "\n".join(body) + "\n")

# No .subsections_via_symbols: both globals share one input section, so the
# section priority is the min over the two matched order-file entries.
assemble("member", "\n".join([
    ".text",
    ".globl _amem0",
    "_amem0:",
    "  retq",
    ".globl _amem1",
    "_amem1:",
    "  retq",
]) + "\n")

order = ["sym00"]  # Matched again below; the earlier entry wins.
order += ["sym%02d" % i for i in range(NUM_FILES - 1, -1, -1)]
order += ["_amem0", "libx.a(member.o):_amem1"]
with open(os.path.join(outDir, "order"), "w") as f:
    f.write("\n".join(order) + "\n")
