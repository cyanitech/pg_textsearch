# 专利问题与项目实现问答纪要

本文档整理了围绕 pg_textsearch 项目实现、PostgreSQL 索引 WAL 机制，以及用户提出的专利思路之间差异的问答内容，便于后续查阅。

## 1. 项目中的段页存储与专利方案有什么差异，为什么不需要额外定义 WAL？

### 问题

用户提出了一份围绕“支持主备故障切换的页式全文索引 WAL 重放与回收方法”的专利方案，并询问：这个项目的段内容页存储和该专利方案相比有什么差异，为什么不需要额外定义 WAL，而是直接使用 PostgreSQL 原生 WAL 就能完成主备同步。

### 回答

核心差异只有一句话：

你的专利方案，本质上是在“索引语义层”额外定义了一套 WAL 协议；
而这个项目当前的做法，是把需要恢复和同步的状态全部落成 PostgreSQL 普通索引页上的物理状态，然后让 PostgreSQL 现成的物理 WAL 去复制这些页变化。

所以它不需要再定义“段可见性切换 WAL”“回收资格登记 WAL”这一层语义日志。

当前实现里，最关键的设计变更是：不再把“需要恢复的真实状态”放在共享内存里，而是放在索引关系自己的页面里。

可以直接看这里：
[docs/memtable_v2.md](e:/2026/codes/pg_textsearch/docs/memtable_v2.md)

这个文档写得很明确，v1 时代：

- memtable 在 DSA/shared memory 里
- 为了复制和恢复，项目曾经用过自定义 rmgr 和自定义 WAL 记录
- 还要靠额外的 docid pages 在重启后重建内存结构

到了 v2：

- L0 memtable 直接变成 index relation 里的 page chain
- 页面修改都通过 `GenericXLog`
- 不再有 custom rmgr
- 不再有 docid pages
- PostgreSQL 原生 replay 直接重建每一页

也就是说，这个项目现在依赖的是“页级物理真实状态”，不是“页外逻辑状态”。

你贴的专利里，核心思想是把一次索引更新拆成三类语义事件：

1. 页内容变更
2. 段可见性切换
3. 回收资格登记

然后要求备库按“先页链、再可见性、后回收资格”的语义顺序重放。

这个思路适合“真正的索引一致性，不完全等价于页面字节一致性”的系统，也就是：

- 有额外的段目录语义
- 有独立的回收候选结构
- 有主备之间要单独协调的安全回收边界
- 备库必须理解这些高层语义，才能知道什么时候一个段算“可见”、什么时候一个旧段算“能回收”

而 pg_textsearch 当前不是这么设计的。它做的是：

- 段内容就在普通 relation pages 上
- metapage 里直接存 `level_heads[]`、`memtable_head_blkno`、`memtable_tail_blkno`
- spill / merge / vacuum 的结果也是普通页修改
- 这些修改本身就按 WAL LSN 顺序进入 PostgreSQL 的物理 WAL

可以看：
[src/access/build.c](e:/2026/codes/pg_textsearch/src/access/build.c)
[src/index/metapage.c](e:/2026/codes/pg_textsearch/src/index/metapage.c)
[src/memtable/log.c](e:/2026/codes/pg_textsearch/src/memtable/log.c)
[src/segment/merge.c](e:/2026/codes/pg_textsearch/src/segment/merge.c)

这里的关键不是“备库理解段语义”，而是“备库拿到和主库一样的页字节结果”。

这个项目解决一致性的方式是：直接把“可见性切换”和“回收前状态”编码成普通页修改的先后顺序，而不是再抽象出一层独立的语义 WAL。

换句话说，专利方案解决的是“语义顺序问题”，这个项目解决的是“物理页面真相源问题”。

你的专利里，备库必须知道：

- 哪条 WAL 表示“段已经可以看见”
- 哪条 WAL 只是“页写好了但不能看”
- 哪条 WAL 只是“可回收登记”而不是“立刻释放”

