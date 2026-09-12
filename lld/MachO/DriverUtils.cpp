//===- DriverUtils.cpp ----------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "Config.h"
#include "Driver.h"
#include "InputFiles.h"
#include "TapiSymbols.h"

#include "lld/Common/Args.h"
#include "lld/Common/CommonLinkerContext.h"
#include "lld/Common/Reproduce.h"
#include "llvm/ADT/CachedHashString.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/BinaryFormat/Magic.h"
#include "llvm/LTO/LTO.h"
#include "llvm/Option/Arg.h"
#include "llvm/Option/ArgList.h"
#include "llvm/Option/Option.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Parallel.h"
#include "llvm/Support/Path.h"
#include "llvm/TextAPI/InterfaceFile.h"
#include "llvm/TextAPI/TextAPIReader.h"
#include <condition_variable>
#include <mutex>

using namespace llvm;
using namespace llvm::MachO;
using namespace llvm::opt;
using namespace llvm::sys;
using namespace lld;
using namespace lld::macho;

#define OPTTABLE_CODE
#include "Options.inc"

MachOOptTable::MachOOptTable() : OptTable(optionTables()) {}

// Set color diagnostics according to --color-diagnostics={auto,always,never}
// or --no-color-diagnostics flags.
static void handleColorDiagnostics(CommonLinkerContext &ctx,
                                   InputArgList &args) {
  const Arg *arg =
      args.getLastArg(OPT_color_diagnostics, OPT_color_diagnostics_eq,
                      OPT_no_color_diagnostics);
  if (!arg)
    return;
  auto &errs = ctx.e.errs();
  if (arg->getOption().getID() == OPT_color_diagnostics) {
    errs.enable_colors(true);
  } else if (arg->getOption().getID() == OPT_no_color_diagnostics) {
    errs.enable_colors(false);
  } else {
    StringRef s = arg->getValue();
    if (s == "always")
      errs.enable_colors(true);
    else if (s == "never")
      errs.enable_colors(false);
    else if (s != "auto")
      error("unknown option: --color-diagnostics=" + s);
  }
}

InputArgList MachOOptTable::parse(CommonLinkerContext &ctx,
                                  ArrayRef<const char *> argv) {
  // Make InputArgList from string vectors.
  unsigned missingIndex;
  unsigned missingCount;
  SmallVector<const char *, 256> vec(argv.data(), argv.data() + argv.size());

  // Expand response files (arguments in the form of @<filename>)
  // and then parse the argument again.
  cl::ExpandResponseFiles(saver(), cl::TokenizeGNUCommandLine, vec);
  InputArgList args = ParseArgs(vec, missingIndex, missingCount);

  // Handle -fatal_warnings early since it converts missing argument warnings
  // to errors.
  errorHandler().fatalWarnings = args.hasArg(OPT_fatal_warnings);
  errorHandler().suppressWarnings = args.hasArg(OPT_w);

  if (missingCount)
    error(Twine(args.getArgString(missingIndex)) + ": missing argument");

  handleColorDiagnostics(ctx, args);

  for (const Arg *arg : args.filtered(OPT_UNKNOWN)) {
    std::string nearest;
    if (findNearest(arg->getAsString(args), nearest) > 1)
      error("unknown argument '" + arg->getAsString(args) + "'");
    else
      error("unknown argument '" + arg->getAsString(args) +
            "', did you mean '" + nearest + "'");
  }
  return args;
}

void MachOOptTable::printHelp(CommonLinkerContext &ctx, const char *argv0,
                              bool showHidden) const {
  auto &outs = ctx.e.outs();
  OptTable::printHelp(outs, (std::string(argv0) + " [options] file...").c_str(),
                      "LLVM Linker", showHidden);
  outs << '\n';
}

// If any SDK contains the directory, use those directories in SDK order
// instead of falling back to the host.
SmallVector<StringRef>
macho::getRerootedSearchPaths(StringRef searchPath, ArrayRef<StringRef> roots) {
  SmallVector<StringRef> paths;
  // NOTE: only absolute paths are re-rooted to syslibroot(s)
  if (path::is_absolute(searchPath, path::Style::posix)) {
    for (StringRef root : roots) {
      SmallString<261> buffer(root);
      path::append(buffer, searchPath);
      // Do not warn about paths that are computed via the syslib roots
      if (fs::is_directory(buffer))
        paths.push_back(saver().save(buffer.str()));
    }
  }
  if (paths.empty())
    paths.push_back(searchPath);
  return paths;
}

