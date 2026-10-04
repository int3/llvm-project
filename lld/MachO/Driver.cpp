//===- Driver.cpp ---------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "Driver.h"
#include "Config.h"
#include "ICF.h"
#include "InputFiles.h"
#include "LTO.h"
#include "MarkLive.h"
#include "ObjC.h"
#include "OutputSection.h"
#include "OutputSegment.h"
#include "SectionPriorities.h"
#include "StripSwiftForceLoad.h"
#include "SymbolTable.h"
#include "Symbols.h"
#include "SyntheticSections.h"
#include "Target.h"
#include "UnwindInfoSection.h"
#include "Writer.h"

#include "lld/Common/Args.h"
#include "lld/Common/CommonLinkerContext.h"
#include "lld/Common/ErrorHandler.h"
#include "lld/Common/LLVM.h"
#include "lld/Common/Memory.h"
#include "lld/Common/Reproduce.h"
#include "lld/Common/Version.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/ScopeExit.h"
#include "llvm/BinaryFormat/MachO.h"
#include "llvm/BinaryFormat/Magic.h"
#include "llvm/CGData/CodeGenDataWriter.h"
#include "llvm/Config/llvm-config.h"
#include "llvm/LTO/LTO.h"
#include "llvm/Object/Archive.h"
#include "llvm/Option/ArgList.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Parallel.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Process.h"
#include "llvm/Support/TarWriter.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/Threading.h"
#include "llvm/Support/TimeProfiler.h"
#include "llvm/TargetParser/Host.h"
#include "llvm/TextAPI/Architecture.h"
#include "llvm/TextAPI/PackedVersion.h"

#include <atomic>
#include <chrono>
#include <deque>
#include <memory>
#include <numeric>
#include <mutex>
#include <queue>
#include <thread>

#if !_WIN32
#include <sys/mman.h>
#endif

using namespace llvm;
using namespace llvm::MachO;
using namespace llvm::object;
using namespace llvm::opt;
using namespace llvm::sys;
using namespace lld;
using namespace lld::macho;

std::unique_ptr<Configuration> macho::config;
std::unique_ptr<DependencyTracker> macho::depTracker;

static HeaderFileType getOutputType(const InputArgList &args) {
  // TODO: -r, -dylinker, -preload...
  Arg *outputArg = args.getLastArg(OPT_bundle, OPT_dylib, OPT_execute);
  if (outputArg == nullptr)
    return MH_EXECUTE;

  switch (outputArg->getOption().getID()) {
  case OPT_bundle:
    return MH_BUNDLE;
  case OPT_dylib:
    return MH_DYLIB;
  case OPT_execute:
    return MH_EXECUTE;
  default:
    llvm_unreachable("internal error");
  }
}

static DenseMap<CachedHashStringRef, StringRef> resolvedLibraries;
static std::optional<StringRef> findLibrary(StringRef name) {
  CachedHashStringRef key(name);
  auto entry = resolvedLibraries.find(key);
  if (entry != resolvedLibraries.end())
    return entry->second;

  auto doFind = [&] {
    // Special case for Csu support files required for Mac OS X 10.7 and older
    // (crt1.o)
    if (name.ends_with(".o"))
      return findPathCombination(name, config->librarySearchPaths, {""});
    if (config->searchDylibsFirst) {
      if (std::optional<StringRef> path =
              findPathCombination("lib" + name, config->librarySearchPaths,
                                  {".tbd", ".dylib", ".so"}))
        return path;
      return findPathCombination("lib" + name, config->librarySearchPaths,
                                 {".a"});
    }
    return findPathCombination("lib" + name, config->librarySearchPaths,
                               {".tbd", ".dylib", ".so", ".a"});
  };

  std::optional<StringRef> path = doFind();
  if (path)
    resolvedLibraries[key] = *path;

  return path;
}

static DenseMap<CachedHashStringRef, StringRef> resolvedFrameworks;
static std::optional<StringRef> findFramework(StringRef name) {
  CachedHashStringRef key(name);
  auto entry = resolvedFrameworks.find(key);
  if (entry != resolvedFrameworks.end())
    return entry->second;

  SmallString<260> symlink;
  StringRef suffix;
  std::tie(name, suffix) = name.split(",");
  for (StringRef dir : config->frameworkSearchPaths) {
    symlink = dir;
    path::append(symlink, name + ".framework", name);

    if (!suffix.empty()) {
      // NOTE: we must resolve the symlink before trying the suffixes, because
      // there are no symlinks for the suffixed paths.
      SmallString<260> location;
      if (!fs::real_path(symlink, location)) {
        // only append suffix if realpath() succeeds
        SmallString<261> suffixed(location.str());
        suffixed += suffix;
        if (memoizedExists(suffixed))
          return resolvedFrameworks[key] = saver().save(suffixed.str());
      }
      // Suffix lookup failed, fall through to the no-suffix case.
    }

    if (std::optional<StringRef> path = resolveDylibPath(symlink.str()))
      return resolvedFrameworks[key] = *path;
  }
  return {};
}

static bool warnIfNotDirectory(StringRef option, StringRef path) {
  if (!fs::exists(path)) {
    warn("directory not found for option -" + option + path);
    return false;
  } else if (!fs::is_directory(path)) {
    warn("option -" + option + path + " references a non-directory path");
    return false;
  }
  return true;
}

static std::vector<StringRef>
getSearchPaths(unsigned optionCode, InputArgList &args,
               const std::vector<StringRef> &roots,
               const SmallVector<StringRef, 2> &systemPaths) {
  std::vector<StringRef> paths;
  StringRef optionLetter{optionCode == OPT_F ? "F" : "L"};
  for (StringRef path : args::getStrings(args, optionCode))
    for (StringRef searchPath : getRerootedSearchPaths(path, roots))
      if (searchPath != path || warnIfNotDirectory(optionLetter, searchPath))
        paths.push_back(searchPath);

  // `-Z` suppresses the standard "system" search paths.
  if (args.hasArg(OPT_Z))
    return paths;

  for (const StringRef &path : systemPaths) {
    for (const StringRef &root : roots) {
      SmallString<261> buffer(root);
      path::append(buffer, path);
      if (fs::is_directory(buffer))
        paths.push_back(saver().save(buffer.str()));
    }
  }
  return paths;
}

static std::vector<StringRef> getSystemLibraryRoots(InputArgList &args) {
  std::vector<StringRef> roots;
  for (const Arg *arg : args.filtered(OPT_syslibroot))
    roots.push_back(arg->getValue());
  // NOTE: the final `-syslibroot` being `/` will ignore all roots
  if (!roots.empty() && roots.back() == "/")
    roots.clear();
  // NOTE: roots can never be empty - add an empty root to simplify the library
  // and framework search path computation.
  if (roots.empty())
    roots.emplace_back("");
  return roots;
}

static std::vector<StringRef>
getLibrarySearchPaths(InputArgList &args, const std::vector<StringRef> &roots) {
  return getSearchPaths(OPT_L, args, roots, {"/usr/lib", "/usr/local/lib"});
}

static std::vector<StringRef>
getFrameworkSearchPaths(InputArgList &args,
                        const std::vector<StringRef> &roots) {
  return getSearchPaths(OPT_F, args, roots,
                        {"/Library/Frameworks", "/System/Library/Frameworks"});
}

static llvm::CachePruningPolicy getLTOCachePolicy(InputArgList &args) {
  SmallString<128> ltoPolicy;
  auto add = [&ltoPolicy](Twine val) {
    if (!ltoPolicy.empty())
      ltoPolicy += ":";
    val.toVector(ltoPolicy);
  };
  for (const Arg *arg :
       args.filtered(OPT_thinlto_cache_policy_eq, OPT_prune_interval_lto,
                     OPT_prune_after_lto, OPT_max_relative_cache_size_lto)) {
    switch (arg->getOption().getID()) {
    case OPT_thinlto_cache_policy_eq:
      add(arg->getValue());
      break;
    case OPT_prune_interval_lto:
      if (!strcmp("-1", arg->getValue()))
        add("prune_interval=87600h"); // 10 years
      else
        add(Twine("prune_interval=") + arg->getValue() + "s");
      break;
    case OPT_prune_after_lto:
      add(Twine("prune_after=") + arg->getValue() + "s");
      break;
    case OPT_max_relative_cache_size_lto:
      add(Twine("cache_size=") + arg->getValue() + "%");
      break;
    }
  }
  return CHECK(parseCachePruningPolicy(ltoPolicy), "invalid LTO cache policy");
}

// What caused a given library to be loaded. Only relevant for archives.
// Note that this does not tell us *how* we should load the library, i.e.
// whether we should do it lazily or eagerly (AKA force loading). The "how" is
// decided within addFile().
enum class LoadType {
  CommandLine,      // Library was passed as a regular CLI argument
  CommandLineForce, // Library was passed via `-force_load`
  LCLinkerOption,   // Library was passed via LC_LINKER_OPTIONS
};

struct ArchiveFileInfo {
  ArchiveFile *file;
  bool isCommandLineLoad;
};

static DenseMap<StringRef, ArchiveFileInfo> loadedArchives;

static void saveThinArchiveToRepro(ArchiveFile const *file) {
  assert(tar && file->getArchive().isThin());

  Error e = Error::success();
  for (const object::Archive::Child &c : file->getArchive().children(e)) {
    MemoryBufferRef mb = CHECK(c.getMemoryBufferRef(),
                               toString(file) + ": failed to get buffer");
    tar->append(relativeToRoot(CHECK(c.getFullName(), file)), mb.getBuffer());
  }
  if (e)
    error(toString(file) +
          ": Archive::children failed: " + toString(std::move(e)));
}

struct DeferredFile {
  StringRef path;
  bool isLazy;
  MemoryBufferRef buffer;
  LoadType loadType = LoadType::CommandLine;
  bool isNeeded = false;
  bool isWeak = false;
  bool isReexport = false;
  bool isHidden = false;
  bool isExplicit = true;
};
using DeferredFiles = std::vector<DeferredFile>;

#if LLVM_ENABLE_THREADS
class SerialBackgroundWorkQueue {
  std::deque<std::function<void()>> queue;
  std::thread *running;
  std::mutex mutex;

public:
  std::atomic_bool stopAllWork = false;
  void queueWork(std::function<void()> work) {
    mutex.lock();
    if (running && queue.empty()) {
      mutex.unlock();
      running->join();
      mutex.lock();
      delete running;
      running = nullptr;
    }

    if (work) {
      queue.emplace_back(std::move(work));
      if (!running)
        running = new std::thread([&]() {
          while (!stopAllWork) {
            mutex.lock();
            if (queue.empty()) {
              mutex.unlock();
              break;
            }
            auto work = std::move(queue.front());
            mutex.unlock();
            work();
            mutex.lock();
            queue.pop_front();
            mutex.unlock();
          }
        });
    }
    mutex.unlock();
  }
};

static SerialBackgroundWorkQueue pageInQueue;

// Most input files have been mapped but not yet paged in.
// This code forces the page-ins on multiple threads so
// the process is not stalled waiting on disk buffer i/o.
void multiThreadedPageInBackground(DeferredFiles &deferred) {
  static const size_t pageSize = Process::getPageSizeEstimate();
  static const size_t largeArchive = 10 * 1024 * 1024;
#ifndef NDEBUG
  using namespace std::chrono;
  static std::atomic_uint64_t totalBytes = 0;
  std::atomic_int numDeferedFilesAdvised = 0;
  auto t0 = high_resolution_clock::now();
#endif

  auto preloadDeferredFile = [&](const DeferredFile &deferredFile) {
    const StringRef &buff = deferredFile.buffer.getBuffer();
    if (buff.size() > largeArchive)
      return;

#ifndef NDEBUG
    totalBytes += buff.size();
    numDeferedFilesAdvised += 1;
#endif
#if _WIN32
    // Reference all file's mmap'd pages to load them into memory.
    for (const char *page = buff.data(), *end = page + buff.size();
         page < end && !pageInQueue.stopAllWork; page += pageSize) {
      [[maybe_unused]] volatile char t = *page;
      (void)t;
    }
#else
#define DEBUG_TYPE "lld-madvise"
    auto aligned =
        llvm::alignDown(reinterpret_cast<uintptr_t>(buff.data()), pageSize);
    if (madvise((void *)aligned, buff.size(), MADV_WILLNEED) < 0)
      LLVM_DEBUG(llvm::dbgs() << "madvise error: " << strerror(errno) << "\n");
#undef DEBUG_TYPE
#endif
  };

  { // Create scope for waiting for the taskGroup
    std::atomic_size_t index = 0;
    llvm::parallel::TaskGroup taskGroup;
    for (int w = 0; w < config->readWorkers; w++)
      taskGroup.spawn([&index, &preloadDeferredFile, &deferred]() {
        while (!pageInQueue.stopAllWork) {
          size_t localIndex = index.fetch_add(1);
          if (localIndex >= deferred.size())
            break;
          preloadDeferredFile(deferred[localIndex]);
        }
      });
  }

#ifndef NDEBUG
  auto dt = high_resolution_clock::now() - t0;
  if (Process::GetEnv("LLD_MULTI_THREAD_PAGE"))
    llvm::dbgs() << "multiThreadedPageIn " << totalBytes << "/"
                 << numDeferedFilesAdvised << "/" << deferred.size() << "/"
                 << duration_cast<milliseconds>(dt).count() / 1000. << "\n";
#endif
}

static void multiThreadedPageIn(const DeferredFiles &deferred) {
  pageInQueue.queueWork([=]() {
    DeferredFiles files = deferred;
    multiThreadedPageInBackground(files);
  });
}
#endif

// The member loads -all_load, -force_load, -ObjC and the forceLoadSwift
// LC_LINKER_OPTION rule perform at an archive's own registration point, before
// its index is published and regardless of any reference.
//
// \p contentsOnly performs none of them and only pushes the member buffers the
// read workers page in: the parallel input phase has held this archive's index
// back, so the loads run later instead -- in the walker's per-archive prelude,
// or, on the paths where the walker does not run, in this same function from the
// point that publishes the index the ordinary way.
static void loadMandatoryArchiveMembers(ArchiveFile *file, StringRef path,
                                        DeferredFiles *archiveContents,
                                        bool isLazy, bool contentsOnly) {
  using MandatoryLoad = ArchiveFile::MandatoryLoad;
  switch (file->mandatoryLoad) {
  case MandatoryLoad::None:
    return;
  case MandatoryLoad::AllLoad:
  case MandatoryLoad::ForceLoad:
  case MandatoryLoad::LcOption: {
    if (contentsOnly || !readFile(path))
      return;
    StringRef reason = file->mandatoryLoad == MandatoryLoad::LcOption
                           ? "LC_LINKER_OPTION"
                           : (file->mandatoryLoad == MandatoryLoad::ForceLoad
                                  ? "-force_load"
                                  : "-all_load");
    Error e = Error::success();
    for (const object::Archive::Child &c : file->getArchive().children(e)) {
      if (Error e = file->fetch(c, reason)) {
        if (config->warnThinArchiveMissingMembers)
          warn(toString(file) + ": " + reason +
               " failed to load archive member: " + toString(std::move(e)));
        else
          llvm::consumeError(std::move(e));
      }
    }
    if (e)
      error(toString(file) +
            ": Archive::children failed: " + toString(std::move(e)));
    return;
  }
  case MandatoryLoad::ObjC: {
    if (!contentsOnly && file->getArchive().hasSymbolTable()) {
      for (const object::Archive::Symbol &sym : file->getArchive().symbols())
        if (sym.getName().starts_with(objc::symbol_names::klass))
          file->fetch(sym);
    }
    bool wantContents = config->readWorkers && archiveContents != nullptr;
    if ((contentsOnly && !wantContents) || !readFile(path))
      return;

    // TODO: no need to look for ObjC sections for a given archive member if
    // we already found that it contains an ObjC symbol.
    Error e = Error::success();
    for (const object::Archive::Child &c : file->getArchive().children(e)) {
      Expected<MemoryBufferRef> mb = c.getMemoryBufferRef();
      if (!mb) {
        // A member whose buffer cannot be read makes the walker decline the
        // whole archive set, and this function then runs for real, so the
        // diagnostic is still emitted exactly once.
        if (contentsOnly) {
          llvm::consumeError(mb.takeError());
          continue;
        }
        // We used to create broken repro tarballs that only included those
        // object files from thin archives that ended up being used.
        if (config->warnThinArchiveMissingMembers)
          warn(toString(file) + ": -ObjC failed to open archive member: " +
               toString(mb.takeError()));
        else
          llvm::consumeError(mb.takeError());
        continue;
      }

      if (wantContents)
        archiveContents->push_back({path, isLazy, *mb});
      if (contentsOnly || !hasObjCSection(*mb))
        continue;
      if (Error e = file->fetch(c, "-ObjC"))
        error(toString(file) + ": -ObjC failed to load archive member: " +
              toString(std::move(e)));
    }
    if (e)
      error(toString(file) +
            ": Archive::children failed: " + toString(std::move(e)));
    return;
  }
  }
}

static InputFile *processFile(std::optional<MemoryBufferRef> buffer,
                              DeferredFiles *archiveContents, StringRef path,
                              LoadType loadType, bool isLazy = false,
                              bool isExplicit = true,
                              bool isBundleLoader = false,
                              bool isForceHidden = false) {
  if (!buffer)
    return nullptr;
  MemoryBufferRef mbref = *buffer;
  InputFile *newFile = nullptr;

  file_magic magic = identify_magic(mbref.getBuffer());
  switch (magic) {
  case file_magic::archive: {
    bool isCommandLineLoad = loadType != LoadType::LCLinkerOption;
    // Avoid loading archives twice. If the archives are being force-loaded,
    // loading them twice would create duplicate symbol errors. In the
    // non-force-loading case, this is just a minor performance optimization.
    // We don't take a reference to cachedFile here because the
    // loadArchiveMember() call below may recursively call addFile() and
    // invalidate this reference.
    auto entry = loadedArchives.find(path);

    ArchiveFile *file;
    if (entry == loadedArchives.end()) {
      // No cached archive, we need to create a new one
      std::unique_ptr<object::Archive> archive = CHECK(
          object::Archive::create(mbref), path + ": failed to parse archive");

      file = make<ArchiveFile>(std::move(archive), isForceHidden);

      if (tar && file->getArchive().isThin())
        saveThinArchiveToRepro(file);
    } else {
      file = entry->second.file;
      // Command-line loads take precedence. If file is previously loaded via
      // command line, or is loaded via LC_LINKER_OPTION and being loaded via
      // LC_LINKER_OPTION again, using the cached archive is enough.
      if (entry->second.isCommandLineLoad || !isCommandLineLoad)
        return file;
    }

    file->mandatoryLoad = ArchiveFile::MandatoryLoad::None;
    if (loadType == LoadType::LCLinkerOption) {
      if (config->forceLoadSwift &&
          path::filename(path).starts_with("libswift"))
        file->mandatoryLoad = ArchiveFile::MandatoryLoad::LcOption;
    } else if (loadType == LoadType::CommandLineForce)
      file->mandatoryLoad = ArchiveFile::MandatoryLoad::ForceLoad;
    else if (config->allLoad)
      file->mandatoryLoad = ArchiveFile::MandatoryLoad::AllLoad;
    else if (config->forceLoadObjC)
      file->mandatoryLoad = ArchiveFile::MandatoryLoad::ObjC;
    loadMandatoryArchiveMembers(file, path, archiveContents, isLazy,
                                /*contentsOnly=*/deferArchiveIndex);
    if ((!archiveContents || archiveContents->empty()) && !deferArchiveIndex)
      file->addLazySymbols();
    loadedArchives[path] = ArchiveFileInfo{file, isCommandLineLoad};
    newFile = file;
    break;
  }
  case file_magic::macho_object:
    newFile = make<ObjFile>(mbref, getModTime(path), "", isLazy);
    break;
  case file_magic::macho_dynamically_linked_shared_lib:
  case file_magic::macho_dynamically_linked_shared_lib_stub:
  case file_magic::tapi_file:
    if (DylibFile *dylibFile =
            loadDylib(mbref, nullptr, /*isBundleLoader=*/false, isExplicit))
      newFile = dylibFile;
    break;
  case file_magic::bitcode:
    newFile = make<BitcodeFile>(mbref, "", 0, isLazy);
    break;
  case file_magic::macho_executable:
  case file_magic::macho_bundle:
    // We only allow executable and bundle type here if it is used
    // as a bundle loader.
    if (!isBundleLoader)
      error(path + ": unhandled file type");
    if (DylibFile *dylibFile = loadDylib(mbref, nullptr, isBundleLoader))
      newFile = dylibFile;
    break;
  default:
    error(path + ": unhandled file type");
  }
  if (newFile && !isa<DylibFile>(newFile)) {
    if ((isa<ObjFile>(newFile) || isa<BitcodeFile>(newFile)) && newFile->lazy &&
        config->forceLoadObjC) {
      for (Symbol *sym : newFile->symbols)
        if (sym && sym->getName().starts_with(objc::symbol_names::klass)) {
          extract(*newFile, "-ObjC");
          break;
        }
      if (newFile->lazy && hasObjCSection(mbref))
        extract(*newFile, "-ObjC");
    }

    // printArchiveMemberLoad() prints both .a and .o names, so no need to
    // print the .a name here. Similarly skip lazy files.
    if (config->printEachFile && magic != file_magic::archive && !isLazy)
      message(toString(newFile));
    inputFiles.insert(newFile);
  }
  return newFile;
}

static InputFile *addFile(StringRef path, LoadType loadType,
                          bool isLazy = false, bool isExplicit = true,
                          bool isBundleLoader = false,
                          bool isForceHidden = false) {
  return processFile(readFile(path), nullptr, path, loadType, isLazy,
                     isExplicit, isBundleLoader, isForceHidden);
}

static DenseSet<StringRef> loadedObjectFrameworks;

static void applyDylibMetadata(InputFile *file, bool isNeeded, bool isWeak,
                               bool isReexport) {
  if (auto *dylibFile = dyn_cast_or_null<DylibFile>(file)) {
    dylibFile->forceNeeded |= isNeeded;
    dylibFile->forceWeakImport |= isWeak;
    if (isReexport) {
      config->hasReexports = true;
      dylibFile->reexport = true;
    }
  }
}

static void checkAndCacheFramework(InputFile *file, StringRef path) {
  if (isa_and_nonnull<ObjFile>(file) || isa_and_nonnull<BitcodeFile>(file)) {
    if (path.contains(".framework"))
      loadedObjectFrameworks.insert(path);
  }
}

