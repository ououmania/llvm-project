//===--- PreambleCache.cpp - Persistent preamble PCH cache ----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "PreambleCache.h"
#include "index/SymbolID.h"
#include "support/Logger.h"
#include "clang/Tooling/Inclusions/StandardLibrary.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/SHA1.h"
#include "llvm/Support/raw_ostream.h"
#include <string>
#include <vector>

namespace clang {
namespace clangd {

//===----------------------------------------------------------------------===//
// Hash utilities (file-local).
//===----------------------------------------------------------------------===//

static std::string hashForCache(llvm::StringRef Content) {
  auto Digest = llvm::SHA1::hash(llvm::arrayRefFromStringRef(Content));
  return llvm::toHex(Digest).substr(0, 16);
}

static std::string normalizePathForCache(llvm::StringRef Path) {
  llvm::SmallString<256> Normalized(Path);
  llvm::sys::path::remove_dots(Normalized, /*remove_dot_dot=*/true);
  return Normalized.str().lower();
}

static std::string
normalizeCompileCommand(const tooling::CompileCommand &Cmd) {
  static constexpr llvm::StringRef OutputFlags[] = {"-o", "-MF", "-MT", "-MQ"};
  std::string Result;
  Result.append(Cmd.Directory);
  Result.push_back('\0');

  std::vector<std::string> SemanticFlags;
  for (size_t I = 0; I < Cmd.CommandLine.size(); ++I) {
    llvm::StringRef Arg = Cmd.CommandLine[I];
    bool Skip = false;
    for (auto Flag : OutputFlags) {
      if (Arg == Flag) {
        ++I;
        Skip = true;
        break;
      }
      if (Arg.starts_with(Flag) && Arg.size() > Flag.size()) {
        Skip = true;
        break;
      }
    }
    if (!Skip && Arg != Cmd.Filename)
      SemanticFlags.push_back(Arg.str());
  }
  llvm::sort(SemanticFlags);
  for (const auto &F : SemanticFlags) {
    Result.append(F);
    Result.push_back('\0');
  }
  return Result;
}

//===----------------------------------------------------------------------===//
// Binary write helpers (matching index/Serialization.cpp style).
//===----------------------------------------------------------------------===//

static void write32(uint32_t V, llvm::raw_ostream &OS) {
  char Buf[4];
  llvm::support::endian::write32le(Buf, V);
  OS.write(Buf, 4);
}

static void write64(uint64_t V, llvm::raw_ostream &OS) {
  char Buf[8];
  llvm::support::endian::write64le(Buf, V);
  OS.write(Buf, 8);
}

static void writeStr(llvm::StringRef S, llvm::raw_ostream &OS) {
  write32(static_cast<uint32_t>(S.size()), OS);
  OS.write(S.data(), S.size());
}

static void writeRange(const Range &R, llvm::raw_ostream &OS) {
  write32(static_cast<uint32_t>(R.start.line), OS);
  write32(static_cast<uint32_t>(R.start.character), OS);
  write32(static_cast<uint32_t>(R.end.line), OS);
  write32(static_cast<uint32_t>(R.end.character), OS);
}

static void writeMacroOccurrence(const MacroOccurrence &Occ,
                                 llvm::raw_ostream &OS) {
  write64(static_cast<uint64_t>(Occ.StartOffset), OS);
  write64(static_cast<uint64_t>(Occ.EndOffset), OS);
  uint8_t Flags =
      (Occ.IsDefinition ? 1 : 0) | (Occ.InConditionalDirective ? 2 : 0);
  OS.write(static_cast<char>(Flags));
}

//===----------------------------------------------------------------------===//
// Compound write functions for sidecar types.
//===----------------------------------------------------------------------===//

static void writeMarks(const std::vector<PragmaMark> &Marks,
                       llvm::raw_ostream &OS) {
  write32(static_cast<uint32_t>(Marks.size()), OS);
  for (const auto &M : Marks) {
    writeRange(M.Rng, OS);
    writeStr(M.Trivia, OS);
  }
}

static void writeMacros(const MainFileMacros &Macros, llvm::raw_ostream &OS) {
  write32(static_cast<uint32_t>(Macros.Names.size()), OS);
  for (const auto &N : Macros.Names)
    writeStr(N.getKey(), OS);

  write32(static_cast<uint32_t>(Macros.MacroRefs.size()), OS);
  for (const auto &[ID, Occs] : Macros.MacroRefs) {
    OS.write(reinterpret_cast<const char *>(ID.raw().data()), 8);
    write32(static_cast<uint32_t>(Occs.size()), OS);
    for (const auto &Occ : Occs)
      writeMacroOccurrence(Occ, OS);
  }

  write32(static_cast<uint32_t>(Macros.UnknownMacros.size()), OS);
  for (const auto &Occ : Macros.UnknownMacros)
    writeMacroOccurrence(Occ, OS);

  write32(static_cast<uint32_t>(Macros.SkippedRanges.size()), OS);
  for (const auto &R : Macros.SkippedRanges)
    writeRange(R, OS);
}

static void writeIncludes(const IncludeStructure &Includes,
                          llvm::raw_ostream &OS) {
  const auto &RPN = Includes.allHeaders();
  write32(static_cast<uint32_t>(RPN.size()), OS);
  for (const auto &P : RPN)
    writeStr(P, OS);

  write32(static_cast<uint32_t>(Includes.MainFileIncludes.size()), OS);
  for (const auto &Inc : Includes.MainFileIncludes) {
    OS.write(static_cast<char>(static_cast<uint8_t>(Inc.Directive)));
    writeStr(Inc.Written, OS);
    writeStr(Inc.Resolved, OS);
    write32(Inc.HashOffset, OS);
    write32(static_cast<uint32_t>(Inc.HashLine), OS);
    OS.write(static_cast<char>(static_cast<uint8_t>(Inc.FileKind)));
    OS.write(static_cast<char>(Inc.HeaderID.has_value() ? 1 : 0));
    if (Inc.HeaderID.has_value())
      write32(static_cast<uint32_t>(*Inc.HeaderID), OS);
  }

  write32(static_cast<uint32_t>(Includes.SearchPathsCanonical.size()), OS);
  for (const auto &P : Includes.SearchPathsCanonical)
    writeStr(P, OS);

  write32(static_cast<uint32_t>(Includes.IncludeChildren.size()), OS);
  for (const auto &[Parent, Children] : Includes.IncludeChildren) {
    write32(static_cast<uint32_t>(Parent), OS);
    write32(static_cast<uint32_t>(Children.size()), OS);
    for (auto Child : Children)
      write32(static_cast<uint32_t>(Child), OS);
  }
}

//===----------------------------------------------------------------------===//
// Reader (matching index/Serialization.cpp style).
//===----------------------------------------------------------------------===//

namespace {
class Reader {
  const char *Begin, *End;
  bool Err = false;

public:
  Reader(llvm::StringRef Data) : Begin(Data.begin()), End(Data.end()) {}
  bool err() const { return Err; }
  bool eof() const { return Begin == End || Err; }