static std::string rewritePath(StringRef s) {
  if (fs::exists(s))
    return relativeToRoot(s);
  return std::string(s);
}

static std::string rewriteInputPath(StringRef s) {
  // Don't bother rewriting "absolute" paths that are actually under the
  // syslibroot; simply rewriting the syslibroot is sufficient.
  if (rerootPath(s) == s && fs::exists(s))
    return relativeToRoot(s);
  return std::string(s);
}

// Reconstructs command line arguments so that so that you can re-run
// the same command with the same inputs. This is for --reproduce.
std::string macho::createResponseFile(const InputArgList &args) {
  SmallString<0> data;
  raw_svector_ostream os(data);

  // Copy the command line to the output while rewriting paths.
  for (const Arg *arg : args) {
    switch (arg->getOption().getID()) {
    case OPT_reproduce:
      break;
    case OPT_INPUT:
      os << quote(rewriteInputPath(arg->getValue())) << "\n";
      break;
    case OPT_o:
      os << "-o " << quote(path::filename(arg->getValue())) << "\n";
      break;
    case OPT_filelist:
      if (std::optional<MemoryBufferRef> buffer = readFile(arg->getValue()))
        for (StringRef path : args::getLines(*buffer))
          os << quote(rewriteInputPath(path)) << "\n";
      break;
    case OPT_force_load:
    case OPT_weak_library:
    case OPT_load_hidden:
      os << arg->getSpelling() << " "
         << quote(rewriteInputPath(arg->getValue())) << "\n";
      break;
    case OPT_F:
    case OPT_L:
      // Resolve SDK prefixes before making search paths relative: relative
      // search paths are not rerooted when the reproducer is replayed.
      for (StringRef path :
           getRerootedSearchPaths(arg->getValue(), config->systemLibraryRoots))
        os << arg->getSpelling() << " " << quote(rewritePath(path)) << "\n";
      break;
    case OPT_non_global_symbols_strip_list:
    case OPT_non_global_symbols_no_strip_list:
    case OPT_bundle_loader:
    case OPT_exported_symbols_list:
    case OPT_order_file:
    case OPT_syslibroot:
    case OPT_unexported_symbols_list:
      os << arg->getSpelling() << " " << quote(rewritePath(arg->getValue()))
         << "\n";
      break;
    case OPT_sectcreate:
      os << arg->getSpelling() << " " << quote(arg->getValue(0)) << " "
         << quote(arg->getValue(1)) << " "
         << quote(rewritePath(arg->getValue(2))) << "\n";
      break;
    default:
      os << toString(*arg) << "\n";
    }
  }
  return std::string(data);
}

static void searchedDylib(const Twine &path, bool found) {
  if (config->printDylibSearch)
    message("searched " + path + (found ? ", found " : ", not found"));
  if (!found)
    depTracker->logFileNotFound(path);
}

std::optional<StringRef> macho::resolveDylibPath(StringRef dylibPath) {
  // TODO: if a tbd and dylib are both present, we should check to make sure
  // they are consistent.
  SmallString<261> tbdPath = dylibPath;
  path::replace_extension(tbdPath, ".tbd");
  bool tbdExists = fs::exists(tbdPath);
  searchedDylib(tbdPath, tbdExists);
  if (tbdExists)
    return saver().save(tbdPath.str());

  bool dylibExists = fs::exists(dylibPath);
  searchedDylib(dylibPath, dylibExists);
  if (dylibExists)
    return saver().save(dylibPath);
  return {};
}

// It's not uncommon to have multiple attempts to load a single dylib,
// especially if it's a commonly re-exported core library.
static DenseMap<CachedHashStringRef, DylibFile *> loadedDylibs;

static DenseMap<CachedHashStringRef, PreparedTapiFile *> preparedTapiFiles;

macho::PreparedTapiFile::PreparedTapiFile(MemoryBufferRef buffer)
    : buffer(buffer), path(buffer.getBufferIdentifier()) {}
macho::PreparedTapiFile::PreparedTapiFile(PreparedTapiFile &&) = default;
macho::PreparedTapiFile &
macho::PreparedTapiFile::operator=(PreparedTapiFile &&) = default;
macho::PreparedTapiFile::~PreparedTapiFile() = default;

