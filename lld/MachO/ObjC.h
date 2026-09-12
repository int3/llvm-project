//===- ObjC.h ---------------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLD_MACHO_OBJC_H
#define LLD_MACHO_OBJC_H

#include "llvm/Support/MemoryBuffer.h"

namespace lld::macho {

namespace objc {

namespace symbol_names {
// The common prefix of every method-list symbol name in this namespace --
// instanceMethods, classMethods, categoryInstanceMethods and
// categoryClassMethods, the four that ObjCMethListSection::isMethodList()
// matches. The Defined constructor tests a symbol name against it to set
// InputSection::mayBeMethodList, which lets isMethodList() reject a section
// without dereferencing any of its symbols. The static_asserts at the end of
// this namespace keep the two in step: add a method-list name that does not
// start with this prefix and the build fails rather than the filter silently
// dropping it.
constexpr const char methodListPrefix[] = "__OBJC_$_";

constexpr const char klass[] = "_OBJC_CLASS_$_";
constexpr const char klassPropList[] = "__OBJC_$_CLASS_PROP_LIST_";

constexpr const char metaclass[] = "_OBJC_METACLASS_$_";
constexpr const char ehtype[] = "_OBJC_EHTYPE_$_";
constexpr const char ivar[] = "_OBJC_IVAR_$_";
constexpr const char instanceMethods[] = "__OBJC_$_INSTANCE_METHODS_";
constexpr const char classMethods[] = "__OBJC_$_CLASS_METHODS_";
constexpr const char listProprieties[] = "__OBJC_$_PROP_LIST_";

constexpr const char category[] = "__OBJC_$_CATEGORY_";
constexpr const char categoryInstanceMethods[] =
    "__OBJC_$_CATEGORY_INSTANCE_METHODS_";
constexpr const char categoryClassMethods[] =
    "__OBJC_$_CATEGORY_CLASS_METHODS_";
constexpr const char categoryProtocols[] = "__OBJC_CATEGORY_PROTOCOLS_$_";

namespace detail {
constexpr bool startsWithMethodListPrefix(const char *name) {
  for (size_t i = 0; methodListPrefix[i] != '\0'; ++i)
    if (name[i] != methodListPrefix[i])
      return false;
  return true;
}
} // namespace detail

// InputSection::mayBeMethodList is set from methodListPrefix alone, so every
// name isMethodList() matches must start with it or the filter would reject a
// real method list.
static_assert(detail::startsWithMethodListPrefix(instanceMethods),
              "instanceMethods must start with methodListPrefix");
static_assert(detail::startsWithMethodListPrefix(classMethods),
              "classMethods must start with methodListPrefix");
static_assert(detail::startsWithMethodListPrefix(categoryInstanceMethods),
              "categoryInstanceMethods must start with methodListPrefix");
static_assert(detail::startsWithMethodListPrefix(categoryClassMethods),
              "categoryClassMethods must start with methodListPrefix");
} // namespace symbol_names

// Check for duplicate method names within related categories / classes.
void checkCategories();
void mergeCategories();

void doCleanup();
} // namespace objc

bool hasObjCSection(llvm::MemoryBufferRef);

} // namespace lld::macho

#endif
