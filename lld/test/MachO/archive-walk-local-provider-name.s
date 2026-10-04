# REQUIRES: x86
## An archive member that names a symbol in the archive's index but does not
## define it externally: an N_EXT label in a __cfstring section is turned into a
## local by the parse. The walker reserves the slot of every name an extracted
## member is indexed under, at the index entry the walk passed, and since the
## member parses stream, it applies those reservations only once the parses are
## joined. Whether or not another member references the name, the output and the
## map must be what the serial linker produces.

# RUN: rm -rf %t; split-file %s %t
# RUN: mkdir -p %t/a1 %t/a2 %t/a8 %t/b2 %t/b8 %t/bns
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/main.s -o %t/main.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/mainuser.s -o %t/mainuser.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/m.s -o %t/m.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/u.s -o %t/u.o
# RUN: llvm-ar --format=darwin rcs %t/lib.a %t/m.o
# RUN: llvm-ar --format=darwin rcs %t/user.a %t/u.o

## Nothing else references _cf.
# RUN: %lld -lSystem -undefined dynamic_lookup -o %t/a1/out --threads=1 -map %t/a1/map %t/main.o %t/lib.a 2> %t/a1/err
# RUN: %lld -lSystem -undefined dynamic_lookup -o %t/a2/out --threads=2 -map %t/a2/map %t/main.o %t/lib.a 2> %t/a2/err
# RUN: %lld -lSystem -undefined dynamic_lookup -o %t/a8/out --threads=8 -map %t/a8/map %t/main.o %t/lib.a 2> %t/a8/err
# RUN: cmp %t/a1/out %t/a2/out
# RUN: cmp %t/a1/out %t/a8/out
# RUN: sed -e 's/a[0-9]\/out/OUT/' %t/a1/map > %t/a1/map.norm
# RUN: sed -e 's/a[0-9]\/out/OUT/' %t/a2/map > %t/a2/map.norm
# RUN: sed -e 's/a[0-9]\/out/OUT/' %t/a8/map > %t/a8/map.norm
# RUN: cmp %t/a1/map.norm %t/a2/map.norm
# RUN: cmp %t/a1/map.norm %t/a8/map.norm
# RUN: cmp %t/a1/err %t/a2/err
# RUN: cmp %t/a1/err %t/a8/err

## A member of a later archive references _cf. The serial path cannot be the
## reference here: it leaves the lazy symbol of the index entry in the slot,
## which nothing defines, and crashes in addNonLazyBindingEntries() on the
## reference (upstream ld64.lld 20 links it). So this pins only that the
## walker's answer does not depend on the thread count or on streaming, i.e. on
## when the reservation lands relative to the member parses.
# RUN: %lld -lSystem -undefined dynamic_lookup -o %t/b2/out --threads=2 -map %t/b2/map %t/mainuser.o %t/lib.a %t/user.a 2> %t/b2/err
# RUN: %lld -lSystem -undefined dynamic_lookup -o %t/b8/out --threads=8 -map %t/b8/map %t/mainuser.o %t/lib.a %t/user.a 2> %t/b8/err
# RUN: env LLD_NO_STREAM_MEMBER_PARSE=1 %lld -lSystem -undefined dynamic_lookup -o %t/bns/out --threads=8 -map %t/bns/map %t/mainuser.o %t/lib.a %t/user.a 2> %t/bns/err
# RUN: cmp %t/b2/out %t/b8/out
# RUN: cmp %t/b2/out %t/bns/out
# RUN: sed -e 's/b[0-9ns]*\/out/OUT/' %t/b2/map > %t/b2/map.norm
# RUN: sed -e 's/b[0-9ns]*\/out/OUT/' %t/b8/map > %t/b8/map.norm
# RUN: sed -e 's/b[0-9ns]*\/out/OUT/' %t/bns/map > %t/bns/map.norm
# RUN: cmp %t/b2/map.norm %t/b8/map.norm
# RUN: cmp %t/b2/map.norm %t/bns/map.norm
# RUN: cmp %t/b2/err %t/b8/err
# RUN: cmp %t/b2/err %t/bns/err

#--- main.s
.globl _main
_main:
  callq _f
  retq

#--- mainuser.s
.globl _main
_main:
  callq _f
  callq _g
  retq

#--- m.s
.globl _f, _cf
.text
_f:
  retq
.cstring
L_.str:
  .asciz "foo"
.section __DATA,__cfstring
.p2align 3
_cf:
  .quad ___CFConstantStringClassReference
  .long 1992
  .space 4
  .quad L_.str
  .quad 3

#--- u.s
.globl _g
.text
_g:
  retq
.data
  .quad _cf