static void prepareTapiFile(PreparedTapiFile *file) {
  Expected<std::unique_ptr<InterfaceFile>> result =
      TextAPIReader::get(file->buffer);
  if (result) {
    file->interfaceFile = std::move(*result);
    file->symbols = std::make_unique<PreparedTapiSymbols>();
    prepareTapiSymbols(*file->interfaceFile, *file->symbols);
  } else
    file->error = toString(result.takeError());
}

void macho::prepareTapiFiles(ArrayRef<MemoryBufferRef> buffers,
                             std::vector<PreparedTapiFile> &prepared) {
  prepared.reserve(buffers.size());
  for (MemoryBufferRef buffer : buffers) {
    CachedHashStringRef path(buffer.getBufferIdentifier());
    auto it = loadedDylibs.find(path);
    // loadDylib() will reuse the existing file, but the deferred entry still
    // needs to be consumed to apply its metadata in the original order.
    if (it != loadedDylibs.end() && it->second)
      continue;
    prepared.emplace_back(buffer);
  }

  SmallVector<PreparedTapiFile *> files;
  for (PreparedTapiFile &file : prepared)
    files.push_back(&file);
  llvm::parallelForEach(files, prepareTapiFile);
}

void macho::setPreparedTapiFiles(
    ArrayRef<PreparedTapiFile *> preparedTapiFiles) {
  assert(::preparedTapiFiles.empty());
  for (PreparedTapiFile *file : preparedTapiFiles)
    ::preparedTapiFiles.insert(
        std::make_pair(CachedHashStringRef(file->path), file));
}

void macho::clearPreparedTapiFiles() { ::preparedTapiFiles.clear(); }

//===----------------------------------------------------------------------===//
// Pipelined TAPI preparation
//
// TBD parsing is a large, order-independent part of initial loading. The
// pipeline starts those parses before the ordered input walk and overlaps them
// with it. Preparation never reads the symbol table, never saves a string and
// never emits a diagnostic: the ordered walk still reads, diagnoses and
// publishes every input in its original order, and simply picks up an already
// parsed InterfaceFile when one is ready.
//===----------------------------------------------------------------------===//

namespace {
struct TapiPipelineState {
  // Entries keyed by the path loadDylib() looks up. unique_ptr keeps addresses
  // stable while workers hold them.
  llvm::DenseMap<CachedHashStringRef, std::unique_ptr<PreparedTapiFile>> files;
  std::mutex mutex;
  std::condition_variable cv;
  // Declared last so that it is destroyed first: the group joins its workers
  // while the state they use is still alive.
  llvm::parallel::TaskGroup group;
};
} // namespace

static std::unique_ptr<TapiPipelineState> tapiPipeline;

static void runTapiPipelineTask(TapiPipelineState *state,
                                PreparedTapiFile *file);

// Registers one already-read TBD. Returns null when the path is already
// pipelined. \p ownPath copies the path, for callers whose path is transient.
static PreparedTapiFile *addPipelinedTapiFile(TapiPipelineState *state,
                                              StringRef path, StringRef content,
                                              std::unique_ptr<MemoryBuffer> owned,
                                              bool ownPath) {
  auto entry = std::make_unique<PreparedTapiFile>(MemoryBufferRef());
  if (ownPath) {
    entry->pathStorage = path.str();
    path = entry->pathStorage;
  }
  entry->path = path;
  entry->buffer = MemoryBufferRef(content, path);
  entry->ownedBuffer = std::move(owned);
  entry->done = false;
  PreparedTapiFile *file = entry.get();

  std::lock_guard<std::mutex> lock(state->mutex);
  if (!state->files.try_emplace(CachedHashStringRef(path), std::move(entry))
           .second)
    return nullptr;
  return file;
}

// A silent mirror of resolveDylibPath(): it must not log a search, must not
// save a string and must not touch the dependency tracker, because the ordered
// findDylib() still performs the real, diagnosed lookup. A miss here only means
// that this re-export is not prepared.
static bool probeDylibPath(const Twine &candidate, SmallVectorImpl<char> &out) {
  SmallString<261> buf;
  StringRef path = candidate.toStringRef(buf);
  SmallString<261> tbdPath = path;
  llvm::sys::path::replace_extension(tbdPath, ".tbd");
  if (fs::exists(tbdPath)) {
    out.assign(tbdPath.begin(), tbdPath.end());
    return true;
  }
  if (fs::exists(path)) {
    out.assign(path.begin(), path.end());
    return true;
  }
  return false;
}

