import sys
out = sys.stdout
out.write('.text\n.globl _main\n_main: retq\n.data\n')
# Include a boundary-sized name, a name bigger than one chunk, and a tail.
for i, length in enumerate([1024 * 1024 - 3, 2 * 1024 * 1024] + [16384] * 450 + [11]):
    prefix = '_s' + str(i) + '_'
    name = prefix + 'x' * (length - len(prefix))
    out.write(f'.globl {name}\n{name}: .quad 0\n')