而 pg_textsearch 现在的设计是：只要 standby 把 WAL 顺序回放完，页面自然就处于正确状态。

因为：

- “段是否可见”体现在 metapage / segment linkage 上
- “旧页是否还能回收”体现在 dead flag、dead_fxid、alive bitset、以及后续 vacuum/merge 的物理页状态上
- “当前 memtable 是什么”体现在链表页本身，不在共享内存里

所以 standby 不需要额外理解“这是第一类日志、第二类日志还是第三类日志”。它只需要照 PostgreSQL 的原生 WAL 机制把页面改出来。

再说最关键的点：为什么这里不需要额外定义 WAL？因为 PostgreSQL 原生物理复制已经保证了两件事：

1. 同一个 relation page 的修改按 LSN 顺序重放
2. standby 上最终得到的就是主库当时写出来的那批页面状态

对 pg_textsearch 来说，这已经够了，前提是你把所有关键状态都放进这些页里，并且更新顺序本身是 crash-safe 的。

这个项目正是这么做的。

例如 memtable spill，代码里最重要的一步不是发一条“逻辑切换 WAL”，而是直接修改：

- 新 segment 页
- metapage
- 旧 chain 页状态

并且顺序做得很谨慎。

在 [src/access/build.c](e:/2026/codes/pg_textsearch/src/access/build.c) 的 `tp_do_spill()` 里，注释写得非常直接：

- 先 `tp_spill_finalize`
- 再把旧 chain 标 DEAD

原因是：如果先标 DEAD 再改 metapage，崩溃时可能出现“链还可达，但页已可回收”的错误状态。

所以就不需要再额外造一层“索引语义 WAL 协议”。

最简结论：

- 你的专利路线：索引有自己独立的复制/回收语义，所以定义更高层的三阶段 WAL 协议
- 这个项目路线：把索引真实状态尽量压回 PostgreSQL 普通页，所有关键变化都变成页修改，于是直接复用 PostgreSQL 原生物理 WAL

它“不需要额外定义 WAL”的根本原因不是“段式索引天然不需要”，而是：它把以前需要额外 WAL 处理的那部分“页外状态”干掉了。

## 2. PostgreSQL 的 GIN 索引是不是也是这个原理？

### 问题

用户询问：那 pg 的 GIN 索引是不是也具备类似原理。

### 回答

对，高层原理上是相通的，但实现层面不完全一样。

可以分成两层看。

第一层，相同点：GIN 也属于“主备同步靠 WAL 重放，把索引页恢复出来”的路线。也就是说，GIN 的可恢复真相同样在磁盘页里，而不是放在某个 standby 无法自动重建的页外状态里。主库改了索引页，WAL 记录下来，备库按顺序重放，最后得到一致的索引结构。从这个角度说，GIN 和现在这个项目的 v2 设计，思路是一致的。

第二层，关键差异：GIN 不是“什么都不额外定义，纯靠 GenericXLog”。GIN 是 PostgreSQL 内核自带的索引 AM，它有自己在 PostgreSQL core 里面实现的专用 WAL 记录和 redo 逻辑。也就是说：

- GIN 当然是靠 WAL 同步主备
- 但它用的是 core 已经认识的 GIN 专用 WAL
- 不是像这个项目一样，刻意把状态压缩成普通页修改，然后尽量只用 `GenericXLog`

更准确地说：

- GIN：也是页级真相源，也是 WAL 驱动主备一致；但它走的是“内核自带 custom rmgr / custom redo”的路
- pg_textsearch 现在这套 v2：也是页级真相源，但它特意避免自定义 rmgr，而是把关键状态都做成普通索引页修改，让 PostgreSQL 原生物理 WAL 直接处理

为什么 GIN 可以这么做，而这个扩展不这么做？

因为 GIN 是 PostgreSQL 内建索引，redo 代码就在 PostgreSQL 服务器里，standby 天然认识它的 WAL 语义。
而扩展如果自己搞一套自定义 WAL 语义，会遇到一个现实问题：某些恢复场景并不会加载扩展 `.so`，这时 redo 端未必能理解你扩展定义的索引语义。`memtable_v2.md` 里说的 single-page redo 兼容性，本质就是这个问题。

