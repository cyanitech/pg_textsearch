# 让 SQL 开口说话 —— PostgreSQL 全文检索引擎核心技术揭秘

> **分享人**：[姓名] &nbsp;|&nbsp; **时长**：45 分钟 &nbsp;|&nbsp; **受众**：后端/数据库研发

---

## Slide 1 · 封面

```
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
       让 SQL 开口说话
 PostgreSQL 全文检索引擎核心技术揭秘

            pg_textsearch (Tapir)

        分享人：XXX ｜ 2026.06
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
```

> **标题优化思路**：用"开口说话"暗示搜索的智能化，"核心技术揭秘"体现深度拆解，比原来的平淡标题更有吸引力。

---

## Slide 2 · 为什么需要全文检索？

```
三个问题：

  Q1: 用户搜 "PostgreSQL高性能全文检索"
      你怎么匹配？

  Q2: 匹配出来了 10000 条结果
      哪条更相关？

  Q3: 数据每时每刻在变
      索引怎么跟得上？
```

**Key Message**：传统 LIKE / ILIKE 解决不了相关性排序和实时性。

---

## Slide 3 · PG 内置搜索 vs pg_textsearch

```
                PostgreSQL GIN          pg_textsearch
                ──────────────          ─────────────
检索模型         布尔匹配                 BM25 概率模型
排序             0/1 (精确词匹配)        0~∞ (相关性分数)
Top-K            全量扫描后排序           BMW 剪枝，只算最好的 K 个
语法             to_tsquery('a & b')    ORDER BY col <@> 'a b'
数据同步         数据库原生               数据库原生
底层存储         GIN B-Tree              LSM-Tree
```

**Key Message**：pg_textsearch 让 PG 同时具备搜索和排序能力，不用再搭一套 ES。

---

## Slide 4 · 一句话看懂架构

```
 ┌─────────────────────────────────────┐
 │  SQL: ORDER BY content <@> 'query' │  用户写的
 └──────────────┬──────────────────────┘
                │ Planner Hook 自动转换
 ┌──────────────▼──────────────────────┐
 │  text <@> bm25query                 │  内部 rewritten
 └──────────────┬──────────────────────┘
                │ IndexScan
 ┌──────────────▼──────────────────────┐
 │  BMW 算法 → 倒排索引 → Top-K 堆    │  核心路径
 └──────────────┬──────────────────────┘
                │ CTID 回表
 ┌──────────────▼──────────────────────┐
 │  返回 Top-K 结果                    │
 └─────────────────────────────────────┘
```

**Key Message**：用户无感知，写的依然是纯 SQL。

---

## Slide 5 · Roadmap: 今天讲什么

```
  数据怎么存？           怎么搜得快？          怎么稳得住？
  ────────────         ────────────         ────────────
      §2                    §3                   §4
  LSM 存储引擎          BMW 查询引擎         并发 & 崩溃恢复
       │                     │                    │
  · memtable 写缓冲      · BM25 公式          · 锁序约定
  · segment 段格式       · Block-Max WAND     · GenericXLog WAL
  · 块压缩              · Top-K 堆           · 主备复制
  · 段合并(LSM)         · 多词项 WAND
```

---

## Slide 6 · §2.1 写路径全景

```
 INSERT INTO articles VALUES ('hello world')
          │
          ▼
    tp_insert()
          │
          ▼
   to_tsvector('english', 'hello world')
   → lexeme: 'hello', 'world'
          │
   ┌──────┴──────┐
   ▼             ▼
 内存缓存       磁盘链(Memtable Page Chain)
 │ dshash       │ TpMemtableRecord{hello→doc_id, world→doc_id}
 │ hello→[ctid] │ → 页满分配 continuation 页
 │ world→[ctid] │ → GenericXLog WAL 写入
 └──┬───────────┘
    │ 内存满 / 手动 spill
    ▼
 ┌──────────────┐
 │ L0 Segment   │  → 不可变，字典+倒排+CTID映射
 └──────────────┘
```

**Key Message**：写很快，只追加；分级存储（内存 → 磁盘链 → 段）。

---

## Slide 7 · §2.2 Segment 磁盘格式（核心！）

```
┌──────────────────────────────────────┐
│  Segment（一段逻辑连续的字节流）        │
├──────────────────────────────────────┤
│  TpSegmentHeader                     │  所有 section 的偏移量指针
├──────────────────────────────────────┤
│  词典(Dictionary)                     │  二分查找 O(log N)
│  {"hello":dict_entry, "world":...}   │
├──────────────────────────────────────┤
│  DictEntry[词项数]                    │  → skip_index, block_count, doc_freq
├──────────────────────────────────────┤
│  Posting Blocks                      │  每128条一块，可Delta压缩
│  hello: [doc0:tf1][doc1:tf1]...      │
│  world: [doc0:tf1][doc2:tf1]...      │
├──────────────────────────────────────┤
│  Skip Index                          │  BMW 跳过决策用
│  每块存: max_tf, min_fieldnorm        │
├──────────────────────────────────────┤
│  Fieldnorm Table                     │  1字节/文档 (Lucene SmallFloat)
│  CTID Map (doc_id → 物理地址)         │  6字节/文档
│  Alive Bitset (VACUUM标记)           │  1bit/文档
└──────────────────────────────────────┘
```

