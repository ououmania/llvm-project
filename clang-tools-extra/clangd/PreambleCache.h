//===--- PreambleCache.h - Persistent preamble PCH cache --------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANGD_PREAMBLECACHE_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANGD_PREAMBLECACHE_H

#include "support/Path.h"
#include "CollectMacros.h"
#include "Headers.h"
#include "clang-include-cleaner/Record.h"
#include "clang/Frontend/PrecompiledPreamble.h"
#include "clang/Tooling/CompilationDatabase.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"
#include <cstdint>
#include <string>
#include <vector>

namespace clang {
namespace clangd {

/// Manages a persistent on-disk cache of preamble PCH files so that clangd
/// restarts do not require re-parsing unchanged preambles.
///
/// Cache layout:
///   <root>/preambles/<file_hash>/<cmd_hash>.pch
///
/// file_hash: SHA1 of the canonical source file path (16 hex chars).
/// cmd_hash:  SHA1 of the normalized compile command (16 hex chars).
///
/// When a new PCH is built its temp file is renamed into the cache via an
/// atomic rename, so concurrent writers are safe — the last rename wins and
/// both candidates are valid complete files.
///
/// Stale entries (from an old compile command for the same source file) are
/// deleted opportunistically when a new entry is written for that file.
///
/// The cache root follows the same priority as the module cache:
///   1. <project-source-root>/.cache/clangd/preambles/
///   2. $XDG_CACHE_HOME/clangd/preambles/  (or ~/.cache/clangd/preambles/)
///   3. <system-temp>/clangd/preambles/
class PreambleCache {
public:
  /// Disk-size cap in bytes. Once exceeded, LRU entries are removed on each
  /// write until the total drops below 80% of the cap.
  /// 0 disables the persistent cache entirely.
  static constexpr uint64_t kDefaultMaxSizeBytes = 10ULL * 1024 * 1024 * 1024;

  explicit PreambleCache(PathRef SourceRoot = "",
                         uint64_t MaxSizeBytes = kDefaultMaxSizeBytes);

  /// Returns the stable PCH path for this (source file, compile command) pair.
  std::string stablePath(PathRef FileName,
                         const tooling::CompileCommand &Cmd) const;

  /// Metadata needed to reconstruct a PrecompiledPreamble from cache.
  struct PreambleMeta {
    // Fields for CanReuse() — must always be present.
    std::vector<char> PreambleBytes;
    bool PreambleEndsAtStartOfLine = false;
    llvm::StringMap<PrecompiledPreamble::PreambleFileHash> FilesInPreamble;
    llvm::StringSet<> MissingFiles;

    // Phase-2 sidecar fields — allow full PreambleData reconstruction.
    bool MainIsIncludeGuarded = false;
    std::vector<PragmaMark> Marks;
    MainFileMacros Macros;
    IncludeStructure Includes;
    include_cleaner::PragmaIncludes Pragmas;
  };

  /// Write metadata sidecar for a preamble that was already built with
  /// PersistentFile mode. Removes stale entries for other compile commands.
  llvm::Error storeMeta(PathRef FileName, const tooling::CompileCommand &Cmd,
                        const PreambleMeta &Meta) const;

  /// Try to load a cached PCH and its metadata for the given (file, command).
  /// Returns the metadata on success; the PCH path is available via
  /// stablePath(). Returns an error if no cache entry exists or the metadata
  /// file is corrupt/missing.
  llvm::Expected<PreambleMeta>
  load(PathRef FileName, const tooling::CompileCommand &Cmd) const;

  /// Returns true if a cached PCH exists at the stable path for this
  /// (source file, compile command) pair. Does not validate the PCH contents —
  /// the caller must run CanReuse() after loading.
  bool hasCachedPCH(PathRef FileName,
                    const tooling::CompileCommand &Cmd) const;

  bool isEnabled() const { return MaxSizeBytes > 0; }

  // Access helper for IncludeStructure private member (friend-based).
  static std::vector<std::string> &
  includeRealPathNames(IncludeStructure &IS) {
    return IS.RealPathNames;
  }
  static llvm::StringMap<llvm::SmallVector<unsigned>> &
  includesBySpelling(IncludeStructure &IS) {
    return IS.MainFileIncludesBySpelling;
  }
  static llvm::DenseMap<llvm::sys::fs::UniqueID, IncludeStructure::HeaderID> &
  includeUIDToIndex(IncludeStructure &IS) {
    return IS.UIDToIndex;
  }

  // Access helpers for PragmaIncludes private members (friend-based).
  static auto &pragmaIWYUPublic(include_cleaner::PragmaIncludes &PI) {
    return PI.IWYUPublic;
  }
  static auto &pragmaIWYUExportBy(include_cleaner::PragmaIncludes &PI) {
    return PI.IWYUExportBy;
  }
  static auto &pragmaStdIWYUExportBy(include_cleaner::PragmaIncludes &PI) {
    return PI.StdIWYUExportBy;
  }
  static auto &pragmaNonSelfContained(include_cleaner::PragmaIncludes &PI) {
    return PI.NonSelfContainedFiles;
  }
  static auto &pragmaShouldKeep(include_cleaner::PragmaIncludes &PI) {
    return PI.ShouldKeep;
  }
  static auto &pragmaArena(include_cleaner::PragmaIncludes &PI) {
    return PI.Arena;
  }

private:
  std::string cacheRoot() const;
  std::string fileHashDir(PathRef FileName) const;
  void evictIfNeeded() const;

  std::string SourceRoot;
  uint64_t MaxSizeBytes;
};

} // namespace clangd
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANGD_PREAMBLECACHE_H