// A silent mirror of the on-disk half of findDylib()'s search order. Install
// names which need the not-yet-constructed umbrella (@loader_path, @rpath,
// @executable_path) and documents inlined in a parent TBD are not prepared.
static bool probeReexportPath(StringRef path, SmallVectorImpl<char> &out) {
  if (path.contains('@'))
    return false;

  // 1. Install name basename in -F / -L directories.
  StringRef stem = llvm::sys::path::stem(path);
  SmallString<128> frameworkName("/");
  frameworkName += stem;
  frameworkName += ".framework/";
  size_t i = path.rfind(frameworkName);
  if (i != StringRef::npos) {
    StringRef frameworkPath = path.substr(i + 1);
    for (StringRef dir : config->frameworkSearchPaths) {
      SmallString<261> candidate = dir;
      llvm::sys::path::append(candidate, frameworkPath);
      if (probeDylibPath(candidate.str(), out))
        return true;
    }
  } else {
    for (StringRef dir : config->librarySearchPaths)
      for (StringRef ext : {".tbd", ".dylib", ".so"}) {
        SmallString<261> candidate = dir;
        llvm::sys::path::append(candidate, stem);
        candidate += ext;
        if (fs::exists(candidate)) {
          out.assign(candidate.begin(), candidate.end());
          return true;
        }
      }
  }

  // 2. As absolute path.
  if (llvm::sys::path::is_absolute(path, llvm::sys::path::Style::posix))
    for (StringRef root : config->systemLibraryRoots)
      if (probeDylibPath(root + path, out))
        return true;

  return false;
}

// Re-exports are the larger half of the initial TBD parsing work, and they are
// only known once the parent is parsed, so the worker resolves and spawns them
// itself. The ordered parseReexports() still recurses in its original order.
static void pipelineReexports(TapiPipelineState *state,
                              const InterfaceFile &interface) {
  for (const llvm::MachO::InterfaceFileRef &ref :
       interface.reexportedLibraries()) {
    SmallString<261> resolved;
    if (!probeReexportPath(ref.getInstallName(), resolved))
      continue;
    ErrorOr<std::unique_ptr<MemoryBuffer>> mbOrErr =
        MemoryBuffer::getFile(resolved, /*IsText=*/false,
                              /*RequiresNullTerminator=*/false);
    if (!mbOrErr)
      continue;
    MemoryBufferRef mbref = (*mbOrErr)->getMemBufferRef();
    if (identify_magic(mbref.getBuffer()) != file_magic::tapi_file)
      continue;
    StringRef content = mbref.getBuffer();
    if (PreparedTapiFile *child =
            addPipelinedTapiFile(state, resolved, content,
                                 std::move(*mbOrErr), /*ownPath=*/true))
      state->group.spawn([state, child] { runTapiPipelineTask(state, child); });
  }
}

static void runTapiPipelineTask(TapiPipelineState *state,
                                PreparedTapiFile *file) {
  prepareTapiFile(file);
  if (file->interfaceFile)
    pipelineReexports(state, *file->interfaceFile);
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    file->done = true;
  }
  state->cv.notify_all();
}

void macho::startTapiPipeline() {
  assert(!tapiPipeline);
  tapiPipeline = std::make_unique<TapiPipelineState>();
}

void macho::pipelineTapiFile(MemoryBufferRef buffer) {
  TapiPipelineState *state = tapiPipeline.get();
  assert(state);
  if (PreparedTapiFile *file = addPipelinedTapiFile(
          state, buffer.getBufferIdentifier(), buffer.getBuffer(),
          /*owned=*/nullptr, /*ownPath=*/false))
    state->group.spawn([state, file] { runTapiPipelineTask(state, file); });
}

void macho::finishTapiPipeline() {
  // ~TapiPipelineState destroys the task group first, joining every worker
  // before the state they use goes away.
  tapiPipeline.reset();
}