namespace {
// One held archive's prepass: everything the walk needs from the archive that
// depends only on the archive's bytes -- its index, each entry's member offset,
// the per-member maps, and each member's undefined names. None of it reads the
// symbol table or allocates from anything the main thread shares, so it runs as
// worker tasks from the moment the archive is held (see startArchivePrepass())
// instead of at the top of the walk, which puts it behind the rest of the
// argument walk -- the dylib registrations and the wait on the TAPI pipeline --
// rather than in front of the first thing the walk actually has to do serially.
//
// The stages depend on each other but the tasks never block on each other: each
// stage's last task starts the next one. Nesting a parallelFor inside a task
// instead would let one worker's nested wait pull in another archive's whole
// prepass, so the depth would grow with the number of archives, and a Rust link
// holds two hundred of them.
struct ArchivePrepassState {
  ArchiveFile *archive = nullptr;
  DeferredDiagnostics diags;
  // Cleared if this archive cannot be walked, which sends the whole archive set
  // back to addLazySymbols().
  std::atomic<bool> ok{true};
  // Set once every stage has finished. Not set when the prepass was abandoned.
  std::atomic<bool> done{false};
  // Outstanding tasks of the stage now running.
  std::atomic<uint32_t> pending{0};
  std::vector<std::pair<ArchiveFile *, uint64_t>> members;
};

// E006 design 7, stage 1: direct object files are parsed on workers.
//
// The command line is still walked in order, so that every diagnostic, every
// --reproduce member and every dylib registration keeps its position. A direct
// object file is created at its position but its parse is held back; the held
// parses run together, on workers, at the first point where the serial order
// would become observable. That point is the first archive (a lazy symbol in
// the table would make the fold's rank decisions wrong, and an archive's
// extraction decisions depend on every preceding object having been resolved),
// the first bitcode or --start-lib input, or the end of the walk.
struct ParallelInputPhase {
  bool active = false;
  // Contexts are referenced by the files that own them, so the container must
  // not reallocate.
  std::deque<ParseContext> contexts;
  std::vector<std::pair<ObjFile *, uint32_t>> objects;
  // Archives whose index registration the walker owns, in command-line order,
  // each with the ordinal of its position on the command line.
  std::vector<ArchiveFile *> archives;
  std::vector<uint32_t> archiveOrdinals;
  uint32_t nextOrdinal = 0;
  // Set when a held object carries an LC_LINKER_OPTION, which means a library
  // loads after the phase and the walk cannot run. Learned at hold time, from
  // the object's load commands, so the decision is available before the first
  // archive is held instead of only at finishParallelInput(). Monotone: an
  // object that follows a held archive is not held at all.
  bool heldLinkerOptions = false;
  // One prepass state per held archive, in the same order as `archives`. A deque
  // because the running tasks hold references into it while the argument walk
  // appends more.
  std::deque<ArchivePrepassState> prepassStates;
  // Holds every prepass task of every held archive. Created with the first held
  // archive and joined once, at the end of the phase. The tasks spawn their
  // successors through the raw pointer, not the owner: unique_ptr::reset() clears
  // the owner before the destructor waits for the tasks, so the owner is null for
  // exactly the window in which they are still running.
  std::unique_ptr<llvm::parallel::TaskGroup> prepass;
  llvm::parallel::TaskGroup *prepassGroup = nullptr;
  // Holds every streamed member parse the select loop has spawned. Owned by the
  // phase rather than by walkArchives() for the same reason as the prepass: a
  // fatal() raised on the main thread inside the select loop never unwinds, so
  // the driver's cleanup callback has to be able to reach these tasks and wait
  // for them before it frees the archive buffers and the symbol arena they are
  // still writing. Same two-handle shape: the raw pointer is what spawns go
  // through, and it is null for exactly the window in which unique_ptr::reset()
  // is waiting.
  std::unique_ptr<llvm::parallel::TaskGroup> memberParses;
  llvm::parallel::TaskGroup *memberParseGroup = nullptr;
  // Holds the held object files' parses, which are spawned in chunks behind the
  // argument walk instead of all at once at its end. Created by
  // startParallelInput() and joined by finishParallelInput(), or by the cleanup
  // callback for a link that leaves the walk through a fatal(). `streamedObjects`
  // is how many of `objects` have been spawned.
  std::unique_ptr<llvm::parallel::TaskGroup> heldParse;
  size_t streamedObjects = 0;
  // Held objects already parsed when an inline dylib drained the streamed
  // parse (drainStreamedHeldParse()); the rest are parsed at the phase's end.
  size_t drainedObjects = 0;
  // Adopted members not yet parsed. Read by the select loop, decremented by the
  // tasks; it outlives walkArchives()'s frame for the fatal() case above.
  std::atomic<int> memberParsesInFlight{0};
};

// Where the first member defining a name lives. The walker needs the earliest
// provider, because that is the one whose LazyArchive symbol the serial linker
// would have left in the table.
struct ArchiveProvider {
  uint32_t archiveIndex;
  uint32_t entryIndex;
  uint64_t childOffset;
  // Set by the walk when a member defining this name has been extracted. The
  // serial linker would then have created the name's slot inside that member's
  // parse, so no later index entry for it extracts anything. Keeping the flag
  // here folds what was a separate "provided by an extracted member" set into
  // the provider lookup the walk already has to make.
  bool extracted = false;
  // The pre-walk answer to definedAlready() for this name, captured on a worker
  // during the provider fold -- before the select loop starts, and so before any
  // streamed member parse can have folded a record. The select loop must not read
  // the symbol table while workers fold into it, and it does not have to: the
  // answer it needs is a function of the table as it stood once the direct objects
  // and the dylibs were resolved and nothing else was, which is exactly this
  // snapshot. See "Streaming the member parses" in walkArchives().
  //
  // A provider is only inserted for a name that is neither Defined nor Common at
  // that point, so the only state that can suppress an extraction is a non-weak
  // dylib definition: `strongDylib`, with either `dylibAlways` (a dynamic-lookup
  // symbol, which suppresses against every archive) or `dylibId`, the defining
  // dylib's file id, which suppresses against an archive ordered after it.
  bool strongDylib = false;
  bool dylibAlways = false;
  int dylibId = 0;

  bool precedes(const ArchiveProvider &other) const {
    return std::tie(archiveIndex, entryIndex) <
           std::tie(other.archiveIndex, other.entryIndex);
  }
};

// One slot of a provider shard's open-addressing table. The name's hash and
// length sit next to its pointer, so that a probe rejects a non-matching slot
// without touching the name's bytes, and only a hash-and-length match reads
// them. An empty slot has providerIndex == emptyProvider.
struct ProviderSlot {
  uint32_t hash;
  uint32_t len;
  const char *name;
  uint32_t providerIndex;
};
static constexpr uint32_t emptyProvider = UINT32_MAX;

// The select loop's lookups are its largest cost, and each one used to be a
// chain of two or three dependent cache misses (the DenseMap bucket, then the
// key's bytes), one name at a time. The shard is a flat table instead, sized
// before the filter pass and kept at or under half full, with
// linear probing, so that a lookup can be split into stages and the misses of a
// whole block of names overlapped (see resolveArchiveProviders()). It is written
// only in the filter pass, under `mu`; afterwards the slots are read-only and
// only the providers' `extracted` flags change, on the main thread.
struct ProviderShard {
  std::vector<ProviderSlot> slots;
  uint32_t mask = 0;
  uint32_t used = 0;
  std::vector<ArchiveProvider> providers;
  std::mutex mu;

  void init(size_t entries) {
    // `entries` bounds the shard's distinct names, but on the corpora they are
    // 30-100% of it (Chromium's 2.68 M index entries name 1.07 M names), so a
    // table sized for load 0.5 on the entries was at load 0.13 there and cost
    // 190 MiB of slots. Size it for the entries at load 1 instead and grow past
    // load 0.5: one growth at most on a link whose names are all distinct.
    size_t capacity = llvm::PowerOf2Ceil(std::max<size_t>(16, entries));
    slots.assign(capacity, ProviderSlot{0, 0, nullptr, emptyProvider});
    mask = capacity - 1;
    used = 0;
  }

  // The slot holding `name`, or the empty slot where it would go.
  ProviderSlot *probe(llvm::CachedHashStringRef name) {
    StringRef str = name.val();
    for (uint32_t pos = name.hash() & mask;; pos = (pos + 1) & mask) {
      ProviderSlot &slot = slots[pos];
      if (slot.providerIndex == emptyProvider ||
          (slot.hash == name.hash() && slot.len == str.size() &&
           memcmp(slot.name, str.data(), str.size()) == 0))
        return &slot;
    }
  }

  void grow() {
    std::vector<ProviderSlot> old = std::move(slots);
    slots.assign(old.size() * 2, ProviderSlot{0, 0, nullptr, emptyProvider});
    mask = slots.size() - 1;
    for (const ProviderSlot &slot : old) {
      if (slot.providerIndex == emptyProvider)
        continue;
      uint32_t pos = slot.hash & mask;
      while (slots[pos].providerIndex != emptyProvider)
        pos = (pos + 1) & mask;
      slots[pos] = slot;
    }
  }

  // The provider for `name`, inserting `provider` if there is none. The caller
  // holds `mu`.
  std::pair<ArchiveProvider *, bool>
  findOrInsert(llvm::CachedHashStringRef name, const ArchiveProvider &provider) {
    ProviderSlot *slot = probe(name);
    if (slot->providerIndex != emptyProvider)
      return {&providers[slot->providerIndex], false};
    // Keep the load at or under one half (see init()).
    if (2 * (used + 1) > slots.size()) {
      grow();
      slot = probe(name);
    }
    ++used;
    *slot = ProviderSlot{name.hash(), static_cast<uint32_t>(name.size()),
                         name.val().data(),
                         static_cast<uint32_t>(providers.size())};
    providers.push_back(provider);
    return {&providers.back(), true};
  }
};
} // namespace

static ParallelInputPhase parallelInput;

// Everything the parallel input phase owns across a whole link. A second link in
// the same process must not see any of it: the provider hook in particular is
// consulted for the rest of the link and would reach into freed ArchiveFiles.
static void resetParallelInputPhase();

// See weakOverrideSuppressedHook. Built once, before the walk, from the provider
// map; read from workers during the member folds and serially afterwards, never
// written again.
static llvm::DenseSet<const Symbol *> weakOverrideSuppressed;
static bool suppressesWeakOverride(const Symbol *sym) {
  return weakOverrideSuppressed.contains(sym);
}

static constexpr unsigned numProviderShards = 64;
static std::unique_ptr<ProviderShard[]> providerShards;
static unsigned providerShardOf(llvm::CachedHashStringRef name) {
  return (name.hash() >> 25) & (numProviderShards - 1);
}
static ArchiveProvider *lookupArchiveProvider(llvm::CachedHashStringRef name) {
  if (!providerShards)
    return nullptr;
  ProviderShard &shard = providerShards[providerShardOf(name)];
  ProviderSlot *slot = shard.probe(name);
  return slot->providerIndex == emptyProvider
             ? nullptr
             : &shard.providers[slot->providerIndex];
}

static inline void prefetchRead(const void *p) {
#if defined(__GNUC__) || defined(__clang__)
  __builtin_prefetch(p, /*rw=*/0, /*locality=*/3);
#else
  (void)p;
#endif
}

// lookupArchiveProvider() for `n` names at once: out[i] is the provider of
// nameAt(i), or null. The same answer as n separate lookups, but staged over
// blocks of names so that the cache misses of a block overlap instead of
// forming one dependent chain per name: first the home slot of every name is
// prefetched; then each name's probe runs, stopping at the first slot whose hash
// and length match (whose name bytes are prefetched in turn) or at an empty
// slot; then the matches' bytes are compared, the provider records prefetched,
// and the rare hash-and-length collision finishes its probe the slow way.
// Linear probing keeps a chain's later slots next to the prefetched one.
static constexpr size_t providerLookupBlock = 64;
template <typename NameAt>
static void resolveArchiveProviders(size_t n, NameAt nameAt,
                                    ArchiveProvider **out) {
  ProviderShard *shards = providerShards.get();
  ProviderSlot *hit[providerLookupBlock];
  for (size_t begin = 0; begin < n; begin += providerLookupBlock) {
    size_t count = std::min(providerLookupBlock, n - begin);
    for (size_t i = 0; i != count; ++i) {
      llvm::CachedHashStringRef name = nameAt(begin + i);
      ProviderShard &shard = shards[providerShardOf(name)];
      prefetchRead(&shard.slots[name.hash() & shard.mask]);
    }
    for (size_t i = 0; i != count; ++i) {
      llvm::CachedHashStringRef name = nameAt(begin + i);
      ProviderShard &shard = shards[providerShardOf(name)];
      hit[i] = nullptr;
      for (uint32_t pos = name.hash() & shard.mask;; pos = (pos + 1) & shard.mask) {
        ProviderSlot &slot = shard.slots[pos];
        if (slot.providerIndex == emptyProvider)
          break;
        if (slot.hash == name.hash() && slot.len == name.size()) {
          prefetchRead(slot.name);
          hit[i] = &slot;
          break;
        }
      }
    }
    for (size_t i = 0; i != count; ++i) {
      ArchiveProvider *&result = out[begin + i];
      ProviderSlot *slot = hit[i];
      if (!slot) {
        result = nullptr;
        continue;
      }
      llvm::CachedHashStringRef name = nameAt(begin + i);
      ProviderShard &shard = shards[providerShardOf(name)];
      if (memcmp(slot->name, name.val().data(), name.size()) != 0) {
        // A different name with the same hash and length: finish the probe.
        result = lookupArchiveProvider(name);
        continue;
      }
      result = &shard.providers[slot->providerIndex];
      prefetchRead(result);
    }
  }
}

static bool reverseHeldParseOrder();

// Test hooks for the streamed held parse. LLD_NO_STREAM_HELD_PARSE parses the
// held objects all at once at the end of the phase, as before streaming, so a
// link can be compared against itself both ways. LLD_HELD_PARSE_CHUNK sets the
// chunk size, so that a small test streams at all, and LLD_HELD_PARSE_DELAY_US
// sleeps before each held parse, so that the main thread's dylib publication
// runs while the folds are still in flight.
static bool streamHeldParse() {
  static bool value = !reverseHeldParseOrder() &&
                      ::getenv("LLD_NO_STREAM_HELD_PARSE") == nullptr;
  return value;
}

static size_t heldParseChunk() {
  static size_t value = [] {
    unsigned n = 0;
    if (const char *v = ::getenv("LLD_HELD_PARSE_CHUNK"))
      if (llvm::to_integer(v, n) && n != 0)
        return static_cast<size_t>(n);
    // Chunks rather than one task per object: a spawn per object was measurably
    // dearer in condition-variable traffic (S5), and 32 gives Chromium's 11,000
    // objects about 360 tasks.
    return static_cast<size_t>(32);
  }();
  return value;
}

static unsigned heldParseDelayUs() {
  static unsigned value = [] {
    unsigned n = 0;
    if (const char *v = ::getenv("LLD_HELD_PARSE_DELAY_US"))
      llvm::to_integer(v, n);
    return n;
  }();
  return value;
}

static void parseHeldObject(ObjFile *obj) {
  deferredDiagnostics = &obj->parseCtx->diags;
  obj->parseFile();
  deferredDiagnostics = nullptr;
}

// Spawn the held objects that have not been spawned yet as one task.
static void spawnHeldParseChunk() {
  auto &objects = parallelInput.objects;
  size_t begin = parallelInput.streamedObjects, end = objects.size();
  if (begin == end)
    return;
  SmallVector<ObjFile *, 32> chunk;
  chunk.reserve(end - begin);
  for (size_t i = begin; i != end; ++i)
    chunk.push_back(objects[i].first);
  parallelInput.streamedObjects = end;
  // From the first spawn on, the main thread's inserts can meet a fold in the
  // same shard, and its allocations go to its own arenas. Before it nothing is
  // concurrent, so a walk that holds no object pays nothing.
  if (begin == 0) {
    symtab->setHeldParseLive(true);
    parallelParseActive = true;
  }
  parallelInput.heldParse->spawn([chunk = std::move(chunk)] {
    unsigned delay = heldParseDelayUs();
    for (ObjFile *obj : chunk) {
      if (delay)
        std::this_thread::sleep_for(std::chrono::microseconds(delay));
      parseHeldObject(obj);
    }
  });
}

// Wait for every streamed held parse and take the symbol table out of its
// locked mode. Called at the end of the phase, and again from the driver's
// cleanup callback for the link that left the walk through a fatal(): the tasks
// write the symbol table and read the object buffers that the callback is about
// to release.
static void joinHeldParse() {
  if (!parallelInput.heldParse)
    return;
  parallelInput.heldParse.reset(); // The destructor waits for the tasks.
  parallelInput.streamedObjects = 0;
  symtab->setHeldParseLive(false);
  parallelParseActive = false;
}

// See Driver.h. Only an input that cannot defer its dylib names gets here (a
// TBD the pipeline did not prepare); every corpus defers them all.
void macho::drainStreamedHeldParse() {
  if (!parallelInput.heldParse)
    return;
  TimeTraceScope timeScope("Drain streamed held parse");
  spawnHeldParseChunk();
  joinHeldParse();
  parallelInput.drainedObjects = parallelInput.objects.size();
  flushAutolinkBatch();
}

static void startParallelInput() {
#if LLVM_ENABLE_THREADS
  // Check ThreadsRequested rather than getThreadCount(), which would create an
  // executor capable of spawning a worker even for one thread.
  if (parallel::strategy.ThreadsRequested == 1)
    return;
  parallelInput.active = true;
  symtab->beginParallelInput();
  // The held objects are parsed on workers while the main thread is still
  // walking the command line, in chunks spawned from endHeldInput(), instead of
  // all at once when the phase ends. Nothing the walk decides reads a held
  // parse's result, and the fold is already order-independent (it runs in
  // arbitrary worker order, and LLD_REVERSE_HELD_PARSE tests the reverse), so
  // this changes when the parses run, never what they produce. What it adds is
  // concurrency with the walk's own symbol table inserts, which take the shard
  // lock while the parse is live (SymbolTable::addDylibImpl), and with the walk's
  // allocations, which go to the main thread's own arenas from the first spawn.
  if (streamHeldParse()) {
    parallelInput.heldParse = std::make_unique<llvm::parallel::TaskGroup>();
    parallelInput.streamedObjects = 0;
  }
#endif
}

// Set when the phase learns it will not walk after all. The prepass is pure, so
// a task that has not started yet can simply return; what has already been spent
// is spent.
static std::atomic<bool> prepassCancelled{false};

// Index entries per offset-resolution task. One archive can hold most of the
// entries on the command line -- one Rust staticlib has 157,000 -- so the chunk
// is the unit of work, not the archive.
static constexpr uint32_t prepassChunkEntries = 8192;

static void prepassResolveOffsets(ArchivePrepassState &state);
static void prepassBuildMaps(ArchivePrepassState &state);
static void prepassFillUndefineds(ArchivePrepassState &state);

static bool prepassStopped(const ArchivePrepassState &state) {
  return prepassCancelled.load(std::memory_order_relaxed) ||
         !state.ok.load(std::memory_order_relaxed);
}

// Stage 1: the archive's symbol table, which the Archive API can only walk in
// order, so this is one task per archive.
static void prepassReadIndex(ArchivePrepassState &state) {
  if (prepassStopped(state))
    return;
  deferredDiagnostics = &state.diags;
  bool ok = state.archive->buildIndex();
  deferredDiagnostics = nullptr;
  if (!ok) {
    state.ok.store(false, std::memory_order_relaxed);
    return;
  }
  prepassResolveOffsets(state);
}

// Stage 2: each entry's member offset. Resolving one parses that member's header,
// so this is chunked; doing it once per entry rather than once per member was
// most of what reading an index cost.
static void prepassResolveOffsets(ArchivePrepassState &state) {
  if (prepassStopped(state))
    return;
  ArchiveFile *archive = state.archive;
  uint32_t entries = archive->indexEntries.size();
  uint32_t numChunks = llvm::divideCeil(entries, prepassChunkEntries);
  if (numChunks == 0) {
    prepassBuildMaps(state);
    return;
  }
  state.pending.store(numChunks, std::memory_order_relaxed);
  for (uint32_t chunk = 0; chunk != numChunks; ++chunk)
    parallelInput.prepassGroup->spawn([&state, chunk, entries] {
      if (!prepassStopped(state)) {
        uint32_t begin = chunk * prepassChunkEntries;
        if (!state.archive->resolveIndexOffsets(
                begin, std::min(begin + prepassChunkEntries, entries)))
          state.ok.store(false, std::memory_order_relaxed);
      }
      if (state.pending.fetch_sub(1, std::memory_order_acq_rel) == 1)
        prepassBuildMaps(state);
    });
}

// Stage 3: the per-member maps, map work only now that every offset is resolved
// and every member header is warm. One task per archive.
static void prepassBuildMaps(ArchivePrepassState &state) {
  if (prepassStopped(state))
    return;
  if (!state.archive->buildMemberMaps()) {
    state.ok.store(false, std::memory_order_relaxed);
    return;
  }
  prepassFillUndefineds(state);
}

// Stage 4: every member's undefined names. This is the walk's largest cost by a
// wide margin -- not the symbol lookups but the walk over each member's own
// symbol table, which first-touches tens of thousands of pages of a
// multi-gigabyte archive mapping. A member that is never extracted costs a
// worker one wasted scan.
static void prepassFillUndefineds(ArchivePrepassState &state) {
  if (prepassStopped(state))
    return;
  state.archive->reserveMemberUndefineds(state.members);
  size_t members = state.members.size();
  if (members == 0) {
    state.done.store(true, std::memory_order_release);
    return;
  }
  size_t chunk =
      std::max<size_t>(1, members / (llvm::parallel::getThreadCount() * 4));
  size_t numChunks = llvm::divideCeil(members, chunk);
  state.pending.store(numChunks, std::memory_order_relaxed);
  for (size_t c = 0; c != numChunks; ++c)
    parallelInput.prepassGroup->spawn([&state, c, chunk, members] {
      if (!prepassStopped(state))
        for (size_t i = c * chunk, e = std::min(i + chunk, members); i != e; ++i)
          if (!state.members[i].first->fillMemberUndefineds(
                  state.members[i].second))
            state.ok.store(false, std::memory_order_relaxed);
      if (state.pending.fetch_sub(1, std::memory_order_acq_rel) == 1)
        state.done.store(true, std::memory_order_release);
    });
}

static void startArchivePrepass(ArchiveFile *archive) {
  if (!parallelInput.prepass) {
    parallelInput.prepass = std::make_unique<llvm::parallel::TaskGroup>();
    parallelInput.prepassGroup = parallelInput.prepass.get();
  }
  ArchivePrepassState &state = parallelInput.prepassStates.emplace_back();
  state.archive = archive;
  parallelInput.prepassGroup->spawn([&state] { prepassReadIndex(state); });
}

// Nothing after this point may read an archive the prepass is still writing, and
// the publishing paths write the archive's symbols themselves. `willWalk` is the
// caller's knowledge at the join, which is before the held objects' own
// LC_LINKER_OPTIONs have been seen: it only decides whether the remaining tasks
// are worth finishing, never whether the result is used.
static void joinArchivePrepass(bool willWalk) {
  if (!parallelInput.prepass)
    return;
  if (!willWalk)
    prepassCancelled.store(true, std::memory_order_relaxed);
  parallelInput.prepass.reset(); // The destructor waits for the tasks.
  parallelInput.prepassGroup = nullptr;
}

// Wait for every member parse the select loop streamed. Called at the end of the
// walk, and again from the driver's cleanup callback for the link that left the
// walk through a fatal().
static void joinStreamedMemberParses() {
  if (!parallelInput.memberParses)
    return;
  parallelInput.memberParseGroup = nullptr;
  parallelInput.memberParses.reset(); // The destructor waits for the tasks.
  parallelInput.memberParsesInFlight.store(0, std::memory_order_relaxed);
  parallelParseActive = false;
}

// Test hook. Reversing the order in which the workers take the held files apart
// exercises the fold's out-of-order paths deterministically, at any thread
// count; the output must not change.
static bool reverseHeldParseOrder() {
  static bool value = ::getenv("LLD_REVERSE_HELD_PARSE") != nullptr;
  return value;
}

// Give a file that the phase will parse on a worker its ordinal and context. A
// direct input takes the next command-line ordinal; an extracted member is an
// event inside its archive's registration, so it takes that archive's ordinal
// and a walk value from the archive's own counter.
static void adoptHeldFile(ObjFile *obj, uint32_t top = UINT32_MAX,
                          uint32_t walk = 0) {
  parallelInput.contexts.emplace_back();
  ParseContext *ctx = &parallelInput.contexts.back();
  ctx->top = top == UINT32_MAX ? parallelInput.nextOrdinal++ : top;
  ctx->walk = walk;
  obj->parseCtx = ctx;
  obj->deferUnwindRegistration = true;
  parallelInput.objects.emplace_back(obj, ctx->top);
}

static void parseHeldFiles(size_t begin) {
  std::vector<ObjFile *> order;
  for (size_t i = begin, e = parallelInput.objects.size(); i != e; ++i)
    order.push_back(parallelInput.objects[i].first);
  if (reverseHeldParseOrder())
    std::reverse(order.begin(), order.end());
  parallelParseActive = true;
  parallelForEach(order, parseHeldObject);
  parallelParseActive = false;
}

