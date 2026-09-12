//===- InputFiles.h ---------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLD_MACHO_INPUT_FILES_H
#define LLD_MACHO_INPUT_FILES_H

#include "MachOStructs.h"
#include "Target.h"

#include "lld/Common/DWARF.h"
#include "lld/Common/ErrorHandler.h"
#include "lld/Common/LLVM.h"
#include "lld/Common/Memory.h"
#include "llvm/ADT/CachedHashString.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/BinaryFormat/MachO.h"
#include "llvm/DebugInfo/DWARF/DWARFUnit.h"
#include "llvm/Object/Archive.h"
#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/Support/Allocator.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Threading.h"
#include "llvm/TextAPI/TextAPIReader.h"

#include <memory>
#include <vector>

namespace llvm {
namespace lto {
class InputFile;
} // namespace lto
namespace MachO {
class InterfaceFile;
} // namespace MachO
class TarWriter;
} // namespace llvm

namespace lld {
namespace macho {

struct PlatformInfo;
class ConcatInputSection;
class InputSection;
class InputFile;
class Symbol;
class Defined;
class AliasSymbol;
struct Relocation;
enum class RefState : uint8_t;

// Ordinal field widths. The three fields are packed into one 64-bit integer, so
// they have to share 63 bits -- bit 63 carries SlotMeta's deferred flag. Measured
// maxima over the eight replayed corpora at eight threads:
//
//   field | largest seen                                   | width | headroom
//   top   | 11,370 (Chromium debug, 11,372 inputs)         |  17   |  11.5x
//   walk  | 418,243 (Godot iOS: 291,628 index entries plus |  23   |  20x
//         |  126,615 member and undefined events)          |       |
//   seq   | 82,488 (Firefox iOS launcher)                  |  23   |  100x
//
// They are variables rather than constants so that a test can narrow them and
// exercise the overflow paths; nothing changes them after
// initRecordOrdinalWidths() runs, before the first ordinal exists.
extern unsigned ordinalSeqBits, ordinalWalkBits, ordinalTopBits;
extern uint32_t ordinalSeqMax, ordinalWalkMax, ordinalTopMax;
void initRecordOrdinalWidths();
[[noreturn]] void reportOrdinalOverflow(uint32_t top, uint32_t walk,
                                        uint32_t seq);

// A symbol record's position in the historical serial input order: comparing two
// ordinals answers "which of these two records would the serial linker have
// applied first?". It is the lexicographic triple
//
//   top  -- the input's ordinal on the command line;
//   walk -- zero for an object's or a dylib's own records; for the events an
//           archive registration produces, that archive's own monotone walk
//           counter, so that an index entry, the member it extracts and that
//           member's undefined symbols all sort inside the archive's
//           command-line position and before whatever the command line lists
//           after it. The counter advances past the entries the walk does not
//           visit arithmetically, so a skipped entry still has a position
//           without the walk having to touch it (see walkArchives()).
//   seq  -- the record's sequence number within one file's parse.
//
// packed lexicographically into one integer, because the fold pays the key's
// width twice per name in its per-slot header and once per comparison in the
// compaction sort.
struct RecordOrdinal {
  uint64_t v = 0;

  RecordOrdinal() = default;
  RecordOrdinal(uint32_t top, uint32_t walk, uint32_t seq) {
    if (LLVM_UNLIKELY(top > ordinalTopMax || walk > ordinalWalkMax ||
                      seq > ordinalSeqMax))
      reportOrdinalOverflow(top, walk, seq);
    v = (static_cast<uint64_t>(top) << (ordinalWalkBits + ordinalSeqBits)) |
        (static_cast<uint64_t>(walk) << ordinalSeqBits) | seq;
  }