  uint8_t consume8() {
    if (LLVM_UNLIKELY(Begin == End)) {
      Err = true;
      return 0;
    }
    return *Begin++;
  }

  uint32_t consume32() {
    if (LLVM_UNLIKELY(Begin + 4 > End)) {
      Err = true;
      return 0;
    }
    auto Ret = llvm::support::endian::read32le(Begin);
    Begin += 4;
    return Ret;
  }

  uint64_t consume64() {
    if (LLVM_UNLIKELY(Begin + 8 > End)) {
      Err = true;
      return 0;
    }
    auto Ret = llvm::support::endian::read64le(Begin);
    Begin += 8;
    return Ret;
  }

  llvm::StringRef consume(int N) {
    if (LLVM_UNLIKELY(Begin + N > End)) {
      Err = true;
      return {};
    }
    llvm::StringRef Ret(Begin, N);
    Begin += N;
    return Ret;
  }

  llvm::StringRef consumeStr() {
    uint32_t Len = consume32();
    return err() ? llvm::StringRef() : consume(Len);
  }

  SymbolID consumeID() {
    llvm::StringRef Raw = consume(SymbolID::RawSize);
    return err() ? SymbolID() : SymbolID::fromRaw(Raw);
  }
};
} // namespace

//===----------------------------------------------------------------------===//
// Compound read functions for sidecar types.
//===----------------------------------------------------------------------===//

static Range readRange(Reader &R) {
  Range Rng;
  Rng.start.line = static_cast<int>(R.consume32());
  Rng.start.character = static_cast<int>(R.consume32());
  Rng.end.line = static_cast<int>(R.consume32());
  Rng.end.character = static_cast<int>(R.consume32());
  return Rng;
}

static MacroOccurrence readMacroOccurrence(Reader &R) {
  MacroOccurrence Occ;
  Occ.StartOffset = static_cast<size_t>(R.consume64());
  Occ.EndOffset = static_cast<size_t>(R.consume64());
  uint8_t Flags = R.consume8();
  Occ.IsDefinition = (Flags & 1) != 0;
  Occ.InConditionalDirective = (Flags & 2) != 0;
  return Occ;
}

static std::vector<PragmaMark> readMarks(Reader &R) {
  uint32_t Count = R.consume32();
  std::vector<PragmaMark> Marks(Count);
  for (auto &M : Marks) {
    M.Rng = readRange(R);
    M.Trivia = R.consumeStr().str();
  }
  return Marks;
}

static MainFileMacros readMacros(Reader &R) {
  MainFileMacros Macros;
  uint32_t NamesCount = R.consume32();
  for (uint32_t I = 0; I < NamesCount; ++I)
    Macros.Names.insert(R.consumeStr());

  uint32_t RefsCount = R.consume32();
  for (uint32_t I = 0; I < RefsCount; ++I) {
    SymbolID ID = R.consumeID();
    uint32_t OccsCount = R.consume32();
    auto &Occs = Macros.MacroRefs[ID];
    Occs.resize(OccsCount);
    for (auto &Occ : Occs)
      Occ = readMacroOccurrence(R);
  }

  uint32_t UnknownCount = R.consume32();
  Macros.UnknownMacros.resize(UnknownCount);
  for (auto &Occ : Macros.UnknownMacros)
    Occ = readMacroOccurrence(R);

  uint32_t SkippedCount = R.consume32();
  Macros.SkippedRanges.resize(SkippedCount);
  for (auto &Rng : Macros.SkippedRanges)
    Rng = readRange(R);

  return Macros;
}

static IncludeStructure readIncludes(Reader &R) {
  IncludeStructure IS;

  uint32_t RPNCount = R.consume32();
  auto &RealPathNames = PreambleCache::includeRealPathNames(IS);
  RealPathNames.resize(RPNCount);
  for (uint32_t I = 0; I < RPNCount; ++I)
    RealPathNames[I] = R.consumeStr().str();

  uint32_t IncCount = R.consume32();
  IS.MainFileIncludes.resize(IncCount);
  for (auto &Inc : IS.MainFileIncludes) {
    Inc.Directive = static_cast<tok::PPKeywordKind>(R.consume8());
    Inc.Written = R.consumeStr().str();
    Inc.Resolved = R.consumeStr().str();
    Inc.HashOffset = R.consume32();
    Inc.HashLine = static_cast<int>(R.consume32());
    Inc.FileKind = static_cast<SrcMgr::CharacteristicKind>(R.consume8());
    bool HasHeaderID = R.consume8() != 0;
    if (HasHeaderID)
      Inc.HeaderID = static_cast<unsigned>(R.consume32());
  }

  uint32_t SearchCount = R.consume32();
  IS.SearchPathsCanonical.resize(SearchCount);
  for (auto &P : IS.SearchPathsCanonical)
    P = R.consumeStr().str();

  uint32_t ChildrenCount = R.consume32();
  for (uint32_t I = 0; I < ChildrenCount; ++I) {
    auto Parent =
        static_cast<IncludeStructure::HeaderID>(R.consume32());
    uint32_t NumChildren = R.consume32();
    auto &Children = IS.IncludeChildren[Parent];
    Children.resize(NumChildren);
    for (auto &C : Children)
      C = static_cast<IncludeStructure::HeaderID>(R.consume32());
  }

  // Rebuild derived data structures that aren't serialized.
  // UIDToIndex: stat each RealPathName to get its UniqueID.
  auto &UIDToIndex = PreambleCache::includeUIDToIndex(IS);
  for (unsigned I = 1; I < RealPathNames.size(); ++I) {
    if (RealPathNames[I].empty())
      continue;
    llvm::sys::fs::file_status St;
    if (!llvm::sys::fs::status(RealPathNames[I], St))
      UIDToIndex[St.getUniqueID()] =
          static_cast<IncludeStructure::HeaderID>(I);
  }

  // MainFileIncludesBySpelling: rebuild index from MainFileIncludes.
  auto &BySpelling = PreambleCache::includesBySpelling(IS);
  for (unsigned I = 0; I < IS.MainFileIncludes.size(); ++I)
    BySpelling[IS.MainFileIncludes[I].Written].push_back(I);

  return IS;
}

static void writeUniqueID(llvm::sys::fs::UniqueID ID, llvm::raw_ostream &OS) {
  write64(ID.getDevice(), OS);
  write64(ID.getFile(), OS);
}

static llvm::sys::fs::UniqueID readUniqueID(Reader &R) {
  uint64_t Dev = R.consume64();
  uint64_t File = R.consume64();
  return llvm::sys::fs::UniqueID(Dev, File);
}

static void
writePragmaIncludes(const include_cleaner::PragmaIncludes &PI,
                    llvm::raw_ostream &OS) {
  auto &IWYUPublic = PreambleCache::pragmaIWYUPublic(
      const_cast<include_cleaner::PragmaIncludes &>(PI));
  write32(static_cast<uint32_t>(IWYUPublic.size()), OS);
  for (const auto &[UID, Spelling] : IWYUPublic) {
    writeUniqueID(UID, OS);
    writeStr(Spelling, OS);
  }

  auto &IWYUExportBy = PreambleCache::pragmaIWYUExportBy(
      const_cast<include_cleaner::PragmaIncludes &>(PI));
  write32(static_cast<uint32_t>(IWYUExportBy.size()), OS);
  for (const auto &[UID, Exporters] : IWYUExportBy) {
    writeUniqueID(UID, OS);
    write32(static_cast<uint32_t>(Exporters.size()), OS);
    for (const auto &E : Exporters)
      writeStr(E, OS);
  }

  auto &StdExportBy = PreambleCache::pragmaStdIWYUExportBy(
      const_cast<include_cleaner::PragmaIncludes &>(PI));
  write32(static_cast<uint32_t>(StdExportBy.size()), OS);
  for (const auto &[Hdr, Exporters] : StdExportBy) {
    writeStr(Hdr.name(), OS);
    write32(static_cast<uint32_t>(Exporters.size()), OS);
    for (const auto &E : Exporters)
      writeStr(E, OS);
  }

  auto &NonSelfContained = PreambleCache::pragmaNonSelfContained(
      const_cast<include_cleaner::PragmaIncludes &>(PI));
  write32(static_cast<uint32_t>(NonSelfContained.size()), OS);
  for (const auto &UID : NonSelfContained)
    writeUniqueID(UID, OS);

  auto &Keep = PreambleCache::pragmaShouldKeep(
      const_cast<include_cleaner::PragmaIncludes &>(PI));
  write32(static_cast<uint32_t>(Keep.size()), OS);
  for (const auto &UID : Keep)
    writeUniqueID(UID, OS);
}

static include_cleaner::PragmaIncludes readPragmaIncludes(Reader &R) {
  include_cleaner::PragmaIncludes PI;
  auto Arena = std::make_shared<llvm::BumpPtrAllocator>();

  auto &IWYUPublic = PreambleCache::pragmaIWYUPublic(PI);
  uint32_t PubCount = R.consume32();
  for (uint32_t I = 0; I < PubCount; ++I) {
    auto UID = readUniqueID(R);
    llvm::StringRef Str = R.consumeStr();
    char *Buf = Arena->Allocate<char>(Str.size());
    std::copy(Str.begin(), Str.end(), Buf);
    IWYUPublic[UID] = llvm::StringRef(Buf, Str.size());
  }

  auto &IWYUExportBy = PreambleCache::pragmaIWYUExportBy(PI);
  uint32_t ExportCount = R.consume32();
  for (uint32_t I = 0; I < ExportCount; ++I) {
    auto UID = readUniqueID(R);
    uint32_t NumExporters = R.consume32();
    auto &Exporters = IWYUExportBy[UID];
    for (uint32_t J = 0; J < NumExporters; ++J) {
      llvm::StringRef Str = R.consumeStr();
      char *Buf = Arena->Allocate<char>(Str.size());
      std::copy(Str.begin(), Str.end(), Buf);
      Exporters.push_back(llvm::StringRef(Buf, Str.size()));
    }
  }

  auto &StdExportBy = PreambleCache::pragmaStdIWYUExportBy(PI);
  uint32_t StdCount = R.consume32();
  for (uint32_t I = 0; I < StdCount; ++I) {
    llvm::StringRef HdrName = R.consumeStr();
    uint32_t NumExporters = R.consume32();
    // Try both C++ and C to find the header.
    auto Hdr = tooling::stdlib::Header::named(HdrName, tooling::stdlib::Lang::CXX);
    if (!Hdr)
      Hdr = tooling::stdlib::Header::named(HdrName, tooling::stdlib::Lang::C);
    llvm::SmallVector<llvm::StringRef> ExporterList;
    for (uint32_t J = 0; J < NumExporters; ++J) {
      llvm::StringRef Str = R.consumeStr();
      char *Buf = Arena->Allocate<char>(Str.size());
      std::copy(Str.begin(), Str.end(), Buf);
      ExporterList.push_back(llvm::StringRef(Buf, Str.size()));
    }
    if (Hdr)
      StdExportBy[*Hdr] = std::move(ExporterList);
  }

  auto &NonSelfContained = PreambleCache::pragmaNonSelfContained(PI);
  uint32_t NonSCCount = R.consume32();
  for (uint32_t I = 0; I < NonSCCount; ++I)
    NonSelfContained.insert(readUniqueID(R));

  auto &Keep = PreambleCache::pragmaShouldKeep(PI);
  uint32_t KeepCount = R.consume32();
  for (uint32_t I = 0; I < KeepCount; ++I)
    Keep.insert(readUniqueID(R));

  PreambleCache::pragmaArena(PI).push_back(std::move(Arena));
  return PI;
}

//===----------------------------------------------------------------------===//
// Meta format constants.
//===----------------------------------------------------------------------===//

static constexpr uint32_t MetaMagic = 0x50434D45;   // 'PCME'
static constexpr uint32_t MetaVersion = 2;

//===----------------------------------------------------------------------===//
// PreambleCache implementation.
//===----------------------------------------------------------------------===//

PreambleCache::PreambleCache(PathRef SourceRoot, uint64_t MaxSizeBytes)
    : SourceRoot(SourceRoot.str()), MaxSizeBytes(MaxSizeBytes) {}

std::string PreambleCache::cacheRoot() const {
  llvm::SmallString<256> Result;
  if (!SourceRoot.empty()) {
    Result = SourceRoot;
    llvm::sys::path::append(Result, ".cache", "clangd", "preambles");
    return Result.str().str();
  }
  if (llvm::sys::path::cache_directory(Result)) {
    llvm::sys::path::append(Result, "clangd", "preambles");
    return Result.str().str();
  }
  llvm::sys::path::system_temp_directory(/*erasedOnReboot=*/false, Result);
  llvm::sys::path::append(Result, "clangd", "preambles");
  return Result.str().str();
}

std::string PreambleCache::fileHashDir(PathRef FileName) const {
  std::string Hash = hashForCache(normalizePathForCache(FileName));
  llvm::SmallString<256> Dir(cacheRoot());
  llvm::sys::path::append(Dir, Hash);
  return Dir.str().str();
}

std::string PreambleCache::stablePath(
    PathRef FileName, const tooling::CompileCommand &Cmd) const {
  std::string CmdHash = hashForCache(normalizeCompileCommand(Cmd));
  llvm::SmallString<256> Path(fileHashDir(FileName));
  llvm::sys::path::append(Path, CmdHash + ".pch");
  return Path.str().str();
}

bool PreambleCache::hasCachedPCH(
    PathRef FileName, const tooling::CompileCommand &Cmd) const {
  if (!isEnabled())
    return false;
  return llvm::sys::fs::exists(stablePath(FileName, Cmd));
}

llvm::Error PreambleCache::storeMeta(PathRef FileName,
                                     const tooling::CompileCommand &Cmd,
                                     const PreambleMeta &Meta) const {
  if (!isEnabled())
    return llvm::Error::success();

  std::string Dir = fileHashDir(FileName);
  if (auto EC = llvm::sys::fs::create_directories(Dir))
    return llvm::createStringError(EC,
                                   "failed to create preamble cache dir: " +
                                       Dir);

  std::string CmdHash = hashForCache(normalizeCompileCommand(Cmd));
  std::string CurrentPCH = CmdHash + ".pch";
  std::string CurrentMeta = CmdHash + ".meta";

  // Remove stale entries for other compile commands of the same file.
  std::error_code EC;
  for (llvm::sys::fs::directory_iterator It(Dir, EC), End; !EC && It != End;
       It.increment(EC)) {
    llvm::StringRef Entry = It->path();
    llvm::StringRef Ext = llvm::sys::path::extension(Entry);
    llvm::StringRef Base = llvm::sys::path::filename(Entry);
    if ((Ext == ".pch" || Ext == ".meta") && Base != CurrentPCH &&
        Base != CurrentMeta)
      llvm::sys::fs::remove(Entry);
  }

  // Write metadata sidecar. The PCH file was already written by
  // PrecompiledPreamble::Build() with PersistentFile mode.
  std::string MetaPath =
      (llvm::sys::path::parent_path(stablePath(FileName, Cmd)) + "/" +
       CmdHash + ".meta")
          .str();
  std::error_code MetaEC;
  llvm::raw_fd_ostream OS(MetaPath, MetaEC, llvm::sys::fs::OF_None);
  if (MetaEC)
    return llvm::createStringError(
        MetaEC, "failed to open meta for writing: " + MetaPath);

  write32(MetaMagic, OS);
  write32(MetaVersion, OS);

  // PrecompiledPreamble metadata (for CanReuse).
  write32(static_cast<uint32_t>(Meta.PreambleBytes.size()), OS);
  OS.write(Meta.PreambleBytes.data(), Meta.PreambleBytes.size());
  OS.write(static_cast<char>(Meta.PreambleEndsAtStartOfLine ? 1 : 0));
  OS.write(static_cast<char>(Meta.MainIsIncludeGuarded ? 1 : 0));

  write32(static_cast<uint32_t>(Meta.FilesInPreamble.size()), OS);
  for (const auto &[Path, Hash] : Meta.FilesInPreamble) {
    writeStr(Path, OS);
    write64(static_cast<uint64_t>(Hash.Size), OS);
    write64(static_cast<uint64_t>(Hash.ModTime), OS);
    OS.write(reinterpret_cast<const char *>(Hash.MD5.data()), 16);
  }

  write32(static_cast<uint32_t>(Meta.MissingFiles.size()), OS);
  for (const auto &F : Meta.MissingFiles)
    writeStr(F.getKey(), OS);

  // Sidecar fields.
  writeMarks(Meta.Marks, OS);
  writeMacros(Meta.Macros, OS);
  writeIncludes(Meta.Includes, OS);
  writePragmaIncludes(Meta.Pragmas, OS);

  OS.flush();
  if (OS.has_error())
    return llvm::createStringError(OS.error(),
                                   "failed to write meta: " + MetaPath);

  log("PreambleCache: meta stored for {0}", FileName);
  evictIfNeeded();
  return llvm::Error::success();
}

llvm::Expected<PreambleCache::PreambleMeta>
PreambleCache::load(PathRef FileName,
                    const tooling::CompileCommand &Cmd) const {
  if (!isEnabled())
    return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                   "cache disabled");
  std::string PCHPath = stablePath(FileName, Cmd);
  if (!llvm::sys::fs::exists(PCHPath))
    return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                   "no cached PCH");

