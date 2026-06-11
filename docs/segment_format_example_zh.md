# pg_textsearch Segment 段格式详解（附完整示例）

> 基于 V5 格式，`src/segment/format.h` + `src/segment/merge.c` + `src/segment/compression.c`

---

## 一、核心设计思想

Segment 是 LSM 树中 L1 及以上的持久化存储单元，类似 Lucene 的 segment。一句话概括：

> **以词项（term）为中心组织倒排索引，配合段内 doc_id → CTID 映射，实现高效的 BM25 搜索。**

设计目标：
1. **BMW 友好**：每个词项的 posting 分块（block），每块携带 max_tf/min_norm 用于跳过决策
2. **顺序 I/O**：段是一次性写入、不可变的，所有数据连续排列
3. **紧凑存储**：doc_id 使用 4 字节段内编号 + Delta 压缩，fieldnorm 用 1 字节量化
4. **延迟 CTID 解析**：posting 不存完整 CTID，而是段内 doc_id，查询结束时批量解析

---

## 二、完整示例：3 篇文档 + 3 个词项

### 2.1 原始数据

假设我们有 3 篇文档插入表 `articles(content text)`：

```
文档 A: CTID (100,1)   content = "hello world"          → 词项: hello, world
文档 B: CTID (100,2)   content = "hello postgres"        → 词项: hello, postgres
文档 C: CTID (200,1)   content = "world"                 → 词项: world
```

经过 PostgreSQL `to_tsvector('english', content)` 分词并写入 segment 后：

### 2.2 doc_id 分配

docmap 会按 **CTID 排序** 分配段内 doc_id：

```
段内 doc_id   CTID        content            doc_length (词数)
─────────────────────────────────────────────────────────
  0           (100,1)    "hello world"             2
  1           (100,2)    "hello postgres"          2
  2           (200,1)    "world"                   1
─────────────────────────────────────────────────────────
总计: total_docs=3, total_tokens=5, avg_doc_len=5/3≈1.67
```

**关键：doc_id 按 CTID 排序分配，保证 posting 按 doc_id 排序 = 按 CTID 排序。**

### 2.3 倒排索引（构建期）

```
词项     doc_freq    出现在哪些 (doc_id, tf)
────────────────────────────────────────────
hello       2        doc_id=0 tf=1, doc_id=1 tf=1
postgres    1        doc_id=1 tf=1
world       2        doc_id=0 tf=1, doc_id=2 tf=1
```

---

## 三、段文件的完整磁盘布局

### 3.1 整体结构图

```
Segment (逻辑连续字节流，跨多 PostgreSQL 页)
┌──────────────────────────────────────────────────────────────┐
│ SECTION 1: TpSegmentHeader (约 140 字节)                      │
│   先写 placeholder，最后回写真实值                              │
├──────────────────────────────────────────────────────────────┤
│ SECTION 2: Dictionary                                         │
│   ┌─ dict.num_terms = 3                  (4 bytes)           │
│   ├─ string_offsets[0..2]               (12 bytes)           │
│   │   [0]=0, [1]=12, [2]=27                                  │
│   └─ string_pool                         可变                  │
│       [0]: len=5 + "hello"   + dict_offset=0    → 13 bytes   │
│       [1]: len=8 + "postgres"+ dict_offset=16   → 16 bytes   │
│       [2]: len=5 + "world"   + dict_offset=32   → 13 bytes   │
│       string_pool 总计: 42 bytes                              │
├──────────────────────────────────────────────────────────────┤
│ SECTION 3: TpDictEntry[3] placeholder                        │
│   每条 16 bytes × 3 = 48 bytes (全 0，之后回写)                │
├──────────────────────────────────────────────────────────────┤
│ SECTION 4: Posting Blocks (按词项顺序)                         │
│   hello 的 posting blocks (见 3.4)                            │
│   postgres 的 posting blocks (见 3.4)                         │
│   world 的 posting blocks (见 3.4)                            │
├──────────────────────────────────────────────────────────────┤
│ SECTION 5: Skip Index (所有块的 TpSkipEntry，连续存放)          │
│   每个 block 一条 skip entry (20 bytes)                        │
│   本例共 3 条 (每个词项各 1 block) = 60 bytes                   │
├──────────────────────────────────────────────────────────────┤
│ SECTION 6: Fieldnorm Table                                    │
│   doc_id 0: encode_fieldnorm(2) → byte                    │
│   doc_id 1: encode_fieldnorm(2) → byte                    │
│   doc_id 2: encode_fieldnorm(1) → byte                    │
│   3 bytes                                                     │
├──────────────────────────────────────────────────────────────┤
│ SECTION 7: CTID Pages Array                                   │
│   按 doc_id 顺序: [100, 100, 200]                             │
│   3 × 4 = 12 bytes                                            │
├──────────────────────────────────────────────────────────────┤
│ SECTION 8: CTID Offsets Array                                 │
│   按 doc_id 顺序: [1, 2, 1]                                   │
│   3 × 2 = 6 bytes                                             │
├──────────────────────────────────────────────────────────────┤
│ SECTION 9: Alive Bitset                                       │
│   3 docs → 1 byte (3 bits each=1)                             │
├──────────────────────────────────────────────────────────────┤
│ SECTION 10: Page Index (跨页映射表)                             │
└──────────────────────────────────────────────────────────────┘
```