// E006 design 7, stage 2: the causal walker.
//
// Extraction from an indexed archive is decided by the index and by which
// members have already been extracted, never by a member's parse, so the
// decisions stay serial and in order while the parses they trigger go to
// workers. Returns false if the archives cannot be walked, in which case the
// caller falls back to addLazySymbols().
static bool walkArchives() {
  std::vector<ArchiveFile *> &archives = parallelInput.archives;

  // The prepass -- each archive's index, its entries' member offsets, its member
  // maps and its members' undefined names -- ran on workers from the point each
  // archive was held, and finishParallelInput() joined it before it parsed the
  // held objects, so everything it produced is finished and visible here. Its
  // diagnostics replay in archive order, which is where the serial linker
  // reported them.
  assert(parallelInput.prepassStates.size() == archives.size());
  std::unique_ptr<TimeTraceScope> ts;
  for (ArchivePrepassState &state : parallelInput.prepassStates)
    if (!state.ok.load(std::memory_order_acquire) ||
        !state.done.load(std::memory_order_acquire))
      return false;

  // Chunks of index entries. Everything that is per entry rather than per archive
  // is scheduled this way, because one Rust staticlib or one component archive
  // can hold most of the entries on the command line and a per-archive
  // parallelFor would leave it on a single worker.
  struct EntryChunk {
    uint32_t archiveIndex;
    uint32_t begin;
    uint32_t end;
  };
  std::vector<EntryChunk> chunks;
  for (uint32_t i = 0; i != archives.size(); ++i) {
    uint32_t n = archives[i]->indexEntries.size();
    for (uint32_t b = 0; b < n; b += prepassChunkEntries)
      chunks.push_back({i, b, std::min(b + prepassChunkEntries, n)});
  }

  {
    // An extracted member that brings in its own libraries would have those
    // inputs reference names after the phase has ended. The walk creates no slot
    // for an index entry it does not extract, so such a name would be created
    // where it was referenced rather than at its index entry, and symVector's
    // order -- and with it the symbol table and the stubs -- would differ from
    // the serial link. Give the whole archive set back to addLazySymbols().
    for (ArchiveFile *archive : archives)
      if (archive->memberLinkerOptions.load(std::memory_order_relaxed))
        return false;
    // The walk counter's ceiling for one archive: one value per index entry,
    // visited or not, plus one per member and one per member undefined record.
    // A member of an earlier archive can be extracted inside this archive's
    // registration, so the member and undefined terms are summed over every
    // archive. If that does not fit in the ordinal's walk field, the walk cannot
    // number its events, and the archives publish their indexes the ordinary way
    // instead.
    uint64_t events = 0;
    for (ArchiveFile *archive : archives)
      for (const auto &kv : archive->indexChildren)
        events += 1 + archive->memberUndefineds(kv.first).size();
    uint64_t maxEntries = 0;
    for (ArchiveFile *archive : archives)
      maxEntries = std::max<uint64_t>(maxEntries, archive->indexEntries.size());
    if (maxEntries + events > ordinalWalkMax)
      return false;
  }

  // Every decline is behind us, so the prepass's diagnostics can be replayed:
  // buildIndex() makes the same first-member architecture check as
  // addLazySymbols(), and on a command line that declines after the prepass has
  // run it is addLazySymbols() that reports it. Replaying before the decline
  // decisions emitted that warning twice.
  for (ArchivePrepassState &state : parallelInput.prepassStates)
    if (!state.diags.empty())
      replayDiagnostics(state.diags);

  // One pass over every index entry, on workers, read-only on the symbol table,
  // producing both of the things the serial walk needs from it: the earliest
  // provider of each name, and the entries whose name is in a state the walk can
  // extract for, which are the only ones it has to look at. These were two
  // passes, and the second repeated the first one's lookup per entry; but the
  // provider rule's "skip a name already defined" is exactly the candidate rule's
  // first test, so one lookup answers both. Chromium's 2.68 M entries pay half
  // the lookups and one pass over the index arrays instead of two.
  //
  // Deriving the providers from the archive bytes alone would take this pass off
  // the walk's critical path altogether, but then a member undefined naming a
  // direct object's definition -- the common case -- would reach the table in the
  // serial select loop instead of failing a provider lookup, which on Chromium is
  // half a million lookups moved from here into the serial part.
  ts.reset(new TimeTraceScope("Filter and fold archive indexes"));
  providerShards = std::make_unique<ProviderShard[]>(numProviderShards);
  {
    // Each shard's share of the index entries bounds its distinct names.
    size_t totalEntries = 0;
    for (ArchiveFile *archive : archives)
      totalEntries += archive->indexEntries.size();
    size_t share = llvm::divideCeil(totalEntries, numProviderShards);
    parallelFor(0, numProviderShards,
                [&](size_t i) { providerShards[i].init(share); });
  }
  std::vector<llvm::SmallVector<uint32_t, 0>> chunkCandidates(chunks.size());
  parallelFor(0, chunks.size(), [&](size_t chunkIndex) {
    const EntryChunk &chunk = chunks[chunkIndex];
    ArchiveFile *archive = archives[chunk.archiveIndex];
    llvm::SmallVector<uint32_t, 0> &candidates = chunkCandidates[chunkIndex];
    for (uint32_t entryIndex = chunk.begin; entryIndex != chunk.end;
         ++entryIndex) {
      const ArchiveFile::IndexEntry &entry = archive->indexEntries[entryIndex];
      ArchiveProvider provider{chunk.archiveIndex, entryIndex,
                               entry.childOffset};
      // The direct objects are resolved and nothing mutates the table until the
      // walk begins, so this lookup needs no lock. It now answers three things:
      // the candidate test, the provider rule, and the walk's suppression
      // snapshot, which the select loop reads in place of the table once the
      // member parses stream behind it.
      Symbol *sym = symtab->find(entry.name);
      if (sym) {
        // A name a direct object or a preceding dylib already defines can never
        // be extracted for, so it is neither a candidate nor in need of a
        // provider.
        if (isa<Defined>(sym) || isa<CommonSymbol>(sym))
          continue;
        // The candidate test is the negation of the walk's own suppression rule,
        // not simply "undefined now": this runs after the inputs that follow the
        // archive have been registered, so a dylib listed after it may be sitting
        // in a slot whose lazy symbol the serial linker had already put there,
        // and such an entry is still a candidate.
        if (isa<Undefined>(sym)) {
          candidates.push_back(entryIndex);
        } else if (auto *dysym = dyn_cast<DylibSymbol>(sym)) {
          const bool weak = dysym->isWeakDef();
          const bool dynamicLookup = dysym->isDynamicLookup();
          // A weak dylib definition never suppresses an extraction; a strong one
          // does exactly when it was registered before this archive.
          if (dysym->isReferenced() &&
              (weak || (!dynamicLookup && dysym->getFile()->id > archive->id)))
            candidates.push_back(entryIndex);
          // The same rule again for the snapshot (see ArchiveProvider), left
          // unresolved because the walk applies it against the *provider's*
          // archive, which this pass does not know. getFile() is null for a
          // dynamic-lookup symbol, which suppresses against every archive, so it
          // is recorded as a flag and the id is not read.
          if (!weak) {
            provider.strongDylib = true;
            provider.dylibAlways = dynamicLookup;
            if (!dynamicLookup)
              provider.dylibId = dysym->getFile()->id;
          }
        }
      }
      // A name nothing has mentioned yet is not a candidate, but it still needs a
      // provider: a member the walk extracts later can be the first to reference
      // it.
      ProviderShard &shard = providerShards[providerShardOf(entry.name)];
      std::lock_guard<std::mutex> lock(shard.mu);
      auto [existing, inserted] = shard.findOrInsert(entry.name, provider);
      if (!inserted && provider.precedes(*existing))
        *existing = provider;
    }
  });
  // The walk visits an archive's candidates in index order. The chunks are built
  // archive by archive with ascending begins, so concatenating their candidate
  // lists in chunk order is that order.
  {
    std::vector<size_t> counts(archives.size(), 0);
    for (size_t i = 0, e = chunks.size(); i != e; ++i)
      counts[chunks[i].archiveIndex] += chunkCandidates[i].size();
    for (size_t i = 0, e = archives.size(); i != e; ++i)
      archives[i]->candidateEntries.reserve(counts[i]);
    for (size_t i = 0, e = chunks.size(); i != e; ++i)
      archives[chunks[i].archiveIndex]->candidateEntries.append(
          chunkCandidates[i].begin(), chunkCandidates[i].end());
  }

  ts.reset(new TimeTraceScope("Suppress weak dylib overrides"));
  // A weak dylib definition that nothing has referenced yet is not what the
  // serial linker had in the slot when an archive member's definition arrived:
  // the archive's index entry replaced it with a LazyArchive symbol first, and
  // addDefined then saw a lazy symbol rather than a weak dylib one, so it set no
  // override. The walk leaves no lazy symbol behind, so the weak dylib symbol is
  // still there and both the fold and the post-phase path would set one. Mark
  // them here -- after the direct objects have taken their references, which is
  // the state at the archive's own position -- and suppress the override on both
  // paths. The candidate test above drops such an entry for the same reason, so
  // the walk itself never extracts at it.
  //
  // Nothing mutates the symbol table or the provider table between the filter
  // pass and the walk, so the lookups run one provider shard per task and only
  // the set's inserts are serial; the set's contents do not depend on the order
  // they are inserted in.
  {
    std::vector<std::vector<Symbol *>> suppressed(numProviderShards);
    parallelFor(0, numProviderShards, [&](size_t i) {
      const ProviderShard &shard = providerShards[i];
      for (const ProviderSlot &slot : shard.slots) {
        if (slot.providerIndex == emptyProvider)
          continue;
        Symbol *sym = symtab->find(llvm::CachedHashStringRef(
            StringRef(slot.name, slot.len), slot.hash));
        auto *dysym = dyn_cast_or_null<DylibSymbol>(sym);
        if (!dysym || !dysym->isWeakDef() || dysym->isReferenced())
          continue;
        const ArchiveProvider &provider = shard.providers[slot.providerIndex];
        if (dysym->getFile()->id < archives[provider.archiveIndex]->id)
          suppressed[i].push_back(sym);
      }
    });
    for (const std::vector<Symbol *> &syms : suppressed)
      weakOverrideSuppressed.insert(syms.begin(), syms.end());
  }
  weakOverrideSuppressedHook = suppressesWeakOverride;

  ts.reset();
  // Is the name already satisfied at the point archive `archiveIndex` is
  // registered? A definition in a direct object, a definition in a member that
  // has already been extracted, or a strong dylib definition registered before
  // this archive all suppress the extraction.
  // Only the part of the suppression rule that needs the symbol table: the
  // "already provided by an extracted member" part is the provider's `extracted`
  // flag, which every caller has tested before it gets here.
  // Answered from the provider's pre-walk snapshot rather than from the symbol
  // table, because workers are folding streamed member parses into the table
  // while this loop runs. The snapshot is the value find() returned at the point
  // the loop begins: a name with no provider entry was Defined or Common then
  // (the fold skips exactly those), and a name with one was neither, so all that
  // is left to record is the strong-dylib case.
  auto definedAlready = [&](uint32_t archiveIndex,
                            const ArchiveProvider &provider) {
    if (!provider.strongDylib)
      return false;
    // A weak dylib definition never suppresses an extraction; a strong one does
    // exactly when it was registered before this archive.
    return provider.dylibAlways || provider.dylibId < archives[archiveIndex]->id;
  };

  // The point the walk has reached: every index entry before it has been
  // registered and can satisfy an undefined symbol, and nothing after it has.
  uint32_t frontierArchive = 0;
  uint32_t frontierEntry = 0;

  // Every event an archive registration produces -- the index entry itself, the
  // member it extracts, that member's undefined symbols, and anything those
  // extract in turn -- sorts inside that archive's command-line position, so it
  // takes the archive's ordinal and a value from that archive's own monotone walk
  // counter. Nothing here draws from the command-line counter, which is already
  // past every input.
  //
  // An index entry the walk never visits still needs a position, because a slot
  // reservation puts a name at the earliest entry that names it, and
  // that entry may be one the walk passed over. The counter therefore leaves a
  // gap of one value per skipped entry, jumped arithmetically rather than by
  // touching those entries: the walk only visits candidates, which is where its
  // gain comes from. `visited` records the block each visited entry got, so the
  // position of a skipped entry k is one value past the block of the last
  // visited entry before it, offset by how far past it k is.
  struct EntryWalk {
    uint32_t entryIndex;
    uint32_t walkStart;
    uint32_t walkEnd;
  };
  std::vector<std::vector<EntryWalk>> visited(archives.size());
  uint32_t registrationTop = 0;
  uint32_t walkCounter = 0;
  uint32_t lastVisitedEntry = UINT32_MAX; // "before entry 0"
  // Where an archive's index entries start in its walk counter. Zero unless a
  // mandatory-member prelude ran first and consumed values ahead of them.
  std::vector<uint32_t> walkBases(archives.size(), 0);
  // Set while a prelude is running: its members load at the archive's own
  // registration point, before its index is registered, so no entry of the
  // current archive has been passed yet -- not even entry 0.
  bool prelude = false;
  auto walkOfEntry = [&](uint32_t archiveIndex, uint32_t entryIndex) -> uint32_t {
    const std::vector<EntryWalk> &blocks = visited[archiveIndex];
    // The last visited entry at or before entryIndex.
    auto it = std::upper_bound(blocks.begin(), blocks.end(), entryIndex,
                               [](uint32_t k, const EntryWalk &b) {
                                 return k < b.entryIndex;
                               });
    if (it == blocks.begin())
      // No visited entry before it: the gaps start where the entries do.
      return walkBases[archiveIndex] + entryIndex;
    --it;
    if (it->entryIndex == entryIndex)
      return it->walkStart;
    return it->walkEnd + (entryIndex - it->entryIndex - 1);
  };

  // Index entries a member's undefined symbol has scheduled because its earliest
  // provider was still ahead of the frontier. The walk merges them into each
  // archive's candidate list in index order.
  std::vector<std::priority_queue<uint32_t, std::vector<uint32_t>,
                                  std::greater<uint32_t>>>
      scheduled(archives.size());

  const size_t firstMember = parallelInput.objects.size();

  // Streaming the member parses (design step S5).
  //
  // A member's whole position in the serial order is fixed on the main thread at
  // extraction time -- its ordinal (adoptHeldFile), the sub-position of each of
  // its own undefined records, and its completion ordinal -- and so are the
  // `seen` set, -why_load order, inputFiles order and the order its deferred
  // diagnostics replay in, which is parallelInput.contexts order. None of that
  // depends on the parse. So the parse is spawned at the end of extract(), once
  // the member's undefined walk has taken its sub-positions, and the select loop
  // goes on while workers fold. The phase's window becomes max(select, parse)
  // instead of select + parse.
  //
  // What the concurrent folds could have reached, and how each is handled:
  //
  //  * definedAlready() would have read a table that workers are mutating: not
  //    only can a lock-free find() rehash under it, but a slot's contents can
  //    change between the lookup and the predicate. It no longer reads the table
  //    at all -- the answer is the provider's pre-walk snapshot, which is the
  //    value find() returned before any member parse started. The loop itself
  //    makes no mutation of the table at all (the slot reservations are
  //    collected and applied after the join), so the snapshot stays exact. The extraction decisions are therefore both
  //    unchanged and independent of worker timing.
  //
  //    This also settles the reviewer's named concern about `definedAlready`
  //    under concurrent folds. The concern was a weak dylib slot that a member's
  //    strong definition displaces mid-walk, and the interaction with
  //    weakOverrideSuppressed: if the walk read the live table, the answer for
  //    such a name would depend on whether the displacing member's fold had
  //    landed yet, which is a race, not a semantics question. The walk does not
  //    read the live table, so the weak-dylib answer is the one the pre-walk
  //    state gives, which is what the serial linker had at the archive's own
  //    position -- and it is the same state weakOverrideSuppressed was built
  //    from, one pass earlier, so the two cannot disagree. For a name an
  //    extracted member defines, the walk still decides by provider->extracted,
  //    set on the main thread before the member is queued, and never consults a
  //    slot the member's fold will write.
  //
  //  * The slot reservations, which used to insert into a shard under its lock
  //    while workers folded into it, are collected per shard and applied once
  //    the parses are joined; see SymbolTable::applyArchiveReservations() for
  //    why their effects commute with the folds'.
  //
  //  * Everything else the loop touches while workers run is either main-thread
  //    only (the `seen` set, the provider map's `extracted` flags, inputFiles,
  //    the walk counters, the reservation lists) or read-only for the whole
  //    loop (memberUndefineds, the provider table's slots,
  //    weakOverrideSuppressed).
  //
  // LLD_REVERSE_HELD_PARSE keeps its old meaning by turning streaming off: it
  // exists to force one deterministic out-of-order fold order, and streaming
  // would make that order a race instead. LLD_NO_STREAM_MEMBER_PARSE turns it off
  // on its own, so that a link can be compared against itself both ways.
  const bool stream = !reverseHeldParseOrder() &&
                      !::getenv("LLD_NO_STREAM_MEMBER_PARSE");
  // The executor already bounds the number of parses actually running to the
  // worker count. This bounds how far the select loop may run ahead of them, so a
  // link extracting tens of thousands of members does not queue them all at once,
  // and it is the knob to turn down if a screen shows system time rising.
  //
  // LLD_INLINE_MEMBER_PARSE is a test hook that sets the cap to zero, so that
  // every member is parsed inline on the main thread (see streamParse).
  const int inFlightCap =
      ::getenv("LLD_INLINE_MEMBER_PARSE")
          ? 0
          : std::max<int>(1, static_cast<int>(llvm::parallel::getThreadCount()));
  if (stream) {
    parallelInput.memberParses =
        std::make_unique<llvm::parallel::TaskGroup>();
    parallelInput.memberParseGroup = parallelInput.memberParses.get();
    parallelInput.memberParsesInFlight.store(0, std::memory_order_relaxed);
    parallelParseActive = true;
  }
  auto streamParse = [&](ObjFile *obj) {
    std::atomic<int> &inFlight = parallelInput.memberParsesInFlight;
    // At the cap the loop is ahead of the parses, so rather than wait for a
    // worker, the main thread parses this member itself: exactly what the task
    // would have run, on a thread that parallelParseActive already covers. Its
    // diagnostics go to the member's own deferred list, as on a worker. It runs
    // no other task: a fatal() in a task popped from the executor would leave
    // through the crash-recovery context past the task's latch decrement, and
    // the cleanup's joinStreamedMemberParses() would then wait forever. A
    // fatal() here leaves no task behind.
    if (inFlight.load(std::memory_order_acquire) >= inFlightCap) {
      DeferredDiagnostics *saved = deferredDiagnostics;
      deferredDiagnostics = &obj->parseCtx->diags;
      obj->parseFile();
      deferredDiagnostics = saved;
      return;
    }
    inFlight.fetch_add(1, std::memory_order_acq_rel);
    parallelInput.memberParseGroup->spawn([obj]() {
      deferredDiagnostics = &obj->parseCtx->diags;
      obj->parseFile();
      deferredDiagnostics = nullptr;
      parallelInput.memberParsesInFlight.fetch_sub(1,
                                                   std::memory_order_acq_rel);
    });
  };

  // The slot reservations of every extracted member, one list per symbol table
  // shard, applied once the member parses are joined (see
  // SymbolTable::applyArchiveReservations()). The select loop used to insert
  // each one itself under the shard lock, against the folds.
  std::vector<std::vector<SymbolTable::ArchiveReservation>> reservations(
      SymbolTable::numDylibShards);

  // The providers of the names extract() is walking, resolved in one batch per
  // member before its loops run (resolveArchiveProviders()). A stack, because
  // extract() recurses: each frame owns the range from its own base, and
  // indexes it rather than holding a pointer, since a nested frame's push can
  // reallocate. Only the provider pointers are resolved ahead; the `extracted`
  // flags they lead to are still read at the point the loops reach them, after
  // whatever the nested extractions have set.
  std::vector<ArchiveProvider *> resolved;

  // Extract one member, then walk its own undefined symbols depth first, in
  // nlist order: that is where the serial linker applied them, and it applied
  // the whole of an extracted member before moving to the next undefined.
  std::function<void(uint32_t, uint64_t, StringRef)> extract =
      [&](uint32_t archiveIndex, uint64_t childOffset, StringRef reason) {
        ArchiveFile *archive = archives[archiveIndex];
        if (archive->isFetched(childOffset))
          return;
        ObjFile *obj = archive->createIndexedMember(childOffset, reason);
        if (!obj)
          return;
        ArrayRef<uint32_t> ownEntries = archive->memberEntries[childOffset];
        ArrayRef<llvm::CachedHashStringRef> undefNames =
            archive->memberUndefineds(childOffset);
        const size_t base = resolved.size();
        const size_t numOwn = ownEntries.size();
        resolved.resize(base + numOwn + undefNames.size());
        resolveArchiveProviders(
            numOwn + undefNames.size(),
            [&](size_t i) {
              return i < numOwn ? archive->indexEntries[ownEntries[i]].name
                                : undefNames[i - numOwn];
            },
            resolved.data() + base);
        for (size_t k = 0; k != numOwn; ++k) {
          const ArchiveFile::IndexEntry &entry =
              archive->indexEntries[ownEntries[k]];
          // If the walk has already passed an index entry for this name, the
          // serial linker created its slot there, and a definition arriving
          // later did not move it. A name with no provider entry is one no
          // index entry can ever ask for, so it needs no slot and no flag.
          ArchiveProvider *first = resolved[base + k];
          if (!first)
            continue;
          // If a member extracted earlier already provides this name, the serial
          // linker created its slot when that member was parsed, and passing a
          // later index entry for it created nothing.
          if (first->extracted)
            continue;
          first->extracted = true;
          // Only an entry the walk has already passed created a slot there. A
          // prelude has passed nothing in its own archive: the index is not
          // registered until the prelude is over.
          bool passed = first->archiveIndex < frontierArchive ||
                        (first->archiveIndex == frontierArchive && !prelude &&
                         first->entryIndex <= frontierEntry);
          if (!passed)
            continue;
          // The position is final here (walkOfEntry() only ever reads blocks
          // the walk has closed, and this entry's own walkStart), so it is
          // computed now; only the insert waits for the join.
          reservations[SymbolTable::shardOfName(entry.name)].push_back(
              {entry.name, archive,
               RecordOrdinal{parallelInput.archiveOrdinals[first->archiveIndex],
                             walkOfEntry(first->archiveIndex, first->entryIndex),
                             0}});
        }
        // The member's records apply before those of anything it extracts, so
        // it takes its ordinal here...
        adoptHeldFile(obj, registrationTop, walkCounter++);

        // The member's undefined names were enumerated on a worker before the
        // walk started; the serial walk only reads the list.
        for (size_t k = 0, e = undefNames.size(); k != e; ++k) {
          StringRef undefName = undefNames[k].val();
          // The serial linker applies this undefined record here, so it takes
          // this sub-position and anything it extracts takes the ones after it.
          obj->parseCtx->undefinedWalks.push_back(walkCounter++);
          // The provider lookup comes first because it is the only one of these
          // tests that most undefined names fail: a name no archive provides can
          // never extract anything, whatever the symbol table says about it, and
          // one whose provider is already extracted was resolved inside that
          // member's parse. Both are pure tests, so hoisting them above the
          // table lookup does not change which members are extracted.
          ArchiveProvider *provider = resolved[base + numOwn + k];
          if (!provider || provider->extracted)
            continue;
          // Against the provider's archive, not the one being walked: the serial
          // linker created this name's slot at the provider's index entry and left
          // a LazyArchive symbol there, which a dylib export registered after that
          // entry could not displace. So a strong dylib suppresses this extraction
          // exactly when it precedes the provider.
          if (definedAlready(provider->archiveIndex, *provider))
            continue;
          // Only an index entry the walk has already passed can satisfy this.
          // One still ahead of the frontier is scheduled so that the walk visits
          // it in index order when it gets there, which is where the serial
          // linker's own registration of that entry would have extracted it.
          if (provider->archiveIndex > frontierArchive ||
              (provider->archiveIndex == frontierArchive &&
               provider->entryIndex >= frontierEntry)) {
            scheduled[provider->archiveIndex].push(provider->entryIndex);
            continue;
          }
          extract(provider->archiveIndex, provider->childOffset, undefName);
        }
        resolved.resize(base);
        obj->parseCtx->completion =
            RecordOrdinal{registrationTop, walkCounter, 0};

        // ...but the serial linker only published it once its parse, and with
        // it every nested extraction, had finished.
        archive->publishIndexedMember(obj, reason);

        // Everything above fixed this member's place in the serial order, so its
        // parse can run behind the rest of the select loop.
        if (stream)
          streamParse(obj);
      };

  ts.reset(new TimeTraceScope("Select archive members"));
  for (uint32_t archiveIndex = 0; archiveIndex != archives.size();
       ++archiveIndex) {
    ArchiveFile *archive = archives[archiveIndex];
    frontierArchive = archiveIndex;
    registrationTop = parallelInput.archiveOrdinals[archiveIndex];
    walkCounter = 0;
    lastVisitedEntry = UINT32_MAX;

    // -ObjC, -force_load and -all_load load members at the archive's own
    // registration point, before its index is registered, so those members and
    // their undefined symbols take the first walk values of this archive and its
    // index entries start after them. Nothing in this archive can satisfy an
    // undefined yet, so a provider here is scheduled for the entry loop below,
    // which is where the serial linker's own registration of that entry would
    // have extracted it; a provider in an earlier archive extracts at once.
    prelude = true;
    frontierEntry = 0;
    switch (archive->mandatoryLoad) {
    case ArchiveFile::MandatoryLoad::None:
      break;
    case ArchiveFile::MandatoryLoad::LcOption:
      // beginHeldInput() only ever holds a CommandLine or CommandLineForce
      // archive, and this kind is set only for an LC_LINKER_OPTION load, which
      // arrives after the phase has ended.
      llvm_unreachable("the walker cannot own an LC_LINKER_OPTION archive");
    case ArchiveFile::MandatoryLoad::ObjC:
      for (uint32_t entryIndex : archive->classEntries) {
        const ArchiveFile::IndexEntry &entry = archive->indexEntries[entryIndex];
        extract(archiveIndex, entry.childOffset, entry.name.val());
      }
      for (uint64_t childOffset : archive->childOrder)
        if (archive->memberHasObjCSection(childOffset))
          extract(archiveIndex, childOffset, "-ObjC");
      break;
    case ArchiveFile::MandatoryLoad::AllLoad:
    case ArchiveFile::MandatoryLoad::ForceLoad: {
      StringRef reason =
          archive->mandatoryLoad == ArchiveFile::MandatoryLoad::ForceLoad
              ? "-force_load"
              : "-all_load";
      for (uint64_t childOffset : archive->childOrder)
        extract(archiveIndex, childOffset, reason);
      break;
    }
    }
    prelude = false;
    walkBases[archiveIndex] = walkCounter;

    size_t next = 0;
    auto &pending = scheduled[archiveIndex];
    // The candidates' providers, resolved a block at a time ahead of the walk
    // (resolveArchiveProviders()); a scheduled entry that is not also a
    // candidate is looked up on its own. As in extract(), only the pointers are
    // resolved ahead, never the `extracted` flags.
    ArrayRef<uint32_t> candidates = archive->candidateEntries;
    ArchiveProvider *candidateProviders[providerLookupBlock];
    size_t blockBegin = 0, blockEnd = 0;
    while (next != candidates.size() || !pending.empty()) {
      uint32_t entryIndex;
      if (next == candidates.size())
        entryIndex = pending.top();
      else if (pending.empty())
        entryIndex = candidates[next];
      else
        entryIndex = std::min(candidates[next], pending.top());
      const bool isCandidate =
          next != candidates.size() && candidates[next] == entryIndex;
      size_t candidatePos = next;
      while (next != candidates.size() && candidates[next] == entryIndex)
        ++next;
      while (!pending.empty() && pending.top() == entryIndex)
        pending.pop();

      const ArchiveFile::IndexEntry &entry = archive->indexEntries[entryIndex];
      frontierEntry = entryIndex;
      if (archive->isFetched(entry.childOffset))
        continue;
      // Same two pure tests as the undefined walk: an entry whose name a
      // member extracted earlier already provides created nothing in the serial
      // linker, and every candidate entry has a provider by construction (the
      // merged pass gives an entry a provider unless its name is already
      // defined, which is also the first thing that disqualifies a candidate).
      ArchiveProvider *entryProvider;
      if (isCandidate) {
        if (candidatePos >= blockEnd) {
          blockBegin = candidatePos;
          blockEnd = std::min(candidates.size(),
                              blockBegin + providerLookupBlock);
          resolveArchiveProviders(
              blockEnd - blockBegin,
              [&](size_t i) {
                return archive->indexEntries[candidates[blockBegin + i]].name;
              },
              candidateProviders);
        }
        entryProvider = candidateProviders[candidatePos - blockBegin];
      } else {
        entryProvider = lookupArchiveProvider(entry.name);
      }
      // A candidate entry with no provider at all is one the fold found already
      // Defined or Common, which is what definedAlready()'s first test used to
      // answer from the table: nothing to extract here either way.
      if (!entryProvider || entryProvider->extracted)
        continue;
      if (definedAlready(archiveIndex, *entryProvider))
        continue;
      // Only an entry that extracts takes a block of walk values; one the walk
      // looked at and passed over is a gap, exactly like one it never looked at.
      // lastVisitedEntry is UINT32_MAX before the first one, so the unsigned
      // subtraction leaves a gap of exactly entryIndex values.
      // The candidate list and the scheduled queue are both merged in index
      // order, so the walk only ever moves forward; the gap arithmetic below
      // relies on it.
      assert((lastVisitedEntry == UINT32_MAX || entryIndex > lastVisitedEntry) &&
             "the walk must visit an archive's entries in index order");
      walkCounter += entryIndex - lastVisitedEntry - 1;
      uint32_t walkStart = walkCounter++;
      lastVisitedEntry = entryIndex;
      // The block goes in before the extraction, not after: a slot reservation
      // may ask for this very entry's position while the extraction is running
      // (the frontier rule admits first->entryIndex == frontierEntry), and for
      // its own entry the answer is walkStart, which is already final. walkEnd is
      // only ever read for an entry strictly below the one being asked about, so
      // leaving it provisional until the extraction returns is safe.
      visited[archiveIndex].push_back({entryIndex, walkStart, walkStart + 1});
      extract(archiveIndex, entry.childOffset, entry.name.val());
      visited[archiveIndex].back().walkEnd = walkCounter;
    }
  }

  ts.reset();
  if (stream) {
    TimeTraceScope joinScope("Join streamed member parses");
    joinStreamedMemberParses();
  } else {
    TimeTraceScope parseScope("Parse archive members");
    parseHeldFiles(firstMember);
  }
  {
    TimeTraceScope applyScope("Apply archive slot reservations");
    symtab->applyArchiveReservations(reservations);
  }
  return true;
}