// Returns the prepared entry for \p path, waiting for a pipelined parse which
// is still running, or null if there is none. A pipelined entry is owned by the
// pipeline, so its ownership is handed to \p owner: the caller's use of the
// entry outlives its removal from the map, and nothing may point into it after
// that scope ends.
static PreparedTapiFile *
takePreparedTapiFile(CachedHashStringRef path,
                     std::unique_ptr<PreparedTapiFile> &owner) {
  if (TapiPipelineState *state = tapiPipeline.get()) {
    std::unique_lock<std::mutex> lock(state->mutex);
    auto it = state->files.find(path);
    if (it != state->files.end()) {
      // The entry itself is stable, because only this function removes one and
      // unique_ptr keeps its address fixed.
      PreparedTapiFile *file = it->second.get();
      state->cv.wait(lock, [file] { return file->done; });
      // The wait released the mutex, so a worker discovering re-exports may have
      // inserted into the map and rehashed it. Look the entry up again rather
      // than erasing through an iterator from before the wait.
      it = state->files.find(path);
      assert(it != state->files.end());
      owner = std::move(it->second);
      state->files.erase(it);
      return file;
    }
  }

  // Entries prepared by the read workers are owned by their caller.
  auto it = ::preparedTapiFiles.find(path);
  if (it == ::preparedTapiFiles.end())
    return nullptr;
  PreparedTapiFile *file = it->second;
  ::preparedTapiFiles.erase(it);
  return file;
}

static StringRef realPathIfDifferent(StringRef path) {
  SmallString<128> realPathBuf;
  if (fs::real_path(path, realPathBuf))
    return StringRef();

  SmallString<128> absPathBuf = path;
  if (!fs::make_absolute(absPathBuf) && realPathBuf == absPathBuf)
    return StringRef();

  return uniqueSaver().save(StringRef(realPathBuf));
}

DylibFile *macho::loadDylib(MemoryBufferRef mbref, DylibFile *umbrella,
                            bool isBundleLoader, bool explicitlyLinked) {
  CachedHashStringRef path(mbref.getBufferIdentifier());
  DylibFile *&file = loadedDylibs[path];
  if (file) {
    if (explicitlyLinked)
      file->setExplicitlyLinked();
    return file;
  }

  // Frameworks can be found from different symlink paths, so resolve
  // symlinks and look up in the dylib cache.
  CachedHashStringRef realPath(
      realPathIfDifferent(mbref.getBufferIdentifier()));
  if (!realPath.val().empty()) {
    // Avoid map insertions here so that we do not invalidate the "file"
    // reference.
    auto it = loadedDylibs.find(realPath);
    if (it != loadedDylibs.end()) {
      DylibFile *realfile = it->second;
      if (explicitlyLinked)
        realfile->setExplicitlyLinked();
      return realfile;
    }
  }

  DylibFile *newFile;
  file_magic magic = identify_magic(mbref.getBuffer());
  if (magic == file_magic::tapi_file) {
    std::unique_ptr<InterfaceFile> interfaceFile;
    const PreparedTapiSymbols *preparedSymbols = nullptr;
    // Keeps a pipelined entry, and so the symbol vectors pointing into its
    // InterfaceFile, alive for as long as preparedSymbols is used below.
    std::unique_ptr<PreparedTapiFile> preparedOwner;
    if (PreparedTapiFile *prepared =
            takePreparedTapiFile(path, preparedOwner)) {
      if (prepared->error) {
        error("could not load TAPI file at " + mbref.getBufferIdentifier() +
              ": " + *prepared->error);
        return nullptr;
      }
      interfaceFile = std::move(prepared->interfaceFile);
      preparedSymbols = prepared->symbols.get();
    } else {
      Expected<std::unique_ptr<InterfaceFile>> result = TextAPIReader::get(mbref);
      if (!result) {
        error("could not load TAPI file at " + mbref.getBufferIdentifier() +
              ": " + toString(result.takeError()));
        return nullptr;
      }
      interfaceFile = std::move(*result);
    }
    if (!interfaceFile) {
      error("could not load TAPI file at " + mbref.getBufferIdentifier() +
            ": empty prepared TAPI file");
      return nullptr;
    }
    file = make<DylibFile>(*interfaceFile, umbrella, isBundleLoader,
                           explicitlyLinked, preparedSymbols);

    // parseReexports() can recursively call loadDylib(). That's fine since
    // we wrote the DylibFile we just loaded to the loadDylib cache via the
    // `file` reference. But the recursive load can grow loadDylibs, so the
    // `file` reference might become invalid after parseReexports() -- so copy
    // the pointer it refers to before continuing.
    newFile = file;
    if (newFile->exportingFile)
      newFile->parseReexports(*interfaceFile);
  } else {
    assert(magic == file_magic::macho_dynamically_linked_shared_lib ||
           magic == file_magic::macho_dynamically_linked_shared_lib_stub ||
           magic == file_magic::macho_executable ||
           magic == file_magic::macho_bundle);
    file = make<DylibFile>(mbref, umbrella, isBundleLoader, explicitlyLinked);

    // parseLoadCommands() can also recursively call loadDylib(). See comment
    // in previous block for why this means we must copy `file` here.
    newFile = file;
    if (newFile->exportingFile)
      newFile->parseLoadCommands(mbref);
  }

  if (explicitlyLinked && !newFile->allowableClients.empty()) {
    bool allowed =
        llvm::any_of(newFile->allowableClients, [&](StringRef allowableClient) {
          // We only do a prefix match to match LD64's behaviour.
          return allowableClient.starts_with(config->clientName);
        });

    // TODO: This behaviour doesn't quite match the latest available source
    // release of LD64 (ld64-951.9), which allows "parents" and "siblings"
    // to link to libraries even when they're not explicitly named as
    // allowable clients. However, behaviour around this seems to have
    // changed in the latest release of Xcode (ld64-1115.7.3), so it's not
    // clear what the correct thing to do is yet.
    if (!allowed)
      error("cannot link directly with '" +
            sys::path::filename(newFile->installName) + "' because " +
            config->clientName + " is not an allowed client");
  }

  // If the load path was a symlink, cache the real path too.
  if (!realPath.val().empty())
    loadedDylibs[realPath] = newFile;

  return newFile;
}

