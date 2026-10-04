# REQUIRES: aarch64
## When the archive walker's select loop is ahead of its streamed member parses,
## the main thread parses the member it is about to queue itself. A fatal() in
## that parse must end the link with the error, also under the crash-recovery
## context that lit runs lld in (LLD_IN_TEST), whose cleanup waits for the
## member parses still in flight. LLD_INLINE_MEMBER_PARSE parses every member
## inline, so the path is taken deterministically. bad.o has a section symbol
## with section index 0, which the symbol parse rejects with fatal().

# RUN: rm -rf %t; split-file %s %t
# RUN: llvm-mc -filetype=obj -triple=arm64-apple-darwin %t/main.s -o %t/main.o
# RUN: llvm-mc -filetype=obj -triple=arm64-apple-darwin %t/good.s -o %t/good.o
# RUN: yaml2obj %t/bad.yaml -o %t/bad.o
# RUN: llvm-ar --format=darwin rcs %t/lib.a %t/good.o %t/bad.o

# RUN: env LLD_INLINE_MEMBER_PARSE=1 not %no-arg-lld -arch arm64 -platform_version macos 11.0 11.0 -syslibroot %S/Inputs/MacOSX.sdk -lSystem --threads=2 -o %t/out %t/main.o %t/lib.a 2>&1 | FileCheck %s
# RUN: env LLD_INLINE_MEMBER_PARSE=1 not %no-arg-lld -arch arm64 -platform_version macos 11.0 11.0 -syslibroot %S/Inputs/MacOSX.sdk -lSystem --threads=8 -o %t/out %t/main.o %t/lib.a 2>&1 | FileCheck %s
# RUN: not %no-arg-lld -arch arm64 -platform_version macos 11.0 11.0 -syslibroot %S/Inputs/MacOSX.sdk -lSystem --threads=2 -o %t/out %t/main.o %t/lib.a 2>&1 | FileCheck %s
# RUN: not %no-arg-lld -arch arm64 -platform_version macos 11.0 11.0 -syslibroot %S/Inputs/MacOSX.sdk -lSystem --threads=1 -o %t/out %t/main.o %t/lib.a 2>&1 | FileCheck %s

## A good member parsed inline links exactly as the serial linker does.
# RUN: llvm-mc -filetype=obj -triple=arm64-apple-darwin %t/main2.s -o %t/main2.o
# RUN: llvm-ar --format=darwin rcs %t/goodlib.a %t/good.o
# RUN: mkdir -p %t/inl %t/ser
# RUN: env LLD_INLINE_MEMBER_PARSE=1 %no-arg-lld -arch arm64 -platform_version macos 11.0 11.0 -syslibroot %S/Inputs/MacOSX.sdk -lSystem --threads=2 -why_load -o %t/inl/out %t/main2.o %t/goodlib.a > %t/inl/why
# RUN: %no-arg-lld -arch arm64 -platform_version macos 11.0 11.0 -syslibroot %S/Inputs/MacOSX.sdk -lSystem --threads=1 -why_load -o %t/ser/out %t/main2.o %t/goodlib.a > %t/ser/why
# RUN: cmp %t/inl/out %t/ser/out
# RUN: cmp %t/inl/why %t/ser/why

# CHECK: error: section symbol ltmp0 in {{.*}}lib.a(bad.o) has an invalid section index [0]

#--- main.s
.globl _main
_main:
  bl _good
  bl _mysec
  ret

#--- main2.s
.globl _main
_main:
  bl _good
  ret

#--- good.s
.globl _good
_good:
  ret

#--- bad.yaml
--- !mach-o
FileHeader:
  magic:           0xFEEDFACF
  cputype:         0x100000C
  cpusubtype:      0x0
  filetype:        0x1
  ncmds:           3
  sizeofcmds:      336
  flags:           0x0
  reserved:        0x0
LoadCommands:
  - cmd:             LC_SEGMENT_64
    cmdsize:         232
    segname:         ''
    vmaddr:          0
    vmsize:          1
    fileoff:         368
    filesize:        1
    maxprot:         7
    initprot:        7
    nsects:          2
    flags:           0
    Sections:
      - sectname:        __text
        segname:         __TEXT
        addr:            0x0
        size:            0
        offset:          0x170
        align:           0
        reloff:          0x0
        nreloc:          0
        flags:           0x80000000
        reserved1:       0x0
        reserved2:       0x0
        reserved3:       0x0
        content:         ''
      - sectname:        __mystuff
        segname:         __TEST
        addr:            0x0
        size:            1
        offset:          0x170
        align:           0
        reloff:          0x0
        nreloc:          0
        flags:           0x0
        reserved1:       0x0
        reserved2:       0x0
        reserved3:       0x0
        content:         C3
  - cmd:             LC_SYMTAB
    cmdsize:         24
    symoff:          376
    nsyms:           3
    stroff:          424
    strsize:         24
  - cmd:             LC_DYSYMTAB
    cmdsize:         80
    ilocalsym:       0
    nlocalsym:       2
    iextdefsym:      2
    nextdefsym:      1
    iundefsym:       3
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
    - n_strx:          14
      n_type:          0xE
      n_sect:          0
      n_desc:          0
      n_value:         0
    - n_strx:          8
      n_type:          0xE
      n_sect:          2
      n_desc:          0
      n_value:         0
    - n_strx:          1
      n_type:          0xF
      n_sect:          2
      n_desc:          0
      n_value:         0
  StringTable:
    - ''
    - _mysec
    - ltmp1
    - ltmp0
    - ''
    - ''
    - ''
    - ''
...