static void resetParallelInputPhase() {
  archiveProviderHook = nullptr;
  weakOverrideSuppressedHook = nullptr;
  weakOverrideSuppressed.clear();
  providerShards.reset();
  parallelInput.active = false;
  parallelInput.contexts.clear();
  parallelInput.objects.clear();
  parallelInput.archives.clear();
  parallelInput.archiveOrdinals.clear();
  parallelInput.nextOrdinal = 0;
  parallelInput.heldLinkerOptions = false;
  // Already joined at the end of the phase; this is for the link that never
  // reached one.
  joinHeldParse();
  parallelInput.drainedObjects = 0;
  // Already joined at the end of the phase; the reset() is for the link that
  // never reached one, and its destructor still waits.
  parallelInput.prepass.reset();
  parallelInput.prepassGroup = nullptr;
  parallelInput.prepassStates.clear();
  // Already joined at the end of the walk; this is for the link that never
  // reached one, and the destructor still waits.
  joinStreamedMemberParses();
  prepassCancelled.store(false, std::memory_order_relaxed);
}

// After the phase, a name referenced for the first time still has to be able to
// reach an archive member the walker did not extract.
static void fetchFromArchiveProvider(StringRef name, uint32_t belowArchiveId) {
  const ArchiveProvider *provider =
      lookupArchiveProvider(llvm::CachedHashStringRef(name));
  if (!provider)
    return;
  ArchiveFile *archive = parallelInput.archives[provider->archiveIndex];
  // The caller already holds a lazy symbol from an archive with this id; only an
  // archive that precedes it on the command line would have owned the slot.
  // InputFile::id is a non-negative int; the hook takes the caller's archive id
  // as uint32_t.
  if (static_cast<uint32_t>(archive->id) >= belowArchiveId)
    return;
  if (archive->isFetched(provider->childOffset))
    return;
  // The serial linker would have left a LazyArchive symbol in this slot, which
  // shadows a dylib definition registered after the archive.
  if (Symbol *sym = symtab->find(name)) {
    if (isa<Defined>(sym) || isa<CommonSymbol>(sym))
      return;
    if (auto *dysym = dyn_cast<DylibSymbol>(sym))
      if (!dysym->isWeakDef() && (dysym->isDynamicLookup() ||
                                  dysym->getFile()->id < archive->id))
        return;
  }
  ObjFile *obj = archive->createIndexedMember(provider->childOffset, name);
  if (!obj)
    return;
  obj->parseFile();
  archive->publishIndexedMember(obj, name);
}

// Run the held parses and put the symbol table back into its serial mode.
static void finishParallelInput(bool moreInputsFollow = false) {
  if (!parallelInput.active)
    return;
  parallelInput.active = false;
  symtab->serialCtx = nullptr;

  auto &objects = parallelInput.objects;

  // Join the prepass before the held objects are parsed, not just before the
  // archives are walked or published. Both are fault-heavy parallel phases --
  // the prepass first-touches the index and every member's symbol table, the
  // held parse the objects' own pages -- and overlapping them costs more in the
  // kernel than the overlap saves: with the join after the parse, Chromium debug
  // spent about 150 ms more system time and lost 2.1% of wall while its user
  // cycles fell (`PROTO-stage2-s4-prepass-001`, and `-002` isolating the merged
  // pass). Joining here keeps the two apart structurally rather than by reading
  // a scheduling signal. Where the argument walk after the first archive is long
  // enough to absorb the prepass -- Firefox, both Rust links, the dylib and TAPI
  // wait -- this join returns at once and the overlap is still free; where it is
  // not, the phases run in the order they ran before the prepass existed.
  {
    TimeTraceScope timeScope("Join archive index prepass");
    joinArchivePrepass(/*willWalk=*/!moreInputsFollow);
  }
  {
    TimeTraceScope timeScope("Parse held object files");
    if (parallelInput.heldParse) {
      // Most of the held objects were spawned behind the argument walk; what is
      // left is the last partial chunk and the tail of the running ones.
      spawnHeldParseChunk();
      joinHeldParse();
    } else {
      parseHeldFiles(parallelInput.drainedObjects);
    }
  }
  // The dylib names deferred while the parse streamed, now that every held
  // object has folded, which is the serial order. The walk below still sees
  // every command-line dylib published, as it did when they went in inline.
  {
    TimeTraceScope timeScope("Publish command-line dylib names");
    flushAutolinkBatch();
  }

  // The same rule as for extracted members, for the direct objects: an
  // LC_LINKER_OPTION loads a library after the phase, and a name only that
  // library references has no slot if the walk skipped its index entry. The held
  // objects' options have not been merged into unprocessedLCLinkerOptions yet --
  // that happens below -- so ask their contexts directly. This is what made the
  // check on unprocessedLCLinkerOptions alone read as empty.
  if (!moreInputsFollow)
    for (ParseContext &ctx : parallelInput.contexts)
      if (!ctx.lcLinkerOptions.empty()) {
        moreInputsFollow = true;
        break;
      }

  // The walker decides extraction from the index instead of from a lazy symbol
  // per index entry, so it leaves nothing behind for the entries it does not
  // extract from. That is invisible at the end of the input but not to an input
  // that follows, so when one does, the held archives publish their indexes the
  // ordinary way instead -- with one correction, because the dylibs between and
  // after them have already been registered.
  bool walked = false;
  bool publishedSerially = false;
  if (!parallelInput.archives.empty()) {
    if (moreInputsFollow) {
      TimeTraceScope timeScope("Publish archive indexes");
      ParseContext archiveCtx;
      for (uint32_t i = 0; i != parallelInput.archives.size(); ++i) {
        archiveCtx.top = parallelInput.archiveOrdinals[i];
        archiveCtx.walk = 0;
        archiveCtx.seq = 0;
        symtab->serialCtx = &archiveCtx;
        // The mandatory members load at this archive's own position, before its
        // index, exactly as they do on the serial path.
        loadMandatoryArchiveMembers(parallelInput.archives[i],
                                    parallelInput.archives[i]->getName(),
                                    /*archiveContents=*/nullptr,
                                    /*isLazy=*/false, /*contentsOnly=*/false);
        parallelInput.archives[i]->publishHeldIndex();
      }
      symtab->serialCtx = nullptr;
      publishedSerially = true;
    } else {
      TimeTraceScope timeScope("Walk archives");
      walked = walkArchives();
    }
  }

  {
    TimeTraceScope timeScope("Compact symbol vector");
    symtab->endParallelInput();
  }
  TimeTraceScope timeScope("Apply deferred symbol effects");

  // Merge everything the workers collected back in file order.
  SmallVector<PendingCoalesce, 0> coalesces;
  SmallVector<DeferredDefinedRecord, 0> deferred;
  llvm::DenseSet<InputFile *> inlineUnwind;
  for (ParseContext &ctx : parallelInput.contexts) {
    if (!ctx.diags.empty())
      replayDiagnostics(ctx.diags);
    coalesces.append(ctx.coalesces.begin(), ctx.coalesces.end());
    deferred.append(ctx.deferredDefined.begin(), ctx.deferredDefined.end());
    unprocessedLCLinkerOptions.append(ctx.lcLinkerOptions);
    inlineUnwind.insert(ctx.inlineUnwindFiles.begin(),
                        ctx.inlineUnwindFiles.end());
  }
  symtab->applyPendingCoalesces(coalesces);

  // Replay the records the fold could not apply, and register unwind entries,
  // in one ordinal-ordered pass: a file whose weak definition a later strong one
  // displaces must still see itself as the owner when it registers, which is
  // what it did at its completion point in the serial order.
  llvm::sort(deferred, [](const DeferredDefinedRecord &a,
                          const DeferredDefinedRecord &b) {
    return a.ordinal < b.ordinal;
  });
  // In completion order, not adoption order: the walker adopts an extracted
  // member before the members its undefined symbols extract, but those complete
  // first, so a parent replayed at its own completion would displace a nested
  // member's weak definition before that member had registered as its owner --
  // which is exactly the case design 7 section 3 flags these files for.
  std::vector<std::pair<RecordOrdinal, ObjFile *>> inlineOrder;
  for (auto &[obj, ordinal] : objects)
    if (inlineUnwind.contains(obj))
      inlineOrder.emplace_back(obj->parseCtx
                                   ? obj->parseCtx->completionOrdinal()
                                   : RecordOrdinal{ordinal + 1, 0, 0},
                               obj);
  llvm::stable_sort(inlineOrder, [](const std::pair<RecordOrdinal, ObjFile *> &a,
                                    const std::pair<RecordOrdinal, ObjFile *> &b) {
    return a.first < b.first;
  });
  size_t next = 0;
  for (auto &[completion, obj] : inlineOrder) {
    while (next != deferred.size() && deferred[next].ordinal < completion)
      symtab->addDefinedRecord(deferred[next++]);
    obj->registerUnwind();
    obj->deferUnwindRegistration = false;
  }
  while (next != deferred.size())
    symtab->addDefinedRecord(deferred[next++]);

  // The rest register on workers. Every winner is final by now, so the
  // cross-file reads registration makes are stable, and each file only writes
  // its own sections and its own symbol list. Diagnostics are collected per file
  // and replayed in inputFiles order, which is where the serial linker
  // registered.
  {
    TimeTraceScope timeScope("Register unwind info");
    std::vector<ObjFile *> pending;
    for (InputFile *file : inputFiles)
      if (auto *obj = dyn_cast<ObjFile>(file))
        if (obj->deferUnwindRegistration)
          pending.push_back(obj);
    std::vector<DeferredDiagnostics> diags(pending.size());
    parallelParseActive = true;
    parallelFor(0, pending.size(), [&](size_t i) {
      deferredDiagnostics = &diags[i];
      pending[i]->registerUnwind();
      pending[i]->deferUnwindRegistration = false;
      deferredDiagnostics = nullptr;
    });
    parallelParseActive = false;
    for (DeferredDiagnostics &d : diags)
      if (!d.empty())
        replayDiagnostics(d);
  }

  for (auto &[obj, ordinal] : objects)
    obj->parseCtx = nullptr;

  objects.clear();
  objects.shrink_to_fit();

  if (walked) {
    archiveProviderHook = fetchFromArchiveProvider;
  } else if (publishedSerially) {
    parallelInput.archives.clear();
    parallelInput.archiveOrdinals.clear();
  } else {
    parallelInput.archiveOrdinals.clear();
    // The walker could not run, so the archives publish their indexes the
    // ordinary way. Every direct object is already resolved, and every archive
    // follows every object, so this is the serial order.
    providerShards.reset();
    for (ArchiveFile *archive : parallelInput.archives) {
      loadMandatoryArchiveMembers(archive, archive->getName(),
                                  /*archiveContents=*/nullptr,
                                  /*isLazy=*/false, /*contentsOnly=*/false);
      archive->addLazySymbols();
    }
    parallelInput.archives.clear();
  }
  // parallelInput.contexts is deliberately not cleared: deferFile() holds a
  // pointer into it across the call that ends the phase.
}

// Decide whether this input's parse can be held back, and give it the ordinal
// and context the phase needs. Anything that would observe a partially resolved
// symbol table runs the held parses first.
static bool beginHeldInput(MemoryBufferRef mbref, bool isLazy,
                           LoadType loadType, ParseContext *ctx,
                           bool &holdArchiveIndex) {
  bool holdParse = false;
  holdArchiveIndex = false;
  if (parallelInput.active) {
    switch (identify_magic(mbref.getBuffer())) {
    case file_magic::macho_object:
      // An object after an archive can define a name the archive would
      // otherwise have been extracted for, so the archives that precede it are
      // walked first and this object is resolved in the ordinary serial way.
      if (isLazy || !parallelInput.archives.empty()) {
        finishParallelInput(/*moreInputsFollow=*/true);
      } else {
        holdParse = true;
        // An autolinked library loads after the phase, so no archive held after
        // this object can be walked. Reading it here instead of waiting for the
        // held parse keeps the whole archive path on the serial position it had
        // before the phase existed, including its overlap with the TAPI
        // pipeline, rather than deferring it to the end of the phase.
        if (!parallelInput.heldLinkerOptions && hasLinkerOptions(mbref))
          parallelInput.heldLinkerOptions = true;
      }
      break;
    case file_magic::archive:
      // An archive's mandatory members (-ObjC, -force_load, -all_load) load at
      // its own position, before its index; the walker replays those loads as a
      // per-archive prelude, so the index can still be held back. An archive
      // loaded from an LC_LINKER_OPTION arrives after the phase.
      //
      // An input that follows the phase -- an autolinked library named by a held
      // or an already-parsed object -- means the walk would decline later anyway,
      // and declining now is strictly better: the archive publishes its index at
      // its own command-line position on the ordinary serial path.
      holdArchiveIndex = (loadType == LoadType::CommandLine ||
                          loadType == LoadType::CommandLineForce) &&
                         !parallelInput.heldLinkerOptions &&
                         unprocessedLCLinkerOptions.empty();
      if (!holdArchiveIndex)
        finishParallelInput(/*moreInputsFollow=*/true);
      break;
    case file_magic::bitcode:
      finishParallelInput(/*moreInputsFollow=*/true);
      break;
    default:
      break;
    }
  }
  if (parallelInput.active)
    symtab->serialCtx = ctx;
  deferDirectObjParse = holdParse;
  deferArchiveIndex = holdArchiveIndex;
  return holdParse;
}

static void endHeldInput(InputFile *file, bool holdParse, bool holdArchiveIndex,
                         ParseContext *ctx) {
  deferDirectObjParse = false;
  deferArchiveIndex = false;
  symtab->serialCtx = nullptr;
  if (holdParse) {
    auto *obj = cast<ObjFile>(file);
    obj->parseCtx = ctx;
    obj->deferUnwindRegistration = true;
    parallelInput.objects.emplace_back(obj, ctx->top);
    if (parallelInput.heldParse &&
        parallelInput.objects.size() - parallelInput.streamedObjects >=
            heldParseChunk())
      spawnHeldParseChunk();
    return;
  }
  if (!holdArchiveIndex)
    return;
  // processFile() returns the cached ArchiveFile for a repeated path, whose
  // index the walker already owns.
  auto *archive = dyn_cast_or_null<ArchiveFile>(file);
  if (!archive || llvm::is_contained(parallelInput.archives, archive))
    return;
  if (archive->canWalk()) {
    parallelInput.archives.push_back(archive);
    parallelInput.archiveOrdinals.push_back(ctx->top);
    // Everything the walk needs from the archive's bytes starts now, on workers,
    // so that it overlaps the rest of the argument walk instead of running in
    // front of the walk's first serial decision.
    startArchivePrepass(archive);
    return;
  }
  // This archive has to publish its index right here, which means every object
  // before it must already be resolved. Its mandatory members load just before
  // the index, which is what processFile() left to this point.
  finishParallelInput(/*moreInputsFollow=*/true);
  loadMandatoryArchiveMembers(archive, archive->getName(),
                              /*archiveContents=*/nullptr, /*isLazy=*/false,
                              /*contentsOnly=*/false);
  archive->addLazySymbols();
}

static void deferFile(StringRef path, bool isLazy, DeferredFiles &deferred,
                      LoadType loadType = LoadType::CommandLine,
                      bool isNeeded = false, bool isWeak = false,
                      bool isReexport = false, bool isHidden = false,
                      bool isExplicit = true, bool forceDefer = false) {
  std::optional<MemoryBufferRef> buffer = readFile(path);
  if (!buffer)
    return;
  MemoryBufferRef mbref = *buffer;
  if (config->readWorkers || forceDefer) {
    deferred.push_back({path, isLazy, mbref, loadType, isNeeded, isWeak,
                        isReexport, isHidden, isExplicit});
    return;
  }
  if (loadedObjectFrameworks.contains(path))
    return;

  ParseContext *ctx = nullptr;
  if (parallelInput.active) {
    parallelInput.contexts.emplace_back();
    ctx = &parallelInput.contexts.back();
    ctx->top = parallelInput.nextOrdinal++;
  }
  bool holdArchiveIndex = false;
  bool holdParse =
      beginHeldInput(mbref, isLazy, loadType, ctx, holdArchiveIndex);
  InputFile *file =
      processFile(buffer, nullptr, path, loadType, isLazy, isExplicit,
                  /*isBundleLoader=*/false, isHidden);
  endHeldInput(file, holdParse, holdArchiveIndex, ctx);
  applyDylibMetadata(file, isNeeded, isWeak, isReexport);
  checkAndCacheFramework(file, path);
}

static void prepareTapiFiles(const DeferredFiles &deferred,
                             std::vector<PreparedTapiFile> &prepared) {
  DenseSet<StringRef> paths;
  SmallVector<MemoryBufferRef> buffers;
  for (const DeferredFile &file : deferred) {
    if (identify_magic(file.buffer.getBuffer()) != file_magic::tapi_file ||
        !paths.insert(file.buffer.getBufferIdentifier()).second)
      continue;
    buffers.push_back(file.buffer);
  }
  macho::prepareTapiFiles(buffers, prepared);

  SmallVector<PreparedTapiFile *> filesToConsume;
  for (PreparedTapiFile &file : prepared)
    filesToConsume.push_back(&file);
  setPreparedTapiFiles(filesToConsume);
}

static std::vector<StringRef> missingAutolinkWarnings;
static void addLibrary(StringRef name, bool isNeeded, bool isWeak,
                       bool isReexport, bool isHidden, bool isExplicit,
                       LoadType loadType, DeferredFiles &deferred,
                       bool forceDefer = false) {
  if (std::optional<StringRef> path = findLibrary(name)) {
    deferFile(*path, /*isLazy=*/false, deferred, loadType, isNeeded, isWeak,
              isReexport, isHidden, isExplicit, forceDefer);
    return;
  }
  if (loadType == LoadType::LCLinkerOption) {
    missingAutolinkWarnings.push_back(
        saver().save("auto-linked library not found for -l" + name));
    return;
  }
  error("library not found for -l" + name);
}

