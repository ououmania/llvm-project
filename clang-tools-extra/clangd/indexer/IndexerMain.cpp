//===--- IndexerMain.cpp -----------------------------------------*- C++-*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// clangd-indexer is a tool to gather index data (symbols, xrefs) from source.
//
//===----------------------------------------------------------------------===//

#include "ClangdServer.h"
#include "CompileCommands.h"
#include "Compiler.h"
#include "ConfigProvider.h"
#include "SourceCode.h"
#include "URI.h"
#include "index/IndexAction.h"
#include "index/Merge.h"
#include "index/Ref.h"
#include "index/Serialization.h"
#include "index/Symbol.h"
#include "index/SymbolCollector.h"
#include "index/SymbolOrigin.h"
#include "support/Context.h"
#include "support/Logger.h"
#include "support/ThreadsafeFS.h"
#include "clang/Basic/SourceLocation.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Frontend/FrontendAction.h"
#include "clang/Tooling/CommonOptionsParser.h"
#include "clang/Tooling/CompilationDatabase.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Signals.h"
#include "llvm/Support/ThreadPool.h"
#include "llvm/Support/raw_ostream.h"
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace clang {
namespace clangd {
namespace {

static llvm::cl::opt<IndexFileFormat>
    Format("format", llvm::cl::desc("Format of the index to be written"),
           llvm::cl::values(clEnumValN(IndexFileFormat::YAML, "yaml",
                                       "human-readable YAML format"),
                            clEnumValN(IndexFileFormat::RIFF, "binary",
                                       "binary RIFF format")),
           llvm::cl::init(IndexFileFormat::RIFF));

static llvm::cl::list<std::string> QueryDriverGlobs{
    "query-driver",
    llvm::cl::desc(
        "Comma separated list of globs for white-listing gcc-compatible "
        "drivers that are safe to execute. Drivers matching any of these globs "
        "will be used to extract system includes. e.g. "
        "/usr/bin/**/clang-*,/path/to/repo/**/g++-*"),
    llvm::cl::CommaSeparated,
};

static llvm::cl::opt<bool> EnableConfig{
    "enable-config",
    llvm::cl::desc(config::Provider::EnableConfigFlagDesc),
    llvm::cl::init(true),
};

static llvm::cl::opt<std::string> IndexCache(
    "index-cache",
    llvm::cl::desc("Directory to store the incremental index cache. When set "
                   "together with a list of changed files (positional args "
                   "after the compile database), only translation units that "
                   "depend on those files are re-indexed."),
    llvm::cl::init(""));

// Kept for backward compatibility: the clang::tooling executor framework used
// --executor to choose which translation units to index. We use it to
// distinguish a full rebuild (--executor=all-TUs) from a file-list run.
static llvm::cl::opt<std::string> Executor(
    "executor",
    llvm::cl::desc("(deprecated) 'all-TUs' forces a full rebuild of every "
                   "translation unit, discarding the cache."),
    llvm::cl::init(""));

static llvm::cl::opt<unsigned> Concurrency(
    "concurrency",
    llvm::cl::desc("Number of translation units to index in parallel "
                   "(0 = hardware concurrency)"),
    llvm::cl::init(8));

// We cannot use vfs->makeAbsolute because Cmd.FileName is either absolute or
// relative to Cmd.Directory, which might not be the same as current working
// directory.
llvm::SmallString<128> getAbsolutePath(const tooling::CompileCommand &Cmd) {
  llvm::SmallString<128> AbsolutePath;
  if (llvm::sys::path::is_absolute(Cmd.Filename)) {
    AbsolutePath = Cmd.Filename;
  } else {
    AbsolutePath = Cmd.Directory;
    llvm::sys::path::append(AbsolutePath, Cmd.Filename);
    llvm::sys::path::remove_dots(AbsolutePath, true);
  }
  return AbsolutePath;
}

// Indexes a single translation unit described by \p Cmd, delivering the
// resulting symbols, refs, relations and include graph as an IndexFileIn.
// This is the standalone-indexer equivalent of BackgroundIndex::index, but
// without the file-version bookkeeping needed by a long-running indexer.
llvm::Expected<IndexFileIn> indexSingleFile(const ThreadsafeFS &TFS,
                                            tooling::CompileCommand Cmd,
                                            SymbolCollector::Options IndexOpts) {
  auto AbsolutePath = getAbsolutePath(Cmd);
  auto FS = TFS.view(Cmd.Directory);
  auto Buf = FS->getBufferForFile(AbsolutePath);
  if (!Buf)
    return llvm::errorCodeToError(Buf.getError());

  ParseInputs Inputs;
  Inputs.TFS = &TFS;
  Inputs.CompileCommand = std::move(Cmd);
  IgnoreDiagnostics IgnoreDiags;
  auto CI = buildCompilerInvocation(Inputs, IgnoreDiags);
  if (!CI)
    return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                   "Couldn't build compiler invocation");

  auto Clang = prepareCompilerInstance(
      std::move(CI), /*Preamble=*/nullptr, std::move(*Buf), std::move(FS),
      IgnoreDiags);
  if (!Clang)
    return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                   "Couldn't build compiler instance");

  IndexFileIn Index;
  auto Action = createStaticIndexingAction(
      IndexOpts, [&](IndexFileIn Result) { Index = std::move(Result); });

  const FrontendInputFile &Input = Clang->getFrontendOpts().Inputs.front();
  if (!Action->BeginSourceFile(*Clang, Input))
    return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                   "BeginSourceFile() failed");
  if (llvm::Error Err = Action->Execute())
    return std::move(Err);

  Action->EndSourceFile();

  Index.Cmd = Inputs.CompileCommand;
  assert(Index.Symbols && Index.Refs && Index.Sources &&
         "Symbols, Refs and Sources must be set.");
  return std::move(Index);
}

