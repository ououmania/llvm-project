# Persistent Preamble Cache

## Background

clangd builds a _preamble_ for each open file by pre-compiling the `#include`
chain at the top of the file into a PCH (pre-compiled header). This is
expensive — a file that transitively pulls in hundreds of headers can take
several seconds — but the result is cached in memory and reused as long as
clangd is running and the file's includes and compile flags do not change.

The problem is that the cache does not survive a clangd restart. Every cold
start forces every open file to re-parse its preamble from scratch.

This document describes a design for persisting the preamble cache to disk so
that it survives restarts.

## Goals

- Eliminate preamble rebuilds on clangd restart when neither the source file
  nor its compile command has changed since the last session.
- Reuse the existing in-session invalidation logic (`CanReuse()`,
  `compileCommandsAreEqual()`) — do not invent a parallel mechanism.
- Handle compile-command rotation cleanly: when a file's compile command
  changes, stale cache entries for that file are deleted on the next open.
- Keep disk usage bounded with a simple LRU policy.

## Non-goals

- Sharing the cache between multiple users or machines.
- Compressing PCH files (the format is set by clang).

---

## Data to persist

`PreambleData` (`Preamble.h:100`) has two parts:

**PCH** — `PreambleData::Preamble` (`PrecompiledPreamble`) is already
serialized to disk when `--pch-storage=disk` is used, but written to a temp
file that is deleted on destruction. We need to write it to a stable path
instead.

**Sidecar** — the remaining fields that clangd captures during the preamble
parse and that are not stored inside the PCH:

| Field | Type | Notes |
|---|---|---|
| `Includes` | `IncludeStructure` | include graph, search paths |
| `Pragmas` | `PragmaIncludes` | IWYU pragma mappings |
| `Macros` | `MainFileMacros` | macro refs in preamble section |
| `Marks` | `vector<PragmaMark>` | `#pragma mark` ranges |
| `CompileCommand` | `tooling::CompileCommand` | needed by `CanReuse()` |
| `TargetOpts` | `TargetOptions` | target used at build time; prevents crashes when deserializing PCH under a different target |
| `RequiredModules` | `PrerequisiteModules` | C++20 module prerequisites; serialized if the project uses modules, skipped otherwise |
| `Version` | `string` | clangd's file version string |
| `MainIsIncludeGuarded` | `bool` | propagated to subsequent parses |

`PragmaIncludes` fields are all keyed by `llvm::sys::fs::UniqueID` with string
values. There are no `ASTContext` pointers; the `BumpPtrAllocator` that owns
strings is an implementation detail of in-session construction, not something
that needs to survive.

`IncludeStructure` stores `DenseMap<UniqueID, HeaderID>`,
`DenseMap<HeaderID, SmallVector<HeaderID>>` (include graph),
`vector<Inclusion>` (main-file includes), and `vector<string>` (real path
names). All are plain data.

`MainFileMacros` stores a `StringSet`, a `DenseMap<SymbolID,
vector<MacroOccurrence>>`, a `vector<MacroOccurrence>`, and a
`vector<Range>` — all plain data.

`PragmaMark` is `{Range, string}`.

The sidecar will be serialized using the same LLVM bitstream encoding that
`index/Serialization.cpp` uses for the background index, extended with new
record types for the preamble-specific structures.

---

## Cache layout

```
$XDG_CACHE_HOME/clangd/preambles/   (defaults to ~/.cache/clangd/preambles/)
  <file_hash>/
    <cmd_hash>.pch
    <cmd_hash>.sidecar
```

- `file_hash` — SHA1 of the canonical absolute path of the main source file,
  hex-encoded, truncated to 16 characters. Collision probability is negligible
  for any realistic project.
- `cmd_hash` — SHA1 of the normalized compile command, truncated to 16
  characters. Normalization: expand all paths to absolute, sort `-I` / `-D` /
  `-isystem` flags, drop output-only flags (`-o`, `-MF`, `-MT`), exclude the
  input filename itself. Working directory is included after normalization.
  This ensures that flag-order variations and relative-vs-absolute path
  differences in the compile command don't cause unnecessary cache misses.

When clangd opens a file:

1. Compute `file_hash` and `cmd_hash` for the current compile command.
2. Check whether `<file_hash>/<cmd_hash>.pch` and `.sidecar` exist and pass
   `CanReuse()`.
3. If yes, load them — skip the preamble build.
4. If no (cache miss or `CanReuse()` fails), build normally, then write the new
   pair, and **delete all other files** in `<file_hash>/`. This cleans up
   entries from previous compile commands without a separate GC pass.

On a cache hit the existing in-memory path continues unchanged; the loaded
`PreambleData` is placed in `LatestBuild` exactly as if it had been built.

---

## Disk usage and eviction

From measurement across an active session, PCH sizes range from ~250 KB for
thin files to ~334 MB for heavily-included files. With ~30 files open at once,
total cache size is typically 1–5 GB for a large C++ project.

A configurable cap (default 10 GB) is enforced with an LRU policy based on
file mtime, checked lazily on each cache write:

1. After writing a new entry, enumerate all `*.pch` files under the preambles
   directory and compute total size.
2. If total exceeds the cap, delete the least-recently-used entries (by mtime)
   until usage drops below 80% of the cap.