static void addFramework(StringRef name, bool isNeeded, bool isWeak,
                         bool isReexport, bool isExplicit, LoadType loadType,
                         DeferredFiles &deferred, bool forceDefer = false) {
  if (std::optional<StringRef> path = findFramework(name)) {
    if (loadedObjectFrameworks.contains(*path))
      return;

    deferFile(*path, /*isLazy=*/false, deferred, loadType, isNeeded, isWeak,
              isReexport, /*isHidden=*/false, isExplicit, forceDefer);
    return;
  }
  if (loadType == LoadType::LCLinkerOption) {
    missingAutolinkWarnings.push_back(
        saver().save("auto-linked framework not found for -framework " + name));
    return;
  }
  error("framework not found for -framework " + name);
}

// The LC_LINKER_OPTION names collected so far. An object-heavy link names the
// same few dozen libraries and frameworks tens of thousands of times, and
// resolveLCLinkerOptions() sorts and uniques whatever it is given, so one name
// only has to be collected once. Held objects are parsed on workers, so
// insertion is lock guarded; the lock is taken once per option.
static std::mutex autolinkNamesMutex;
static DenseSet<CachedHashStringRef> autolinkLibraryNames;
static DenseSet<CachedHashStringRef> autolinkFrameworkNames;

// Parses LC_LINKER_OPTION contents, which can add additional command line
// flags. This directly parses the flags instead of using the standard argument
// parser to improve performance.
void macho::parseLCLinkerOption(
    llvm::SmallVectorImpl<StringRef> &LCLinkerOptions, InputFile *f,
    unsigned argc, StringRef data) {
  if (config->ignoreAutoLink)
    return;

  SmallVector<StringRef, 4> argv;
  size_t offset = 0;
  for (unsigned i = 0; i < argc && offset < data.size(); ++i) {
    argv.push_back(data.data() + offset);
    offset += strlen(data.data() + offset) + 1;
  }
  if (argv.size() != argc || offset > data.size())
    fatal(toString(f) + ": invalid LC_LINKER_OPTION");

  unsigned i = 0;
  StringRef arg = argv[i];
  StringRef name;
  bool isFramework = false;
  if (arg.consume_front("-l")) {
    if (config->ignoreAutoLinkOptions.contains(arg))
      return;
    name = arg;
  } else if (arg == "-framework") {
    name = argv[++i];
    if (config->ignoreAutoLinkOptions.contains(name))
      return;
    isFramework = true;
  } else {
    error(arg + " is not allowed in LC_LINKER_OPTION");
    // A malformed option is left entirely alone: it is forwarded unchanged so
    // that every occurrence still reaches resolveLCLinkerOptions() as before,
    // and no empty name is ever entered into the sets or probed on disk.
    LCLinkerOptions.append(argv);
    return;
  }

  // Collect each name once.
  if (argv.size() == (isFramework ? 2u : 1u)) {
    bool isNew;
    {
      std::lock_guard<std::mutex> lock(autolinkNamesMutex);
      isNew = (isFramework ? autolinkFrameworkNames : autolinkLibraryNames)
                  .insert(CachedHashStringRef(name))
                  .second;
    }
    if (!isNew)
      return;
    // First sighting: start this name's TBD parse now, from this worker, so
    // that it overlaps the rest of the object walk instead of running inside
    // resolveLCLinkerOptions().
    pipelineAutolinkName(name, isFramework);
  }

  LCLinkerOptions.append(argv);
}

bool macho::inAutolinkRound = false;

void macho::resolveLCLinkerOptions() {
  inAutolinkRound = true;
  llvm::scope_exit roundsDone([] { inAutolinkRound = false; });
  while (!unprocessedLCLinkerOptions.empty()) {
    SmallVector<StringRef> LCLinkerOptions(unprocessedLCLinkerOptions);
    unprocessedLCLinkerOptions.clear();

    DeferredFiles deferred;
    SmallVector<StringRef> frameworks;
    SmallVector<StringRef> libraries;

    for (unsigned i = 0; i < LCLinkerOptions.size(); ++i) {
      StringRef arg = LCLinkerOptions[i];
      if (arg.consume_front("-l")) {
        assert(!config->ignoreAutoLinkOptions.contains(arg));
        libraries.push_back(arg);
      } else if (arg == "-framework") {
        StringRef name = LCLinkerOptions[++i];
        assert(!config->ignoreAutoLinkOptions.contains(name));
        frameworks.push_back(name);
      } else {
        error(arg + " is not allowed in LC_LINKER_OPTION");
      }
    }

    llvm::sort(frameworks);
    llvm::sort(libraries);

    frameworks.erase(std::unique(frameworks.begin(), frameworks.end()),
                     frameworks.end());
    libraries.erase(std::unique(libraries.begin(), libraries.end()),
                    libraries.end());

    for (const StringRef framework : frameworks) {
      addFramework(framework, /*isNeeded=*/false, /*isWeak=*/false,
                   /*isReexport=*/false, /*isExplicit=*/false,
                   LoadType::LCLinkerOption, deferred, /*forceDefer=*/true);
    }

    for (const StringRef library : libraries) {
      addLibrary(library, /*isNeeded=*/false, /*isWeak=*/false,
                 /*isReexport=*/false, /*isHidden=*/false,
                 /*isExplicit=*/false, LoadType::LCLinkerOption, deferred,
                 /*forceDefer=*/true);
    }

    std::vector<PreparedTapiFile> preparedTapiFiles;
    prepareTapiFiles(deferred, preparedTapiFiles);
    for (auto &file : deferred) {
      if (loadedObjectFrameworks.contains(file.path))
        continue;

      // A dylib run's names may stay batched across other dylibs of the run,
      // because nothing between two of them reads the table. Anything else --
      // the libswiftCompatibility archives here -- resolves against the state
      // the serial linker had at its position, so the run ends first.
      if (identify_magic(file.buffer.getBuffer()) != file_magic::tapi_file)
        flushAutolinkBatch();

      auto inputFile = processFile(file.buffer, nullptr, file.path,
                                   file.loadType, file.isLazy, file.isExplicit,
                                   /*isBundleLoader=*/false, file.isHidden);
      applyDylibMetadata(inputFile, file.isNeeded, file.isWeak,
                         file.isReexport);
      checkAndCacheFramework(inputFile, file.path);
    }
    flushAutolinkBatch();
    clearPreparedTapiFiles();
  }
  flushAutolinkBatch();
}

static void addFileList(StringRef path, bool isLazy,
                        DeferredFiles &deferredFiles) {
  std::optional<MemoryBufferRef> buffer = readFile(path);
  if (!buffer)
    return;
  MemoryBufferRef mbref = *buffer;
  for (StringRef path : args::getLines(mbref))
    deferFile(rerootPath(path), isLazy, deferredFiles);
}

// We expect sub-library names of the form "libfoo", which will match a dylib
// with a path of .*/libfoo.{dylib, tbd}.
// XXX ld64 seems to ignore the extension entirely when matching sub-libraries;
// I'm not sure what the use case for that is.
static bool markReexport(StringRef searchName, ArrayRef<StringRef> extensions) {
  for (InputFile *file : inputFiles) {
    if (auto *dylibFile = dyn_cast<DylibFile>(file)) {
      StringRef filename = path::filename(dylibFile->getName());
      if (filename.consume_front(searchName) &&
          (filename.empty() || llvm::is_contained(extensions, filename))) {
        dylibFile->reexport = true;
        return true;
      }
    }
  }
  return false;
}

// This function is called on startup. We need this for LTO since
// LTO calls LLVM functions to compile bitcode files to native code.
// Technically this can be delayed until we read bitcode files, but
// we don't bother to do lazily because the initialization is fast.
static void initLLVM() {
  InitializeAllTargets();
  InitializeAllTargetMCs();
  InitializeAllAsmPrinters();
  InitializeAllAsmParsers();
}

static bool compileBitcodeFiles() {
  TimeTraceScope timeScope("LTO");
  auto *lto = make<BitcodeCompiler>();
  for (InputFile *file : inputFiles)
    if (auto *bitcodeFile = dyn_cast<BitcodeFile>(file))
      if (!file->lazy)
        lto->add(*bitcodeFile);

  std::vector<ObjFile *> compiled = lto->compile();
  inputFiles.insert_range(compiled);

  return !compiled.empty();
}

// Replaces common symbols with defined symbols residing in __common sections.
// This function must be called after all symbol names are resolved (i.e. after
// all InputFiles have been loaded.) As a result, later operations won't see
// any CommonSymbols.
static void replaceCommonSymbols() {
  TimeTraceScope timeScope("Replace common symbols");
  ConcatOutputSection *osec = nullptr;
  for (Symbol *sym : symtab->getSymbols()) {
    auto *common = dyn_cast<CommonSymbol>(sym);
    if (common == nullptr)
      continue;

    // Casting to size_t will truncate large values on 32-bit architectures,
    // but it's not really worth supporting the linking of 64-bit programs on
    // 32-bit archs.
    ArrayRef<uint8_t> data = {nullptr, static_cast<size_t>(common->size)};
    // FIXME avoid creating one Section per symbol?
    auto *section =
        make<Section>(common->getFile(), segment_names::data,
                      section_names::common, S_ZEROFILL, /*addr=*/0);
    auto *isec = make<ConcatInputSection>(*section, data, common->align);
    if (!osec)
      osec = ConcatOutputSection::getOrCreateForInput(isec);
    isec->parent = osec;
    addInputSection(isec);

    // FIXME: CommonSymbol should store isReferencedDynamically, noDeadStrip
    // and pass them on here.
    replaceSymbol<Defined>(
        sym, sym->getName(), common->getFile(), isec, /*value=*/0, common->size,
        /*isWeakDef=*/false, /*isExternal=*/true, common->privateExtern,
        /*includeInSymtab=*/true, /*isReferencedDynamically=*/false,
        /*noDeadStrip=*/false);
  }
}

static void initializeSectionRenameMap() {
  if (config->dataConst) {
    SmallVector<StringRef> v{section_names::got,
                             section_names::authGot,
                             section_names::authPtr,
                             section_names::nonLazySymbolPtr,
                             section_names::const_,
                             section_names::cfString,
                             section_names::moduleInitFunc,
                             section_names::moduleTermFunc,
                             section_names::objcClassList,
                             section_names::objcNonLazyClassList,
                             section_names::objcCatList,
                             section_names::objcNonLazyCatList,
                             section_names::objcProtoList,
                             section_names::objCImageInfo};
    for (StringRef s : v)
      config->sectionRenameMap[{segment_names::data, s}] = {
          segment_names::dataConst, s};
  }
  config->sectionRenameMap[{segment_names::text, section_names::staticInit}] = {
      segment_names::text, section_names::text};
  config->sectionRenameMap[{segment_names::import, section_names::pointers}] = {
      config->dataConst ? segment_names::dataConst : segment_names::data,
      section_names::nonLazySymbolPtr};
}

static inline char toLowerDash(char x) {
  if (x >= 'A' && x <= 'Z')
    return x - 'A' + 'a';
  else if (x == ' ')
    return '-';
  return x;
}

static std::string lowerDash(StringRef s) {
  return std::string(map_iterator(s.begin(), toLowerDash),
                     map_iterator(s.end(), toLowerDash));
}

struct PlatformVersion {
  PlatformType platform = PLATFORM_UNKNOWN;
  llvm::VersionTuple minimum;
  llvm::VersionTuple sdk;
};

static PlatformVersion parsePlatformVersion(const Arg *arg) {
  assert(arg->getOption().getID() == OPT_platform_version);
  StringRef platformStr = arg->getValue(0);
  StringRef minVersionStr = arg->getValue(1);
  StringRef sdkVersionStr = arg->getValue(2);

  PlatformVersion platformVersion;

  // TODO(compnerd) see if we can generate this case list via XMACROS
  platformVersion.platform =
      StringSwitch<PlatformType>(lowerDash(platformStr))
          .Cases({"macos", "1"}, PLATFORM_MACOS)
          .Cases({"ios", "2"}, PLATFORM_IOS)
          .Cases({"tvos", "3"}, PLATFORM_TVOS)
          .Cases({"watchos", "4"}, PLATFORM_WATCHOS)
          .Cases({"bridgeos", "5"}, PLATFORM_BRIDGEOS)
          .Cases({"mac-catalyst", "6"}, PLATFORM_MACCATALYST)
          .Cases({"ios-simulator", "7"}, PLATFORM_IOSSIMULATOR)
          .Cases({"tvos-simulator", "8"}, PLATFORM_TVOSSIMULATOR)
          .Cases({"watchos-simulator", "9"}, PLATFORM_WATCHOSSIMULATOR)
          .Cases({"driverkit", "10"}, PLATFORM_DRIVERKIT)
          .Cases({"xros", "11"}, PLATFORM_XROS)
          .Cases({"xros-simulator", "12"}, PLATFORM_XROS_SIMULATOR)
          .Default(PLATFORM_UNKNOWN);
  if (platformVersion.platform == PLATFORM_UNKNOWN)
    error(Twine("malformed platform: ") + platformStr);
  // The underlying load command only supports 3 components.
  if (platformVersion.minimum.tryParse(minVersionStr) ||
      platformVersion.minimum.getBuild())
    error(Twine("malformed minimum version: ") + minVersionStr);
  if (platformVersion.sdk.tryParse(sdkVersionStr) ||
      platformVersion.sdk.getBuild())
    error(Twine("malformed sdk version: ") + sdkVersionStr);
  return platformVersion;
}

// Has the side-effect of setting Config::platformInfo and
// potentially Config::secondaryPlatformInfo.
static void setPlatformVersions(StringRef archName, const ArgList &args) {
  std::map<PlatformType, PlatformVersion> platformVersions;
  const PlatformVersion *lastVersionInfo = nullptr;
  for (const Arg *arg : args.filtered(OPT_platform_version)) {
    PlatformVersion version = parsePlatformVersion(arg);

    // For each platform, the last flag wins:
    // `-platform_version macos 2 3 -platform_version macos 4 5` has the same
    // effect as just passing `-platform_version macos 4 5`.
    // FIXME: ld64 warns on multiple flags for one platform. Should we?
    platformVersions[version.platform] = version;
    lastVersionInfo = &platformVersions[version.platform];
  }

  if (platformVersions.empty()) {
    error("must specify -platform_version");
    return;
  }
  if (platformVersions.size() > 2) {
    error("must specify -platform_version at most twice");
    return;
  }
  if (platformVersions.size() == 2) {
    bool isZipperedCatalyst = platformVersions.count(PLATFORM_MACOS) &&
                              platformVersions.count(PLATFORM_MACCATALYST);

    if (!isZipperedCatalyst) {
      error("lld supports writing zippered outputs only for "
            "macos and mac-catalyst");
    } else if (config->outputType != MH_DYLIB &&
               config->outputType != MH_BUNDLE) {
      error("writing zippered outputs only valid for -dylib and -bundle");
    }

    config->platformInfo = {
        MachO::Target(getArchitectureFromName(archName), PLATFORM_MACOS,
                      platformVersions[PLATFORM_MACOS].minimum),
        platformVersions[PLATFORM_MACOS].sdk};
    config->secondaryPlatformInfo = {
        MachO::Target(getArchitectureFromName(archName), PLATFORM_MACCATALYST,
                      platformVersions[PLATFORM_MACCATALYST].minimum),
        platformVersions[PLATFORM_MACCATALYST].sdk};
    return;
  }

  config->platformInfo = {MachO::Target(getArchitectureFromName(archName),
                                        lastVersionInfo->platform,
                                        lastVersionInfo->minimum),
                          lastVersionInfo->sdk};
}

// Has the side-effect of setting Config::target.
static TargetInfo *createTargetInfo(InputArgList &args) {
  StringRef archName = args.getLastArgValue(OPT_arch);
  if (archName.empty()) {
    error("must specify -arch");
    return nullptr;
  }

  setPlatformVersions(archName, args);
  auto [cpuType, cpuSubtype] = getCPUTypeFromArchitecture(config->arch());
  switch (cpuType) {
  case CPU_TYPE_X86_64:
    return createX86_64TargetInfo();
  case CPU_TYPE_ARM64:
    return createARM64TargetInfo();
  case CPU_TYPE_ARM64_32:
    return createARM64_32TargetInfo();
  default:
    error("missing or unsupported -arch " + archName);
    return nullptr;
  }
}

static UndefinedSymbolTreatment
getUndefinedSymbolTreatment(const ArgList &args) {
  StringRef treatmentStr = args.getLastArgValue(OPT_undefined);
  auto treatment =
      StringSwitch<UndefinedSymbolTreatment>(treatmentStr)
          .Cases({"error", ""}, UndefinedSymbolTreatment::error)
          .Case("warning", UndefinedSymbolTreatment::warning)
          .Case("suppress", UndefinedSymbolTreatment::suppress)
          .Case("dynamic_lookup", UndefinedSymbolTreatment::dynamic_lookup)
          .Default(UndefinedSymbolTreatment::unknown);
  if (treatment == UndefinedSymbolTreatment::unknown) {
    warn(Twine("unknown -undefined TREATMENT '") + treatmentStr +
         "', defaulting to 'error'");
    treatment = UndefinedSymbolTreatment::error;
  } else if (config->namespaceKind == NamespaceKind::twolevel &&
             (treatment == UndefinedSymbolTreatment::warning ||
              treatment == UndefinedSymbolTreatment::suppress)) {
    if (treatment == UndefinedSymbolTreatment::warning)
      fatal("'-undefined warning' only valid with '-flat_namespace'");
    else
      fatal("'-undefined suppress' only valid with '-flat_namespace'");
    treatment = UndefinedSymbolTreatment::error;
  }
  return treatment;
}

static ICFLevel getICFLevel(const ArgList &args) {
  StringRef icfLevelStr = args.getLastArgValue(OPT_icf_eq);
  auto icfLevel = StringSwitch<ICFLevel>(icfLevelStr)
                      .Cases({"none", ""}, ICFLevel::none)
                      .Case("safe", ICFLevel::safe)
                      .Case("safe_thunks", ICFLevel::safe_thunks)
                      .Case("all", ICFLevel::all)
                      .Default(ICFLevel::unknown);

  if ((icfLevel == ICFLevel::safe_thunks) && (config->arch() != AK_arm64)) {
    error("--icf=safe_thunks is only supported on arm64 targets");
  }

  if (icfLevel == ICFLevel::unknown) {
    warn(Twine("unknown --icf=OPTION `") + icfLevelStr +
         "', defaulting to `none'");
    icfLevel = ICFLevel::none;
  }
  return icfLevel;
}

static ObjCStubsMode getObjCStubsMode(const ArgList &args) {
  const Arg *arg = args.getLastArg(OPT_objc_stubs_fast, OPT_objc_stubs_small);
  if (!arg)
    return ObjCStubsMode::fast;

  if (arg->getOption().getID() == OPT_objc_stubs_small) {
    if (is_contained({AK_arm64e, AK_arm64}, config->arch()))
      return ObjCStubsMode::small;
    else
      warn("-objc_stubs_small is not yet implemented, defaulting to "
           "-objc_stubs_fast");
  }
  return ObjCStubsMode::fast;
}

static void warnIfDeprecatedOption(const Option &opt) {
  if (!opt.getGroup().isValid())
    return;
  if (opt.getGroup().getID() == OPT_grp_deprecated) {
    warn("Option `" + opt.getPrefixedName() + "' is deprecated in ld64:");
    warn(opt.getHelpText());
  }
}

static void warnIfUnimplementedOption(const Option &opt) {
  if (!opt.getGroup().isValid() || !opt.hasFlag(DriverFlag::HelpHidden))
    return;
  switch (opt.getGroup().getID()) {
  case OPT_grp_deprecated:
    // warn about deprecated options elsewhere
    break;
  case OPT_grp_undocumented:
    warn("Option `" + opt.getPrefixedName() +
         "' is undocumented. Should lld implement it?");
    break;
  case OPT_grp_obsolete:
    warn("Option `" + opt.getPrefixedName() +
         "' is obsolete. Please modernize your usage.");
    break;
  case OPT_grp_ignored:
    warn("Option `" + opt.getPrefixedName() + "' is ignored.");
    break;
  case OPT_grp_ignored_silently:
    break;
  default:
    warn("Option `" + opt.getPrefixedName() +
         "' is not yet implemented. Stay tuned...");
    break;
  }
}

static const char *getReproduceOption(InputArgList &args) {
  if (const Arg *arg = args.getLastArg(OPT_reproduce))
    return arg->getValue();
  return getenv("LLD_REPRODUCE");
}

// Parse options of the form "old;new".
static std::pair<StringRef, StringRef> getOldNewOptions(opt::InputArgList &args,
                                                        unsigned id) {
  auto *arg = args.getLastArg(id);
  if (!arg)
    return {"", ""};

  StringRef s = arg->getValue();
  std::pair<StringRef, StringRef> ret = s.split(';');
  if (ret.second.empty())
    error(arg->getSpelling() + " expects 'old;new' format, but got " + s);
  return ret;
}

// Parse options of the form "old;new[;extra]".
static std::tuple<StringRef, StringRef, StringRef>
getOldNewOptionsExtra(opt::InputArgList &args, unsigned id) {
  auto [oldDir, second] = getOldNewOptions(args, id);
  auto [newDir, extraDir] = second.split(';');
  return {oldDir, newDir, extraDir};
}

static void parseClangOption(StringRef opt, const Twine &msg) {
  std::string err;
  raw_string_ostream os(err);

  const char *argv[] = {"lld", opt.data()};
  if (cl::ParseCommandLineOptions(2, argv, "", &os))
    return;
  error(msg + ": " + StringRef(err).trim());
}

static uint32_t parseDylibVersion(const ArgList &args, unsigned id) {
  const Arg *arg = args.getLastArg(id);
  if (!arg)
    return 0;

  if (config->outputType != MH_DYLIB) {
    error(arg->getAsString(args) + ": only valid with -dylib");
    return 0;
  }

  PackedVersion version;
  if (!version.parse32(arg->getValue())) {
    error(arg->getAsString(args) + ": malformed version");
    return 0;
  }

  return version.rawValue();
}

static uint32_t parseProtection(StringRef protStr) {
  uint32_t prot = 0;
  for (char c : protStr) {
    switch (c) {
    case 'r':
      prot |= VM_PROT_READ;
      break;
    case 'w':
      prot |= VM_PROT_WRITE;
      break;
    case 'x':
      prot |= VM_PROT_EXECUTE;
      break;
    case '-':
      break;
    default:
      error("unknown -segprot letter '" + Twine(c) + "' in " + protStr);
      return 0;
    }
  }
  return prot;
}

static std::vector<SectionAlign> parseSectAlign(const opt::InputArgList &args) {
  std::vector<SectionAlign> sectAligns;
  for (const Arg *arg : args.filtered(OPT_sectalign)) {
    StringRef segName = arg->getValue(0);
    StringRef sectName = arg->getValue(1);
    StringRef alignStr = arg->getValue(2);
    alignStr.consume_front_insensitive("0x");
    uint32_t align;
    if (alignStr.getAsInteger(16, align)) {
      error("-sectalign: failed to parse '" + StringRef(arg->getValue(2)) +
            "' as number");
      continue;
    }
    if (!isPowerOf2_32(align)) {
      error("-sectalign: '" + StringRef(arg->getValue(2)) +
            "' (in base 16) not a power of two");
      continue;
    }
    sectAligns.push_back({segName, sectName, align});
  }
  return sectAligns;
}

PlatformType macho::removeSimulator(PlatformType platform) {
  switch (platform) {
  case PLATFORM_IOSSIMULATOR:
    return PLATFORM_IOS;
  case PLATFORM_TVOSSIMULATOR:
    return PLATFORM_TVOS;
  case PLATFORM_WATCHOSSIMULATOR:
    return PLATFORM_WATCHOS;
  case PLATFORM_XROS_SIMULATOR:
    return PLATFORM_XROS;
  default:
    return platform;
  }
}

static bool supportsNoPie() {
  return !(config->arch() == AK_arm64 || config->arch() == AK_arm64e ||
           config->arch() == AK_arm64_32);
}