### 3.2 偏移量计算（以本例为例）

```
偏移量          内容
──────────────  ───────────────────────────────────────
0x0000          TpSegmentHeader (placeholder, ~140 bytes)
0x008C          dict.num_terms = 3                       ← dictionary_offset
0x0090          string_offsets[0]=0
0x0094          string_offsets[1]=12
0x0098          string_offsets[2]=27
0x009C          "hello\0..." 的开始                       ← strings_offset
                [length=5(4B)][hello(5B)][dict_offset=0(4B)]   0x009C → 0x00A8
                [length=8(4B)][postgres(8B)][dict_offset=16(4B)]  0x00A9 → 0x00B8
                [length=5(4B)][world(5B)][dict_offset=32(4B)]     0x00B9 → 0x00C5
0x00C6          TpDictEntry[0] placeholder              ← entries_offset
0x00D6          TpDictEntry[1] placeholder
0x00E6          TpDictEntry[2] placeholder
0x00F6          hello posting block (uncompressed)       ← postings_offset
0x0106          postgres posting block (uncompressed)
0x010E          world posting block (uncompressed)
0x0116          skip_entry[0] (hello)                    ← skip_index_offset
0x012A          skip_entry[1] (postgres)
0x013E          skip_entry[2] (world)
0x0152          fieldnorm[0..2] = [40,40,39]             ← fieldnorm_offset
0x0155          ctid_pages[0..2] = [100,100,200]        ← ctid_pages_offset
0x0161          ctid_offsets[0..2] = [1,2,1]            ← ctid_offsets_offset
0x0167          alive_bitset = 0x07                      ← alive_bitset_offset
───────────────────────────────────────────────────────
                data_size = 0x016A (≈362 bytes)
```

### 3.3 TpSegmentHeader 回写值

```
字段              值               说明
────────────────  ──────────────   ──────────────
magic            0x54503337       "TP37" (TP_SEGMENT_MAGIC)
version          5                V5 格式（current）
created_at       2026-06-06...    创建时间戳
num_pages        2                跨 2 个 PostgreSQL 页
data_size        362              总字节数
level            0                从 memtable spill 来的，level=0
next_segment     InvalidBlock     链的下一个段（无）
dictionary_offset 0x008C           静态字典起始
strings_offset   0x009C           字符串池起始
entries_offset   0x00C6           字典条目起始
postings_offset  0x00F6           posting 块起始
skip_index_offset 0x0116          跳过索引起始
fieldnorm_offset 0x0152           文档长度表
ctid_pages_offset 0x0155           CTID 页号数组
ctid_offsets_offset 0x0161         CTID 偏移数组
alive_bitset_offset 0x0167         存活位集
alive_count      3                全部存活
num_terms        3                唯一词项数
num_docs         3                文档数
total_tokens     5                总词项数
page_index       ...              页索引的块号（用于物理页定位）
```

### 3.4 Posting Block 详解

