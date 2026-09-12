# REQUIRES: x86
## E006 design 7 stage 2, step S5: the archive walker streams each extracted
## member's parse to a worker as soon as the member has its position, so the
## parses fold into the symbol table while the serial select loop is still
## running. Everything the serial order fixes is decided on the main thread
## before the parse is queued, so a diagnostic a member's parse emits has to
## replay exactly where the serial linker emitted it -- which is ordinal order
## for the fold's deferred records and extraction order for a member's own parse
## errors -- at every thread count, and whether or not streaming is on.

# RUN: rm -rf %t; split-file %s %t
# RUN: mkdir -p %t/g1 %t/g2 %t/g8

# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/dupmain.s -o %t/dupmain.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/dupa.s -o %t/dupa.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/dupb.s -o %t/dupb.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/relmain.s -o %t/relmain.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/good1.s -o %t/good1.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %t/good2.s -o %t/good2.o
# RUN: yaml2obj %t/bad1.yaml -o %t/bad1.o
# RUN: yaml2obj %t/bad2.yaml -o %t/bad2.o
# RUN: llvm-ar --format=darwin rcs %t/dup.a %t/dupa.o %t/dupb.o
# RUN: llvm-ar --format=darwin rcs %t/bad.a %t/bad1.o %t/bad2.o
# RUN: llvm-ar --format=darwin rcs %t/good.a %t/good1.o %t/good2.o

## A duplicate strong definition across two members, the second of which is
## extracted from inside the first one's undefined walk. The parse of the nested
## member is therefore queued before the parse of the member that pulled it in,
## and the two definitions can fold in either order, but the diagnostic belongs
## to the lower ordinal, so dupa.o must always be named first.
# RUN: not %lld -lSystem -o /dev/null --threads=1 %t/dupmain.o %t/dup.a 2>&1 | FileCheck %s --check-prefix=DUP
# RUN: not %lld -lSystem -o /dev/null --threads=2 %t/dupmain.o %t/dup.a 2>&1 | FileCheck %s --check-prefix=DUP
# RUN: not %lld -lSystem -o /dev/null --threads=8 %t/dupmain.o %t/dup.a 2>&1 | FileCheck %s --check-prefix=DUP
# RUN: env LLD_NO_STREAM_MEMBER_PARSE=1 not %lld -lSystem -o /dev/null --threads=8 %t/dupmain.o %t/dup.a 2>&1 | FileCheck %s --check-prefix=DUP
# RUN: env LLD_REVERSE_HELD_PARSE=1 not %lld -lSystem -o /dev/null --threads=8 %t/dupmain.o %t/dup.a 2>&1 | FileCheck %s --check-prefix=DUP

# DUP:      error: duplicate symbol: _dup
# DUP-NEXT: >>> defined in {{.*}}dup.a(dupa.o)
# DUP-NEXT: >>> defined in {{.*}}dup.a(dupb.o)

## Two extracted members that each fail to parse a relocation. These errors are
## emitted inside the member parse itself, on a worker, and are replayed in
## adoption order, so bad1.o's must precede bad2.o's however the two parses
## interleave.
# RUN: not %lld -lSystem -o /dev/null --threads=1 %t/relmain.o %t/bad.a 2>&1 | FileCheck %s --check-prefix=REL
# RUN: not %lld -lSystem -o /dev/null --threads=2 %t/relmain.o %t/bad.a 2>&1 | FileCheck %s --check-prefix=REL
# RUN: not %lld -lSystem -o /dev/null --threads=8 %t/relmain.o %t/bad.a 2>&1 | FileCheck %s --check-prefix=REL
# RUN: env LLD_NO_STREAM_MEMBER_PARSE=1 not %lld -lSystem -o /dev/null --threads=8 %t/relmain.o %t/bad.a 2>&1 | FileCheck %s --check-prefix=REL
# RUN: env LLD_REVERSE_HELD_PARSE=1 not %lld -lSystem -o /dev/null --threads=8 %t/relmain.o %t/bad.a 2>&1 | FileCheck %s --check-prefix=REL

# REL:      error: UNSIGNED relocation has invalid width of 2 bytes at offset 0 of __LD,__compact_unwind{{.*}} in {{.*}}bad.a(bad1.o)
# REL-NEXT: error: UNSIGNED relocation has invalid width of 2 bytes at offset 0 of __LD,__compact_unwind{{.*}} in {{.*}}bad.a(bad2.o)

## The same chained extraction when nothing is wrong: byte-identical output and
## identical -why_load order, streamed or not.
# RUN: %lld -lSystem -o %t/g1/out --threads=1 -why_load %t/dupmain.o %t/good.a > %t/g1/why.txt
# RUN: %lld -lSystem -o %t/g2/out --threads=2 -why_load %t/dupmain.o %t/good.a > %t/g2/why.txt
# RUN: %lld -lSystem -o %t/g8/out --threads=8 -why_load %t/dupmain.o %t/good.a > %t/g8/why.txt
# RUN: env LLD_NO_STREAM_MEMBER_PARSE=1 %lld -lSystem -o %t/g8/nostream --threads=8 -why_load %t/dupmain.o %t/good.a > %t/g8/nostream-why.txt
# RUN: cmp %t/g1/out %t/g2/out
# RUN: cmp %t/g1/out %t/g8/out
# RUN: cmp %t/g1/why.txt %t/g2/why.txt
# RUN: cmp %t/g1/why.txt %t/g8/why.txt
# RUN: cmp %t/g1/why.txt %t/g8/nostream-why.txt
# RUN: FileCheck %s --check-prefix=WHY < %t/g1/why.txt