所以结论是：

- 是的，GIN 本质上也属于“索引一致性靠 WAL 重放磁盘页”这一类
- 但不是同一个技术细节
- GIN 有 core 级专用 WAL
- pg_textsearch v2 是为了避免扩展自定义 redo 的问题，改成了 on-disk truth + GenericXLog 路线

## 3. 这个专利是不是没有价值了？

### 问题

用户进一步追问：那我的这个专利岂不是没有价值了。

### 回答

不一定。技术上看，不会因为 PostgreSQL、GIN 或这个项目能靠原生 WAL 做主备同步，你的专利就自动“没有价值”。真正决定价值的，不是“是否用了 WAL”，而是你主张保护的创新点到底落在哪一层。

如果核心主张只是：

- 页式全文索引也可以靠 WAL 同步主备
- 备库按日志顺序恢复索引页
- 不需要重建索引

那这类表述的价值确实会比较弱，因为这更像已有数据库/索引系统的通用原理，容易被现有技术覆盖。

但如果真正的创新点是下面这些，更可能还有价值：

- 把索引更新明确拆成“页写入、段可见性切换、回收资格登记”三阶段语义协议
- 旧段物理回收必须受“副本确认水位”约束，而不是只看主库本地状态
- 主备切换后对历史段、回收候选段、可见段做专项一致性校验
- 故障切换窗口内允许旧段作为只读回退段继续参与查询
- 把“逻辑删除”和“物理回收”在跨副本场景下安全解耦

这些点，和“GIN/pg_textsearch 通过物理 WAL 重放页面”不是同一层创新。前者是跨副本段生命周期管理协议，后者是页面级持久化与恢复机制。

更直接一点说：

- 如果权利要求写得太宽，只是在说“索引页通过 WAL 同步”，那价值大概率有限。
- 如果权利要求收敛到“段式全文索引在异步复制和故障切换下的安全回收/可见性切换协议”，那仍然可能有明确价值。

现在最该警惕的不是“有没有价值”，而是“是不是把真正有价值的点写进权利要求了”。很多专利不是技术没价值，而是权利要求把创新点写散了、写泛了。

建议用这个标准自查：

1. 你的独特点，是否不依赖“用了 WAL”这个大前提，单独成立？
2. 你的独特点，是否专门解决了“异步复制下旧页提前回收”和“failover 后索引段状态不一致”？
3. 你的权利要求，是否把“副本确认水位 + 回收边界 + 切换校验”写成了必要技术特征，而不是背景描述？

如果这三条都能答“是”，那这件事就还有价值，而且价值点不在“WAL”本身，而在复制安全边界和回收协议。

## 4. 专利中的“页式全文索引”和这个项目中的页存储是不是一样？

### 问题

用户继续追问：那我的这个专利中的页式全文索引，和这个项目中的页存储不是一样吗。

### 回答

不一样，最多只能说“都用了数据库页作为物理存储载体”，但这不等于“技术方案相同”。

你的专利里的“页式全文索引”，如果按前面那套表述理解，核心含义其实是：

- 全文索引的段和元数据都落在页式存储里
- 段有可见性切换语义
- 旧段进入回收候选集合
- 回收受主备确认位点约束
- failover 后还要做索引专属一致性校验

也就是说，你的“页式全文索引”不是单指“数据放在 page 里”，而是指一整套段式索引在页存储上的生命周期管理协议。

而这个项目里的“页存储”更准确地说是：

- memtable v2 变成了 index relation 内的一条 page chain
- segment 也落在索引页里
- metapage 记录 level heads、memtable head/tail 等物理入口
- 依靠 PostgreSQL 原生 WAL + `GenericXLog` 重放这些页修改
- 页回收主要靠本地的 dead 标记、visibility horizon、VACUUM 和 merge 纪律