  bool operator<(const RecordOrdinal &o) const { return v < o.v; }
  bool operator==(const RecordOrdinal &o) const { return v == o.v; }
  bool operator!=(const RecordOrdinal &o) const { return v != o.v; }
};

// A definition that lost the fold, recorded so that the effects it has on
// *other* files (moving local aliases into the prevailing section, and removing
// a definition that a lower-ordinal record displaced) can be applied serially
// once every winner is final. See SymbolTable::applyPendingCoalesces().
struct PendingCoalesce {
  RecordOrdinal ordinal;
  ConcatInputSection *isec;
  Symbol *slot;
  uint64_t value;
  // True when this definition had already been constructed into the slot, and
  // therefore into isec->symbols, before another record displaced it. The
  // serial pass removes it, because in the serial order it was either never put
  // there or is about to be rebuilt by the replay.
  bool eraseSlot;
  // True when the section really did lose a coalescing contest, i.e. it must be
  // marked coalesced and its local aliases moved into the prevailing section.
  // False for a record that is merely being rewound for the replay.
  bool coalesce;
};

// A defined-symbol record that the fold could not apply commutatively. These
// are replayed serially, in ordinal order, through the ordinary addDefined()
// body once the fold is complete.
struct DeferredDefinedRecord {
  RecordOrdinal ordinal;
  StringRef name;
  InputFile *file;
  InputSection *isec;
  uint64_t value;
  uint64_t size;
  bool isWeakDef;
  bool isPrivateExtern;
  bool isReferencedDynamically;
  bool noDeadStrip;
  bool isWeakDefCanBeHidden;
  bool isCold;
};

// Per-file state for a parse that runs on a worker thread. Everything a parse
// would otherwise append to a global lives here and is merged back in file
// order by the coordinator.
struct ParseContext {
  // The input's command-line ordinal and, for an extracted member, the walk
  // value its archive's registration gave it.
  uint32_t top = 0;
  uint32_t walk = 0;
  uint32_t seq = 0;
  // An extracted member's records interleave with those of the members it
  // extracts: the serial linker applies the member's defined records, then walks
  // its undefined symbols, and an undefined that triggers an extraction applies
  // the whole extracted member -- recursively -- before moving to the next one.
  // So each of a member's undefined records takes its own position, allocated by
  // the walker where it enumerated them, and the positions of the members they
  // extract fall in between. Empty for a direct object, which extracts nothing
  // and keeps all of its records on one position.
  llvm::SmallVector<uint32_t, 0> undefinedWalks;
  uint32_t undefinedSeq = 0;
  // The ordinal just past everything this file and its extractions occupy.
  RecordOrdinal completion;
  DeferredDiagnostics diags;
  llvm::SmallVector<PendingCoalesce, 0> coalesces;
  llvm::SmallVector<DeferredDefinedRecord, 0> deferredDefined;
  llvm::SmallVector<StringRef, 4> lcLinkerOptions;
  // Files that owned a weak definition which a later strong definition
  // displaces. Their unwind entries must be registered while they still own it.
  llvm::SmallVector<InputFile *, 0> inlineUnwindFiles;

  RecordOrdinal nextOrdinal() { return {top, walk, seq++}; }
  // The ordinal of this file's next undefined record. They are applied in nlist
  // order, which is the order the walker enumerated them in.
  RecordOrdinal nextUndefinedOrdinal() {
    if (undefinedSeq < undefinedWalks.size())
      return {top, undefinedWalks[undefinedSeq++], 0};
    return nextOrdinal();
  }
  RecordOrdinal completionOrdinal() const {
    return completion != RecordOrdinal() ? completion
                                         : RecordOrdinal{top + 1, 0, 0};
  }
};

// If --reproduce option is given, all input files are written
// to this tar archive.
extern std::unique_ptr<llvm::TarWriter> tar;

// If .subsections_via_symbols is set, each InputSection will be split along
// symbol boundaries. The field offset represents the offset of the subsection
// from the start of the original pre-split InputSection.
struct Subsection {
  uint64_t offset = 0;
  InputSection *isec = nullptr;
};

using Subsections = std::vector<Subsection>;
class InputFile;

class Section {
public:
  InputFile *file;
  StringRef segname;
  StringRef name;
  uint32_t flags;
  uint64_t addr;
  Subsections subsections;

