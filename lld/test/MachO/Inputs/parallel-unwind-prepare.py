# Generate more than two prepare chunks, with alternating personality aliases
# and both section and symbol relocations. Each address has just one CUE owner.
import sys

undefined = len(sys.argv) > 1
print(".text\n.subsections_via_symbols")
for i in range(9000):
    print(f".globl _f{i}\n_f{i}:\n retq")
if not undefined:
    print(".globl _p1, _p1_alias, _p2\n_p1:\nLpersonality1:\n retq")
    print("_p1_alias = _p1\n_p2:\n retq")
print(".section __LD,__compact_unwind,regular,debug\n.p2align 3")
for i in range(9000):
    personalities = ["_p2", "_p1", "_p1_alias", "Lpersonality1"]
    personality = ["_missing2", "_missing1"][i % 2] if undefined else personalities[i % 4]
    print(f".quad _f{i}\n.long 1\n.long 0x02010000\n.quad {personality}\n.quad 0")
