# REQUIRES: x86

## The cold and negative-priority stable_partition()s that sortSegmentsAndSections()
## ran over every output section's input list are folded into the count-and-scatter
## in createOutputSections(), which now writes each input straight into its class's
## run. buildInputSectionPriorities() runs before that pass because it clears
## isCold for order-file sections. The generated input spans several 4096-section
## chunks, so every class has members in several chunks, and it includes sections
## that are both cold and named by the order file: the order file wins, so they
## belong in the prioritized run rather than at the end.

# RUN: rm -rf %t; mkdir -p %t/one %t/eight
# RUN: %python %S/Inputs/gen-section-partition-chunks.py 9000 asm > %t/test.s
# RUN: %python %S/Inputs/gen-section-partition-chunks.py 9000 order > %t/order.txt
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-macos %t/test.s -o %t/test.o

## Without an order file only the cold partition applies.
# RUN: %lld -lSystem -no_uuid --threads=1 %t/test.o -o %t/one/nocold
# RUN: %lld -lSystem -no_uuid --threads=8 %t/test.o -o %t/eight/nocold
# RUN: cmp %t/one/nocold %t/eight/nocold

## With an order file all priorities are negative, so the three-way split runs.
# RUN: %lld -lSystem -no_uuid -order_file %t/order.txt --threads=1 %t/test.o -o %t/one/out
# RUN: %lld -lSystem -no_uuid -order_file %t/order.txt --threads=8 %t/test.o -o %t/eight/out
# RUN: cmp %t/one/out %t/eight/out

## And with dead-stripping and ICF on top, which change which inputs are omitted.
# RUN: %lld -lSystem -no_uuid -order_file %t/order.txt -dead_strip --icf=all \
# RUN:   --threads=1 %t/test.o -o %t/one/icf
# RUN: %lld -lSystem -no_uuid -order_file %t/order.txt -dead_strip --icf=all \
# RUN:   --threads=8 %t/test.o -o %t/eight/icf
# RUN: cmp %t/one/icf %t/eight/icf

## Layout of __text: the order file's sections come first, in order-file order
## (which is the reverse of the input order), then the ordinary sections in input
## order, then the cold ones. _f00003 is cold and not in the order file, so it is
## in the cold run; _f00801 is both cold and in the order file, so the order file
## takes precedence and it sits in the prioritized prefix, ahead of every
## ordinary section.
# RUN: llvm-objdump --syms %t/eight/out > %t/syms.txt
# RUN: %python %S/Inputs/check-section-partition.py %t/syms.txt