  Section(InputFile *file, StringRef segname, StringRef name, uint32_t flags,
          uint64_t addr)
      : file(file), segname(segname), name(name), flags(flags), addr(addr) {}
  // Ensure pointers to Sections are never invalidated.
  Section(const Section &) = delete;
  Section &operator=(const Section &) = delete;
  Section(Section &&) = delete;
  Section &operator=(Section &&) = delete;

private:
  // Whether we have already split this section into individual subsections.
  // For sections that cannot be split (e.g. literal sections), this is always
  // false.
  bool doneSplitting = false;
  friend class ObjFile;
};

// Represents a call graph profile edge.
struct CallGraphEntry {
  // The index of the caller in the symbol table.
  uint32_t fromIndex;
  // The index of the callee in the symbol table.
  uint32_t toIndex;
  // Number of calls from callee to caller in the profile.
  uint64_t count;

  CallGraphEntry(uint32_t fromIndex, uint32_t toIndex, uint64_t count)
      : fromIndex(fromIndex), toIndex(toIndex), count(count) {}
};

class InputFile {
public:
  enum Kind {
    ObjKind,
    OpaqueKind,
    DylibKind,
    ArchiveKind,
    BitcodeKind,
  };

  virtual ~InputFile() = default;
  Kind kind() const { return fileKind; }
  StringRef getName() const { return name; }
  static void resetIdCount() { idCount = 0; }
  bool isCompatible() const { return compatArch; }

  MemoryBufferRef mb;

  std::vector<Symbol *> symbols;
  std::vector<Section *> sections;
  ArrayRef<uint8_t> objCImageInfo;

  // If not empty, this stores the name of the archive containing this file.
  // We use this string for creating error messages.
  std::string archiveName;

  // Provides an easy way to sort InputFiles deterministically.
  const int id;

  // True if this is a lazy ObjFile or BitcodeFile.
  bool lazy = false;

  // Non-null only while this file is being parsed under the parallel input
  // phase. Its presence is what switches the symbol table's add*() entry points
  // from the serial bodies to the ordinal fold.
  ParseContext *parseCtx = nullptr;

protected:
  InputFile(Kind kind, MemoryBufferRef mb, bool lazy = false)
      : mb(mb), id(idCount++), lazy(lazy), fileKind(kind),
        name(mb.getBufferIdentifier()) {}

  InputFile(Kind, const llvm::MachO::InterfaceFile &);

  // If true, this input's arch is compatible with target.
  bool compatArch = true;

private:
  const Kind fileKind;
  const StringRef name;

  static int idCount;
};

struct FDE {
  uint32_t funcLength;
  Symbol *personality;
  InputSection *lsda;
};

// .o file
class ObjFile final : public InputFile {
public:
  ObjFile(MemoryBufferRef mb, uint32_t modTime, StringRef archiveName,
          bool lazy = false, bool forceHidden = false, bool compatArch = true,
          bool builtFromBitcode = false);
  void parseRegularRelocations();
  // Registration of compact-unwind and EH-frame entries reads symbols that
  // other files own, so it cannot run inside a parallel parse. parse() skips it
  // when deferUnwindRegistration is set and registerUnwind() runs it later.
  void registerUnwind();
  ArrayRef<llvm::MachO::data_in_code_entry> getDataInCode() const;
  ArrayRef<uint8_t> getOptimizationHints() const;
  template <class LP> void parse();
  // parse() with the word-size dispatch, for a parse the driver deferred.
  void parseFile();
  template <class LP>
  void parseLinkerOptions(llvm::SmallVectorImpl<StringRef> &LinkerOptions);

  static bool classof(const InputFile *f) { return f->kind() == ObjKind; }

  std::string sourceFile() const;
  // Parses line table information for diagnostics. compileUnit should be used
  // for other purposes.
  lld::DWARFCache *getDwarf();