所以两者的关系更像是：

- 相同点：都把全文索引内容放在数据库页里，而不是外部文件或纯内存结构
- 不同点：你专利强调的是“页上段式索引在主备切换下的可见性、回收、水位、校验协议”；这个项目强调的是“把真实状态落到页里，从而直接复用 PostgreSQL 现有物理 WAL/恢复机制”

换句话说：

- “页式存储”这一层，二者有重叠
- “页上索引的复制一致性协议”这一层，二者并不一样
- 这个项目并没有实现你专利里那种“三类 WAL 语义 + 副本确认回收水位 + 切换后保留历史段”的完整方案

所以不能简单下结论说“你的页式全文索引和这个项目完全一样”。

更严谨的判断应该是：

- 如果专利的创新点只是“全文索引内容存储在数据库页中”，那和这个项目重合度很高，价值会偏弱
- 如果专利的创新点是“页式段索引在主备复制/切换场景下的可见性切换与安全回收机制”，那和这个项目就不是一回事

最关键的一句是：

“页存储”只是物理载体层；真正决定是不是同一技术方案的，是页上的状态机和主备一致性协议。

## 5. 如果一个词汇的倒排索引很大，要多个页去存储，这个词汇的所有页信息存储在哪里？

### 问题

用户询问：这个项目中，如果一个词汇的倒排索引很大，要多个页去存储这个文档倒排信息的，那一个词汇的所有页信息存储在哪里？

### 回答

不是把“某个词的所有页号列表”直接存一份出来。

这个项目里，如果一个词的倒排很大，跨了多个页，它的定位信息是分层存的，大致是这条链：

`term` → `TpDictEntry` → `TpSkipEntry[]` → `posting_offset` → segment 的 page index → 物理页

也就是说，一个词所有倒排块的信息，首先存放在这个词对应的 dictionary entry 和 skip index 里，不是单独再维护一条“词到页链”的 page list。

可以拆成 4 层看。

第一层，词本身先在字典里定位。

在 [src/segment/format.h](e:/2026/codes/pg_textsearch/src/segment/format.h) 里，segment 有一套字典结构：

- `TpDictionary`
- `TpStringEntry`
- `TpDictEntry`

其中 `TpDictEntry` 是每个词最关键的入口，里面有两个核心字段：

- `skip_index_offset`
- `block_count`

含义是：

- 这个词对应的一组 skip entry 从哪里开始
- 这个词总共有多少个 posting block

所以一个词如果倒排很大，不是直接记“用了哪些页”，而是先记“这个词有多少个 block，这些 block 的 skip index 从哪开始”。

第二层，每个 block 再通过 skip entry 找到具体 posting 数据。

还是在 [src/segment/format.h](e:/2026/codes/pg_textsearch/src/segment/format.h) 里，`TpSkipEntry` 里有这个字段：

- `posting_offset`

这个字段非常关键。它不是“页号”，而是：这个 block 的 posting data 在 segment 逻辑文件里的字节偏移。

同时每个 `TpSkipEntry` 还带：

- `doc_count`
- `last_doc_id`
- `block_max_tf`
- `block_max_norm`

所以它既是跳表入口，也是 BMW 用的 block 级统计信息。

换句话说：

- 一个词的所有倒排，不是一个连续“词页链”
- 而是被拆成多个 block
- 每个 block 各自有一个 `posting_offset`
- 这些 `posting_offset` 的集合，就是这个词的“所有倒排块入口”

第三层，offset 不是直接页号，要再经过 segment 的逻辑页映射。

segment header 在 [src/segment/format.h](e:/2026/codes/pg_textsearch/src/segment/format.h) 里定义了很多 section offset，比如：

- `dictionary_offset`
- `entries_offset`
- `postings_offset`
- `skip_index_offset`
- `page_index`

这里的设计是“逻辑文件布局”，不是直接把每个区段写成固定单页结构。

也就是说：

- posting 数据先按逻辑偏移组织
- 再通过 `page_index` 把逻辑偏移映射到实际物理 block