// Merges the per-TU index results into a single set of slabs, deduplicating
// symbols by ID and references by location as the results arrive.
class IndexMerger {
public:
  void add(const IndexFileIn &Result) {
    for (const auto &Sym : *Result.Symbols) {
      if (const auto *Existing = Symbols.find(Sym.ID))
        Symbols.insert(mergeSymbol(*Existing, Sym));
      else
        Symbols.insert(Sym);
    }
    for (const auto &Sym : *Result.Refs) {
      // Deduplication happens during insertion.
      for (const auto &Ref : Sym.second)
        Refs.insert(Sym.first, Ref);
    }
    for (const auto &R : *Result.Relations) {
      Relations.insert(R);
    }
  }

  IndexFileIn build() {
    IndexFileIn Result;
    Result.Symbols = std::move(Symbols).build();
    Result.Refs = std::move(Refs).build();
    Result.Relations = std::move(Relations).build();
    return Result;
  }

private:
  SymbolSlab::Builder Symbols;
  RefSlab::Builder Refs;
  RelationSlab::Builder Relations;
};

// Per-translation-unit cache record. Depends lists the URIs of every file the
// TU depended on at index time; a TU is stale iff any of these was reported as
// changed by the caller.
struct TUCacheEntry {
  std::string IdxFile;              // cache file name, RIFF format
  std::vector<std::string> Depends; // file URIs the TU depends on
};

// Keyed by the main-file URI of each translation unit.
using Manifest = std::map<std::string, TUCacheEntry>;

llvm::Error loadManifest(llvm::StringRef CacheDir, Manifest &M) {
  llvm::SmallString<256> Path(CacheDir);
  llvm::sys::path::append(Path, "manifest.json");
  auto Buf = llvm::MemoryBuffer::getFile(Path);
  if (!Buf)
    return llvm::errorCodeToError(Buf.getError());
  auto V = llvm::json::parse(Buf->get()->getBuffer());
  if (!V)
    return V.takeError();
  const auto *Root = V->getAsObject();
  if (!Root)
    return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                   "manifest is not a JSON object");
  const auto *Tus = Root->getObject("tus");
  if (!Tus)
    return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                   "manifest has no 'tus' object");
  for (const auto &[TU, Val] : *Tus) {
    const auto *E = Val.getAsObject();
    if (!E)
      continue;
    TUCacheEntry Entry;
    if (auto Idx = E->getString("idx"))
      Entry.IdxFile = Idx->str();
    if (const auto *Deps = E->getArray("depends"))
      for (const auto &D : *Deps)
        if (auto DStr = D.getAsString())
          Entry.Depends.push_back(DStr->str());
    M[TU.str()] = std::move(Entry);
  }
  return llvm::Error::success();
}