  llvm::DWARFUnit *compileUnit = nullptr;
  std::unique_ptr<lld::DWARFCache> dwarfCache;
  Section *addrSigSection = nullptr;
  const uint32_t modTime;
  bool forceHidden;
  bool builtFromBitcode;
  std::vector<ConcatInputSection *> debugSections;
  std::vector<CallGraphEntry> callGraph;
  llvm::DenseMap<ConcatInputSection *, FDE> fdes;
  std::vector<AliasSymbol *> aliases;
  // Indices into `symbols` of the non-external Defineds that carry
  // N_NO_DEAD_STRIP, in ascending index order. These are dead-strip roots;
  // recording them while parseSymbols() already has the nlist entry in hand
  // spares markLive() a walk over every symbol slot of every object file, with
  // a dependent Symbol load per slot. See markLive().
  llvm::SmallVector<uint32_t, 0> noDeadStripLocals;
  // Set by the parallel input phase; see registerUnwind().
  bool deferUnwindRegistration = false;

private:
  llvm::once_flag initDwarf;
  template <class LP> void parseRegularRelocations();
  template <class LP> void parseLazy();
  template <class SectionHeader> void parseSections(ArrayRef<SectionHeader>);
  template <class LP>
  void parseSymbols(ArrayRef<typename LP::section> sectionHeaders,
                    ArrayRef<typename LP::nlist> nList, const char *strtab,
                    bool subsectionsViaSymbols);
  template <class NList>
  Symbol *parseNonSectionSymbol(const NList &sym, const char *strtab);
  template <class SectionHeader>
  void parseRelocations(ArrayRef<SectionHeader> sectionHeaders,
                        const SectionHeader &, Section &);
  void parseDebugInfo();
  void splitEhFrames(ArrayRef<uint8_t> dataArr, Section &ehFrameSection);
  void registerCompactUnwind(Section &compactUnwindSection);
  void registerEhFrames(Section &ehFrameSection);
};

// command-line -sectcreate file
class OpaqueFile final : public InputFile {
public:
  OpaqueFile(MemoryBufferRef mb, StringRef segName, StringRef sectName);
  static bool classof(const InputFile *f) { return f->kind() == OpaqueKind; }
};

// .dylib or .tbd file
// Defined in TapiSymbols.h.
struct PreparedTapiSymbols;
struct PreparedLDSymbol;

// Computes the order-independent half of the TAPI DylibFile constructor. Safe
// to call off the ordered input walk.
void prepareTapiSymbols(const llvm::MachO::InterfaceFile &interface,
                        PreparedTapiSymbols &prepared);

class DylibFile final : public InputFile {
public:
  // Mach-O dylibs can re-export other dylibs as sub-libraries, meaning that the
  // symbols in those sub-libraries will be available under the umbrella
  // library's namespace. Those sub-libraries can also have their own
  // re-exports. When loading a re-exported dylib, `umbrella` should be set to
  // the root dylib to ensure symbols in the child library are correctly bound
  // to the root. On the other hand, if a dylib is being directly loaded
  // (through an -lfoo flag), then `umbrella` should be a nullptr.
  explicit DylibFile(MemoryBufferRef mb, DylibFile *umbrella,
                     bool isBundleLoader, bool explicitlyLinked);
  explicit DylibFile(const llvm::MachO::InterfaceFile &interface,
                     DylibFile *umbrella, bool isBundleLoader,
                     bool explicitlyLinked,
                     PreparedTapiSymbols *preparedSymbols = nullptr);
  explicit DylibFile(DylibFile *umbrella);

  void parseLoadCommands(MemoryBufferRef mb);
  void parseReexports(const llvm::MachO::InterfaceFile &interface);
  bool isReferenced() const { return numReferencedSymbols > 0; }
  bool isExplicitlyLinked() const;
  void setExplicitlyLinked() { explicitlyLinked = true; }

  static bool classof(const InputFile *f) { return f->kind() == DylibKind; }

