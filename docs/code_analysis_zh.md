# pg_textsearch（Tapir）代码架构与实现分析

> 版本：v1.4.0-dev | PostgreSQL 17/18 | 作者：Timescale/Tiger Data, Inc.

---

## 一、项目概述

**pg_textsearch**（内部代号 **Tapir**，Textual Analysis for Postgres Information Retrieval）是一个 PostgreSQL 全文检索扩展插件，提供基于 **BM25** 排名的搜索能力。它使用简洁的 SQL 语法（`ORDER BY col <@> 'query'`），并融合了现代搜索引擎优化的存储与检索技术。

### 1.1 核心能力

| 特性 | 实现方式 |
|------|----------|
| BM25 排名 | 完整 BM25 公式，可配置 k1、b |
| 快速 top-k | Block-Max WAND (BMW) 优化 |
| LSM 存储 | memtable + 分层 segment |
| WAL 兼容 | GenericXLog 实现物理复制 |
| 并行构建 | BufFile + N-way merge |
| 分区表 | 透传支持 |
| 表达式索引 | 支持函数表达式上的 BM25 索引 |
| 部分索引 | 支持 WHERE 谓词过滤 |

---

## 二、整体架构（三层设计）

```
┌─────────────────────────────────────────────────────┐
│  Layer 1: PostgreSQL Interface                      │
│  ┌──────────┬──────────────┬─────────────────────┐  │
│  │ access/  │   types/     │   planner/          │  │
│  │ AM 接口  │ bm25query    │ 查询重写 Hook       │  │
│  │          │ bm25vector   │ 路径选择/成本估算    │  │
│  └──────────┴──────────────┴─────────────────────┘  │
├─────────────────────────────────────────────────────┤
│  Layer 2: Index Coordination & Search Engine        │
│  ┌───────────┬────────────────────────────────────┐  │
│  │ scoring/  │  index/         debug/             │  │
│  │ BM25 公式 │  状态生命周期    导出/摘要         │  │
│  │ BMW 算法  │  全局注册表                         │  │
│  │           │  数据源抽象                         │  │
│  └───────────┴────────────────────────────────────┘  │
├─────────────────────────────────────────────────────┤
│  Layer 3: Storage Engine (LSM-style)                │
│  ┌──────────────────┬─────────────────────────────┐  │
│  │   memtable/      │   segment/                  │  │
│  │ L0 写缓冲         │ L1+ 持久化段                │  │
│  │ 链式分页          │ 字典 + Skip Index            │  │
│  │ 内存缓存 + spill  │ 块压缩 + BMW 加速            │  │
│  │ GenericXLog WAL   │ 多段合并 (merge sort)       │  │
│  └──────────────────┴─────────────────────────────┘  │
└─────────────────────────────────────────────────────┘
```

---

## 三、核心数据流

### 3.1 写路径（Document Insertion）

```
INSERT/COPY → tp_insert()
    │
    ├─ 分词 (to_tsvector via text_config)
    │
    ├─ 追加到 Memtable 内存缓存
    │   ├─ dshash: {lexeme → posting list}
    │   ├─ doc-length table
    │   └─ 内存上限触发 spill
    │
    ├─ 追加到 Memtable 磁盘链 (GenericXLog WAL)
    │   ├─ 分配/获取 tail 页
    │   ├─ 写入 TpMemtableRecord (项数 + doc_id + 词项+频率)
    │   ├─ 页满时分配 continuation 页
    │   └─ 更新 metapage.memtable_tail_blkno
    │
    └─ Auto-spill 或 Manual spill
        ├─ tp_spill_memtable()
        ├─ 构建 L0 segment
        └─ tp_maybe_compact_level() → LSM 跨层合并
```

### 3.2 读路径（Query Execution）