  std::string CmdHash = hashForCache(normalizeCompileCommand(Cmd));
  std::string MetaPath =
      (llvm::sys::path::parent_path(PCHPath) + "/" + CmdHash + ".meta").str();

  auto BufOrErr = llvm::MemoryBuffer::getFile(MetaPath);
  if (!BufOrErr)
    return llvm::createStringError(BufOrErr.getError(),
                                   "failed to read meta: " + MetaPath);
  Reader R((*BufOrErr)->getBuffer());

  if (R.consume32() != MetaMagic)
    return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                   "meta magic mismatch");
  uint32_t Version = R.consume32();
  if (R.err())
    return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                   "meta header truncated");
  if (Version != MetaVersion) {
    vlog("PreambleCache: meta version {0} != {1} for {2}, rebuilding",
         Version, MetaVersion, FileName);
    return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                   "meta version mismatch");
  }

  PreambleMeta Meta;

  // PrecompiledPreamble metadata.
  uint32_t BytesLen = R.consume32();
  llvm::StringRef Bytes = R.consume(BytesLen);
  Meta.PreambleBytes.assign(Bytes.begin(), Bytes.end());
  Meta.PreambleEndsAtStartOfLine = R.consume8() != 0;
  Meta.MainIsIncludeGuarded = R.consume8() != 0;

  uint32_t FilesCount = R.consume32();
  for (uint32_t I = 0; I < FilesCount; ++I) {
    llvm::StringRef Path = R.consumeStr();
    PrecompiledPreamble::PreambleFileHash H;
    H.Size = static_cast<off_t>(R.consume64());
    H.ModTime = static_cast<time_t>(R.consume64());
    llvm::StringRef MD5Bytes = R.consume(16);
    if (!R.err())
      std::copy(MD5Bytes.begin(), MD5Bytes.end(), H.MD5.data());
    Meta.FilesInPreamble[Path] = H;
  }

  uint32_t MissingCount = R.consume32();
  for (uint32_t I = 0; I < MissingCount; ++I)
    Meta.MissingFiles.insert(R.consumeStr());

  if (R.err())
    return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                   "meta base fields truncated");

  // Sidecar fields.
  Meta.Marks = readMarks(R);
  Meta.Macros = readMacros(R);
  Meta.Includes = readIncludes(R);
  Meta.Pragmas = readPragmaIncludes(R);

  if (R.err())
    return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                   "meta sidecar truncated");

  return Meta;
}

