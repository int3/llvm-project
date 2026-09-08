# REQUIRES: x86

# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %s -o %t.o
# RUN: %lld %t.o -o %t
# RUN: obj2yaml %t | FileCheck %s

# CHECK:      segname:         __TEXT
# CHECK-NEXT: vmaddr:          4294967296
# CHECK:      RebaseOpcodes:
# CHECK-NEXT: - Opcode:          REBASE_OPCODE_SET_TYPE_IMM
# CHECK-NEXT:   Imm:             1
# CHECK-NEXT: - Opcode:          REBASE_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB
# CHECK-NEXT:   Imm:             2
# CHECK-NEXT:   ExtraData:       [ 0x0 ]
# CHECK-NEXT: - Opcode:          REBASE_OPCODE_DO_REBASE_IMM_TIMES
# CHECK-NEXT:   Imm:             1
# CHECK-NEXT: - Opcode:          REBASE_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB
# CHECK-NEXT:   Imm:             3
# CHECK-NEXT:   ExtraData:       [ 0x0 ]
# CHECK-NEXT: - Opcode:          REBASE_OPCODE_DO_REBASE_IMM_TIMES
# CHECK-NEXT:   Imm:             1
# CHECK-NEXT: - Opcode:          REBASE_OPCODE_DONE

.text
.globl _main
_main:
.globl _target
_target:
  retq

.section __DATA,__data
.quad _target

.section __DATA_CONST,__const
.quad _target