  StringRef installName;
  DylibFile *exportingFile = nullptr;
  DylibFile *umbrella;
  SmallVector<StringRef, 2> rpaths;
  SmallVector<StringRef> allowableClients;
  uint32_t compatibilityVersion = 0;
  uint32_t currentVersion = 0;
  int64_t ordinal = 0; // Ordinal numbering starts from 1, so 0 is a sentinel
  unsigned numReferencedSymbols = 0;
  RefState refState;
  bool reexport = false;
  bool forceNeeded = false;
  bool forceWeakImport = false;
  bool deadStrippable = false;

private:
  bool explicitlyLinked = false; // Access via isExplicitlyLinked().

public:
  // An executable can be used as a bundle loader that will load the output
  // file being linked, and that contains symbols referenced, but not
  // implemented in the bundle. When used like this, it is very similar
  // to a dylib, so we've used the same class to represent it.
  bool isBundleLoader;

  // Synthetic Dylib objects created by $ld$previous symbols in this dylib.
  // Usually empty. These synthetic dylibs won't have synthetic dylibs
  // themselves.
  SmallVector<DylibFile *, 2> extraDylibs;

private:
  DylibFile *getSyntheticDylib(StringRef installName, uint32_t currentVersion,
                               uint32_t compatVersion);

  bool handleLDSymbol(StringRef originalName);
  void applyPreparedLDSymbol(const PreparedLDSymbol &record);
  void handleLDPreviousSymbol(StringRef name, StringRef originalName);
  void handleLDInstallNameSymbol(StringRef name, StringRef originalName);
  void handleLDHideSymbol(StringRef name, StringRef originalName);
  void checkAppExtensionSafety(bool dylibIsAppExtensionSafe) const;
  void parseExportedSymbols(uint32_t offset, uint32_t size);
  void loadReexport(StringRef path, DylibFile *umbrella,
                    const llvm::MachO::InterfaceFile *currentTopLevelTapi);

  llvm::DenseSet<llvm::CachedHashStringRef> hiddenSymbols;

  // Holds the symbol names a worker materialized for this file, when it was
  // built from a prepared interface. The names outlive the InterfaceFile.
  std::unique_ptr<llvm::BumpPtrAllocator> tapiNameAlloc;
};

// .a file
class ArchiveFile final : public InputFile {
public:
  explicit ArchiveFile(std::unique_ptr<llvm::object::Archive> &&file,
                       bool forceHidden);
  void addLazySymbols(bool heldIndex = false);
  void fetch(const llvm::object::Archive::Symbol &);