void PreambleCache::evictIfNeeded() const {
  if (MaxSizeBytes == 0)
    return;

  std::string Root = cacheRoot();
  std::error_code EC;
  std::vector<std::pair<std::time_t, std::string>> Entries;
  uint64_t TotalBytes = 0;

  for (llvm::sys::fs::recursive_directory_iterator It(Root, EC), End;
       !EC && It != End; It.increment(EC)) {
    if (llvm::sys::path::extension(It->path()) != ".pch")
      continue;
    llvm::sys::fs::file_status St;
    if (llvm::sys::fs::status(It->path(), St))
      continue;
    TotalBytes += St.getSize();
    Entries.push_back(
        {llvm::sys::toTimeT(St.getLastModificationTime()), It->path()});
  }

  if (TotalBytes <= MaxSizeBytes)
    return;

  uint64_t Target = MaxSizeBytes * 8 / 10;
  llvm::sort(Entries);
  for (auto &[MTime, Path] : Entries) {
    if (TotalBytes <= Target)
      break;
    llvm::sys::fs::file_status St;
    if (!llvm::sys::fs::status(Path, St)) {
      TotalBytes -= St.getSize();
      if (auto RemEC = llvm::sys::fs::remove(Path))
        vlog("PreambleCache: eviction failed for {0}: {1}",
             llvm::StringRef(Path), RemEC.message());
      else
        log("PreambleCache: evicted {0}", llvm::StringRef(Path));
    }
  }
}

} // namespace clangd
} // namespace clang