llvm::Error saveManifest(llvm::StringRef CacheDir, const Manifest &M) {
  llvm::json::Object Root;
  Root["version"] = 1;
  llvm::json::Object Tus;
  for (const auto &[TU, Entry] : M) {
    llvm::json::Object E;
    E["idx"] = Entry.IdxFile;
    llvm::json::Array Deps;
    for (const auto &D : Entry.Depends)
      Deps.push_back(D);
    E["depends"] = std::move(Deps);
    Tus[TU] = std::move(E);
  }
  Root["tus"] = std::move(Tus);

  llvm::SmallString<256> Path(CacheDir);
  llvm::sys::path::append(Path, "manifest.json");
  std::error_code EC;
  llvm::raw_fd_ostream OS(Path, EC, llvm::sys::fs::OF_None);
  if (EC)
    return llvm::errorCodeToError(EC);
  OS << llvm::json::Value(std::move(Root));
  OS.close();
  if (OS.has_error())
    return llvm::errorCodeToError(OS.error());
  return llvm::Error::success();
}

std::string makeIdxFileName(llvm::StringRef MainFileURI) {
  llvm::StringRef Filename = llvm::sys::path::filename(MainFileURI);
  return Filename.str() + "." + llvm::toHex(digest(MainFileURI)) + ".idx";
}

llvm::Error writeIdx(llvm::StringRef CacheDir, llvm::StringRef IdxFile,
                     const IndexFileIn &Index) {
  llvm::SmallString<256> Path(CacheDir);
  llvm::sys::path::append(Path, IdxFile);
  std::error_code EC;
  llvm::raw_fd_ostream OS(Path, EC, llvm::sys::fs::OF_None);
  if (EC)
    return llvm::errorCodeToError(EC);
  IndexFileOut Out(Index);
  Out.Format = IndexFileFormat::RIFF;
  OS << Out;
  OS.close();
  if (OS.has_error())
    return llvm::errorCodeToError(OS.error());
  return llvm::Error::success();
}

llvm::Expected<IndexFileIn> readIdx(llvm::StringRef CacheDir,
                                    llvm::StringRef IdxFile) {
  llvm::SmallString<256> Path(CacheDir);
  llvm::sys::path::append(Path, IdxFile);
  auto Buf = llvm::MemoryBuffer::getFile(Path);
  if (!Buf)
    return llvm::errorCodeToError(Buf.getError());
  return readIndexFile(Buf->get()->getBuffer(), SymbolOrigin::Static);
}

// Encapsulates the state needed to index translation units and merge their
// results, so the caller holds a single StaticIndexer and dispatches TUs to it
// rather than a closure capturing a dozen variables.
class StaticIndexer {
public:
  using ContextProviderFn = std::function<Context(PathRef)>;

  StaticIndexer(const tooling::CompilationDatabase &CDB,
                std::shared_ptr<CommandMangler> Mangler,
                const ThreadsafeFS &TFS, StringRef CacheDir, bool UseCache,
                ContextProviderFn ContextProvider)
      : CDB(CDB), Mangler(std::move(Mangler)), TFS(TFS), CacheDir(CacheDir),
        UseCache(UseCache), ContextProvider(std::move(ContextProvider)) {}

  void setTotal(unsigned N) { Total = N; }

  // Indexes one TU and merges its result, recording it in the cache.
  void indexOne(StringRef File);

  // Incremental: reuse the cached result for a non-stale TU, or re-index it.
  void processIncremental(StringRef File, const Manifest &OldManifest,
                          const std::set<std::string> &DirtyTUs);

  IndexFileIn build() { return Merger.build(); }
  Manifest &newManifest() { return NewManifest; }

private:
  void reportProgress(StringRef File);

  const tooling::CompilationDatabase &CDB;
  std::shared_ptr<CommandMangler> Mangler;
  const ThreadsafeFS &TFS;
  StringRef CacheDir;
  bool UseCache;
  ContextProviderFn ContextProvider;

  std::mutex FilesMu; // Protects Files (dedup across TUs).
  llvm::StringSet<> Files;

  IndexMerger Merger;
  std::mutex MergeMu; // Protects Merger and NewManifest.
  Manifest NewManifest;

  std::mutex ProgressMu;
  unsigned Processed = 0;
  unsigned Total = 0;
};

void StaticIndexer::reportProgress(StringRef File) {
  std::lock_guard<std::mutex> Lock(ProgressMu);
  llvm::errs() << "[" << ++Processed << "/" << Total << "] Indexing file "
               << File << "\n";
}