每个词项的 posting 数据分块存储，每块最多 **TP_BLOCK_SIZE (128)** 条 posting。本例每个词项只有 1 个 block（条数远小于 128）。

#### 词项 "hello" 的 posting block (未压缩)

```
逻辑偏移 0x00F6 (postings_offset):
┌─────────────────────────────────────────────────────────┐
│ TpBlockPosting[0]:  doc_id=0,  frequency=1,  fieldnorm  │
│   doc_id:    0x00000000  (4 bytes)                       │
│   frequency: 0x0001      (2 bytes)  ← 出现 1 次              │
│   fieldnorm: 40          (1 byte)   ← encode_fieldnorm(2) │
│   reserved:  0x00        (1 byte)                        │
│   → 8 bytes                                             │
├─────────────────────────────────────────────────────────┤
│ TpBlockPosting[1]:  doc_id=1,  frequency=1,  fieldnorm  │
│   doc_id:    0x00000001  (4 bytes)                       │
│   frequency: 0x0001      (2 bytes)                       │
│   fieldnorm: 40          (1 byte)                        │
│   reserved:  0x00        (1 byte)                        │
│   → 8 bytes                                             │
├─────────────────────────────────────────────────────────┤
│ block 总计: 2 postings = 16 bytes                        │
└─────────────────────────────────────────────────────────┘
```

对应的 **Skip Entry** (在 skip_index_offset 位置，每个 block 一条):

```
TpSkipEntry for "hello" block 0:
┌─────────────────────────────────────────────────────────┐
│ last_doc_id:     1     (uint32) ← 块内最大 doc_id         │
│ doc_count:       2     (uint8)  ← 块内 posting 数         │
│ block_max_tf:    1     (uint16) ← 块内最大 tf              │
│ block_max_norm:  40    (uint8)  ← 块内最小的 fieldnorm     │
│ posting_offset:  0x00F6 (uint64) ← 指向上述 posting 数据    │
│ flags:           0x00  (uint8)  ← TP_BLOCK_FLAG_UNCOMPRESSED│
│ reserved[3]:     0,0,0                                       │
│ → 20 bytes (packed)                                           │
└─────────────────────────────────────────────────────────┘
```

#### 词项 "postgres" 的 posting block

```
逻辑偏移 0x0106:
┌─────────────────────────────────────────────────────────┐
│ TpBlockPosting[0]:  doc_id=1,  frequency=1,  fieldnorm  │
│   → 8 bytes，只有 1 条                                    │
└─────────────────────────────────────────────────────────┘

对应 Skip Entry (skip_index_offset + 20):
│ last_doc_id: 1, doc_count: 1, block_max_tf: 1
│ block_max_norm: 40, posting_offset: 0x0106, flags: 0x00
```

#### 词项 "world" 的 posting block

```
逻辑偏移 0x010E:
┌─────────────────────────────────────────────────────────┐
│ posting[0]: doc_id=0, freq=1, fieldnorm                │
│ posting[1]: doc_id=2, freq=1, fieldnorm                │
│   → 16 bytes                                            │
└─────────────────────────────────────────────────────────┘

对应 Skip Entry (skip_index_offset + 40):
│ last_doc_id: 2, doc_count: 2, block_max_tf: 1
│ block_max_norm: 39 (encode_fieldnorm(1)=39), posting_offset: 0x010E
```

---

## 四、TpDictEntry 回写值

```
词项         skip_index_offset    block_count    doc_freq
─────────────────────────────────────────────────────────
hello        0x0116 (skip_entry[0])    1              2
postgres     0x012A (skip_entry[1])    1              1
world        0x013E (skip_entry[2])    1              2
```

每条 `TpDictEntry` 是 16 字节（8 字节 offset + 4 字节 block_count + 4 字节 doc_freq）。

---

## 五、查询时如何读取

以搜索 `ORDER BY content <@> 'hello world'` 为例：

### 5.1 找到词项