**Key Message**：紧凑、不可变、BMW 友好的磁盘布局。

---

## Slide 8 · §2.3 段如何落到 PG 物理页上

```
  逻辑地址空间 (segment 内部字节流)
  ════════════════════════════
  0           8167        16335        24503
  ├───────────┼───────────┼───────────┼───
  │  逻辑页 0  │  逻辑页 1  │  逻辑页 2  │  ...
  ├───────────┼───────────┼───────────┼───
       ↕           ↕           ↕
       │           │           │    page_index[] 映射
       ▼           ▼           ▼
  Block 42    Block 55    Block 78    ... (散落的物理块)

  一条 posting 跨页怎么办？ 读取时自动衔接，不需要特殊处理
```

**Key Message**：逻辑连续 + page_index 映射 = 简化上层逻辑，不关心物理位置。

---

## Slide 9 · §2.4 LSM 分层合并

```
  Memtable (L0, 可写)
    │ 每 spill 生成一个 L0 段
    ▼
  L0:  [Seg]→[Seg]→[Seg]→[Seg]→[Seg]
    │  攒够 segments_per_level(10) 个
    │  N-way merge → level+1
    ▼
  L1:  [Seg]→[Seg]
    │  攒够 10 → merge → L2
    ▼
  L2:  [Seg]

  优势:
  ✓ 写放大 O(log N)   而不是 O(N)
  ✓ 读扫描 对数级段数  而不是 O(N)
  ✓ 合并时剔除死文档，回收空间
```

**Key Message**：用"分批次小合并"换"读写平衡"。

---

## Slide 10 · §3.1 BM25 —— 什么是好结果？

```
  BM25(d, q) = Σ IDF(t) × ── TF(t,d) × (k₁+1) ──
                            TF(t,d) + k₁ × len_norm

  IDF(t):   词越稀有 → IDF 越大 → 权重越高
  TF(t,d):  文档中出现次数越多 → 分数越高
  len_norm: 文档越长 → 分母越大 → 分数被"惩罚"

  例: k₁=1.2, b=0.75
  "search" 在 1000 词的文档出现 3 次 → 分数低
  "search" 在 5 词的文档出现 3 次   → 分数高 ✓
```

**Key Message**：不仅有相关性打分，还自动抑制长文档通配作弊。

---

## Slide 11 · §3.2 Block-Max WAND —— 搜得快的秘密

```
  问题: top-10 查询，100万篇文档都要算一遍 BM25？

  答案: NO！用块的"上限分数"跳过不值得看的块

  ┌──────┬──────┬──────┬──────┬──────┐
  │block0│block1│block2│block3│block4│... 10000 blocks
  │max=5 │max=8 │max=4 │max=2 │max=9 │
  └──────┴──────┴──────┴──────┴──────┘
    ↑      ↑              ↑     ↑
  读取    读取   threshold=4 → 跳过！  读取...

  实际只读了很少的块，省去大量 I/O 和 CPU
```

**Key Message**：阈值来自 Top-K 堆——堆填满后阈值升高，跳过更激进。

---

## Slide 12 · §3.3 多词项 WAND —— 更复杂的协作

```
  查询: "hello world postgres"

  hello 迭代器 ─┬─ cursor
  world 迭代器 ─┼─ cursor  →  WAND 主循环
  postgres迭代器┘

  每个词都有:
  · global_max_score (各段取 max_block_max)
  · block_max_scores[] (每 block 一个上界)

  pivot = 第一个位置，到此为止的 max_score 累加 > threshold
  只算包含所有 pivot 词项的那些 doc_id

  相当于: 三个迭代器合作找出"共同出现且可能高分"的文档
```

**Key Message**：WAND = 多词项的智能交集 + 上限剪枝。

---

## Slide 13 · §3.4 Top-K 堆 —— 全局评分汇集

```
  heap (Min-Heap, size=K):
  ┌───┬───┬───┬───┬───┐
  │3.2│4.1│5.0│7.2│9.8│
  └───┴───┴───┴───┴───┘
    ↑              ↑
  threshold=3.2   最高分=9.8

  新候选 doc: score=6.0 →  >3.2? YES → 替换堆顶
  新候选 doc: score=2.5 →  >3.2? NO  → 丢弃

  Memtable 文档: 即时 CTID → tp_topk_add_memtable()
  Segment 文档:   doc_id 代理 → tp_topk_add_segment()
  → 提取前批量 tp_topk_resolve_ctids()
```

**Key Message**：全局一个堆，各层（内存+磁盘+多段）统一贡献。

---

## Slide 14 · §4.1 并发与恢复全景