void StaticIndexer::indexOne(StringRef File) {
  reportProgress(File);
  auto Cmds = CDB.getCompileCommands(File);
  if (Cmds.empty()) {
    vlog("Skipping {0}, no compile command available", File);
    return;
  }
  auto Cmd = std::move(Cmds.front());

  std::optional<WithContext> WithCfg;
  if (ContextProvider && llvm::sys::path::is_absolute(File))
    WithCfg.emplace(ContextProvider(File));

  Mangler->operator()(Cmd, File);

  SymbolCollector::Options Opts;
  Opts.CountReferences = true;
  Opts.FileFilter = [this](const SourceManager &SM, FileID FID) {
    const auto F = SM.getFileEntryRefForID(FID);
    if (!F)
      return false; // Skip invalid files.
    auto AbsPath = getCanonicalPath(*F, SM.getFileManager());
    if (!AbsPath)
      return false; // Skip files without absolute path.
    std::lock_guard<std::mutex> Lock(FilesMu);
    return Files.insert(*AbsPath).second; // Skip already processed files.
  };

  auto Result = indexSingleFile(TFS, std::move(Cmd), std::move(Opts));
  if (!Result) {
    elog("Failed to index {0}: {1}", File, Result.takeError());
    return;
  }

  // Prepare the cache entry outside the lock (each TU writes its own file).
  std::string MainURI;
  TUCacheEntry Entry;
  if (UseCache) {
    MainURI = URI::createFile(File).toString();
    Entry.IdxFile = makeIdxFileName(MainURI);
    if (llvm::Error E = writeIdx(CacheDir, Entry.IdxFile, *Result))
      vlog("Failed to write cache for {0}: {1}", File, E);
    for (const auto &Node : *Result->Sources)
      Entry.Depends.push_back(Node.getKey().str());
  }

  std::lock_guard<std::mutex> Lock(MergeMu);
  if (UseCache)
    NewManifest[MainURI] = std::move(Entry);
  Merger.add(*Result);
}

void StaticIndexer::processIncremental(
    StringRef File, const Manifest &OldManifest,
    const std::set<std::string> &DirtyTUs) {
  std::string MainURI = URI::createFile(File).toString();
  auto OldIt = OldManifest.find(MainURI);

  // A TU is stale if it's new, or if one of its dependencies was reported as
  // changed.
  bool Stale = OldIt == OldManifest.end() || DirtyTUs.count(MainURI) != 0;

  std::optional<IndexFileIn> Index;
  if (!Stale) {
    auto Cached = readIdx(CacheDir, OldIt->second.IdxFile);
    if (Cached)
      Index = std::move(*Cached);
    else
      vlog("Cache miss for {0}: {1}, reindexing", File, Cached.takeError());
  }

  if (Index) {
    std::lock_guard<std::mutex> Lock(MergeMu);
    Merger.add(*Index);
    NewManifest[MainURI] = OldIt->second;
  } else {
    indexOne(File);
  }
}

} // namespace
} // namespace clangd
} // namespace clang