```
SELECT ... ORDER BY col <@> 'search terms'
    │
    ├─ planner/hooks.c 查询重写
    │   text <@> text → text <@> bm25query
    │
    ├─ scan.c tp_beginscan() → tp_rescan() → tp_gettuple()
    │
    └─ tp_execute_scoring_query()
        │
        ├─ 获取 index_state (LWLock SHARED)
        │
        ├─ 1. 查询内存缓存 (memtable cache)
        │   ├─ 单词项: 直接遍历 posting + 计算 BM25
        │   └─ 多词项: hash 累加分数
        │
        ├─ 2. 查询磁盘 memtable 链
        │   └─ TpMemtableChainSource → TpDataSource
        │       遍历链页 → 获取 posting 列表
        │
        ├─ 3. 查询磁盘 segment (L1+)
        │   ├─ 单词项 BMW: block_max_score 过滤
        │   └─ 多词项 BMW: WAND pivot + block_max 跳过
        │
        ├─ Top-K Min-Heap 维护分数
        │
        ├─ tp_topk_resolve_ctids() (segment 延迟解析)
        │
        └─ tp_topk_extract() 排序返回
```

---

## 四、模块详解

### 4.1 access/ — PostgreSQL 访问方法接口

这是插件与 PostgreSQL 核心引擎的桥梁层，实现 `IndexAmRoutine` 的所有回调。

| 文件 | 行数 | 核心职责 |
|------|------|----------|
| `handler.c` | 198 | `tp_handler()` 注册 AM 回调，`tp_options()` 解析索引选项，`tp_validate()` 校验类型 |
| `build.c` | ~1200+ | `tp_build()` 建索引、`tp_insert()` 单行插入、`tp_buildempty()`、并行构建入口、spill 逻辑 |
| `build_context.c` | ~800+ | 构建期上下文：posting 哈希、doc 长度跟踪、词项→freq 聚合 |
| `build_parallel.c` | ~600+ | 并行 worker 启动、同步、BufFile 中间段、N-way 合并 |
| `scan.c` | 530 | `tp_beginscan/rescan/gettuple/endscan`，BMW 评分调用、分数缓存 |
| `vacuum.c` | ~800+ | `tp_bulkdelete()` 删除死元组、`tp_vacuumcleanup()` 段重建 |

**关键设计决策：**
- **amstrategies = 0**，不实现搜索策略。搜索通过 `ORDER BY` 距离操作符实现，这使查询规划器可以正确成本估算
- **amcanorderbyop = true**，支持 ORDER BY 表达式，BM25 分数作为距离值返回（取负值实现 DESC 排序）
- `tp_cached_score` 避免 ORDER BY 中重复计算分数

### 4.2 memtable/ — L0 写缓冲层

写路径的两级存储：**内存缓存**（低延迟）+ **磁盘链**（持久化/恢复）。

#### 4.2.1 内存缓存 (`cache.h/cache.c`)

```
TpMemtable {
    dshash *inverted_index;   // 倒排索引: lexeme → TpPostingList
    dshash *doc_lengths;      // doc_length: ctid → length
    ...
}
```

- **dshash**：使用 PostgreSQL 动态共享哈希表，支持跨进程共享
- **Apply 协议**：`tp_cache_apply()` 将新记录添加到缓存
- **三档内存上限**：GUC 可配，触发 spill 释放内存
- **Cold build**：批量构建时直接从磁盘链读取，绕过缓存

#### 4.2.2 磁盘链 (`log.c/page.c`)

Memtable v2（Issue #374）的持久化格式：

```
┌─────────────────────────────────────────────┐
│ TpMemtablePageHeader (24 bytes)              │
│   magic     = 0x54415052 ("TAPR")            │
│   version   = 2                              │
│   flags     = TAIL | CONTINUATION | DEAD     │
│   n_records                                  │
│   free_offset                                │
│   next_block (链指针)                         │
│   prev_block                                 │
├─────────────────────────────────────────────┤
│ TpMemtableRecord[] (变长，按 free_offset)     │
│   n_entries                                  │
│   doc_id (ItemPointerData, 6 bytes)          │
│   entry[] {                                  │
│       lexeme_id (可选, 4 bytes)               │
│       frequency (2 bytes)                    │
│   }                                          │
└─────────────────────────────────────────────┘
```

