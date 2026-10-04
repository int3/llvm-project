# REQUIRES: aarch64
# RUN: split-file %s %t
# RUN: %python %t/gen.py > %t/input.s
# RUN: llvm-mc -filetype=obj -triple=arm64-apple-darwin %t/input.s -o %t.o
# RUN: mkdir -p %t/1 %t/2 %t/8 %t/16
# RUN: %lld -arch arm64 --threads=1 -no_uuid -o %t/1/linked-output %t.o
# RUN: %lld -arch arm64 --threads=2 -no_uuid -o %t/2/linked-output %t.o
# RUN: %lld -arch arm64 --threads=8 -no_uuid -o %t/8/linked-output %t.o
# RUN: %lld -arch arm64 --threads=16 -no_uuid -o %t/16/linked-output %t.o
# RUN: cmp %t/1/linked-output %t/2/linked-output
# RUN: cmp %t/1/linked-output %t/8/linked-output
# RUN: cmp %t/1/linked-output %t/16/linked-output

## More than sixteen 4096-input chunks exercise slot reuse, empty ranges,
## forward and backward targets across chunks and multiple forced fronts.
## Two threads leave only one producer beside the prepare tasks, exercising
## the consumer's inline fallback without relying on worker scheduling.
#--- gen.py
print('.text\n.subsections_via_symbols\n.globl _main\n_main:\nbl _end')
for i in range(70000):
    print(f'.globl _f{i}\n_f{i}:')
    if i in (4095, 65535):
        print('.space 0x8000000')
    if i % 7:
        print('bl _end\nbl _main')
    print('ret')
print('.globl _end\n_end:\nret')
