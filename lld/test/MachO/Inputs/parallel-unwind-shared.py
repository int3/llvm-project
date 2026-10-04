# Unique function bodies survive ICF, but their identical CUEs and LSDAs fold.
# Interleave CUEs and FDEs in both personality orders in the mixed variants.
import sys

mode = sys.argv[1]
mixed = mode.startswith('mixed')
undefined = mode == 'undefined'
print('.text\n.subsections_via_symbols')
for i in range(9000):
    print(f'.globl _f{i}\n_f{i}:')
    if mixed and (i % 2 == (mode == 'mixed-reverse')):
        print('.cfi_startproc\n.cfi_personality 155, _p1_alias')
        print('.cfi_lsda 16, Llsda1\n.cfi_def_cfa_offset 16\n.cfi_escape 0x2e, 0x10')
    print(f'movl ${i}, %eax\nretq')
    if mixed and (i % 2 == (mode == 'mixed-reverse')):
        print('.cfi_endproc')
if not undefined:
    print('.globl _p1, _p1_alias, _p2\n_p1:\nLpersonality1:\nretq')
    print('_p1_alias = _p1\n_p2:\nnop\nretq')
print('.section __TEXT,__gcc_except_tab')
# The first LSDA is deliberately unreferenced, so the second canonicalizes.
print('_lsda0:\nLlsda0:\n.byte 0\n_lsda1:\nLlsda1:\n.byte 0')
print('.section __LD,__compact_unwind,regular,debug\n.p2align 3')
for i in range(9000):
    if mixed and (i % 2 == (mode == 'mixed-reverse')):
        continue
    p = ['_p2', '_p1', '_p1_alias', 'Lpersonality1'][i % 4]
    if mixed:
        p = '_p1'  # CU and FDE must canonicalize to one personality.
    if undefined:
        p = ['_missing2', '_missing1'][i % 2]
    print(f'.quad _f{i}\n.long 6\n.long 0x02010000\n.quad {p}\n.quad Llsda1')