**关键操作：**
- `tp_memtable_append()`：获取 tail 页 → 写入记录 → 页满则分配 continuation
- `tp_spill_finalize()`：标记当前链为 DEAD，将数据 flush 为 segment
- `tp_memtable_mark_chain_dead()`：VACUUM 标记死链

**WAL 策略**：使用 `GenericXLog`，每条记录的写入都生成 WAL 日志，使备机可以回放。

#### 4.2.3 数据源 (`chain_source.c`)

`TpMemtableChainSource` 实现 `TpDataSource` 接口，将磁盘链封装为统一的读取接口：
- `tp_source_get_postings(source, term)` → 遍历链页，过滤目标词项
- `tp_source_get_doc_length(source, ctid)` → 遍历链页，查找 doc 长度

### 4.3 segment/ — L1+ 持久化段

**Segment** 是 LSM 树中 L1 及以上的持久化存储单元，采用类 Lucene 的倒排索引格式。

#### 4.3.1 段格式 (`format.h`)

```
┌──────────────────────────────────┐
│ TpSegmentHeader (V5)             │  页头
│   magic, version, created_at     │
│   num_terms, num_docs, level     │
│   各 section 的偏移量             │
├──────────────────────────────────┤
│ Dictionary                        │
│   num_terms                      │
│   string_offsets[]               │  二分查找定位词项
│   string_pool (词项+dict_offset)  │
│   TpDictEntry[]                  │
│     skip_index_offset            │
│     block_count, doc_freq        │
├──────────────────────────────────┤
│ Skip Index                        │
│   TpSkipEntry[] (每 block 一个)   │
│     last_doc_id                  │  BMW 跳过决策
│     block_max_tf, block_max_norm │  块级上界
│     posting_offset, flags        │
├──────────────────────────────────┤
│ Posting Blocks                    │
│   TpBlockPosting[] (每 block)     │
│     doc_id (delta 编码)           │
│     frequency, fieldnorm          │
├──────────────────────────────────┤
│ Fieldnorm Table                   │  1 byte/doc
├──────────────────────────────────┤
│ CTID Map                          │  6 bytes/doc
│   ctid_pages[]                    │
│   ctid_offsets[]                  │
├──────────────────────────────────┤
│ Alive Bitset                      │  1 bit/doc (VACUUM)
└──────────────────────────────────┘
```

#### 4.3.2 核心操作

| 操作 | 文件 | 说明 |
|------|------|------|
| 段打开/读取 | `segment.c` (63KB) | `tp_segment_open()`，版本感知，缓存 CTID |
| 段扫描 | `scan.c` | 按词项遍历 posting 块 |
| 段合并 | `merge.c` (53KB) | LSM 合并 (N-way merge sort → 新段) |
| 块压缩 | `compression.c` | Delta 编码 + FOR/PFOR 压缩 |
| 存活标记 | `alive_bitset.c` | VACUUM 时标记死文档 |

**段合并算法** (`merge.c`):
1. 读取每个 source segment 的字典 (N-way merge source)
2. 按词项字典序合并：`merge_find_min_source()`
3. 对相同词项，收集所有 segment 引用
4. 构建合并 docmap (streaming N-way merge，按 CTID)
5. 流式写入新段：词典 → posting 块 → skip index → fieldnorm → CTID map
6. 使用 `GenericXLog` 回写 dict entry 和 header（增量 WAL）
7. 释放旧段页面到 FSM
8. 更新 metapage 链指针

### 4.4 scoring/ —BM25 与 Block-Max WAND 评分

#### 4.4.1 BM25 公式 (`bm25.c`)

标准 BM25 实现：

```
IDF = log(1 + (N - df + 0.5) / (df + 0.5))
BM25 = IDF * (TF * (k1 + 1)) / (TF + k1 * (1 - b + b * (dl / avgdl)))
```

- 多词项查询：各词项 BM25 贡献 **累加**
- query_freq 支持：查询中词项频率可加权

#### 4.4.2 Block-Max WAND (`bmw.c`，45KB)

BMW 是 WAND 算法的块级优化版本，是 top-k 查询的核心性能引擎。

