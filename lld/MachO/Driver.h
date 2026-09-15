//===- Driver.h -------------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLD_MACHO_DRIVER_H
#define LLD_MACHO_DRIVER_H

#include "lld/Common/LLVM.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/BinaryFormat/MachO.h"
#include "llvm/Option/OptTable.h"
#include "llvm/Support/MemoryBuffer.h"
#include <memory>
#include <optional>

#include <set>
#include <string>
#include <type_traits>
#include <vector>

namespace llvm {
namespace MachO {
class InterfaceFile;
}
}// namespace llvm

namespace lld {
class CommonLinkerContext;
}
namespace lld::macho {

class DylibFile;
class InputFile;

class MachOOptTable : public llvm::opt::OptTable {
public:
  MachOOptTable();
  llvm::opt::InputArgList parse(CommonLinkerContext &ctx,
                                ArrayRef<const char *> argv);
  void printHelp(CommonLinkerContext &ctx, const char *argv0,
                 bool showHidden) const;
};

// Create enum with OPT_xxx values for each option in Options.td
enum {
  OPT_INVALID = 0,
#define OPTION(...) LLVM_MAKE_OPT_ID(__VA_ARGS__),
#include "Options.inc"
#undef OPTION
};

void parseLCLinkerOption(llvm::SmallVectorImpl<StringRef> &LCLinkerOptions,
                         InputFile *f, unsigned argc, StringRef data);
void resolveLCLinkerOptions();

SmallVector<StringRef> getRerootedSearchPaths(StringRef path,
                                              ArrayRef<StringRef> roots);

std::string createResponseFile(const llvm::opt::InputArgList &args);

// Check for both libfoo.dylib and libfoo.tbd (in that order).
std::optional<StringRef> resolveDylibPath(llvm::StringRef path);

// fs::exists(), answered from the autolink pipeline's probes when one of them
// already asked about this path. See its definition in DriverUtils.cpp.
bool memoizedExists(const llvm::Twine &path);

DylibFile *loadDylib(llvm::MemoryBufferRef mbref, DylibFile *umbrella = nullptr,
                     bool isBundleLoader = false,
                     bool explicitlyLinked = false);

// Defined in InputFiles.h, which can name llvm::MachO::Symbol without
// making the unqualified name ambiguous in Driver.cpp.
struct PreparedTapiSymbols;

struct PreparedTapiFile {
  explicit PreparedTapiFile(llvm::MemoryBufferRef buffer);
  PreparedTapiFile(PreparedTapiFile &&);
  PreparedTapiFile &operator=(PreparedTapiFile &&);
  ~PreparedTapiFile();
  PreparedTapiFile(const PreparedTapiFile &) = delete;
  PreparedTapiFile &operator=(const PreparedTapiFile &) = delete;

  llvm::MemoryBufferRef buffer;
  llvm::StringRef path;
  std::unique_ptr<llvm::MachO::InterfaceFile> interfaceFile;
  std::optional<std::string> error;
  std::unique_ptr<PreparedTapiSymbols> symbols;
  // Pipelined entries are published by a worker thread; guarded by the
  // pipeline mutex. Entries prepared up front are already done.
  bool done = true;
  // A pipelined entry owns the path and the buffer it was read from, because
  // it is created outside the ordered walk.
  std::string pathStorage;
  std::unique_ptr<llvm::MemoryBuffer> ownedBuffer;
};

void prepareTapiFiles(llvm::ArrayRef<llvm::MemoryBufferRef> buffers,
                      std::vector<PreparedTapiFile> &preparedTapiFiles);
void setPreparedTapiFiles(
    llvm::ArrayRef<PreparedTapiFile *> preparedTapiFiles);
void clearPreparedTapiFiles();

// True only while resolveLCLinkerOptions() is loading one round's inputs. The
// batched dylib publication is scoped to that: it is the only place where a
// dylib run is bounded by a flush on both sides.
extern bool inAutolinkRound;

// Publishes the pending dylib batch and tears down the interfaces retired since
// the previous flush, which is the run boundary the batched publication needs.
void flushAutolinkBatch();

// Pipelined TAPI preparation: TBD parses started before the ordered input walk
// and overlapped with it. Preparation is speculative; the ordered walk still
// reads, diagnoses and publishes every input exactly as before.
void startTapiPipeline();
void finishTapiPipeline();
// Starts preparing an already-read TBD buffer. The buffer identifier must be
// the path loadDylib() will look up, and must outlive the pipeline.
void pipelineTapiFile(llvm::MemoryBufferRef buffer);
// Starts preparing the TBD an LC_LINKER_OPTION names, from the worker which
// parsed that option. Silent and speculative: resolveLCLinkerOptions() still
// resolves, reads, diagnoses and publishes the name in its own sorted order.
void pipelineAutolinkName(llvm::StringRef name, bool isFramework);

void resetLoadedDylibs();

// Search for all possible combinations of `{root}/{name}.{extension}`.
// If \p extensions are not specified, then just search for `{root}/{name}`.
std::optional<llvm::StringRef>
findPathCombination(const llvm::Twine &name,
                    const std::vector<llvm::StringRef> &roots,
                    ArrayRef<llvm::StringRef> extensions = {""});

// If -syslibroot is specified, absolute paths to non-object files may be
// rerooted.
llvm::StringRef rerootPath(llvm::StringRef path);

uint32_t getModTime(llvm::StringRef path);

void printArchiveMemberLoad(StringRef reason, const InputFile *);

// Map simulator platforms to their underlying device platform.
llvm::MachO::PlatformType removeSimulator(llvm::MachO::PlatformType platform);

// Helper class to export dependency info.
class DependencyTracker {
public:
  explicit DependencyTracker(llvm::StringRef path);

  // Adds the given path to the set of not-found files.
  inline void logFileNotFound(const Twine &path) {
    if (active)
      notFounds.insert(path.str());
  }

  // Writes the dependencies to specified path. The content is first sorted by
  // OpCode and then by the filename (in alphabetical order).
  void write(llvm::StringRef version,
             const llvm::SetVector<InputFile *> &inputs,
             llvm::StringRef output);

private:
  enum DepOpCode : uint8_t {
    // Denotes the linker version.
    Version = 0x00,
    // Denotes the input files.
    Input = 0x10,
    // Denotes the files that do not exist(?)
    NotFound = 0x11,
    // Denotes the output files.
    Output = 0x40,
  };

  const llvm::StringRef path;
  bool active;

  // The paths need to be alphabetically ordered.
  // We need to own the paths because some of them are temporarily
  // constructed.
  std::set<std::string> notFounds;
};

extern std::unique_ptr<DependencyTracker> depTracker;

} // namespace lld::macho

#endif