# WHY:      _dupa forced load of {{.*}}good.a(good1.o)
# WHY-NEXT: _need2 forced load of {{.*}}good.a(good2.o)

#--- dupmain.s
.globl _main
.subsections_via_symbols
_main:
  callq _dupa
  retq

#--- dupa.s
.globl _dupa, _dup
.subsections_via_symbols
.section __TEXT,dupa
_dupa:
  callq _need2
  retq
_dup:
  retq

#--- dupb.s
.globl _need2, _dup
.subsections_via_symbols
.section __TEXT,dupb
_need2:
  retq
_dup:
  retq

#--- good1.s
.globl _dupa
.subsections_via_symbols
.section __TEXT,good1
_dupa:
  callq _need2
  retq

#--- good2.s
.globl _need2
.subsections_via_symbols
.section __TEXT,good2
_need2:
  retq

#--- relmain.s
.globl _main
.subsections_via_symbols
_main:
  callq _b1
  callq _b2
  retq

#--- bad1.yaml
--- !mach-o
FileHeader:
  magic:           0xFEEDFACF
  cputype:         0x1000007
  cpusubtype:      0x3
  filetype:        0x1
  ncmds:           3
  sizeofcmds:      336
  flags:           0x2000
  reserved:        0x0
LoadCommands:
  - cmd:             LC_SEGMENT_64
    cmdsize:         232
    segname:         ''
    vmaddr:          0
    vmsize:          33
    fileoff:         368
    filesize:        33
    maxprot:         7
    initprot:        7
    nsects:          2
    flags:           0
    Sections:
      - sectname:        __text
        segname:         __TEXT
        addr:            0x0
        size:            1
        offset:          0x170
        align:           0
        reloff:          0x0
        nreloc:          0
        flags:           0x80000400
        reserved1:       0x0
        reserved2:       0x0
        reserved3:       0x0
        content:         C3
      - sectname:        __compact_unwind
        segname:         __LD
        addr:            0x1
        size:            32
        offset:          0x171
        align:           0
        reloff:          0x198
        nreloc:          1
        flags:           0x2000000
        reserved1:       0x0
        reserved2:       0x0
        reserved3:       0x0
        content:         '0000000000000000080000000000000000000000000000000000000000000000'
        relocations:
          - address:         0x0
            symbolnum:       1
            pcrel:           false
            length:          1
            extern:          false
            type:            0
            scattered:       false
            value:           0
  - cmd:             LC_SYMTAB
    cmdsize:         24
    symoff:          416
    nsyms:           1
    stroff:          432
    strsize:         8
  - cmd:             LC_DYSYMTAB
    cmdsize:         80
    ilocalsym:       0
    nlocalsym:       0
    iextdefsym:      0
    nextdefsym:      1
    iundefsym:       1
    nundefsym:       0
    tocoff:          0
    ntoc:            0
    modtaboff:       0
    nmodtab:         0
    extrefsymoff:    0
    nextrefsyms:     0
    indirectsymoff:  0
    nindirectsyms:   0
    extreloff:       0
    nextrel:         0
    locreloff:       0
    nlocrel:         0
LinkEditData:
  NameList:
    - n_strx:          1
      n_type:          0xF
      n_sect:          1
      n_desc:          0
      n_value:         0
  StringTable:
    - ''
    - _b1
    - ''
    - ''
    - ''
...

#--- bad2.yaml
--- !mach-o
FileHeader:
  magic:           0xFEEDFACF
  cputype:         0x1000007
  cpusubtype:      0x3
  filetype:        0x1
  ncmds:           3
  sizeofcmds:      336
  flags:           0x2000
  reserved:        0x0
LoadCommands:
  - cmd:             LC_SEGMENT_64
    cmdsize:         232
    segname:         ''
    vmaddr:          0
    vmsize:          33
    fileoff:         368
    filesize:        33
    maxprot:         7
    initprot:        7
    nsects:          2
    flags:           0
    Sections:
      - sectname:        __text
        segname:         __TEXT
        addr:            0x0
        size:            1
        offset:          0x170
        align:           0
        reloff:          0x0
        nreloc:          0
        flags:           0x80000400
        reserved1:       0x0
        reserved2:       0x0
        reserved3:       0x0
        content:         C3
      - sectname:        __compact_unwind
        segname:         __LD
        addr:            0x1
        size:            32
        offset:          0x171
        align:           0
        reloff:          0x198
        nreloc:          1
        flags:           0x2000000
        reserved1:       0x0
        reserved2:       0x0
        reserved3:       0x0
        content:         '0000000000000000080000000000000000000000000000000000000000000000'
        relocations:
          - address:         0x0
            symbolnum:       1
            pcrel:           false
            length:          1
            extern:          false
            type:            0
            scattered:       false
            value:           0
  - cmd:             LC_SYMTAB
    cmdsize:         24
    symoff:          416
    nsyms:           1
    stroff:          432
    strsize:         8
  - cmd:             LC_DYSYMTAB
    cmdsize:         80
    ilocalsym:       0
    nlocalsym:       0
    iextdefsym:      0
    nextdefsym:      1
    iundefsym:       1
    nundefsym:       0
    tocoff:          0
    ntoc:            0
    modtaboff:       0
    nmodtab:         0
    extrefsymoff:    0
    nextrefsyms:     0
    indirectsymoff:  0
    nindirectsyms:   0
    extreloff:       0
    nextrel:         0
    locreloff:       0
    nlocrel:         0
LinkEditData:
  NameList:
    - n_strx:          1
      n_type:          0xF
      n_sect:          1
      n_desc:          0
      n_value:         0
  StringTable:
    - ''
    - _b2
    - ''
    - ''
    - ''
...