**单词项 BMW：**
```
for each block:
    block_max = idf * (max_tf * (k1+1)) / (max_tf + k1 * min_len_norm)
    if block_max < threshold → skip block   // 块级剪枝
    else → 遍历块内 posting → 精确评分
```

**多词项 WAND 主循环：**

```
while active_terms > 0:
    1. threshold = heap.root.score  // 当前 top-k 最低分
    
    2. find_wand_pivot():
       terms 按 cur_doc_id 排序
       遍历累加 max_score
       if sum > threshold: pivot = 当前位置
       
    3. seek_to_pivot(): 将 pivot 前的 term 定位到 pivot_doc_id
    
    4. block_max_refinement:
       block_upper = Σ(pivot_terms_block_max)
       non_pivot_max = Σ(non_pivot_terms_max)
       if block_upper + non_pivot_max <= threshold:
           block_max_skip_advance()  // 跳过当前块
           continue
    
    5. verify_pivot_alignment() + 跳过死文档
    
    6. score_pivot_document(): BM25 精确评分
    
    7. 所有 pivot term 前进到下一条 posting
    
    8. restore_ordering()  // 维护 term 按 doc_id 排序
```

**Top-K Min-Heap：**
- 最小堆维护 k 个最高分文档
- `tp_topk_add_memtable()`：即时 CTID
- `tp_topk_add_segment()`：延迟 CTID（doc_id 代理，批量解析）
- 同分 tie-breaking：CTID 升序（保证确定性结果）

