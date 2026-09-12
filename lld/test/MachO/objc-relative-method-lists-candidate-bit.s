# REQUIRES: aarch64
# UNSUPPORTED: target=arm{{.*}}-unknown-linux-gnueabihf

## ObjCMethListSection::isMethodList() used to dereference every symbol of every
## subsection while gathering. A candidate bit set by the Defined constructor
## whose name starts with "__OBJC_$_" -- the shared prefix of the four
## method-list symbol names -- now filters that down to the sections that can
## possibly match, and the full check still runs on those. __DATA,__objc_const
## here is split by symbols into a class method list, an instance method list,
## two RO structs whose names start with "__OBJC_" but not "__OBJC_$_" (bit
## clear, rejected without touching a symbol), and a property list whose name
## does start with "__OBJC_$_" but is not a method list (bit set, rejected by
## the full check). The relative method lists, the input order of the remaining
## __objc_const inputs and the bytes must be exactly as before.

# RUN: rm -rf %t; split-file %s %t && cd %t
# RUN: mkdir -p one eight
# RUN: llvm-mc -filetype=obj -triple=arm64-apple-macos10.15 -o cls.o cls.s

# RUN: %no-lsystem-lld cls.o -o one/out -dylib -install_name /usr/lib/libcls.dylib -arch arm64 --threads=1
# RUN: %no-lsystem-lld cls.o -o eight/out -dylib -install_name /usr/lib/libcls.dylib -arch arm64 --threads=8
# RUN: cmp one/out eight/out
# RUN: llvm-objdump --macho --objc-meta-data eight/out | FileCheck %s --check-prefix=REL

## Dead-stripping reaches the method lists through the class, and the property
## list must still be part of __objc_const rather than the method-list section.
# RUN: %no-lsystem-lld cls.o -o one/dead -dylib -install_name /usr/lib/libcls.dylib -arch arm64 -dead_strip --threads=1
# RUN: %no-lsystem-lld cls.o -o eight/dead -dylib -install_name /usr/lib/libcls.dylib -arch arm64 -dead_strip --threads=8
# RUN: cmp one/dead eight/dead
# RUN: llvm-objdump --macho --objc-meta-data eight/dead | FileCheck %s --check-prefix=REL
# RUN: llvm-objdump --syms eight/dead | FileCheck %s --check-prefix=SYMS

## Without relative method lists nothing consults the bit at all.
# RUN: %no-lsystem-lld cls.o -o one/norel -dylib -install_name /usr/lib/libcls.dylib -arch arm64 --threads=1 -no_objc_relative_method_lists
# RUN: %no-lsystem-lld cls.o -o eight/norel -dylib -install_name /usr/lib/libcls.dylib -arch arm64 --threads=8 -no_objc_relative_method_lists
# RUN: cmp one/norel eight/norel

# REL:       Contents of (__DATA_CONST,__objc_classlist) section
# REL-NEXT:  _OBJC_CLASS_$_MyClass
# REL:       baseMethods
# REL-NEXT:  entsize 12 (relative)
# REL-NEXT:  count 3
# REL:       baseProperties
# REL-SAME:  0x
# REL:       entsize 12 (relative)
# REL-NEXT:  count 3

## The property list is a __objc_const input, not a method-list input.
# SYMS: __OBJC_$_PROP_LIST_MyClass

#--- objc-macros.s
.macro .objc_selector_def name
	.p2align	2
"\name":
	.cfi_startproc
	ret
	.cfi_endproc
.endm

#--- cls.s
.include "objc-macros.s"

.section	__TEXT,__text,regular,pure_instructions
.build_version macos, 10, 15

.objc_selector_def "-[MyClass instance_method_00]"
.objc_selector_def "-[MyClass instance_method_01]"
.objc_selector_def "-[MyClass instance_method_02]"

.objc_selector_def "+[MyClass class_method_00]"
.objc_selector_def "+[MyClass class_method_01]"
.objc_selector_def "+[MyClass class_method_02]"

.globl	__objc_empty_vtable
.zerofill __DATA,__common,__objc_empty_vtable,8,3
.section	__DATA,__objc_data
.globl	_OBJC_CLASS_$_MyClass
.p2align	3, 0x0

_OBJC_CLASS_$_MyClass:
	.quad	_OBJC_METACLASS_$_MyClass
	.quad	0
	.quad	__objc_empty_cache
	.quad	__objc_empty_vtable
	.quad	__OBJC_CLASS_RO_$_MyClass
	.globl	_OBJC_METACLASS_$_MyClass
	.p2align	3, 0x0

