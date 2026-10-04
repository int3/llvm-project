//===- MapFile.h ------------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLD_MACHO_MAPFILE_H
#define LLD_MACHO_MAPFILE_H

#include <cstddef>

namespace lld::macho {
class ArchiveFile;

// Record the files published by a deferred archive registration, in completion
// order. Their map position is the registration archive, not the owning archive
// of a recursively extracted member. Call only when a map was requested.
void recordMapArchiveMembers(ArchiveFile *archive, size_t first);
void resetMapFile();
void writeMapFile();
} // namespace lld::macho

#endif