```
1. 读取 TpDictionary → num_terms=3
2. 读取 string_offsets[] → [0, 12, 27]
3. 二分查找 "hello":
   - 读取 strings_offset + string_offsets[0] → "hello"  ✓ 匹配
   - dict_entry 在 entries_offset + 0*16 = 0x00C6
4. 二分查找 "world":
   - 读取 strings_offset + string_offsets[2] → "world"  ✓ 匹配
   - dict_entry 在 entries_offset + 2*16 = 0x00E6
```

### 5.2 加载 posting 数据

```
对 "hello":
  TpDictEntry → skip_index_offset=0x0116, block_count=1
  读取 TpSkipEntry[0] → last_doc_id=1, block_max_tf=1, block_max_norm=40
  block_max_score = IDF × (1 × (1.2+1)) / (1 + 1.2 × (1-0.75+0.75×40/1.67))
  BMW: 如果 block_max_score ≥ threshold → 加载 block
    读取 posting_offset=0x00F6 → 解压（或直接读取）TpBlockPosting[0..1]

对 "world":
  同理，skip_entry → block_max → 判断 → 加载

WAND pivot:
  "hello" cur_doc_id=0, max_score=...
  "world" cur_doc_id=0, max_score=...
  累加 max_score > threshold → pivot_doc_id=0

score_pivot_document(doc_id=0):
  hello score = IDF_hello × BM25(tf=1, len_norm=doc_len=2)
  world score = IDF_world × BM25(tf=1, len_norm=doc_len=2)
  doc_score = hello_score + world_score

CTID 解析 (tp_topk_resolve_ctids):
  doc_id=0 → ctid_pages[0]=100, ctid_offsets[0]=1 → CTID(100,1) ✓
```

---

## 六、Fieldnorm 量化

fieldnorm 用于存储 **文档长度**，采用 Lucene SmallFloat 编码，1 字节可表示 0~2,013,265,944 的范围。

```
文档长度  →  encode_fieldnorm  →  存储的 byte
─────────────────────────────────────────
   1      →       39           →        0x27
   2      →       40           →        0x28
   3      →       41           →        0x29
  10      →       47           →        0x2F
 100      →       66           →        0x42
1000      →       88           →        0x58
```

解码表（`FIELDNORM_DECODE_TABLE[256]`）：

| byte | 解码值 |
|------|--------|
| 0 | 0 |
| 1 | 1 |
| ... | ... |
| 39 | 39 (精确) |
| 40 | 40 (精确) |
| 41 | 42 (步长2开始) |
| 42 | 44 |
| ... | ... |
| 255 | 2,013,265,944 |

**为什么量化？** 每个 posting 都存 fieldnorm（BMW 块跳过和 BM25 公式都需要），1 字节可节省大量存储空间。

---

## 七、块压缩详解

当 `tp_compress_segments = true`（默认），每个块会使用 Delta 编码压缩 doc_id 和 frequency。

### 7.1 压缩格式

```
压缩后的块数据:
┌──────────────────────────────────────────────────────┐
│ TpCompressedBlockHeader (2 bytes)                     │
│   doc_id_bits: uint8  (delta 需要多少 bit)             │
│   freq_bits:   uint8  (frequency 需要多少 bit)         │
├──────────────────────────────────────────────────────┤
│ bitpacked doc_id deltas                               │
│   ─ 第 1 个存绝对 doc_id（视为 delta from 0）           │
│   ─ 后续存 Δ = doc_id[i] - doc_id[i-1]                │
│   总 bits = doc_id_bits × count                       │
├──────────────────────────────────────────────────────┤
│ bitpacked frequencies                                 │
│   总 bits = freq_bits × count                         │
├──────────────────────────────────────────────────────┤
│ fieldnorm bytes (1 byte per doc, 不压缩)               │
└──────────────────────────────────────────────────────┘
```

### 7.2 压缩示例

以 "hello" 的 posting block 为例：