static bool shouldAdhocSignByDefault(Architecture arch, PlatformType platform) {
  if (arch != AK_arm64 && arch != AK_arm64e)
    return false;

  return platform == PLATFORM_MACOS || platform == PLATFORM_IOSSIMULATOR ||
         platform == PLATFORM_TVOSSIMULATOR ||
         platform == PLATFORM_WATCHOSSIMULATOR ||
         platform == PLATFORM_XROS_SIMULATOR;
}

template <std::size_t N>
using MinVersions = std::array<std::pair<PlatformType, VersionTuple>, N>;

/// Returns true if the platform is greater than the min version.
/// Returns false if the platform does not exist.
template <std::size_t N>
static bool greaterEqMinVersion(const MinVersions<N> &minVersions,
                                bool ignoreSimulator) {
  PlatformType platform = config->platformInfo.target.Platform;
  if (ignoreSimulator)
    platform = removeSimulator(platform);
  auto it = llvm::find_if(minVersions,
                          [&](const auto &p) { return p.first == platform; });
  if (it != minVersions.end())
    if (config->platformInfo.target.MinDeployment >= it->second)
      return true;
  return false;
}

static bool dataConstDefault(const InputArgList &args) {
  static const MinVersions<6> minVersion = {{
      {PLATFORM_MACOS, VersionTuple(10, 15)},
      {PLATFORM_IOS, VersionTuple(13, 0)},
      {PLATFORM_TVOS, VersionTuple(13, 0)},
      {PLATFORM_WATCHOS, VersionTuple(6, 0)},
      {PLATFORM_XROS, VersionTuple(1, 0)},
      {PLATFORM_BRIDGEOS, VersionTuple(4, 0)},
  }};
  if (!greaterEqMinVersion(minVersion, true))
    return false;

  switch (config->outputType) {
  case MH_EXECUTE:
    return !(args.hasArg(OPT_no_pie) && supportsNoPie());
  case MH_BUNDLE:
    // FIXME: return false when -final_name ...
    // has prefix "/System/Library/UserEventPlugins/"
    // or matches "/usr/libexec/locationd" "/usr/libexec/terminusd"
    return true;
  case MH_DYLIB:
    return true;
  case MH_OBJECT:
    return false;
  default:
    llvm_unreachable(
        "unsupported output type for determining data-const default");
  }
  return false;
}

static bool shouldEmitChainedFixups(const InputArgList &args) {
  const Arg *arg = args.getLastArg(OPT_fixup_chains, OPT_no_fixup_chains);
  if (arg && arg->getOption().matches(OPT_no_fixup_chains))
    return false;

  bool requested = arg && arg->getOption().matches(OPT_fixup_chains);
  if (!config->isPic) {
    if (requested)
      error("-fixup_chains is incompatible with -no_pie");

    return false;
  }

  if (!is_contained({AK_x86_64, AK_x86_64h, AK_arm64}, config->arch())) {
    if (requested)
      error("-fixup_chains is only supported on x86_64 and arm64 targets");

    return false;
  }

  if (args.hasArg(OPT_preload)) {
    if (requested)
      error("-fixup_chains is incompatible with -preload");

    return false;
  }

  if (requested)
    return true;

  static const MinVersions<9> minVersion = {{
      {PLATFORM_IOS, VersionTuple(13, 4)},
      {PLATFORM_IOSSIMULATOR, VersionTuple(16, 0)},
      {PLATFORM_MACOS, VersionTuple(13, 0)},
      {PLATFORM_TVOS, VersionTuple(14, 0)},
      {PLATFORM_TVOSSIMULATOR, VersionTuple(15, 0)},
      {PLATFORM_WATCHOS, VersionTuple(7, 0)},
      {PLATFORM_WATCHOSSIMULATOR, VersionTuple(8, 0)},
      {PLATFORM_XROS, VersionTuple(1, 0)},
      {PLATFORM_XROS_SIMULATOR, VersionTuple(1, 0)},
  }};
  return greaterEqMinVersion(minVersion, false);
}

static bool shouldEmitRelativeMethodLists(const InputArgList &args) {
  const Arg *arg = args.getLastArg(OPT_objc_relative_method_lists,
                                   OPT_no_objc_relative_method_lists);
  if (arg && arg->getOption().getID() == OPT_objc_relative_method_lists)
    return true;
  if (arg && arg->getOption().getID() == OPT_no_objc_relative_method_lists)
    return false;

  // If no flag is specified, enable this on newer versions by default.
  // The min versions is taken from
  // ld64(https://github.com/apple-oss-distributions/ld64/blob/47f477cb721755419018f7530038b272e9d0cdea/src/ld/ld.hpp#L310)
  // to mimic to operation of ld64
  // [here](https://github.com/apple-oss-distributions/ld64/blob/47f477cb721755419018f7530038b272e9d0cdea/src/ld/Options.cpp#L6085-L6101)
  static const MinVersions<6> minVersion = {{
      {PLATFORM_MACOS, VersionTuple(10, 16)},
      {PLATFORM_IOS, VersionTuple(14, 0)},
      {PLATFORM_WATCHOS, VersionTuple(7, 0)},
      {PLATFORM_TVOS, VersionTuple(14, 0)},
      {PLATFORM_BRIDGEOS, VersionTuple(5, 0)},
      {PLATFORM_XROS, VersionTuple(1, 0)},
  }};
  return greaterEqMinVersion(minVersion, true);
}

void SymbolPatterns::clear() {
  literals.clear();
  globs.clear();
}

void SymbolPatterns::insert(StringRef symbolName) {
  Expected<GlobPattern> pattern = GlobPattern::create(symbolName);
  if (!pattern) {
    error("invalid symbol-name pattern: " + symbolName + ": " +
          toString(pattern.takeError()));
    return;
  }
  // A pattern that denotes a single string is kept as a literal: literals are
  // matched by hash lookup, and only literals seed the force-load of lazy
  // archive members below.
  if (std::optional<std::string> literal = pattern->asLiteral()) {
    literals.insert(CachedHashStringRef(saver().save(*literal)));
    return;
  }
  globs.emplace_back(std::move(*pattern));
}

bool SymbolPatterns::matchLiteral(StringRef symbolName) const {
  return literals.contains(CachedHashStringRef(symbolName));
}

bool SymbolPatterns::matchGlob(StringRef symbolName) const {
  for (const GlobPattern &glob : globs)
    if (glob.match(symbolName))
      return true;
  return false;
}

bool SymbolPatterns::match(StringRef symbolName) const {
  return matchLiteral(symbolName) || matchGlob(symbolName);
}

static void parseSymbolPatternsFile(const Arg *arg,
                                    SymbolPatterns &symbolPatterns) {
  StringRef path = arg->getValue();
  std::optional<MemoryBufferRef> buffer = readFile(path);
  if (!buffer) {
    error("Could not read symbol file: " + path);
    return;
  }
  MemoryBufferRef mbref = *buffer;
  for (StringRef line : args::getLines(mbref)) {
    line = line.take_until([](char c) { return c == '#'; }).trim();
    if (!line.empty())
      symbolPatterns.insert(line);
  }
}

static void handleSymbolPatterns(InputArgList &args,
                                 SymbolPatterns &symbolPatterns,
                                 unsigned singleOptionCode,
                                 unsigned listFileOptionCode) {
  for (const Arg *arg : args.filtered(singleOptionCode))
    symbolPatterns.insert(arg->getValue());
  for (const Arg *arg : args.filtered(listFileOptionCode))
    parseSymbolPatternsFile(arg, symbolPatterns);
}

// Pipelined TAPI preparation (see Driver.h). Parsing a TBD is a large,
// order-independent part of initial loading, so it is started before the
// ordered input walk and overlapped with it.
static bool canPipelineTapiFiles() {
#if !LLVM_ENABLE_THREADS
  return false;
#else
  // At one thread a spawned task runs inline, in a different order.
  if (parallel::strategy.ThreadsRequested == 1)
    return false;
  // --reproduce records each tar member at the ordered readFile() call.
  if (tar)
    return false;
  // -print_dylib_search would report the speculative searches out of order.
  if (config->printDylibSearch)
    return false;
  return true;
#endif
}

// Reads one resolved input path silently and, if it is a TBD, starts its parse.
// Speculative: a read failure, a non-TBD file or an input which is never loaded
// costs worker time only, and is still read, diagnosed and published by the
// ordered walk.
// A TBD is either named .tbd, or is a stub standing in for a dylib (.dylib), or
// is a framework or dylib binary with no extension at all. Everything else --
// .o, .a, .rlib, .bc, .swiftmodule -- cannot be one, and a negative list missed
// them: a Rust link passes 207 .rlib archives, and each one was opened, mapped
// and sniffed on the main thread here and then opened and mapped a second time
// by the ordered walk, because only TBDs are entered into cachedReads.
static bool mayBeTapiPath(StringRef path) {
  StringRef ext = sys::path::extension(path);
  return ext.empty() || ext == ".tbd" || ext == ".dylib";
}

static void pipelineTapiPath(StringRef path) {
  // Do not fault in an input that cannot be a TBD.
  if (!mayBeTapiPath(path))
    return;
  if (cachedReads.contains(CachedHashStringRef(path)))
    return;

  // Not readFile(): that diagnoses a failure and records a --reproduce member
  // at the ordered position. The scope names every input this speculative pass
  // opens, so a trace shows which inputs are opened twice.
  TimeTraceScope timeScope("Speculative TAPI read", path);
  ErrorOr<std::unique_ptr<MemoryBuffer>> mbOrErr =
      MemoryBuffer::getFile(path, /*IsText=*/false,
                            /*RequiresNullTerminator=*/false);
  if (!mbOrErr)
    return;
  MemoryBufferRef mbref = (*mbOrErr)->getMemBufferRef();
  if (identify_magic(mbref.getBuffer()) != file_magic::tapi_file)
    return;

  // A TBD is never a fat file, so the ordered readFile() would return this
  // buffer unchanged.
  StringRef savedPath = saver().save(path);
  MemoryBufferRef saved(mbref.getBuffer(), savedPath);
  make<std::unique_ptr<MemoryBuffer>>(std::move(*mbOrErr));
  cachedReads[CachedHashStringRef(savedPath)] = saved;
  pipelineTapiFile(saved);
}

static void prepareCommandLineTapiFiles(const InputArgList &args) {
  if (!canPipelineTapiFiles())
    return;
  startTapiPipeline();

  // findLibrary(), findFramework() and rerootPath() are position independent,
  // and the first two memoize their results, so resolving here changes nothing
  // for the ordered walk.
  for (const Arg *arg : args) {
    switch (arg->getOption().getID()) {
    case OPT_INPUT:
    case OPT_needed_library:
    case OPT_weak_library:
    case OPT_reexport_library:
    case OPT_force_load:
    case OPT_load_hidden:
      pipelineTapiPath(rerootPath(arg->getValue()));
      break;
    case OPT_filelist: {
      ErrorOr<std::unique_ptr<MemoryBuffer>> mbOrErr =
          MemoryBuffer::getFile(arg->getValue(), /*IsText=*/false,
                                /*RequiresNullTerminator=*/false);
      if (!mbOrErr)
        break;
      for (StringRef path : args::getLines((*mbOrErr)->getMemBufferRef()))
        pipelineTapiPath(rerootPath(path));
      break;
    }
    case OPT_l:
    case OPT_needed_l:
    case OPT_weak_l:
    case OPT_reexport_l:
    case OPT_hidden_l:
      // A -l resolving to an archive or object is skipped by pipelineTapiPath().
      if (std::optional<StringRef> path = findLibrary(arg->getValue()))
        pipelineTapiPath(*path);
      break;
    case OPT_framework:
    case OPT_needed_framework:
    case OPT_weak_framework:
    case OPT_reexport_framework:
      if (std::optional<StringRef> path = findFramework(arg->getValue()))
        pipelineTapiPath(*path);
      break;
    default:
      break;
    }
  }
}


static void createFiles(const InputArgList &args) {
  TimeTraceScope timeScope("Load input files");
  prepareCommandLineTapiFiles(args);
  // This loop should be reserved for options whose exact ordering matters.
  // Other options should be handled via filtered() and/or getLastArg().
  bool isLazy = false;
  // If we've processed an opening --start-lib, without a matching --end-lib
  bool inLib = false;
  DeferredFiles deferredFiles;
  startParallelInput();

  for (const Arg *arg : args) {
    const Option &opt = arg->getOption();
    warnIfDeprecatedOption(opt);
    warnIfUnimplementedOption(opt);

    switch (opt.getID()) {
    case OPT_INPUT:
      deferFile(rerootPath(arg->getValue()), isLazy, deferredFiles);
      break;
    case OPT_needed_library:
      deferFile(rerootPath(arg->getValue()), /*isLazy=*/false, deferredFiles,
                LoadType::CommandLine, /*isNeeded=*/true);
      break;
    case OPT_reexport_library:
      deferFile(rerootPath(arg->getValue()), /*isLazy=*/false, deferredFiles,
                LoadType::CommandLine, /*isNeeded=*/false, /*isWeak=*/false,
                /*isReexport=*/true);
      break;
    case OPT_weak_library:
      deferFile(rerootPath(arg->getValue()), /*isLazy=*/false, deferredFiles,
                LoadType::CommandLine, /*isNeeded=*/false, /*isWeak=*/true);
      break;
    case OPT_filelist:
      addFileList(arg->getValue(), isLazy, deferredFiles);
      break;
    case OPT_force_load:
      deferFile(rerootPath(arg->getValue()), /*isLazy=*/false, deferredFiles,
                LoadType::CommandLineForce);
      break;
    case OPT_load_hidden:
      deferFile(rerootPath(arg->getValue()), /*isLazy=*/false, deferredFiles,
                LoadType::CommandLine, /*isNeeded=*/false, /*isWeak=*/false,
                /*isReexport=*/false, /*isHidden=*/true);
      break;
    case OPT_l:
    case OPT_needed_l:
    case OPT_reexport_l:
    case OPT_weak_l:
    case OPT_hidden_l:
      addLibrary(arg->getValue(), opt.getID() == OPT_needed_l,
                 opt.getID() == OPT_weak_l, opt.getID() == OPT_reexport_l,
                 opt.getID() == OPT_hidden_l,
                 /*isExplicit=*/true, LoadType::CommandLine, deferredFiles);
      break;
    case OPT_framework:
    case OPT_needed_framework:
    case OPT_reexport_framework:
    case OPT_weak_framework:
      addFramework(arg->getValue(), opt.getID() == OPT_needed_framework,
                   opt.getID() == OPT_weak_framework,
                   opt.getID() == OPT_reexport_framework, /*isExplicit=*/true,
                   LoadType::CommandLine, deferredFiles);
      break;
    case OPT_start_lib:
      if (inLib)
        error("nested --start-lib");
      inLib = true;
      if (!config->allLoad)
        isLazy = true;
      break;
    case OPT_end_lib:
      if (!inLib)
        error("stray --end-lib");
      inLib = false;
      isLazy = false;
      break;
    default:
      break;
    }
  }

#if LLVM_ENABLE_THREADS
  if (config->readWorkers) {
    multiThreadedPageIn(deferredFiles);

    DeferredFiles archiveContents;
    for (auto &file : deferredFiles) {
      if (loadedObjectFrameworks.contains(file.path))
        continue;

      ParseContext *ctx = nullptr;
      if (parallelInput.active) {
        parallelInput.contexts.emplace_back();
        ctx = &parallelInput.contexts.back();
        ctx->top = parallelInput.nextOrdinal++;
      }
      bool holdArchiveIndex = false;
      bool holdParse = beginHeldInput(file.buffer, file.isLazy, file.loadType,
                                      ctx, holdArchiveIndex);
      auto inputFile = processFile(file.buffer, &archiveContents, file.path,
                                   file.loadType, file.isLazy, file.isExplicit,
                                   /*isBundleLoader=*/false, file.isHidden);
      endHeldInput(inputFile, holdParse, holdArchiveIndex, ctx);
      applyDylibMetadata(inputFile, file.isNeeded, file.isWeak,
                         file.isReexport);
      checkAndCacheFramework(inputFile, file.path);

      // endHeldInput() has already dealt with the index when it held it back:
      // either the walker owns it or it published it on the spot.
      if (auto *archive = dyn_cast_or_null<ArchiveFile>(inputFile))
        if (!holdArchiveIndex)
          archive->addLazySymbols();
    }

    if (!archiveContents.empty())
      multiThreadedPageIn(archiveContents);

    pageInQueue.stopAllWork = true;
  }
#endif

  // Anything still held back is parsed now. LC_LINKER_OPTION libraries are
  // loaded afterwards and have to see the archives' residual index.
  finishParallelInput(/*moreInputsFollow=*/!unprocessedLCLinkerOptions.empty());
}

namespace {

// The input order of literal and ObjC synthetic sections is assigned once, at
// the first input to each synthetic section. Keep those inputs in the
// coordinator's serial order. Everything else is a ConcatInputSection whose
// output slot and input order can be assigned independently.
struct GatheredInputSection {
  struct SpecialInput {
    InputSection *isec;
    size_t ordinaryCountBefore;
  };

  struct OrdinaryRun {
    size_t begin;
    size_t end;
    size_t destinationIndex;
    int inputOrderBase;
    ConcatOutputSection *osec;
  };

  std::vector<ConcatInputSection *> ordinary;
  std::vector<SpecialInput> special;
  std::vector<OrdinaryRun> runs;
  // Indices into `ordinary` of the sections markLive() enqueues on their flags
  // alone, and this record's base slot in `deadStripSectionRoots`.
  std::vector<uint32_t> roots;
  size_t rootBase = 0;
};

struct GatheredInputFile {
  std::vector<GatheredInputSection> sections;
};

static void classifyInputSection(const Section &section,
                                 GatheredInputSection &result) {
  // Compact unwind entries require special handling elsewhere. (In contrast,
  // EH frames are handled like regular ConcatInputSections.)
  if (section.name == section_names::compactUnwind)
    return;
  // Addrsig sections contain metadata only needed at link time.
  if (section.name == section_names::addrSig)
    return;

  for (const Subsection &subsection : section.subsections) {
    InputSection *isec = subsection.isec;
    if (auto *concat = dyn_cast<ConcatInputSection>(isec)) {
      if (concat->isCoalescedWeak())
        continue;
      // This must inspect each subsection's symbols: a Section can have been
      // split by symbols, and only some subsections may be method lists.
      if (config->emitRelativeMethodLists &&
          ObjCMethListSection::isMethodList(concat)) {
        result.special.push_back({isec, result.ordinary.size()});
      } else if (config->emitInitOffsets &&
                 sectionType(concat->getFlags()) == S_MOD_INIT_FUNC_POINTERS) {
        result.special.push_back({isec, result.ordinary.size()});
      } else {
        if (config->deadStrip && isDeadStripRootSection(concat->getFlags())) {
          assert(!config->emitInitOffsets ||
                 sectionType(concat->getFlags()) !=
                     llvm::MachO::S_MOD_INIT_FUNC_POINTERS);
          result.roots.push_back(result.ordinary.size());
        }
        result.ordinary.push_back(concat);
      }
    } else if (isa<CStringInputSection>(isec) ||
               isa<WordLiteralInputSection>(isec)) {
      result.special.push_back({isec, result.ordinary.size()});
    } else {
      llvm_unreachable("unexpected input section kind");
    }
  }
}

} // namespace

static void gatherInputSections() {
  TimeTraceScope timeScope("Gathering input sections");

  ArrayRef<InputFile *> files = inputFiles.getArrayRef();
  std::vector<GatheredInputFile> gatheredFiles(files.size());
  parallelFor(0, files.size(), [&](size_t fileIndex) {
    const InputFile *file = files[fileIndex];
    GatheredInputFile &gatheredFile = gatheredFiles[fileIndex];
    gatheredFile.sections.resize(file->sections.size());
    for (size_t sectionIndex = 0; sectionIndex != file->sections.size();
         ++sectionIndex)
      classifyInputSection(*file->sections[sectionIndex],
                           gatheredFile.sections[sectionIndex]);
  });

  // Keep all effects which depend on the original traversal order here. This
  // includes output-section creation and the one-time input-order assignment
  // for special sections. The resulting ordinary runs have disjoint output
  // slots, so workers can fill them without touching shared graph state.
  size_t inputSectionIndex = inputSections.size();
  // replaceCommonSymbols() adds sections before this runs; they are not part of
  // any gathered record, so classify them here. This prefix is empty unless the
  // link has common symbols.
  if (config->deadStrip)
    for (ConcatInputSection *isec :
         ArrayRef<ConcatInputSection *>(inputSections))
      if (isDeadStripRootSection(isec->getFlags()))
        deadStripSectionRoots.push_back(isec);
  size_t rootIndex = deadStripSectionRoots.size();
  for (size_t fileIndex = 0; fileIndex != files.size(); ++fileIndex) {
    const InputFile *file = files[fileIndex];
    for (GatheredInputSection &gatheredSection :
         gatheredFiles[fileIndex].sections) {
      ConcatOutputSection *osec = nullptr;
      size_t ordinaryBegin = 0;
      auto addOrdinaryRun = [&](size_t ordinaryEnd) {
        if (ordinaryBegin == ordinaryEnd)
          return;
        assert(inputSectionsOrder + ordinaryEnd - ordinaryBegin <=
               UnspecifiedInputOrder);
        if (!osec)
          osec = ConcatOutputSection::getOrCreateForInput(
              gatheredSection.ordinary[ordinaryBegin]);
        gatheredSection.runs.push_back(
            {ordinaryBegin, ordinaryEnd, inputSectionIndex,
             inputSectionsOrder, osec});
        inputSectionIndex += ordinaryEnd - ordinaryBegin;
        inputSectionsOrder += ordinaryEnd - ordinaryBegin;
        ordinaryBegin = ordinaryEnd;
      };

      for (const GatheredInputSection::SpecialInput &special :
           gatheredSection.special) {
        addOrdinaryRun(special.ordinaryCountBefore);
        addInputSection(special.isec);
      }
      addOrdinaryRun(gatheredSection.ordinary.size());
      // Roots are a subsequence of `ordinary`, which the runs lay down in
      // order, so numbering the records here reproduces `inputSections` order.
      gatheredSection.rootBase = rootIndex;
      rootIndex += gatheredSection.roots.size();
    }
    if (!file->objCImageInfo.empty())
      in.objCImageInfo->addFile(file);
  }

  inputSections.resize(inputSectionIndex);
  deadStripSectionRoots.resize(rootIndex);
  parallelForEach(gatheredFiles, [](GatheredInputFile &gatheredFile) {
    for (GatheredInputSection &gatheredSection : gatheredFile.sections) {
      for (size_t j = 0, e = gatheredSection.roots.size(); j != e; ++j)
        deadStripSectionRoots[gatheredSection.rootBase + j] =
            gatheredSection.ordinary[gatheredSection.roots[j]];
      for (const GatheredInputSection::OrdinaryRun &run :
           gatheredSection.runs) {
        for (size_t i = run.begin; i != run.end; ++i) {
          ConcatInputSection *isec = gatheredSection.ordinary[i];
          isec->parent = run.osec;
          isec->outSecOff = run.inputOrderBase + i - run.begin;
          inputSections[run.destinationIndex + i - run.begin] = isec;
        }
      }
    }
  });
}

static void codegenDataGenerate() {
  TimeTraceScope timeScope("Generating codegen data");

  OutlinedHashTreeRecord globalOutlineRecord;
  StableFunctionMapRecord globalMergeRecord;
  for (ConcatInputSection *isec : inputSections) {
    if (isec->getSegName() != segment_names::data)
      continue;
    if (isec->getName() == section_names::outlinedHashTree) {
      // Read outlined hash tree from each section.
      OutlinedHashTreeRecord localOutlineRecord;
      // Use a pointer to allow modification by the function.
      auto *data = isec->data.data();
      localOutlineRecord.deserialize(data);

      // Merge it to the global hash tree.
      globalOutlineRecord.merge(localOutlineRecord);
    }
    if (isec->getName() == section_names::functionMap) {
      // Read stable functions from each section.
      StableFunctionMapRecord localMergeRecord;
      // Use a pointer to allow modification by the function.
      auto *data = isec->data.data();
      localMergeRecord.deserialize(data);

      // Merge it to the global function map.
      globalMergeRecord.merge(localMergeRecord);
    }
  }

  globalMergeRecord.finalize();

  CodeGenDataWriter Writer;
  if (!globalOutlineRecord.empty())
    Writer.addRecord(globalOutlineRecord);
  if (!globalMergeRecord.empty())
    Writer.addRecord(globalMergeRecord);

  std::error_code EC;
  auto fileName = config->codegenDataGeneratePath;
  assert(!fileName.empty());
  raw_fd_ostream Output(fileName, EC, sys::fs::OF_None);
  if (EC)
    error("fail to create " + fileName + ": " + EC.message());

  if (auto E = Writer.write(Output))
    error("fail to write CGData: " + toString(std::move(E)));
}