  // --- parallel input phase ---
  //
  // The walker (see Driver.cpp) decides which members to extract from the
  // archive's index rather than from a LazyArchive symbol per index entry, so
  // the index is read into these two maps on a worker instead.
  struct IndexEntry {
    llvm::CachedHashStringRef name;
    uint64_t childOffset;
  };
  // Every index entry, in index order, and the position the walk gave each one
  // it has passed (UINT32_MAX until then). A symbol an extracted member defines
  // belongs at the position of the earliest index entry that names it, because
  // that is where the serial linker created its slot.
  llvm::SmallVector<IndexEntry, 0> indexEntries;
  // Index entries whose name is in undefined state once the direct objects are
  // resolved, in index order. Computed on a worker; the serial walk visits only
  // these, plus entries a member's undefined symbol schedules.
  llvm::SmallVector<uint32_t, 0> candidateEntries;
  // Set if any member carries an LC_LINKER_OPTION; see fillMemberUndefineds().
  std::atomic<bool> memberLinkerOptions{false};
  // One per index entry while the index is being read, so that the member
  // offsets can be resolved in parallel afterwards; dropped once they are.
  std::vector<llvm::object::Archive::Symbol> indexSymbols;
  // Each member's child offset, and the index entries it provides.
  llvm::DenseMap<uint64_t, llvm::object::Archive::Child> indexChildren;
  llvm::DenseMap<uint64_t, llvm::SmallVector<uint32_t, 2>> memberEntries;
  // Can the walker own this archive's index? Checked at the archive's position
  // on the command line, because the answer decides whether the index is
  // published there or later, so it looks only at the archive's own header and
  // its first member, never at every member.
  bool canWalk();
  // Each member's undefined names, in the order ObjFile::parseSymbols() applies
  // its undefined records. Enumerating them is the serial walk's largest cost,
  // and it depends only on the archive bytes, so the walk has workers do it
  // first: reserveMemberUndefineds() appends one work item per member (serial,
  // one entry per distinct child), and fillMemberUndefineds() fills that
  // member's slot on a worker.
  void reserveMemberUndefineds(
      std::vector<std::pair<ArchiveFile *, uint64_t>> &work);
  bool fillMemberUndefineds(uint64_t childOffset);
  ArrayRef<StringRef> memberUndefineds(uint64_t childOffset) const {
    auto it = undefinedNames.find(childOffset);
    assert(it != undefinedNames.end());
    return it->second;
  }
  // Fill candidateEntries. Read-only on the symbol table; safe on a worker.
  void filterIndex();
  // buildIndex() reads the archive's symbol table in order, which is all the
  // Archive API allows, and records a name and the symbol's coordinates per
  // entry. resolveIndexOffsets() then fills in each entry's member offset --
  // Symbol::getMember() parses a member header, so this is chunked across
  // workers -- and buildMemberMaps() builds the per-member maps from the
  // resolved offsets. Any of them returning false means this archive cannot be
  // walked, and the link falls back to addLazySymbols(). All three are safe on
  // a worker; only the first two may run concurrently for one archive.
  bool buildIndex();
  bool resolveIndexOffsets(uint32_t begin, uint32_t end);
  bool buildMemberMaps();
  bool isFetched(uint64_t childOffset) const { return seen.contains(childOffset); }
  // Publish the index the ordinary way, for a link whose shape the walker does
  // not cover.
  void publishHeldIndex();
  // fetch() split in two for the walker. The serial linker created a member and
  // parsed it -- which extracted the members it needs, in full -- before adding
  // it to inputFiles and reporting it, so a nested extraction is published
  // first even though its records apply later.
  ObjFile *createIndexedMember(uint64_t childOffset, StringRef reason);
  void publishIndexedMember(ObjFile *member, StringRef reason);
  // LLD normally doesn't use Error for error-handling, but the underlying
  // Archive library does, so this is the cleanest way to wrap it.
  Error fetch(const llvm::object::Archive::Child &, StringRef reason);
  const llvm::object::Archive &getArchive() const { return *file; };
  static bool classof(const InputFile *f) { return f->kind() == ArchiveKind; }

private:
  // Keyed by child offset. Every value is created before any worker runs, so the
  // workers only write into their own value and never rehash the map.
  llvm::DenseMap<uint64_t, llvm::SmallVector<StringRef, 0>> undefinedNames;
  Expected<InputFile *> childToObjectFile(const llvm::object::Archive::Child &c,
                                          bool lazy);
  std::unique_ptr<llvm::object::Archive> file;
  // Keep track of children fetched from the archive by tracking
  // which address offsets have been fetched already.
  llvm::DenseSet<uint64_t> seen;
  llvm::DenseSet<uint64_t> seenLazy;
  // Load all symbols with hidden visibility (-load_hidden).
  bool forceHidden;
};

class BitcodeFile final : public InputFile {
public:
  explicit BitcodeFile(MemoryBufferRef mb, StringRef archiveName,
                       uint64_t offsetInArchive, bool lazy = false,
                       bool forceHidden = false, bool compatArch = true);
  static bool classof(const InputFile *f) { return f->kind() == BitcodeKind; }
  void parse();