这个可以从 [src/segment/segment.h](e:/2026/codes/pg_textsearch/src/segment/segment.h) 和 [src/segment/merge.c](e:/2026/codes/pg_textsearch/src/segment/merge.c) 看出来。`merge.c` 里不断写 `sink->current_offset`，说明系统内部把整个 segment 当成一个“逻辑连续文件”来写，然后再落到多个实际页中。

所以一个词跨多个页时，真正保存“跨页信息”的，不是“页链表”，而是：

- 这个词有哪些 block：`block_count`
- 每个 block 从哪个逻辑 offset 开始：`TpSkipEntry.posting_offset`
- 逻辑 offset 再通过 segment 的 page index 找到物理页

第四层，读取时就是按这条链走的。

在 [src/segment/scan.c](e:/2026/codes/pg_textsearch/src/segment/scan.c) 里，迭代器读取某个词时，会拿到：

- `dict_entry.block_count`
- 当前 block 对应的 `skip_entry.posting_offset`

然后按 offset 去读 block 数据。

所以从实现角度说，这个项目对“大词条”的组织方式更像：

- “一个 term 对应多个 posting block”
- “每个 block 用 skip entry 索引”
- “block 数据通过逻辑 offset 分布在多个 data page 上”

而不是：

- “一个 term 挂一条 page chain，把所有页号都串起来”

结论：

如果一个词的倒排索引很大，跨多个页：

- 这个词的总入口保存在 `TpDictEntry` 中
- 这个词下每个倒排块的位置保存在 `TpSkipEntry[]` 中的 `posting_offset`
- 这些 offset 再通过 segment 的 `page_index` 映射到具体物理页
- 不存在一份单独的“这个词用了哪些页”的显式页号数组或页链结构

可以把它理解成：

- 词级元数据：`TpDictEntry`
- 块级元数据：`TpSkipEntry[]`
- 页级定位：segment `page_index`
--------------------------


我先核对扩展被 PostgreSQL 装载时的入口，以及这个自定义索引方法暴露给 SQL 的入口函数。

Searched for regex `_PG_init|_PG_fini|PG_MODULE_MAGIC|tp_handler` (`**/src/**`), 8 results

Searched for regex `MODULE_PATHNAME|CREATE ACCESS METHOD|tp_handler` (`**/sql/pg_textsearch--1.4.0-dev.sql`), 37 results