The cap is exposed as a new clangd flag: `--preamble-cache-size=<bytes>`
(default `10G`, accepts `K/M/G` suffixes). Setting it to `0` disables the
persistent cache entirely.

---

## Sidecar format

The sidecar file uses a versioned binary format (LLVM RIFF, same as the
background index). The first record is a header:

```
PreambleSidecarVersion: uint32  (bump on any schema change — drives sidecar invalidation only)
ClangdVersion: string           (git hash or release tag)
MainFilePath: string
CompileCommand: serialized tooling::CompileCommand
```

`PreambleSidecarVersion` governs only the sidecar format. PCH format
compatibility is handled by clang's own AST serialization version check, which
fires naturally when the PCH is loaded — a version mismatch there manifests as
a `CanReuse()` failure or a load error, both of which are already treated as
cache misses. There is no need to duplicate this check in the sidecar header.

If `PreambleSidecarVersion` does not match the current binary, the sidecar is
treated as a cache miss and the preamble is rebuilt. The stale files are then
replaced.

Subsequent records encode each sidecar field. The encoding reuses the string
table and `SymbolID` encoding from `index/Serialization.cpp` where applicable.

---

## Code changes

### `clang/lib/Frontend/PrecompiledPreamble.cpp`

No changes required. `buildPreamble()` continues to write the PCH to a
`TempPCHFile` under `/tmp/preamble-XXXXXX.pch` as today. After the build
succeeds, `PreambleCache` creates a hard link from the stable cache path to
the temp file via `llvm::sys::fs::create_hard_link()`. The running session's
`PrecompiledPreamble` continues to reference the original temp path internally
(for `AddImplicitPreamble`, etc.). When the session ends and the `TempPCHFile`
destructor removes the temp path, the data survives on disk through the hard
link at the stable path, ready for the next session.

If hard linking fails (e.g. cross-device), `PreambleCache` falls back to
`llvm::sys::fs::copy_file()`.

Concurrent writes are safe: each clangd process produces its own uniquely
named temp file, and `create_hard_link()` with a preceding `remove()` of the
target is equivalent to an atomic "link-or-overwrite". Two processes racing to
populate the same cache entry each link their own temp file over the target;
the last writer wins and both results are valid, so no data is corrupted.

### `clang-tools-extra/clangd/Preamble.h` / `Preamble.cpp`

Add:
```cpp
llvm::Error storePreamble(const PreambleData &, PathRef PCHPath,
                          PathRef SidecarPath);
llvm::Expected<std::shared_ptr<PreambleData>>
    loadPreamble(PathRef PCHPath, PathRef SidecarPath,
                 const ParseInputs &, const CompilerInvocation &);
```

`loadPreamble` calls `CanReuse()` before returning; a stale entry returns an
error and the caller falls back to a rebuild.

### `clang-tools-extra/clangd/TUScheduler.cpp`

In `PreamblePeer::build()` (around line 1088), before calling
`buildPreamble()`, attempt `loadPreamble()` from the cache path. On success,
set `LatestBuild` and skip the build. On failure (miss, version mismatch, or
IO error), proceed with the normal build and call `storePreamble()` afterward.

### `clang-tools-extra/clangd/tool/ClangdMain.cpp`

Wire the new `--preamble-cache-size` flag and pass it through
`ClangdServer::Options`.

### New file: `clang-tools-extra/clangd/PreambleCache.h` / `.cpp`

Owns cache path computation, LRU eviction, and the read/write orchestration
described above. Keeps `TUScheduler.cpp` and `Preamble.cpp` focused on their
existing responsibilities.

---

## Implementation phases

### Phase 1 — PCH persistence (est. 1–2 weeks)

Implement the stable-path PCH storage in `PrecompiledPreamble.cpp`, add
`PreambleCache` with path computation and eviction, wire it into
`TUScheduler`. At this point the sidecar fields are not cached. On a cache
hit, the PCH is loaded and then a preprocessor-only re-parse is run over it
(no semantic analysis, no AST construction) to reconstruct the sidecar fields
via the existing PP callback infrastructure. This is significantly faster than
a full preamble rebuild, though not as fast as Phase 2.

Atomic writes via rename-into-place are included in Phase 1, not deferred:
without them, two clangd instances opening the same file simultaneously can
write a torn PCH that the other reads. This is a correctness issue on any
multi-process setup (CI, parallel editors) and is too cheap to defer.

### Phase 2 — Sidecar serialization (est. 2–4 weeks)

Implement `storePreamble` / `loadPreamble` for all sidecar fields. Start with
`IncludeStructure`, `MainFileMacros`, `Marks` (all plain data). Handle
`PragmaIncludes` last since it needs `FileManager` access on load to convert
stored paths back to `FileEntry` pointers. Keep `PragmaIncludes` as a
rebuild-from-PCH fallback until its serialization is complete.

### Phase 3 — Hardening (est. 3–5 days)

Add checksum validation (CRC32 over sidecar body) and stress testing with
simultaneous clangd instances to verify the rename-into-place behaviour under
contention.

---

## Testing

- Unit tests for `storePreamble` / `loadPreamble` roundtrip correctness.
- Unit test for compile-command rotation: write cache with command A, change
  command to B, verify A's files are deleted and B's are created.
- Integration test: start clangd, open a file (preamble built), restart
  clangd, reopen file, verify no `buildPreamble` call in logs.
- Fuzz the sidecar loader with corrupted input to validate error handling.