```
  写入并发:           读取并发:            崩溃恢复:
  ──────────          ────────            ────────
  metapage SHARE     metapage SHARE      WAL REDO 重放
  → 读tail指针       → 读段链头          → memtable链恢复
  → 释放             → 释放               → segment页恢复
                                         → 主备一致
  memtail EXCLUSIVE   segment 读
  → 追加记录           → page_map加载     PG Crash-Recovery
  → 页满分配新页       → BMW评分          → pg_textsearch
                         (无锁，段不可变)     无需额外恢复
  metapage EXCLUSIVE
  → 更新链尾指针
       │
  GenericXLog (WAL)
```

**Key Message**：读无锁（段不可变），写有锁序（防死锁），WAL 保证一致。

---

## Slide 15 · §4.2 WAL 兼容 —— 为什么主备也能用？

```
  主库 INSERT → memtable 链追加
                    │
              GenericXLog 记录 (page delta)
                    │
  ┌─────────────────┼─────────────────┐
  ▼                 ▼                  ▼
 主库数据页          WAL Stream        直接可用！
  正常写入          ───────→ 备库 REDO


  segment 合并时:
  · 大块数据: log_newpage_buffer (全页镜像)
  · 小量回写: GenericXLog (增量 delta)
  · 比例最优化


  对比 ES: 需要独立的数据同步机制
  pg_textsearch: 开箱即用，主备一致
```

**Key Message**：融入 PG 的 WAL 体系，复制零额外成本。

---

## Slide 16 · 性能数据（示意）

```
  数据集: MS MARCO Passage (884 万文档)

  查询类型          | pg_textsearch (BMW) | 全量 BM25 | 加速比
  ─────────────────────────────────────────────────
  单词项 top10       |    2.1 ms             58 ms    │ 27×
  单词项 top100      |    5.8 ms            62 ms    │ 10×
  多词项(3词) top10  |    4.3 ms            74 ms    │ 17×
  多词项(3词) top100 |    8.2 ms            78 ms    │ 9×


  Block-Max WAND 跳过率:
  · top-10: 95%+ blocks 被跳过
  · top-100: 92%+ blocks 被跳过
```

> *注意：以上数据需用项目自带的 benchmarks/ 套件实测*

---

## Slide 17 · 核心技术要点回顾

```
    存储                      查询                      可靠性
    ─────                    ──────                    ──────

  LSM 分层引擎              BM25 模型              GenericXLog WAL
  ├ 写：Memtable追加        ├ IDF×TF 公式           ├ 全页镜像 / 增量Delta
  ├ 读：不可变段 + BMW      ├ 可配 k1, b 参数       ├ 主备无缝复制
  └ 合并：N-way merge       └ 字段长度归一化        └ 崩溃恢复零干预

  段紧凑格式                Block-Max WAND          Planner Hook
  ├ 词典 + 分块倒排         ├ 块级最大化分数        ├ 隐式查询转换
  ├ DocID 替代 CTID         ├ 安全跳过条件          ├ 索引自动选择
  └ 每块 128 条, 可压缩     └ Top-K Min-Heap        └ 分区表透明支持
```

---

## Slide 18 · 一句话总结

```
  ┌────────────────────────────────────────────────────┐
  │                                                    │
  │    pg_textsearch =                                 │
  │    PostgreSQL 的碎片化管理  +  Lucene 的搜索能力    │
  │                                                    │
  │    用户只需要写:                                    │
  │    SELECT * FROM t ORDER BY col <@> 'query' LIMIT K│
  │                                                    │
  └────────────────────────────────────────────────────┘
```

---

## Slide 19 · Q&A

```
  ┌─────────────────────────┐
  │                         │
  │         Q & A           │
  │                         │
  │   欢迎提问 & 技术讨论     │
  │                         │
  └─────────────────────────┘

  相关文档:
  · docs/code_analysis_zh.md        (完整架构分析)
  · docs/segment_format_example_zh.md (段格式详解)
  · docs/paper_zh.md                 (期刊论文稿)

  代码仓库: https://github.com/xxx/pg_textsearch
```

---

## 演讲备注（Presenter Notes）

**Slide 2:** 抛出三个真实痛点，引导听众思考。"这三件事，传统数据库哪件能做好？"

**Slide 4:** 强调"用户零感知"——写 SQL 跟平时一样，Planner 自动接住了。

**Slide 7:** 最核心的一张图。段格式需要顺序展开讲解：先有词典 → 词典指到 posting → posting 分块 → skip entry 加速 BMW。

**Slide 8:** 用房子和门牌号做类比：数据是"虚拟连续"的，page_index 就是门牌号。

**Slide 11:** BMW 算法的直觉：想象你在找"得分最高的 10 个全年级试卷"，你不需要批改所有试卷——看一眼每班第一名成绩就够了。如果第一名都进不了全校前十，整个班跳过。

**Slide 14:** 重点讲"读无锁"——因为段是不可变的，所以并发读不需要任何锁。这是设计上最漂亮的地方。

**Slide 18:** 收尾金句，让听众带走核心印象。