  std::unique_ptr<llvm::lto::InputFile> obj;
  bool forceHidden;

private:
  void parseLazy();
};

extern llvm::SetVector<InputFile *> inputFiles;
extern llvm::DenseMap<llvm::CachedHashStringRef, MemoryBufferRef> cachedReads;
extern llvm::SmallVector<StringRef> unprocessedLCLinkerOptions;

// While set, a direct (non-lazy, non-archive-member) ObjFile does not parse
// itself on construction; the driver parses the whole batch on workers.
extern bool deferDirectObjParse;

// True only while workers are parsing input files. Allocations made during a
// parse then come from thread-local arenas; outside the region they come from
// the ordinary ones, which keeps the serial phases' allocation locality.
extern bool parallelParseActive;
// While set, a command-line archive does not publish its index as LazyArchive
// symbols; the walker reads the index instead.
extern bool deferArchiveIndex;

// True if the object carries an LC_LINKER_OPTION, read from the load commands
// alone. The parallel input phase asks this before it holds an object, because
// an autolinked library loads after the phase and the archive walker cannot run
// when any input follows it.
bool hasLinkerOptions(MemoryBufferRef mb);

// Calls fn with the name of every external undefined symbol of a Mach-O object
// file, in nlist order, without parsing it. This is what lets the walker decide
// which archive member to extract next without waiting for the parse of the
// member that triggered it.
void forEachUndefinedName(MemoryBufferRef mb,
                          llvm::function_ref<void(StringRef)> fn,
                          bool *sawLinkerOption = nullptr);

// Set by the parallel input phase's walker for the rest of the link. Where the
// serial linker would have left a LazyArchive symbol for an index entry it did
// not extract, the walker leaves nothing, so a name first referenced after the
// phase (from an LC_LINKER_OPTION library, say) asks this hook instead. The
// second argument bounds which archives may answer: a slot that already holds a
// LazyArchive symbol of its own is only overtaken by a walked archive that comes
// earlier on the command line, which is the one the serial linker would have
// fetched from.
extern void (*archiveProviderHook)(StringRef name, uint32_t belowArchiveId);

// Dylib symbols whose weak definition the serial linker would no longer have had
// in the slot when a definition arrived, because an archive index entry had
// replaced it with a LazyArchive symbol first. A definition displacing one of
// these must not set overridesWeakDef, or the output grows a weak-bind entry.
extern bool (*weakOverrideSuppressedHook)(const Symbol *);

std::optional<MemoryBufferRef> readFile(StringRef path);

void extract(InputFile &file, StringRef reason);

namespace detail {

template <class CommandType, class... Types>
std::vector<const CommandType *>
findCommands(const void *anyHdr, size_t maxCommands, Types... types) {
  std::vector<const CommandType *> cmds;
  std::initializer_list<uint32_t> typesList{types...};
  const auto *hdr = reinterpret_cast<const llvm::MachO::mach_header *>(anyHdr);
  const uint8_t *p =
      reinterpret_cast<const uint8_t *>(hdr) + target->headerSize;
  for (uint32_t i = 0, n = hdr->ncmds; i < n; ++i) {
    auto *cmd = reinterpret_cast<const CommandType *>(p);
    if (llvm::is_contained(typesList, cmd->cmd)) {
      cmds.push_back(cmd);
      if (cmds.size() == maxCommands)
        return cmds;
    }
    p += cmd->cmdsize;
  }
  return cmds;
}

} // namespace detail

// anyHdr should be a pointer to either mach_header or mach_header_64
template <class CommandType = llvm::MachO::load_command, class... Types>
const CommandType *findCommand(const void *anyHdr, Types... types) {
  std::vector<const CommandType *> cmds =
      detail::findCommands<CommandType>(anyHdr, 1, types...);
  return cmds.size() ? cmds[0] : nullptr;
}

template <class CommandType = llvm::MachO::load_command, class... Types>
std::vector<const CommandType *> findCommands(const void *anyHdr,
                                              Types... types) {
  return detail::findCommands<CommandType>(anyHdr, 0, types...);
}

std::string replaceThinLTOSuffix(StringRef path);
} // namespace macho

std::string toString(const macho::InputFile *file);
std::string toString(const macho::Section &);
} // namespace lld

#endif