Read [](file:///e%3A/2026/codes/pg_textsearch/src/mod.c#1-1), lines 1 to 220

我已经确认有两个层面的入口：扩展装载入口和 bm25 索引方法入口。再补 handler 这一段，把链路闭合。

Read [](file:///e%3A/2026/codes/pg_textsearch/src/access/handler.c#1-1), lines 1 to 140

这个插件如果说“入口点”，严格说有两个最重要的入口。

第一个是 PostgreSQL 加载扩展共享库时的入口：mod.c。它在 src/mod.c 里定义，用来做扩展级初始化，比如：
- 检查必须通过 `shared_preload_libraries` 预加载
- 注册 GUC 参数
- 安装 shared memory / hook / planner hook 等基础设施

从 PostgreSQL 扩展生命周期看，这个是“插件装载入口”。

第二个是这个自定义索引访问方法 `bm25` 的入口：`tp_handler`，定义在 handler.c。SQL 安装脚本把它注册成 access method handler，见 pg_textsearch--1.4.0-dev.sql 和 pg_textsearch--1.4.0-dev.sql。`tp_handler()` 返回一个 `IndexAmRoutine`，把 PostgreSQL 需要的各个索引回调接上，比如：
- `ambuild = tp_build`
- `aminsert = tp_insert`
- `ambulkdelete = tp_bulkdelete`
- `amvacuumcleanup = tp_vacuumcleanup`
- `ambeginscan = tp_beginscan`
- `amgettuple = tp_gettuple`

所以如果你问“这个 pg 插件最核心的入口点在哪”，答案是：

- 扩展加载入口在 mod.c 的 `_PG_init`
- bm25 索引方法入口在 handler.c 的 `tp_handler`

如果再往上追一层，PostgreSQL 通过安装脚本里的 `AS 'MODULE_PATHNAME', 'tp_handler'` 把 SQL 世界和这个 C 入口连起来，见 pg_textsearch--1.4.0-dev.sql。

如果你愿意，我可以下一步继续给你画一条完整调用链：

`CREATE EXTENSION / shared_preload_libraries` -> `_PG_init` -> `CREATE ACCESS METHOD bm25` -> `tp_handler` -> `tp_build / tp_insert / tp_gettuple`

Added 3 todos

-------------------------

是否支持并行查询，暂不支持

我先核对查询路径和 AM 能力位，确认它是不是只支持并行建索引、不支持并行扫描，更不用说段级并行查询。

Searched for regex `amcanparallel|amestimateparallelscan|aminitparallelscan|amparallelrescan|parallel scan|parallel` (`**/src/access/handler.c`), 9 results

Searched for regex `parallel.*scan|segment.*parallel|worker|parallel` (`**/src/access/scan.c`), no results

Searched for regex `parallel build|parallel scan|amcanparallel` (`**/CLAUDE.md`), no results

结论方向已经很明确了：查询侧没有并行扫描能力。我再补一处评分/扫描入口，确认它确实是在单个 scan 路径里串行汇总 memtable 和各层 segments。

Read [](file:///e%3A/2026/codes/pg_textsearch/src/access/handler.c#76-76), lines 76 to 118

Searched for regex `tp_score_single_term_bmw|tp_score_multi_term_bmw|level_heads|for \(level = 0; level < TP_MAX_LEVELS` (`**/src/scoring/bm25.c`), 15 results

Read [](file:///e%3A/2026/codes/pg_textsearch/src/access/handler.c#76-76), lines 76 to 118

Searched for regex `tp_score_documents|tp_execute_scoring_query|BMW|multi_term_bmw|single_term_bmw` (`**/src/access/scan.c`), 3 results

Searched for regex `tp_score_single_term_bmw|tp_score_multi_term_bmw` (`**/src/scoring/**`), 6 results

Read [](file:///e%3A/2026/codes/pg_textsearch/src/scoring/bm25.c#38-38), lines 38 to 110

Added 3 todos

当前这个项目里，不支持你说的“段并行查询”。

最直接的证据在 handler.c：
- `amcanparallel = false`
- `amestimateparallelscan = NULL`
- `aminitparallelscan = NULL`
- `amparallelrescan = NULL`

这说明这个 bm25 access method 没有实现 PostgreSQL 的并行扫描接口。相对地，它只开启了并行建索引，`amcanbuildparallel = true`，也就是“build 可以并行，query scan 不行”，同样在 handler.c。

查询实现本身也体现为单条扫描路径里的串行聚合。BM25 评分在 bm25.c 先把 memtable 和所有 level 的 segment 统计合并，代码就是一个 `for (level = 0; level < TP_MAX_LEVELS; level++)` 顺序遍历各层 `level_heads`。后面再调用 `tp_score_single_term_bmw` 或 `tp_score_multi_term_bmw`，见 bm25.c 和 bm25.c。扫描入口也还是单个 `IndexScanDesc` 的执行路径，见 scan.c。

所以准确回答是：

- 单个 bm25 索引查询：不支持段并行查询，不支持并行 index scan。
- 当前支持的并行能力：并行建索引。
- 查询提速依赖的是 BMW 剪枝，不是多 worker 并行扫各个 segment。

补一句边界情况：如果你是分区表，PostgreSQL 在更外层计划上可能有机会做 partition 级别的并发/并行调度，但那不是这个 AM 自己实现的“段并行查询”。对单个索引内部的多个 segment，目前就是串行处理。

如果你要，我可以继续往下给你拆一版“要支持段并行查询，这个项目至少要改哪些接口和代码路径”。



