# REQUIRES: x86

# A live root fans out to more than MarkLive's serial-frontier threshold. This
# exercises the parallel traversal while keeping the expected output simple.
# RUN: rm -rf %t
# RUN: mkdir -p %t
# RUN: %python -c "print('.globl _main\\n_main:\\n' + ''.join('callq _leaf%d\\n' % i for i in range(1024)) + 'retq\\n' + ''.join('.globl _leaf%d\\n.private_extern _leaf%d\\n_leaf%d:\\nretq\\n' % (i, i, i) for i in range(1024)) + '.subsections_via_symbols\\n')" > %t/parallel.s
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-macos %t/parallel.s -o %t/parallel.o
# RUN: %lld -lSystem -dead_strip --threads=1 -no_uuid %t/parallel.o -o %t/threads1
# RUN: %lld -lSystem -dead_strip --threads=4 -no_uuid %t/parallel.o -o %t/threads4
# RUN: cmp %t/threads1 %t/threads4