int main(int argc, const char **argv) {
  llvm::sys::PrintStackTraceOnErrorSignal(argv[0]);

  const char *Overview = R"(
  Creates an index of symbol information etc in a whole project.

  Full rebuild of every translation unit:

    $ clangd-indexer --executor=all-TUs compile_commands.json > clangd.dex

  Incremental: pass changed files after the compile database, together with an
  index cache, and only translation units that depend on those files are
  re-indexed (deleted files clean up their cached result):

    $ clangd-indexer --index-cache <dir> compile_commands.json <changed files...>

  File sequence index without a compile database:

    $ clangd-indexer File1.cpp File2.cpp ... FileN.cpp > clangd.dex

  Note: only symbols from header files will be indexed.
  )";

  auto OptionsParser = clang::tooling::CommonOptionsParser::create(
      argc, argv, llvm::cl::getGeneralCategory(), llvm::cl::OneOrMore,
      Overview);
  if (!OptionsParser) {
    llvm::errs() << llvm::toString(OptionsParser.takeError()) << "\n";
    return 1;
  }
  auto &CDB = OptionsParser->getCompilations();

  auto Mangler = std::make_shared<clang::clangd::CommandMangler>(
      clang::clangd::CommandMangler::detect());
  Mangler->SystemIncludeExtractor = clang::clangd::getSystemIncludeExtractor(
      static_cast<llvm::ArrayRef<std::string>>(
          clang::clangd::QueryDriverGlobs));

  clang::clangd::RealThreadsafeFS TFS;

  std::vector<std::unique_ptr<clang::clangd::config::Provider>> ProviderStack;
  if (clang::clangd::EnableConfig)
    ProviderStack =
        clang::clangd::config::Provider::createDefaultProviders(TFS);
  auto ConfigProvider =
      clang::clangd::config::Provider::combine(std::move(ProviderStack));
  auto ContextProvider =
      clang::clangd::ClangdServer::createConfiguredContextProvider(
          ConfigProvider.get(), /*Callbacks=*/nullptr);

  bool UseCache = !clang::clangd::IndexCache.empty();
  bool FullRebuild = clang::clangd::Executor == "all-TUs";
  llvm::StringRef CacheDir = clang::clangd::IndexCache;

  // Split the positional arguments into the compile database (a .json path)
  // and the list of changed files.
  std::vector<std::string> DirtyFiles;
  for (const auto &P : OptionsParser->getSourcePathList())
    if (llvm::sys::path::extension(P) != ".json")
      DirtyFiles.push_back(P);

  if (UseCache) {
    if (std::error_code EC = llvm::sys::fs::create_directories(CacheDir)) {
      clang::clangd::elog("Failed to create cache dir {0}: {1}", CacheDir,
                          EC.message());
      UseCache = false;
    }
  }

  // A full rebuild discards the cache first so stale entries don't linger.
  if (UseCache && FullRebuild) {
    std::error_code EC;
    for (llvm::sys::fs::directory_iterator It(CacheDir, EC), End;
         It != End; It.increment(EC))
      llvm::sys::fs::remove(It->path());
  }

  clang::clangd::Manifest OldManifest;
  if (UseCache && !FullRebuild) {
    if (llvm::Error E =
            clang::clangd::loadManifest(CacheDir, OldManifest)) {
      clang::clangd::vlog("Failed to load manifest, indexing everything: {0}",
                          E);
      llvm::consumeError(std::move(E));
      OldManifest.clear();
    }
  }

  clang::clangd::StaticIndexer Indexer(CDB, Mangler, TFS, CacheDir, UseCache,
                                       std::move(ContextProvider));

  llvm::DefaultThreadPool Pool(
      llvm::hardware_concurrency(clang::clangd::Concurrency));

  if (FullRebuild || !UseCache) {
    // Full rebuild (--executor=all-TUs) or plain file-list run: index every
    // requested file unconditionally.
    std::vector<std::string> FilesToIndex =
        FullRebuild ? CDB.getAllFiles() : DirtyFiles;
    Indexer.setTotal(FilesToIndex.size());
    for (const auto &File : FilesToIndex)
      Pool.async([&Indexer](std::string F) { Indexer.indexOne(F); }, File);
    Pool.wait();
  } else {
    // Incremental: build a reverse dependency index (file URI -> dependent
    // TUs) and compute the set of stale TUs up front, so each TU's staleness
    // check during the traversal is O(1).
    llvm::StringMap<std::vector<std::string>> ReverseDep;
    for (const auto &[TU, Entry] : OldManifest)
      for (const auto &Dep : Entry.Depends)
        ReverseDep[Dep].push_back(TU);

    std::set<std::string> DirtyTUs;
    for (const auto &F : DirtyFiles) {
      std::string URI = clang::clangd::URI::createFile(F).toString();
      auto It = ReverseDep.find(URI);
      if (It != ReverseDep.end())
        for (const auto &TU : It->second)
          DirtyTUs.insert(TU);
    }

    Indexer.setTotal(DirtyTUs.size());
    for (const auto &File : CDB.getAllFiles())
      Pool.async(
          [&Indexer, &OldManifest, &DirtyTUs](std::string F) {
            Indexer.processIncremental(F, OldManifest, DirtyTUs);
          },
          File);
    Pool.wait();
  }

  // Deleted files: remove any cached result for a TU whose main file no
  // longer exists. (Header deletions already force dependent TUs to re-index
  // via the dependency intersection above.)
  if (UseCache && !FullRebuild) {
    for (const auto &F : DirtyFiles) {
      if (llvm::sys::fs::exists(F))
        continue;
      std::string MainURI = clang::clangd::URI::createFile(F).toString();
      auto OldIt = OldManifest.find(MainURI);
      if (OldIt == OldManifest.end())
        continue;
      llvm::SmallString<256> P(CacheDir);
      llvm::sys::path::append(P, OldIt->second.IdxFile);
      llvm::sys::fs::remove(P);
      clang::clangd::vlog("Removed cache for deleted file {0}", F);
    }
  }

  if (UseCache) {
    if (llvm::Error E =
            clang::clangd::saveManifest(CacheDir, Indexer.newManifest()))
      clang::clangd::vlog("Failed to save manifest: {0}", E);
  }

  // Emit collected data.
  clang::clangd::IndexFileIn Data = Indexer.build();
  clang::clangd::IndexFileOut Out(Data);
  Out.Format = clang::clangd::Format;
  llvm::outs() << Out;
  return 0;
}