void macho::resetLoadedDylibs() {
  loadedDylibs.clear();
  preparedTapiFiles.clear();
  finishTapiPipeline();
}

std::optional<StringRef>
macho::findPathCombination(const Twine &name,
                           const std::vector<StringRef> &roots,
                           ArrayRef<StringRef> extensions) {
  SmallString<261> base;
  for (StringRef dir : roots) {
    base = dir;
    path::append(base, name);
    for (StringRef ext : extensions) {
      Twine location = base + ext;
      bool exists = fs::exists(location);
      searchedDylib(location, exists);
      if (exists)
        return saver().save(location.str());
    }
  }
  return {};
}

StringRef macho::rerootPath(StringRef path) {
  if (!path::is_absolute(path, path::Style::posix) || path.ends_with(".o"))
    return path;

  if (std::optional<StringRef> rerootedPath =
          findPathCombination(path, config->systemLibraryRoots))
    return *rerootedPath;

  return path;
}

uint32_t macho::getModTime(StringRef path) {
  if (config->zeroModTime)
    return 0;

  fs::file_status stat;
  if (!fs::status(path, stat))
    if (fs::exists(stat))
      return toTimeT(stat.getLastModificationTime());

  warn("failed to get modification time of " + path);
  return 0;
}

void macho::printArchiveMemberLoad(StringRef reason, const InputFile *f) {
  if (config->printEachFile)
    message(toString(f));
  if (config->printWhyLoad)
    message(reason + " forced load of " + toString(f));
}

macho::DependencyTracker::DependencyTracker(StringRef path)
    : path(path), active(!path.empty()) {
  if (active && fs::exists(path) && !fs::can_write(path)) {
    warn("Ignoring dependency_info option since specified path is not "
         "writeable.");
    active = false;
  }
}

void macho::DependencyTracker::write(StringRef version,
                                     const SetVector<InputFile *> &inputs,
                                     StringRef output) {
  if (!active)
    return;

  std::error_code ec;
  raw_fd_ostream os(path, ec, fs::OF_None);
  if (ec) {
    warn("Error writing dependency info to file");
    return;
  }

  auto addDep = [&os](DepOpCode opcode, const StringRef &path) {
    // XXX: Even though DepOpCode's underlying type is uint8_t,
    // this cast is still needed because Clang older than 10.x has a bug,
    // where it doesn't know to cast the enum to its underlying type.
    // Hence `<< DepOpCode` is ambiguous to it.
    os << static_cast<uint8_t>(opcode);
    os << path;
    os << '\0';
  };

  addDep(DepOpCode::Version, version);

  // Sort the input by its names.
  std::vector<StringRef> inputNames;
  inputNames.reserve(inputs.size());
  for (InputFile *f : inputs)
    inputNames.push_back(f->getName());
  llvm::sort(inputNames);

  for (const StringRef &in : inputNames)
    addDep(DepOpCode::Input, in);

  for (const std::string &f : notFounds)
    addDep(DepOpCode::NotFound, f);

  addDep(DepOpCode::Output, output);
}