static void foldIdenticalLiterals() {
  TimeTraceScope timeScope("Fold identical literals");
  // We always create a cStringSection, regardless of whether dedupLiterals is
  // true. If it isn't, we simply create a non-deduplicating CStringSection.
  // Either way, we must unconditionally finalize it here.
  for (auto *sec : in.cStringSections)
    sec->finalizeContents();
  in.wordLiteralSection->finalizeContents();
}

static void addSynthenticMethnames() {
  std::string &data = *make<std::string>();
  llvm::raw_string_ostream os(data);
  for (Symbol *sym : symtab->getSymbols())
    if (isa<Undefined>(sym))
      if (ObjCStubsSection::isObjCStubSymbol(sym))
        os << ObjCStubsSection::getMethname(sym) << '\0';

  if (data.empty())
    return;

  const auto *buf = reinterpret_cast<const uint8_t *>(data.c_str());
  Section &section = *make<Section>(/*file=*/nullptr, segment_names::text,
                                    section_names::objcMethname,
                                    S_CSTRING_LITERALS, /*addr=*/0);

  auto *isec =
      make<CStringInputSection>(section, ArrayRef<uint8_t>{buf, data.size()},
                                /*align=*/1, /*dedupLiterals=*/true);
  isec->splitIntoPieces();
  for (auto &piece : isec->pieces)
    piece.setLive(true);
  section.subsections.push_back({0, isec});
  in.objcMethnameSection->addInput(isec);
  in.objcMethnameSection->isec->markLive(0);
}

// The relocation parse is the last large unscoped parallel region of the link,
// and it is badly unbalanced. parallelFor() hands out contiguous chunks of
// items/(workers*4) files in command-line order, so on the Firefox iOS client
// the two 3.4 MB Swift objects (Metrics.o and FxNimbus.o) land in the same
// 40-file chunk and whichever thread draws it parses alone for ~20 ms after
// every other thread has run out of work. Hand the largest objects out one at
// a time, largest first, and keep parallelFor()'s chunking for the long tail
// of small objects so that a link with tens of thousands of tiny inputs does
// not pay an atomic per file.
//
// parseRegularRelocations() only reads its own file and writes that file's
// subsections, so the order is unobservable in the output.
static void parseRegularRelocationsParallel() {
  TimeTraceScope timeScope("Parse relocations");

  std::vector<ObjFile *> objs;
  objs.reserve(inputFiles.size());
  for (InputFile *file : inputFiles.getArrayRef())
    if (auto *obj = dyn_cast<ObjFile>(file);
        obj && !obj->lazy && obj->isCompatible())
      objs.push_back(obj);
  if (objs.empty())
    return;

  size_t n = objs.size();
  size_t numWorkers =
      parallel::strategy.ThreadsRequested == 1
          ? 1
          : std::min(n, static_cast<size_t>(parallel::getThreadCount()));
  if (numWorkers <= 1) {
    for (ObjFile *obj : objs)
      obj->parseRegularRelocations();
    return;
  }

  // Hoist the largest objects to the front, largest first, and leave the tail
  // in command-line order. A full size sort also fixes the imbalance, but it
  // costs 1.7% on the Firefox desktop link at flat CPU and flat instructions:
  // objects that were built and named together are parsed together today, and
  // scattering the tail costs more in memory locality than the balance is
  // worth. Only the head has to move.
  size_t singles = std::min(n, numWorkers * 8);
  std::vector<size_t> order(n);
  std::iota(order.begin(), order.end(), 0);
  std::partial_sort(order.begin(), order.begin() + singles, order.end(),
                    [&](size_t a, size_t b) {
                      size_t sa = objs[a]->mb.getBufferSize();
                      size_t sb = objs[b]->mb.getBufferSize();
                      return sa != sb ? sa > sb : a < b;
                    });
  std::vector<bool> hoisted(n, false);
  std::vector<ObjFile *> ordered;
  ordered.reserve(n);
  for (size_t i = 0; i < singles; ++i) {
    ordered.push_back(objs[order[i]]);
    hoisted[order[i]] = true;
  }
  for (size_t i = 0; i < n; ++i)
    if (!hoisted[i])
      ordered.push_back(objs[i]);
  objs.swap(ordered);

  size_t chunkSize = std::max<size_t>(1, (n - singles) / (numWorkers * 4));
  std::atomic<size_t> next{0};
  auto worker = [&] {
    size_t i = next.load(std::memory_order_relaxed);
    while (i < n) {
      size_t end = std::min(n, i + (i < singles ? size_t(1) : chunkSize));
      // On failure the compare-exchange reloads i, so the bound is recomputed.
      if (!next.compare_exchange_weak(i, end, std::memory_order_relaxed,
                                      std::memory_order_relaxed))
        continue;
      for (; i < end; ++i)
        objs[i]->parseRegularRelocations();
      i = next.load(std::memory_order_relaxed);
    }
  };

  // As in parallelFor(): one worker runs on the calling thread.
  parallel::TaskGroup tg;
  for (size_t i = 1; i < numWorkers; ++i)
    tg.spawn(worker);
  worker();
}

static void referenceStubBinder() {
  bool needsStubHelper = config->outputType == MH_DYLIB ||
                         config->outputType == MH_EXECUTE ||
                         config->outputType == MH_BUNDLE;
  if (!needsStubHelper || !symtab->find("dyld_stub_binder"))
    return;

  // dyld_stub_binder is used by dyld to resolve lazy bindings. This code here
  // adds a opportunistic reference to dyld_stub_binder if it happens to exist.
  // dyld_stub_binder is in libSystem.dylib, which is usually linked in. This
  // isn't needed for correctness, but the presence of that symbol suppresses
  // "no symbols" diagnostics from `nm`.
  // StubHelperSection::setUp() adds a reference and errors out if
  // dyld_stub_binder doesn't exist in case it is actually needed.
  symtab->addUndefined("dyld_stub_binder", /*file=*/nullptr, /*isWeak=*/false);
}

static void createAliases() {
  for (const auto &pair : config->aliasedSymbols) {
    if (const auto &sym = symtab->find(pair.first)) {
      if (const auto &defined = dyn_cast<Defined>(sym)) {
        symtab->aliasDefined(defined, pair.second, defined->getFile())
            ->noDeadStrip = true;
      } else {
        error("TODO: support aliasing to symbols of kind " +
              Twine(sym->kind()));
      }
    } else {
      warn("undefined base symbol '" + pair.first + "' for alias '" +
           pair.second + "'\n");
    }
  }

  for (const InputFile *file : inputFiles) {
    if (auto *objFile = dyn_cast<ObjFile>(file)) {
      for (const AliasSymbol *alias : objFile->aliases) {
        if (const auto &aliased = symtab->find(alias->getAliasedName())) {
          if (const auto &defined = dyn_cast<Defined>(aliased)) {
            symtab->aliasDefined(defined, alias->getName(), alias->getFile(),
                                 alias->privateExtern);
          } else {
            // Common, dylib, and undefined symbols are all valid alias
            // referents (undefineds can become valid Defined symbols later on
            // in the link.)
            error("TODO: support aliasing to symbols of kind " +
                  Twine(aliased->kind()));
          }
        } else {
          // This shouldn't happen since MC generates undefined symbols to
          // represent the alias referents. Thus we fatal() instead of just
          // warning here.
          fatal("unable to find alias referent " + alias->getAliasedName() +
                " for " + alias->getName());
        }
      }
    }
  }
}

static void handleExplicitExports() {
  static constexpr int kMaxWarnings = 3;
  if (config->hasExplicitExports) {
    std::atomic<uint64_t> warningsCount{0};
    parallelForEach(symtab->getSymbols(), [&warningsCount](Symbol *sym) {
      if (auto *defined = dyn_cast<Defined>(sym)) {
        if (config->exportedSymbols.match(sym->getName())) {
          if (defined->privateExtern) {
            if (defined->weakDefCanBeHidden) {
              // weak_def_can_be_hidden symbols behave similarly to
              // private_extern symbols in most cases, except for when
              // it is explicitly exported.
              // The former can be exported but the latter cannot.
              defined->privateExtern = false;
            } else {
              // Only print the first 3 warnings verbosely, and
              // shorten the rest to avoid crowding logs.
              if (warningsCount.fetch_add(1, std::memory_order_relaxed) <
                  kMaxWarnings)
                warn("cannot export hidden symbol " + toString(*defined) +
                     "\n>>> defined in " + toString(defined->getFile()));
            }
          }
        } else {
          defined->privateExtern = true;
        }
      } else if (auto *dysym = dyn_cast<DylibSymbol>(sym)) {
        dysym->shouldReexport = config->exportedSymbols.match(sym->getName());
      }
    });
    if (warningsCount > kMaxWarnings)
      warn("<... " + Twine(warningsCount - kMaxWarnings) +
           " more similar warnings...>");
  } else if (!config->unexportedSymbols.empty()) {
    parallelForEach(symtab->getSymbols(), [](Symbol *sym) {
      if (auto *defined = dyn_cast<Defined>(sym))
        if (config->unexportedSymbols.match(defined->getName()))
          defined->privateExtern = true;
    });
  }
}

static void eraseInitializerSymbols() {
  for (ConcatInputSection *isec : in.initOffsets->inputs())
    for (Defined *sym : isec->symbols)
      sym->used = false;
}

static SmallVector<StringRef, 0> getRuntimePaths(opt::InputArgList &args) {
  SmallVector<StringRef, 0> vals;
  DenseSet<StringRef> seen;
  for (const Arg *arg : args.filtered(OPT_rpath)) {
    StringRef val = arg->getValue();
    if (seen.insert(val).second)
      vals.push_back(val);
    else if (config->warnDuplicateRpath)
      warn("duplicate -rpath '" + val + "' ignored [--warn-duplicate-rpath]");
  }
  return vals;
}

static SmallVector<StringRef, 0> getAllowableClients(opt::InputArgList &args) {
  SmallVector<StringRef, 0> vals;
  DenseSet<StringRef> seen;
  for (const Arg *arg : args.filtered(OPT_allowable_client)) {
    StringRef val = arg->getValue();
    if (seen.insert(val).second)
      vals.push_back(val);
  }
  return vals;
}

static void computeColdness() {
  TimeTraceScope timeScope("Compute coldness");
  // Each section's isCold bit is derived only from its own symbols, so chunks
  // of the section table are independent.
  constexpr size_t sectionsPerChunk = 4096;
  const size_t sectionCount = inputSections.size();
  const size_t numChunks =
      (sectionCount + sectionsPerChunk - 1) / sectionsPerChunk;
  parallelFor(0, numChunks, [&](size_t chunkIndex) {
    const size_t begin = chunkIndex * sectionsPerChunk;
    const size_t end = std::min(begin + sectionsPerChunk, sectionCount);
    for (size_t i = begin; i != end; ++i) {
      InputSection *isec = inputSections[i];
      if (!isCodeSection(isec))
        continue;
      isec->isCold = llvm::any_of(isec->symbols,
                                  [](Defined *sym) { return sym->isCold(); });
    }
  });
}