```
原始数据 (未压缩) = 16 bytes:
  posting[0]: doc_id=0, freq=1, fieldnorm=40
  posting[1]: doc_id=1, freq=1, fieldnorm=40

压缩过程:
1. Delta encode doc_id: [0, 1]  → max_delta=1 → doc_id_bits=1
2. Frequency: [1, 1]            → max_freq=1  → freq_bits=1
3. Header: [doc_id_bits=1, freq_bits=1]  → 2 bytes
4. Bitpack 2 个 1-bit doc_id:   [0, 1]     → 0x02 (1 byte 不够2 bits，取整)
5. Bitpack 2 个 1-bit freq:     [1, 1]     → 0x03
6. fieldnorm: [40, 40]                      → 2 bytes

压缩后: 2 + 1 + 1 + 2 = 6 bytes (对比原来的 16 bytes，节省 62.5%)
```

对 "world" 的 posting block：

```
原始: doc_id=[0, 2], freq=[1, 1], fieldnorm=[40, 39]
Delta: [0, 2] → max_delta=2  → doc_id_bits=2
Freq:  [1, 1] → max_freq=1   → freq_bits=1
压缩后: 2 + (2*2/8→1) + (2*1/8→1) + 2 = 6 bytes
```

**何时不压缩？** 实际只有压缩后小于原始数据时才启用，通过 `skip_entry.flags` 标记：
- `TP_BLOCK_FLAG_UNCOMPRESSED (0x00)`：未压缩，直接读 `TpBlockPosting`
- `TP_BLOCK_FLAG_DELTA (0x01)`：Delta 编码压缩
- `TP_BLOCK_FLAG_FOR / PFOR (0x02/0x03)`：预留，Phase 3 实现

---

## 八、跨页存储与 Page Index

Segment 数据是一个**逻辑连续**的字节流，但实际存储在多个 PostgreSQL 8KB 页中。需要 `page_index` 将逻辑页号映射到物理块号。

```
逻辑字节流         逻辑页号     物理块号 (page_index)
─────────────────────────────────────────────────
0x0000 - 0x1FFF    page 0    →  block 42
0x2000 - 0x3FFF    page 1    →  block 43
...                   ...

page_index 本身是一个链表页面：
┌─────────────────────────────────────┐
│ TpPageIndexSpecial (special area)    │
│   magic, version, page_type         │
│   next_page, num_entries            │
├─────────────────────────────────────┤
│ BlockNumber entries[N]              │
│   [0]=42, [1]=43, ...              │
└─────────────────────────────────────┘
```

读取时：`tp_segment_read(reader, logical_offset, dest, len)` 通过 page_index 将逻辑偏移映射到物理 buffer 页。

---

## 九、与 V3/V4 格式的兼容差异

| 特性 | V3 | V4 | V5 (current) |
|------|-----|------|------|
| 所有偏移量 | uint32 (4GB 限制) | uint64 | uint64 |
| Skip entry posting_offset | uint32 | uint64 | uint64 |
| Dict entry 大小 | 12 bytes | 16 bytes | 16 bytes |
| block_count 宽度 | uint16 | uint32¹ | uint32¹ |
| Alive bitset | ✗ | ✗ | ✓ |
| 读取版本感知 | `tp_segment_read_dict_entry()` 自动适配 | | |

¹ V4 中 block_count 从 uint16+padding 改为 uint32，在 little-endian 上二进制兼容。

---

## 十、总结

Segment 格式可以概括为 **"字典 + 分块倒排 + 全局映射"** 三重结构：

```
              ┌─ 字典 (Dictionary)
词项 ────────→│  ├─ string_offsets[]: 二分查找
  "hello"     │  └─ TpDictEntry: skip_offset + block_count + doc_freq
              │
              ├─ 分块倒排 (Posting Blocks)
              │  ├─ TpSkipEntry[0..N]: 块级元数据 (BMW 跳过)
              │  └─ TpBlockPosting[]: doc_id + freq + fieldnorm
              │     (每块 ≤ 128 条，可压缩)
              │
              └─ 全局映射 (per segment)
                 ├─ ctid_pages[N]:     doc_id → heap page
                 ├─ ctid_offsets[N]:   doc_id → heap offset
                 └─ fieldnorms[N]:     doc_id → doc_length (1 byte)
```

这种设计使得 BMW 评分只需：
1. 读 skip entry 做块级剪枝
2. 读 posting block 做精确评分
3. 按 doc_id 批量解析 CTID

全程只需顺序 I/O，不需要哈希表查找。