_OBJC_METACLASS_$_MyClass:
	.quad	_OBJC_METACLASS_$_MyClass
	.quad	_OBJC_CLASS_$_MyClass
	.quad	__objc_empty_cache
	.quad	__objc_empty_vtable
	.quad	__OBJC_METACLASS_RO_$_MyClass

	.section	__TEXT,__objc_classname,cstring_literals
l_OBJC_CLASS_NAME_:
	.asciz	"MyClass"
	.section	__TEXT,__objc_methname,cstring_literals
l_OBJC_METH_VAR_NAME_:
	.asciz	"class_method_00"
	.section	__TEXT,__objc_methtype,cstring_literals
l_OBJC_METH_VAR_TYPE_:
	.asciz	"v16@0:8"
	.section	__TEXT,__objc_methname,cstring_literals
l_OBJC_METH_VAR_NAME_.1:
	.asciz	"class_method_01"
l_OBJC_METH_VAR_NAME_.2:
	.asciz	"class_method_02"
	.section	__DATA,__objc_const
	.p2align	3, 0x0
__OBJC_$_CLASS_METHODS_MyClass:
	.long	24
	.long	3
	.quad	l_OBJC_METH_VAR_NAME_
	.quad	l_OBJC_METH_VAR_TYPE_
	.quad	"+[MyClass class_method_00]"
	.quad	l_OBJC_METH_VAR_NAME_.1
	.quad	l_OBJC_METH_VAR_TYPE_
	.quad	"+[MyClass class_method_01]"
	.quad	l_OBJC_METH_VAR_NAME_.2
	.quad	l_OBJC_METH_VAR_TYPE_
	.quad	"+[MyClass class_method_02]"
	.p2align	3, 0x0

__OBJC_METACLASS_RO_$_MyClass:
	.long	3
	.long	40
	.long	40
	.space	4
	.quad	0
	.quad	l_OBJC_CLASS_NAME_
	.quad	__OBJC_$_CLASS_METHODS_MyClass
	.quad	0
	.quad	0
	.quad	0
	.quad	0

	.section	__TEXT,__objc_methname,cstring_literals
l_OBJC_METH_VAR_NAME_.3:
	.asciz	"instance_method_00"
l_OBJC_METH_VAR_NAME_.4:
	.asciz	"instance_method_01"
l_OBJC_METH_VAR_NAME_.5:
	.asciz	"instance_method_02"

	.section	__DATA,__objc_const
	.p2align	3, 0x0
__OBJC_$_INSTANCE_METHODS_MyClass:
	.long	24
	.long	3
	.quad	l_OBJC_METH_VAR_NAME_.3
	.quad	l_OBJC_METH_VAR_TYPE_
	.quad	"-[MyClass instance_method_00]"
	.quad	l_OBJC_METH_VAR_NAME_.4
	.quad	l_OBJC_METH_VAR_TYPE_
	.quad	"-[MyClass instance_method_01]"
	.quad	l_OBJC_METH_VAR_NAME_.5
	.quad	l_OBJC_METH_VAR_TYPE_
	.quad	"-[MyClass instance_method_02]"
	.p2align	3, 0x0

__OBJC_CLASS_RO_$_MyClass:
	.long	2
	.long	0
	.long	0
	.space	4
	.quad	0
	.quad	l_OBJC_CLASS_NAME_
	.quad	__OBJC_$_INSTANCE_METHODS_MyClass
	.quad	0
	.quad	0
	.quad	0
	.quad	__OBJC_$_PROP_LIST_MyClass
	.globl	__objc_empty_cache

.zerofill __DATA,__common,__objc_empty_cache,8,3
	.section	__DATA,__objc_classlist,regular,no_dead_strip
	.p2align	3, 0x0
l_OBJC_LABEL_CLASS_$:
	.quad	_OBJC_CLASS_$_MyClass
	.section	__DATA,__objc_const
	.p2align	3, 0x0
__OBJC_$_PROP_LIST_MyClass:
	.long	16
	.long	1
	.quad	l_OBJC_PROP_NAME_ATTR_
	.quad	l_OBJC_PROP_NAME_ATTR_.1
	.section	__TEXT,__cstring,cstring_literals
l_OBJC_PROP_NAME_ATTR_:
	.asciz	"prop"
l_OBJC_PROP_NAME_ATTR_.1:
	.asciz	"Ti,N"
	.section	__DATA,__objc_imageinfo,regular,no_dead_strip
L_OBJC_IMAGE_INFO:
	.long	0
	.long	64
.subsections_via_symbols