namespace lld {
namespace macho {
bool link(ArrayRef<const char *> argsArr, llvm::raw_ostream &stdoutOS,
          llvm::raw_ostream &stderrOS, bool exitEarly, bool disableOutput) {
  // This driver-specific context will be freed later by lldMain().
  auto *ctx = new CommonLinkerContext;

  ctx->e.initialize(stdoutOS, stderrOS, exitEarly, disableOutput);
  ctx->e.cleanupCallback = []() {
    // A fatal() raised inside the input phase leaves the prepass tasks running,
    // and they are reading archive buffers that this callback and the arena
    // teardown behind it are about to release. Wait for them before anything
    // else here frees what they read; the prepass is pure, so abandoning what
    // has not started costs only the work already spent.
    joinArchivePrepass(/*willWalk=*/false);
    // And for the held object parses streamed behind the argument walk, which
    // write the symbol table and read the object buffers.
    joinHeldParse();
    // Same hazard for the streamed member parses, which are not pure: they are
    // writing the symbol table and reading the archive buffers this callback is
    // about to release. A fatal() on the main thread inside the select loop --
    // the error limit reached in createIndexedMember(), say -- gets here with
    // them still running.
    joinStreamedMemberParses();
    resolvedFrameworks.clear();
    resolvedLibraries.clear();
    cachedReads.clear();
    concatOutputSections.clear();
    inputFiles.clear();
    inputSections.clear();
    deadStripSectionRoots.clear();
    inputSectionsOrder = 0;
    loadedArchives.clear();
    loadedObjectFrameworks.clear();
    missingAutolinkWarnings.clear();
    autolinkLibraryNames.clear();
    autolinkFrameworkNames.clear();
    syntheticSections.clear();
    thunkMap.clear();
    unprocessedLCLinkerOptions.clear();
    ObjCSelRefsHelper::cleanup();
    resetParallelInputPhase();
    resetPriorityBuilder();

    firstTLVDataSection = nullptr;
    tar = nullptr;
    in = InStruct();

    resetLoadedDylibs();
    resetOutputSegments();
    resetWriter();
    InputFile::resetIdCount();

    objc::doCleanup();
  };

  ctx->e.logName = args::getFilenameWithoutExe(argsArr[0]);

  MachOOptTable parser;
  InputArgList args = parser.parse(*ctx, argsArr.slice(1));

  ctx->e.errorLimitExceededMsg = "too many errors emitted, stopping now "
                                 "(use --error-limit=0 to see all errors)";
  ctx->e.errorLimit = args::getInteger(args, OPT_error_limit_eq, 20);
  ctx->e.verbose = args.hasArg(OPT_verbose);

  if (args.hasArg(OPT_help_hidden)) {
    parser.printHelp(*ctx, argsArr[0], /*showHidden=*/true);
    return true;
  }
  if (args.hasArg(OPT_help)) {
    parser.printHelp(*ctx, argsArr[0], /*showHidden=*/false);
    return true;
  }
  if (args.hasArg(OPT_version)) {
    message(getLLDVersion());
    return true;
  }

  config = std::make_unique<Configuration>();
  symtab = std::make_unique<SymbolTable>();
  config->outputType = getOutputType(args);
  target = createTargetInfo(args);
  depTracker = std::make_unique<DependencyTracker>(
      args.getLastArgValue(OPT_dependency_info));

  config->ltoo = args::getInteger(args, OPT_lto_O, 2);
  if (config->ltoo > 3)
    error("--lto-O: invalid optimization level: " + Twine(config->ltoo));
  unsigned ltoCgo =
      args::getInteger(args, OPT_lto_CGO, args::getCGOptLevel(config->ltoo));
  if (auto level = CodeGenOpt::getLevel(ltoCgo))
    config->ltoCgo = *level;
  else
    error("--lto-CGO: invalid codegen optimization level: " + Twine(ltoCgo));

  if (errorCount())
    return false;

  if (args.hasArg(OPT_pagezero_size)) {
    uint64_t pagezeroSize = args::getHex(args, OPT_pagezero_size, 0);

    // ld64 does something really weird. It attempts to realign the value to the
    // page size, but assumes the page size is 4K. This doesn't work with most
    // of Apple's ARM64 devices, which use a page size of 16K. This means that
    // it will first 4K align it by rounding down, then round up to 16K.  This
    // probably only happened because no one using this arg with anything other
    // then 0, so no one checked if it did what is what it says it does.

    // So we are not copying this weird behavior and doing the it in a logical
    // way, by always rounding down to page size.
    if (!isAligned(Align(target->getPageSize()), pagezeroSize)) {
      pagezeroSize -= pagezeroSize % target->getPageSize();
      warn("__PAGEZERO size is not page aligned, rounding down to 0x" +
           Twine::utohexstr(pagezeroSize));
    }

    target->pageZeroSize = pagezeroSize;
  }

  config->osoPrefix = args.getLastArgValue(OPT_oso_prefix);
  if (!config->osoPrefix.empty()) {
    // The max path length is 4096, in theory. However that seems quite long
    // and seems unlikely that any one would want to strip everything from the
    // path. Hence we've picked a reasonably large number here.
    SmallString<1024> expanded;
    // Expand "." into the current working directory.
    if (config->osoPrefix == "." && !fs::current_path(expanded)) {
      // Note: LD64 expands "." to be `<current_dir>/
      // (ie., it has a slash suffix) whereas current_path() doesn't.
      // So we have to append '/' to be consistent because this is
      // meaningful for our text based stripping.
      expanded += sys::path::get_separator();
    } else {
      expanded = config->osoPrefix;
    }
    config->osoPrefix = saver().save(expanded.str());
  }

  bool pie = args.hasFlag(OPT_pie, OPT_no_pie, true);
  if (!supportsNoPie() && !pie) {
    warn("-no_pie ignored for arm64");
    pie = true;
  }

  config->isPic = config->outputType == MH_DYLIB ||
                  config->outputType == MH_BUNDLE ||
                  (config->outputType == MH_EXECUTE && pie);

  // Must be set before any InputSections and Symbols are created.
  config->deadStrip = args.hasArg(OPT_dead_strip);
  config->interposable = args.hasArg(OPT_interposable);

  config->systemLibraryRoots = getSystemLibraryRoots(args);
  if (const char *path = getReproduceOption(args)) {
    // Note that --reproduce is a debug option so you can ignore it
    // if you are trying to understand the whole picture of the code.
    Expected<std::unique_ptr<TarWriter>> errOrWriter =
        TarWriter::create(path, path::stem(path));
    if (errOrWriter) {
      tar = std::move(*errOrWriter);
      tar->append("response.txt", createResponseFile(args));
      tar->append("version.txt", getLLDVersion() + "\n");
    } else {
      error("--reproduce: " + toString(errOrWriter.takeError()));
    }
  }

  if (auto *arg = args.getLastArg(OPT_read_workers)) {
#if LLVM_ENABLE_THREADS
    StringRef v(arg->getValue());
    unsigned workers = 0;
    if (!llvm::to_integer(v, workers, 0))
      error(arg->getSpelling() +
            ": expected a non-negative integer, but got '" + arg->getValue() +
            "'");
    config->readWorkers = workers;
#else
    warn(arg->getSpelling() +
         ": option unavailable because lld was not built with thread support");
#endif
  }
  if (auto *arg = args.getLastArg(OPT_threads_eq)) {
    StringRef v(arg->getValue());
    unsigned threads = 0;
    if (!llvm::to_integer(v, threads, 0) || threads == 0)
      error(arg->getSpelling() + ": expected a positive integer, but got '" +
            arg->getValue() + "'");
    parallel::strategy = hardware_concurrency(threads);
    config->thinLTOJobs = v;
  }
  // The parallel input phase reads, parses and publishes inputs on its own
  // workers; the read workers' separate read loop would only split the ordered
  // walk in two and keep the TAPI pipeline from running. Accept the option and
  // ignore it whenever more than one thread is requested: it never affected
  // output or input order. No warning, since callers link with
  // -fatal_warnings.
  if (parallel::strategy.ThreadsRequested != 1)
    config->readWorkers = 0;
  if (auto *arg = args.getLastArg(OPT_thinlto_jobs_eq))
    config->thinLTOJobs = arg->getValue();
  if (!get_threadpool_strategy(config->thinLTOJobs))
    error("--thinlto-jobs: invalid job count: " + config->thinLTOJobs);

  // The parallel input phase needs the sharded symbol table from the very first
  // symbol, because each of its slots carries a hidden header. At one thread
  // nothing is sharded and the input phase is bit-for-bit the old one. The
  // ordinal field widths have to be settled before the first ordinal, which is
  // the first -u symbol just below.
  initRecordOrdinalWidths();
  if (parallel::strategy.ThreadsRequested != 1)
    symtab->enableSharding();

  for (const Arg *arg : args.filtered(OPT_u)) {
    config->explicitUndefineds.push_back(symtab->addUndefined(
        arg->getValue(), /*file=*/nullptr, /*isWeakRef=*/false));
  }

  for (const Arg *arg : args.filtered(OPT_U))
    config->explicitDynamicLookups.insert(arg->getValue());

  config->mapFile = args.getLastArgValue(OPT_map);
  config->optimize = args::getInteger(args, OPT_O, 1);
  config->outputFile = args.getLastArgValue(OPT_o, "a.out");
  config->finalOutput =
      args.getLastArgValue(OPT_final_output, config->outputFile);
  config->astPaths = args.getAllArgValues(OPT_add_ast_path);
  config->headerPad = args::getHex(args, OPT_headerpad, /*Default=*/32);
  config->headerPadMaxInstallNames =
      args.hasArg(OPT_headerpad_max_install_names);
  config->printDylibSearch =
      args.hasArg(OPT_print_dylib_search) || getenv("RC_TRACE_DYLIB_SEARCHING");
  config->printEachFile = args.hasArg(OPT_t);
  config->printWhyLoad = args.hasArg(OPT_why_load);
  config->omitDebugInfo = args.hasArg(OPT_S);
  config->errorForArchMismatch = args.hasArg(OPT_arch_errors_fatal);
  if (const Arg *arg = args.getLastArg(OPT_bundle_loader)) {
    if (config->outputType != MH_BUNDLE)
      error("-bundle_loader can only be used with MachO bundle output");
    addFile(arg->getValue(), LoadType::CommandLine, /*isLazy=*/false,
            /*isExplicit=*/false, /*isBundleLoader=*/true);
  }
  for (auto *arg : args.filtered(OPT_dyld_env)) {
    StringRef envPair(arg->getValue());
    if (!envPair.contains('='))
      error("-dyld_env's argument is  malformed. Expected "
            "-dyld_env <ENV_VAR>=<VALUE>, got `" +
            envPair + "`");
    config->dyldEnvs.push_back(envPair);
  }
  if (!config->dyldEnvs.empty() && config->outputType != MH_EXECUTE)
    error("-dyld_env can only be used when creating executable output");

  if (const Arg *arg = args.getLastArg(OPT_umbrella)) {
    if (config->outputType != MH_DYLIB)
      warn("-umbrella used, but not creating dylib");
    config->umbrella = arg->getValue();
  }
  config->ltoObjPath = args.getLastArgValue(OPT_object_path_lto);
  config->ltoNewPmPasses = args.getLastArgValue(OPT_lto_newpm_passes);
  config->thinLTOCacheDir = args.getLastArgValue(OPT_cache_path_lto);
  config->thinLTOCachePolicy = getLTOCachePolicy(args);
  config->thinLTOEmitImportsFiles = args.hasArg(OPT_thinlto_emit_imports_files);
  config->thinLTOEmitIndexFiles = args.hasArg(OPT_thinlto_emit_index_files) ||
                                  args.hasArg(OPT_thinlto_index_only) ||
                                  args.hasArg(OPT_thinlto_index_only_eq);
  config->thinLTOIndexOnly = args.hasArg(OPT_thinlto_index_only) ||
                             args.hasArg(OPT_thinlto_index_only_eq);
  config->thinLTOIndexOnlyArg = args.getLastArgValue(OPT_thinlto_index_only_eq);
  config->thinLTOObjectSuffixReplace =
      getOldNewOptions(args, OPT_thinlto_object_suffix_replace_eq);
  std::tie(config->thinLTOPrefixReplaceOld, config->thinLTOPrefixReplaceNew,
           config->thinLTOPrefixReplaceNativeObject) =
      getOldNewOptionsExtra(args, OPT_thinlto_prefix_replace_eq);
  if (config->thinLTOEmitIndexFiles && !config->thinLTOIndexOnly) {
    if (args.hasArg(OPT_thinlto_object_suffix_replace_eq))
      error("--thinlto-object-suffix-replace is not supported with "
            "--thinlto-emit-index-files");
    else if (args.hasArg(OPT_thinlto_prefix_replace_eq))
      error("--thinlto-prefix-replace is not supported with "
            "--thinlto-emit-index-files");
  }
  if (!config->thinLTOPrefixReplaceNativeObject.empty() &&
      config->thinLTOIndexOnlyArg.empty()) {
    error("--thinlto-prefix-replace=old_dir;new_dir;obj_dir must be used with "
          "--thinlto-index-only=");
  }
  config->warnDuplicateRpath =
      args.hasFlag(OPT_warn_duplicate_rpath, OPT_no_warn_duplicate_rpath, true);
  config->runtimePaths = getRuntimePaths(args);
  config->allowableClients = getAllowableClients(args);
  config->allLoad = args.hasFlag(OPT_all_load, OPT_noall_load, false);
  config->archMultiple = args.hasArg(OPT_arch_multiple);
  config->applicationExtension = args.hasFlag(
      OPT_application_extension, OPT_no_application_extension, false);
  config->exportDynamic = args.hasArg(OPT_export_dynamic);
  config->forceLoadObjC = args.hasArg(OPT_ObjC);
  config->forceLoadSwift = args.hasArg(OPT_force_load_swift_libs);
  config->deadStripDylibs = args.hasArg(OPT_dead_strip_dylibs);
  config->demangle = args.hasArg(OPT_demangle);
  config->implicitDylibs = !args.hasArg(OPT_no_implicit_dylibs);
  config->emitFunctionStarts =
      args.hasFlag(OPT_function_starts, OPT_no_function_starts, true);
  config->emitDataInCodeInfo =
      args.hasFlag(OPT_data_in_code_info, OPT_no_data_in_code_info, true);
  config->emitChainedFixups = shouldEmitChainedFixups(args);
  config->emitInitOffsets =
      config->emitChainedFixups || args.hasArg(OPT_init_offsets);
  config->emitRelativeMethodLists = shouldEmitRelativeMethodLists(args);
  config->icfLevel = getICFLevel(args);
  config->keepICFStabs = args.hasArg(OPT_keep_icf_stabs);
  config->dedupStrings =
      args.hasFlag(OPT_deduplicate_strings, OPT_no_deduplicate_strings, true);
  config->dedupSymbolStrings = !args.hasArg(OPT_no_deduplicate_symbol_strings);
  config->deadStripDuplicates = args.hasArg(OPT_dead_strip_duplicates);
  config->stripSwiftForceLoad =
      args.hasFlag(OPT_strip_swift_force_load, OPT_no_strip_swift_force_load,
                   /*Default=*/false);
  config->warnDylibInstallName = args.hasFlag(
      OPT_warn_dylib_install_name, OPT_no_warn_dylib_install_name, false);
  config->ignoreOptimizationHints = args.hasArg(OPT_ignore_optimization_hints);
  config->callGraphProfileSort = args.hasFlag(
      OPT_call_graph_profile_sort, OPT_no_call_graph_profile_sort, true);
  config->printSymbolOrder = args.getLastArgValue(OPT_print_symbol_order_eq);
  config->forceExactCpuSubtypeMatch =
      getenv("LD_DYLIB_CPU_SUBTYPES_MUST_MATCH");
  config->objcStubsMode = getObjCStubsMode(args);
  config->ignoreAutoLink = args.hasArg(OPT_ignore_auto_link);
  for (const Arg *arg : args.filtered(OPT_ignore_auto_link_option))
    config->ignoreAutoLinkOptions.insert(arg->getValue());
  config->strictAutoLink = args.hasArg(OPT_strict_auto_link);
  config->ltoDebugPassManager = args.hasArg(OPT_lto_debug_pass_manager);
  config->emitLLVM = args.hasArg(OPT_lto_emit_llvm);
  config->codegenDataGeneratePath =
      args.getLastArgValue(OPT_codegen_data_generate_path);
  config->csProfileGenerate = args.hasArg(OPT_cs_profile_generate);
  config->csProfilePath = args.getLastArgValue(OPT_cs_profile_path);
  config->pgoWarnMismatch =
      args.hasFlag(OPT_pgo_warn_mismatch, OPT_no_pgo_warn_mismatch, true);
  config->warnThinArchiveMissingMembers =
      args.hasFlag(OPT_warn_thin_archive_missing_members,
                   OPT_no_warn_thin_archive_missing_members, true);
  config->warnMissingSubsectionsViaSymbols =
      args.hasFlag(OPT_warn_missing_subsections_via_symbols,
                   OPT_no_warn_missing_subsections_via_symbols, false);
  config->generateUuid = !args.hasArg(OPT_no_uuid);
  config->disableVerify = args.hasArg(OPT_disable_verify);
  config->separateCstringLiteralSections =
      args.hasFlag(OPT_separate_cstring_literal_sections,
                   OPT_no_separate_cstring_literal_sections, false);
  config->tailMergeStrings =
      args.hasFlag(OPT_tail_merge_strings, OPT_no_tail_merge_strings, false);
  if (auto *arg = args.getLastArg(OPT_slop_scale_eq)) {
    StringRef v(arg->getValue());
    unsigned slop = 0;
    if (!llvm::to_integer(v, slop))
      error(arg->getSpelling() +
            ": expected a non-negative integer, but got '" + v + "'");
    config->slopScale = slop;
  }

  auto IncompatWithCGSort = [&](StringRef firstArgStr) {
    // Throw an error only if --call-graph-profile-sort is explicitly specified
    if (config->callGraphProfileSort)
      if (const Arg *arg = args.getLastArgNoClaim(OPT_call_graph_profile_sort))
        error(firstArgStr + " is incompatible with " + arg->getSpelling());
  };
  if (args.hasArg(OPT_irpgo_profile_sort) ||
      args.hasArg(OPT_irpgo_profile_sort_eq))
    warn("--irpgo-profile-sort is deprecated. Please use "
         "--bp-startup-sort=function");
  if (const Arg *arg = args.getLastArg(OPT_irpgo_profile))
    config->irpgoProfilePath = arg->getValue();

  if (const Arg *arg = args.getLastArg(OPT_irpgo_profile_sort)) {
    config->irpgoProfilePath = arg->getValue();
    config->bpStartupFunctionSort = true;
    IncompatWithCGSort(arg->getSpelling());
  }
  config->bpCompressionSortStartupFunctions =
      args.hasFlag(OPT_bp_compression_sort_startup_functions,
                   OPT_no_bp_compression_sort_startup_functions, false);
  if (const Arg *arg = args.getLastArg(OPT_bp_startup_sort)) {
    StringRef startupSortStr = arg->getValue();
    if (startupSortStr == "function") {
      config->bpStartupFunctionSort = true;
    } else if (startupSortStr != "none") {
      error("unknown value `" + startupSortStr + "` for " + arg->getSpelling());
    }
    if (startupSortStr != "none")
      IncompatWithCGSort(arg->getSpelling());
  }
  if (!config->bpStartupFunctionSort &&
      config->bpCompressionSortStartupFunctions)
    error("--bp-compression-sort-startup-functions must be used with "
          "--bp-startup-sort=function");
  if (config->irpgoProfilePath.empty() && config->bpStartupFunctionSort)
    error("--bp-startup-sort=function must be used with "
          "--irpgo-profile");
  auto addCompressionSortSpec = [&](StringRef value) {
    SmallVector<StringRef, 3> parts;
    value.split(parts, '=');

    StringRef globString = parts[0];
    unsigned layoutPriority = 0;
    std::optional<unsigned> matchPriority;

    if (parts.size() > 1 && !parts[1].empty()) {
      if (!to_integer(parts[1], layoutPriority)) {
        error("--bp-compression-sort-section: expected integer "
              "for layout_priority, got '" +
              parts[1] + "'");
        return;
      }
    }
    if (parts.size() > 2 && !parts[2].empty()) {
      unsigned mp;
      if (!to_integer(parts[2], mp)) {
        error("--bp-compression-sort-section: expected integer "
              "for match_priority, got '" +
              parts[2] + "'");
        return;
      }
      matchPriority = mp;
    }
    if (parts.size() > 3) {
      error("--bp-compression-sort-section: too many '=' in '" + value + "'");
      return;
    }

    auto spec = BPCompressionSortSpec::create(globString, layoutPriority,
                                              matchPriority);
    if (!spec) {
      error("--bp-compression-sort-section: " + toString(spec.takeError()));
      return;
    }
    config->bpCompressionSortSpecs.emplace_back(std::move(*spec));
  };

  for (const Arg *arg : args.filtered(OPT_bp_compression_sort_section))
    addCompressionSortSpec(arg->getValue());
  if (!config->bpCompressionSortSpecs.empty())
    IncompatWithCGSort("--bp-compression-sort-section");
  if (const Arg *arg = args.getLastArg(OPT_bp_compression_sort)) {
    StringRef compressionSortStr = arg->getValue();
    if (compressionSortStr == "function") {
      config->bpFunctionOrderForCompression = true;
    } else if (compressionSortStr == "data") {
      config->bpDataOrderForCompression = true;
    } else if (compressionSortStr == "both") {
      config->bpFunctionOrderForCompression = true;
      config->bpDataOrderForCompression = true;
    } else if (compressionSortStr != "none") {
      error("unknown value `" + compressionSortStr + "` for " +
            arg->getSpelling());
    }
    if (compressionSortStr != "none")
      IncompatWithCGSort(arg->getSpelling());
  }
  config->bpVerboseSectionOrderer = args.hasArg(OPT_verbose_bp_section_orderer);

  for (const Arg *arg : args.filtered(OPT_alias)) {
    config->aliasedSymbols.push_back(
        std::make_pair(arg->getValue(0), arg->getValue(1)));
  }

  if (const char *zero = getenv("ZERO_AR_DATE"))
    config->zeroModTime = strcmp(zero, "0") != 0;
  if (args.getLastArg(OPT_reproducible))
    config->zeroModTime = true;

  std::array<PlatformType, 4> encryptablePlatforms{
      PLATFORM_IOS, PLATFORM_WATCHOS, PLATFORM_TVOS, PLATFORM_XROS};
  config->emitEncryptionInfo =
      args.hasFlag(OPT_encryptable, OPT_no_encryption,
                   is_contained(encryptablePlatforms, config->platform()));

  if (const Arg *arg = args.getLastArg(OPT_install_name)) {
    if (config->warnDylibInstallName && config->outputType != MH_DYLIB)
      warn(
          arg->getAsString(args) +
          ": ignored, only has effect with -dylib [--warn-dylib-install-name]");
    else
      config->installName = arg->getValue();
  } else if (config->outputType == MH_DYLIB) {
    config->installName = config->finalOutput;
  }

  auto getClientName = [&]() {
    StringRef cn = path::filename(config->finalOutput);
    cn.consume_front("lib");
    auto firstDotOrUnderscore = cn.find_first_of("._");
    cn = cn.take_front(firstDotOrUnderscore);
    return cn;
  };
  config->clientName = args.getLastArgValue(OPT_client_name, getClientName());

  if (args.hasArg(OPT_mark_dead_strippable_dylib)) {
    if (config->outputType != MH_DYLIB)
      warn("-mark_dead_strippable_dylib: ignored, only has effect with -dylib");
    else
      config->markDeadStrippableDylib = true;
  }

  if (const Arg *arg = args.getLastArg(OPT_static, OPT_dynamic))
    config->staticLink = (arg->getOption().getID() == OPT_static);

  if (const Arg *arg =
          args.getLastArg(OPT_flat_namespace, OPT_twolevel_namespace))
    config->namespaceKind = arg->getOption().getID() == OPT_twolevel_namespace
                                ? NamespaceKind::twolevel
                                : NamespaceKind::flat;

  config->undefinedSymbolTreatment = getUndefinedSymbolTreatment(args);

  if (config->outputType == MH_EXECUTE)
    config->entry = symtab->addUndefined(args.getLastArgValue(OPT_e, "_main"),
                                         /*file=*/nullptr,
                                         /*isWeakRef=*/false);

  config->librarySearchPaths =
      getLibrarySearchPaths(args, config->systemLibraryRoots);
  config->frameworkSearchPaths =
      getFrameworkSearchPaths(args, config->systemLibraryRoots);
  if (const Arg *arg =
          args.getLastArg(OPT_search_paths_first, OPT_search_dylibs_first))
    config->searchDylibsFirst =
        arg->getOption().getID() == OPT_search_dylibs_first;

  config->dylibCompatibilityVersion =
      parseDylibVersion(args, OPT_compatibility_version);
  config->dylibCurrentVersion = parseDylibVersion(args, OPT_current_version);

  config->dataConst =
      args.hasFlag(OPT_data_const, OPT_no_data_const, dataConstDefault(args));
  // Populate config->sectionRenameMap with builtin default renames.
  // Options -rename_section and -rename_segment are able to override.
  initializeSectionRenameMap();
  // Reject every special character except '.' and '$'
  // TODO(gkm): verify that this is the proper set of invalid chars
  StringRef invalidNameChars("!\"#%&'()*+,-/:;<=>?@[\\]^`{|}~");
  auto validName = [invalidNameChars](StringRef s) {
    if (s.find_first_of(invalidNameChars) != StringRef::npos)
      error("invalid name for segment or section: " + s);
    return s;
  };
  for (const Arg *arg : args.filtered(OPT_rename_section)) {
    config->sectionRenameMap[{validName(arg->getValue(0)),
                              validName(arg->getValue(1))}] = {
        validName(arg->getValue(2)), validName(arg->getValue(3))};
  }
  for (const Arg *arg : args.filtered(OPT_rename_segment)) {
    config->segmentRenameMap[validName(arg->getValue(0))] =
        validName(arg->getValue(1));
  }

  config->sectionAlignments = parseSectAlign(args);

  for (const Arg *arg : args.filtered(OPT_segprot)) {
    StringRef segName = arg->getValue(0);
    uint32_t maxProt = parseProtection(arg->getValue(1));
    uint32_t initProt = parseProtection(arg->getValue(2));

    // FIXME: Check if this works on more platforms.
    bool allowsDifferentInitAndMaxProt =
        config->platform() == PLATFORM_MACOS ||
        config->platform() == PLATFORM_MACCATALYST;
    if (allowsDifferentInitAndMaxProt) {
      if (initProt > maxProt)
        error("invalid argument '" + arg->getAsString(args) +
              "': init must not be more permissive than max");
    } else {
      if (maxProt != initProt && config->arch() != AK_i386)
        error("invalid argument '" + arg->getAsString(args) +
              "': max and init must be the same for non-macOS non-i386 archs");
    }

    if (segName == segment_names::linkEdit)
      error("-segprot cannot be used to change __LINKEDIT's protections");
    config->segmentProtections.push_back({segName, maxProt, initProt});
  }

  config->hasExplicitExports =
      args.hasArg(OPT_no_exported_symbols) ||
      args.hasArgNoClaim(OPT_exported_symbol, OPT_exported_symbols_list);
  handleSymbolPatterns(args, config->exportedSymbols, OPT_exported_symbol,
                       OPT_exported_symbols_list);
  handleSymbolPatterns(args, config->unexportedSymbols, OPT_unexported_symbol,
                       OPT_unexported_symbols_list);
  if (config->hasExplicitExports && !config->unexportedSymbols.empty())
    error("cannot use both -exported_symbol* and -unexported_symbol* options");

  if (args.hasArg(OPT_no_exported_symbols) && !config->exportedSymbols.empty())
    error("cannot use both -exported_symbol* and -no_exported_symbols options");

  // Imitating LD64's:
  // -non_global_symbols_no_strip_list and -non_global_symbols_strip_list can't
  // both be present.
  // But -x can be used with either of these two, in which case, the last arg
  // takes effect.
  // (TODO: This is kind of confusing - considering disallowing using them
  // together for a more straightforward behaviour)
  {
    bool includeLocal = false;
    bool excludeLocal = false;
    for (const Arg *arg :
         args.filtered(OPT_x, OPT_non_global_symbols_no_strip_list,
                       OPT_non_global_symbols_strip_list)) {
      switch (arg->getOption().getID()) {
      case OPT_x:
        config->localSymbolsPresence = SymtabPresence::None;
        break;
      case OPT_non_global_symbols_no_strip_list:
        if (excludeLocal) {
          error("cannot use both -non_global_symbols_no_strip_list and "
                "-non_global_symbols_strip_list");
        } else {
          includeLocal = true;
          config->localSymbolsPresence = SymtabPresence::SelectivelyIncluded;
          parseSymbolPatternsFile(arg, config->localSymbolPatterns);
        }
        break;
      case OPT_non_global_symbols_strip_list:
        if (includeLocal) {
          error("cannot use both -non_global_symbols_no_strip_list and "
                "-non_global_symbols_strip_list");
        } else {
          excludeLocal = true;
          config->localSymbolsPresence = SymtabPresence::SelectivelyExcluded;
          parseSymbolPatternsFile(arg, config->localSymbolPatterns);
        }
        break;
      default:
        llvm_unreachable("unexpected option");
      }
    }
  }
  // Explicitly-exported literal symbols must be defined, but might
  // languish in an archive if unreferenced elsewhere or if they are in the
  // non-global strip list. Light a fire under those lazy symbols!
  for (const CachedHashStringRef &cachedName : config->exportedSymbols.literals)
    symtab->addUndefined(cachedName.val(), /*file=*/nullptr,
                         /*isWeakRef=*/false);

  for (const Arg *arg : args.filtered(OPT_why_live))
    config->whyLive.insert(arg->getValue());
  if (!config->whyLive.empty() && !config->deadStrip)
    warn("-why_live has no effect without -dead_strip, ignoring");

  config->saveTemps = args.hasArg(OPT_save_temps);

  config->adhocCodesign = args.hasFlag(
      OPT_adhoc_codesign, OPT_no_adhoc_codesign,
      shouldAdhocSignByDefault(config->arch(), config->platform()));

  if (args.hasArg(OPT_v)) {
    message(getLLDVersion(), ctx->e.errs());
    message(StringRef("Library search paths:") +
                (config->librarySearchPaths.empty()
                     ? ""
                     : "\n\t" + join(config->librarySearchPaths, "\n\t")),
            ctx->e.errs());
    message(StringRef("Framework search paths:") +
                (config->frameworkSearchPaths.empty()
                     ? ""
                     : "\n\t" + join(config->frameworkSearchPaths, "\n\t")),
            ctx->e.errs());
  }

  config->progName = argsArr[0];

  config->timeTraceEnabled = args.hasArg(OPT_time_trace_eq);
  config->timeTraceGranularity =
      args::getInteger(args, OPT_time_trace_granularity_eq, 500);

  // Initialize time trace profiler.
  if (config->timeTraceEnabled)
    timeTraceProfilerInitialize(config->timeTraceGranularity, config->progName);

  {
    TimeTraceScope timeScope("ExecuteLinker");

    initLLVM(); // must be run before any call to addFile()
    // The pipeline outlives createFiles(): autolinked TBDs are started from the
    // object parses and consumed by resolveLCLinkerOptions() below.
    llvm::scope_exit endTapiPipeline([] { finishTapiPipeline(); });
    createFiles(args);

    // Now that all dylibs have been loaded, search for those that should be
    // re-exported.
    {
      auto reexportHandler = [](const Arg *arg,
                                const std::vector<StringRef> &extensions) {
        config->hasReexports = true;
        StringRef searchName = arg->getValue();
        if (!markReexport(searchName, extensions))
          error(arg->getSpelling() + " " + searchName +
                " does not match a supplied dylib");
      };
      std::vector<StringRef> extensions = {".tbd"};
      for (const Arg *arg : args.filtered(OPT_sub_umbrella))
        reexportHandler(arg, extensions);

      extensions.push_back(".dylib");
      for (const Arg *arg : args.filtered(OPT_sub_library))
        reexportHandler(arg, extensions);
    }

    cl::ResetAllOptionOccurrences();

    // Parse LTO options.
    if (const Arg *arg = args.getLastArg(OPT_mcpu))
      parseClangOption(saver().save("-mcpu=" + StringRef(arg->getValue())),
                       arg->getSpelling());

    for (const Arg *arg : args.filtered(OPT_mllvm)) {
      parseClangOption(arg->getValue(), arg->getSpelling());
      config->mllvmOpts.emplace_back(arg->getValue());
    }

    config->passPlugins = args::getStrings(args, OPT_load_pass_plugins);

    createSyntheticSections();
    createSyntheticSymbols();
    addSynthenticMethnames();

    createAliases();
    // If we are in "explicit exports" mode, hide everything that isn't
    // explicitly exported. Do this before running LTO so that LTO can better
    // optimize.
    handleExplicitExports();

    bool didCompileBitcodeFiles = compileBitcodeFiles();

    resolveLCLinkerOptions();

    // If either --thinlto-index-only or --lto-emit-llvm is given, we should
    // not create object files. Index file creation is already done in
    // compileBitcodeFiles, so we are done if that's the case.
    if (config->thinLTOIndexOnly || config->emitLLVM)
      return errorCount() == 0;

    // LTO may emit a non-hidden (extern) object file symbol even if the
    // corresponding bitcode symbol is hidden. In particular, this happens for
    // cross-module references to hidden symbols under ThinLTO. Thus, if we
    // compiled any bitcode files, we must redo the symbol hiding.
    if (didCompileBitcodeFiles)
      handleExplicitExports();
    replaceCommonSymbols();

    StringRef orderFile = args.getLastArgValue(OPT_order_file);
    if (!orderFile.empty())
      priorityBuilder.parseOrderFile(orderFile);

    referenceStubBinder();

    // FIXME: should terminate the link early based on errors encountered so
    // far?

    for (const Arg *arg : args.filtered(OPT_sectcreate)) {
      StringRef segName = arg->getValue(0);
      StringRef sectName = arg->getValue(1);
      StringRef fileName = arg->getValue(2);
      std::optional<MemoryBufferRef> buffer = readFile(fileName);
      if (buffer)
        inputFiles.insert(make<OpaqueFile>(*buffer, segName, sectName));
    }

    for (const Arg *arg : args.filtered(OPT_add_empty_section)) {
      StringRef segName = arg->getValue(0);
      StringRef sectName = arg->getValue(1);
      inputFiles.insert(make<OpaqueFile>(MemoryBufferRef(), segName, sectName));
    }

    parseRegularRelocationsParallel();

    gatherInputSections();

    if (!config->codegenDataGeneratePath.empty())
      codegenDataGenerate();

    if (config->callGraphProfileSort)
      priorityBuilder.extractCallGraphProfile();

    if (config->deadStrip)
      markLive();

    // Ensure that no symbols point inside __mod_init_func sections if they are
    // removed due to -init_offsets. This must run after dead stripping.
    if (config->emitInitOffsets)
      eraseInitializerSymbols();

    // Categories are not subject to dead-strip. The __objc_catlist section is
    // marked as NO_DEAD_STRIP and that propagates into all category data.
    if (args.hasArg(OPT_check_category_conflicts))
      objc::checkCategories();

    // Category merging uses "->live = false" to erase old category data, so
    // it has to run after dead-stripping (markLive).
    if (args.hasFlag(OPT_objc_category_merging, OPT_no_objc_category_merging,
                     false))
      objc::mergeCategories();

    computeColdness();

    // ICF assumes that all literals have been folded already, so we must run
    // foldIdenticalLiterals before foldIdenticalSections.
    foldIdenticalLiterals();
    if (config->icfLevel != ICFLevel::none) {
      if (config->icfLevel == ICFLevel::safe ||
          config->icfLevel == ICFLevel::safe_thunks)
        markAddrSigSymbols();
      foldIdenticalSections(/*onlyCfStrings=*/false);
    } else if (config->dedupStrings) {
      foldIdenticalSections(/*onlyCfStrings=*/true);
    }

    stripSwiftForceLoadFixups();

    // Write to an output file.
    if (target->wordSize == 8)
      writeResult<LP64>();
    else
      writeResult<ILP32>();

    depTracker->write(getLLDVersion(), inputFiles, config->outputFile);
  }

  if (config->timeTraceEnabled) {
    checkError(timeTraceProfilerWrite(
        args.getLastArgValue(OPT_time_trace_eq).str(), config->outputFile));

    timeTraceProfilerCleanup();
  }

  if (errorCount() != 0 || config->strictAutoLink)
    for (const auto &warning : missingAutolinkWarnings)
      warn(warning);

  return errorCount() == 0;
}
} // namespace macho
} // namespace lld
