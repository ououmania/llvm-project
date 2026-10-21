# clangd-indexer 增量索引改动记录

`indexer/IndexerMain.cpp` 的改动,支持增量索引。配套的 watcher/systemd 部署见
YouCompleteMe 侧的 `clangd-watch-index-notes.md`。

## 改动

原来的 `clang::tooling::Executor` 全量驱动,重写为「手动遍历 + 增量缓存」:

- **三种模式**:全量(`--executor=all-TUs`)、增量(`--index-cache` + 位置参数传脏文件)、
  文件列表(位置参数无 CDB)。
- **增量缓存**:per-TU 一个 RIFF `.idx`(命名 `<文件名>.<hash>.idx`,与 clangd 后台索引一致),
  `manifest.json` 记录每 TU 的依赖文件 URI 列表(`Depends`)。
- **判脏策略由使用者定**:indexer 只提供机制,「哪些文件脏」由调用方传参;indexer 用
  反向索引(文件→依赖 TU)把脏文件映射成要重跑的 TU,其余复用缓存。
- **并发**:`llvm::DefaultThreadPool` 跑 TU(`--concurrency`,默认 8,0 = 硬件并发)。
- **进度输出**:每个重跑的 TU 打印 `[N/M] Indexing file ...`(复用不打印),匹配原 all-TUs executor 的行为。
- **结构**:`StaticIndexer` 类封装索引状态(CDB/Mangler/TFS/缓存/mutex/slab/manifest/进度),
  `main` 只做遍历分发。
- **兼容**:`--executor` 保留为 no-op(旧调用不报错)。

## 设计决策

1. **TU 级增量 > 文件级**:clangd 后台索引的 FileFilter 是「解析后跳过头文件符号收集」,
   省的是小头(符号收集),大头(解析)省不掉。TU 级增量(缓存复用)完全不解析未变 TU。
   代价是「改核心头文件 → 重跑几百上千依赖它的 TU」——语义正确,无法避免。
2. **不用 Executor**:Executor 只能「跑全部」,没法「只跑脏 TU」。手动遍历 + ThreadPool
   才能同时拿到 TU 级增量和并发。
3. **判脏策略上移**:「哪些文件脏」由使用者(watcher 的 inotify)通过参数传,indexer 不自算
   digest 判脏,职责清晰。
4. **「符号变」级增量不划算**:判断「符号变没变」要重新解析,成本 ≈ 重跑本身,无净收益;
   clangd 也是 digest 变了就重跑。故不加。

## 部署注意

- **resource dir**:LLVM 24 的 indexer 需要 clang-24 内置头,放 `~/.local/lib/clang/24/include/`
  (detect 的第一候选,不依赖 `$PATH`;systemd 的 `$PATH` 没有 `~/.local/llvm/bin`)。
- **并发**:默认 `--concurrency=8` 避免全量 OOM(32 核下大项目内存峰值过高)。