**关键优化：**
- `block_last_doc_ids` 预缓存：`seek_term_to_doc()` 使用二分查找 O(log B)
- `block_max_scores` 预计算：避免重复计算
- `cached_skip_entries` + `compressed_buf_cache`：减少重复 I/O
- 安全跳过条件 (Issue #365)：`block_upper + non_pivot_max <= threshold`

### 4.5 types/ — 自定义数据类型

#### bm25query
- 文本表示：`"search terms"` 或 `"index_name:{search terms}"`
- `to_bm25query(text)` 函数创建
- 内嵌索引 OID（可选）支持跨索引搜索

#### bm25vector
- 文本表示：`"index_name:{lexeme1:freq1,...}"`
- 等价操作符支持
- 用于内部词项→频率传递

### 4.6 planner/ — 查询规划器 Hook

**核心功能：** 使 `ORDER BY col <@> 'query'` 能自动匹配 BM25 索引。

**实现链路：**
1. **post_parse_analyze_hook**：解析阶段识别 `<@>` 操作符
2. **set_rel_pathlist_hook**：为表添加 IndexPath（BM25 索引扫描路径）
3. **planner_hook**：查询重写：
   - `text <@> text` → `text <@> to_bm25query(text)` 
   - `text <@> bm25query` → 保持原样
   - 分区表：递归处理每个分区

**关键数据结构：**
- `BM25OidCache`：缓存 OID（避免重复 syscache 查询）
- `tp_find_bm25_index_for_column()`：为列查找最佳 BM25 索引
- `bm25_implicit_to_explicit_cast_check()`：安全检查

### 4.7 index/ — 索引状态与协调

#### 状态管理

| 结构 | 范围 | 用途 |
|------|------|------|
| `TpLocalIndexState` | 后端私有 | 内存缓存引用、锁管理、segment 引用 |
| `TpSharedIndexState` | DSA 共享 | 段链信息、memtable 缓存句柄、事件通知 |
| `TpIndexMetaPage` | 磁盘元页 | 段层级信息、BM25 参数、语料统计、memtable 链指针 |

**生命周期：**
1. 首次访问 → `tp_get_local_index_state()` 创建/从共享状态附加
2. 使用 → `tp_acquire_index_lock(LW_SHARED)` 保护读取
3. 修改 → `tp_acquire_index_lock(LW_EXCLUSIVE)` 保护写入
4. 清理 → `tp_shutdown_spill_callback()` 后端退出时 spill memtable

**全局注册表 (`registry.c`)：**
- `TpGlobalRegistry`：共享内存中所有活跃索引的注册
- 支持动态注册/注销、查询

#### 数据源抽象 (`source.h`)

```c
typedef struct TpDataSource {
    TpPostingData *(*get_postings)(TpDataSource *, const char *term);
    void           (*free_postings)(TpDataSource *, TpPostingData *);
    int32          (*get_doc_length)(TpDataSource *, ItemPointerData *);
    int64          (*total_docs)(TpDataSource *);
    ...
} TpDataSource;
```

**两种实现：**
1. `TpMemtableChainSource`：从磁盘链读取
2. `TpMemtableCacheSource`：从内存缓存读取

BMW 评分对两者使用统一接口，无需感知数据来源。

---

## 五、关键设计要点

### 5.1 写路径的并发模型

**锁序约定** (`memtable/log.h` 文档化):
1. 先获取 metapage SHARED → 读取 tail 指针 → 释放
2. 再获取 tail 页 EXCLUSIVE → 分配 continuation → 获取 metapage EXCLUSIVE → 更新 tail
3. 页间锁序：尾页 → 元页（避免死锁）

**GenericXLog** 统一 WAL 方案：
- 非日志表 (UNLOGGED/TEMP)：跳过 WAL
- 持久化表：每条记录生成增量 WAL

### 5.2 LSM 风格分层存储

```
L0 (memtable):  频繁写入，内存+磁盘链
L1 (segment):   小段（< segments_per_level 个）
L2 (segment):   合并后较大的段
...
Lmax (segment): 背景合并至最顶层
```

**合并触发条件：**
- `tp_maybe_compact_level(level)`：当 `level_counts[level] >= segments_per_level` 时
- 递归触发：合并后检查 `level+1`
- `tp_force_merge_all()`：类似 Lucene `forceMerge(1)`，合并所有段

### 5.3 VACUUM 集成

**`tp_bulkdelete()`：**
- 遍历段中的 CTID 映射
- 对死元组调用 `callback()`
- 在段的 alive_bitset 中标记死文档

**`tp_vacuumcleanup()`：**
- 统计删除页数
- 段重建（剔除死文档）
- 更新索引统计信息

### 5.4 并行构建

**流程：**
1. Leader 分配 worker 和表扫描范围
2. 每个 worker 独立解析文档、构建本地 posting 哈希
3. Worker 将 posting 写入 BufFile
4. Leader 进行 N-way merge（与段合并类似）
5. 最终写入 segment

### 5.5 段压缩

**压缩策略** (`compression.c`)：
- Doc ID：delta 编码 + FOR (Frame of Reference)
- Frequency / Fieldnorm：变长整数编码
- 条件性压缩：仅当压缩后更小时启用（`TP_BLOCK_FLAG_DELTA`）

---

## 六、GUC 配置参数

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `bm25.segments_per_level` | 10 | 触发合并的最小段数 |
| `bm25.k1` | 1.2 | BM25 词频饱和参数 |
| `bm25.b` | 0.75 | BM25 长度归一化参数 |
| `bm25.memtable_cache_limit` | 64MB | 内存缓存上限 |
| `bm25.compress_segments` | true | 是否压缩段数据 |
| `bm25.log_scores` | false | 是否在 NOTICE 中记录分数 |
| `bm25.default_limit` | 100 | 默认返回结果数上限 |

---

## 七、测试体系

```
test/
├── sql/          # 70+ SQL 测试脚本
├── expected/     # 对应预期输出
└── scripts/      # 24 个 shell 测试
    ├── 并发测试
    ├── 崩溃恢复测试
    ├── 多后端段测试
    ├── 压力测试
    └── 物理/逻辑复制测试
```

---

## 八、构建与发布

**构建系统：** PGXS (PostgreSQL Extension Building Infrastructure)

**关键 Makefile target：**
- `make install` — 编译安装
- `make installcheck` — SQL 回归测试
- `make test-concurrency` — 并发测试
- `make test-recovery` — 崩溃恢复
- `make test-replication` — 复制测试
- `make coverage` — 代码覆盖率

**版本兼容：** 保留 v0.3.0 → v1.4.0-dev 的全部升级路径（21 个 SQL 迁移脚本），每个版本变更独立为一个 migration 脚本。

---

## 九、跨版本迁移机制

```sql
-- 版本链示例
pg_textsearch--0.3.0.sql              -- 最早期版本
pg_textsearch--0.3.0--0.4.0.sql       -- 升级脚本
pg_textsearch--0.4.0--0.5.0.sql
...
pg_textsearch--1.3.0--1.4.0-dev.sql   -- 最新升级脚本
pg_textsearch--1.4.0-dev.sql          -- 当前完整安装
```

每对版本间的迁移脚本仅包含差异，保证 `ALTER EXTENSION ... UPDATE` 能正确执行。

---

## 十、源码文件清单

```
src/
├── mod.c                 (18KB)  扩展入口、GUC、共享内存、Hook 安装
├── constants.h           (6.9KB) 全局常量
│
├── access/               # PostgreSQL AM 接口
│   ├── am.h                      核心头文件
│   ├── handler.c          (198行) tp_handler()
│   ├── build.c            (48KB)  索引构建 & INSERT
│   ├── build_context.c    (30KB)  构建上下文
│   ├── build_parallel.c   (24KB)  并行构建
│   ├── scan.c             (530行) 索引扫描
│   └── vacuum.c           (32KB)  VACUUM
│
├── memtable/             # L0 磁盘 memtable
│   ├── page.h/c           页格式 & 操作
│   ├── log.h/c            (50KB)  写路径
│   ├── cache.h/c          (36KB)  内存缓存
│   ├── cache_source.c     (18KB)  缓存数据源
│   ├── chain_source.c     (36KB)  链数据源
│   ├── chain_walker.c     (14KB)  链遍历器
│   ├── posting.c                  倒排哈希
│   ├── stringtable.c              词项驻留
│   ├── expull.c                   过期清理
│   ├── scan.c                     memtable 扫描
│   └── arena.c                    内存竞技场
│
├── segment/              # L1+ 持久化段
│   ├── format.h                  段格式定义
│   ├── segment.h/c        主段操作
│   ├── scan.c            (20KB)  段扫描
│   ├── merge.c           (53KB)  段合并 (LSM)
│   ├── docmap.c                  文档映射
│   ├── alive_bitset.c            存活标记
│   ├── compression.c             块压缩
│   ├── dictionary.c              词项字典
│   ├── fieldnorm.c               长度归一化
│   └── io.h                      段 I/O 工具
│
├── scoring/              # BM25 & BMW 评分
│   ├── bm25.h/c           BM25 公式
│   └── bmw.h/c           (45KB)  Block-Max WAND
│
├── index/                # 索引协调
│   ├── state.h/c         (43KB)  状态管理
│   ├── metapage.h/c              元页
│   ├── registry.c        (13KB)  全局注册表
│   ├── limit.c                   查询限制
│   ├── resolve.c                 索引解析
│   └── source.h                  数据源抽象
│
├── types/                # 自定义类型
│   ├── vector.h/c        (23KB)  bm25vector
│   ├── query.h/c         (35KB)  bm25query
│   └── array.c                   text[] 支持
│
├── planner/              # 查询规划
│   ├── hooks.h/c         (51KB)  查询重写 Hook
│   └── cost.c                    成本估算
│
└── debug/                # 调试工具
    └── dump.h/c          (34KB)  索引导出 & 摘要
```

**总计：89 个文件 (41 .h + 40 .c + 其他)，约 500KB+ 源代码。**

---

## 十一、总结

pg_textsearch 是一个精心设计的 PostgreSQL 全文检索插件，其架构融合了多个现代搜索引擎的最佳实践：

1. **LSM 风格存储**：memtable（L0）+ 分层 segment（L1+），平衡写入与查询性能
2. **Block-Max WAND**：块级上界剪枝，top-k 查询性能核心
3. **物理复制兼容**：GenericXLog 实现 WAL，支持主备复制
4. **透明接入**：Planner Hook 自动转换 `text <@> text` → 索引扫描，对用户零感知
5. **PGXS 构建**：标准 PostgreSQL 扩展，易于安装和升级
6. **完善的测试**：SQL 回归 + 并发 + 恢复 + 复制 + 压力测试全覆盖
