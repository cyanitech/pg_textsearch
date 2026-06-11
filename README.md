# pg_textsearch

[![CI](https://github.com/timescale/pg_textsearch/actions/workflows/ci.yml/badge.svg)](https://github.com/timescale/pg_textsearch/actions/workflows/ci.yml)
[![Benchmarks](https://github.com/timescale/pg_textsearch/actions/workflows/benchmark.yml/badge.svg)](https://timescale.github.io/pg_textsearch/benchmarks/)
[![Coverity Scan](https://scan.coverity.com/projects/32822/badge.svg)](https://scan.coverity.com/projects/pg_textsearch)

面向 Postgres 的现代化排序文本搜索。

- 简洁语法：`ORDER BY content <@> 'search terms'`
- 基于 BM25 的排序，支持可配置参数（k1、b）
- 兼容 Postgres 文本搜索配置（english、french、german 等）
- 支持 JSONB 字段、多列搜索和文本变换的表达式索引
- 支持用于限定搜索范围和多语言表的部分索引
- 通过 Block-Max WAND 优化实现快速 top-k 查询
- 支持大表并行建索引
- 支持分区表
- 具备同类最佳的性能与可扩展性

🚀 **状态**：v1.4.0-dev - 可用于生产环境。

![Tapir and Friends](images/tapir_and_friends_v1.3.0.png)

## 历史说明

该项目最初名为 Tapir，即 **T**extual **A**nalysis for **P**ostgres **I**nformation **R**etrieval。我们仍然使用 tapir 作为项目吉祥物，这个名字也会出现在源码中的不同位置。

## PostgreSQL 版本兼容性

pg_textsearch 支持 PostgreSQL 17 和 18。

## 安装

### 预构建二进制包

可以从 [Releases 页面](https://github.com/timescale/pg_textsearch/releases) 下载预构建二进制包。支持 Linux 和 macOS（amd64 与 arm64），以及 PostgreSQL 17 和 18。

### 从源码构建

```sh
cd /tmp
git clone https://github.com/timescale/pg_textsearch
cd pg_textsearch
make
make install # 可能需要 sudo
```

## 快速开始

pg_textsearch 必须通过 `shared_preload_libraries` 加载。请在 `postgresql.conf` 中添加以下配置，并重启服务器：

```
shared_preload_libraries = 'pg_textsearch'  # 如有需要，请追加到现有列表中
```

然后启用扩展（每个数据库执行一次）：

```sql
CREATE EXTENSION pg_textsearch;
```

创建一个包含文本内容的表：

```sql
CREATE TABLE documents (id bigserial PRIMARY KEY, content text);
INSERT INTO documents (content) VALUES
    ('PostgreSQL is a powerful database system'),
    ('BM25 is an effective ranking function'),
    ('Full text search with custom scoring');
```

在文本列上创建 pg_textsearch 索引：

```sql
CREATE INDEX docs_idx ON documents USING bm25(content) WITH (text_config='english');
```

## 查询

使用 `<@>` 运算符获取最相关的文档：

```sql
SELECT * FROM documents
ORDER BY content <@> 'database system'
LIMIT 5;
```

注意：由于 Postgres 仅支持对运算符执行 `ASC` 顺序的索引扫描，`<@>` 返回的是负的 BM25 分数。分数越低，表示匹配越好。

索引会根据列自动识别。如果需要显式指定索引：

```sql
SELECT * FROM documents
ORDER BY content <@> to_bm25query('database system', 'docs_idx')
LIMIT 5;
```

支持的操作：
- `text <@> 'query'` - 根据查询对文本评分（自动识别索引）
- `text <@> bm25query` - 显式指定索引进行评分

### 验证是否使用索引

使用 EXPLAIN 查看查询计划：

```sql
EXPLAIN SELECT * FROM documents
ORDER BY content <@> 'database system'
LIMIT 5;
```

对于较小的数据集，PostgreSQL 可能更倾向于顺序扫描。可通过以下方式强制使用索引：

```sql
SET enable_seqscan = off;
```

注意：即使 EXPLAIN 显示为顺序扫描，`<@>` 和 `to_bm25query` 仍然会使用索引来获取 BM25 评分所需的语料统计信息（文档数、平均长度）。

### 使用 WHERE 子句过滤

过滤条件与 BM25 索引扫描的交互有两种方式：

**预过滤** 会先使用另一个索引（B-tree 等）先缩小行集合，再进行评分：

```sql
-- 在过滤列上创建索引
CREATE INDEX ON documents (category_id);

-- 查询先过滤，再对匹配行评分
SELECT * FROM documents
WHERE category_id = 123
ORDER BY content <@> 'search terms'
LIMIT 10;
```

**后过滤** 则是先执行 BM25 索引扫描，再过滤结果。没有独立索引的列会在 BM25 扫描之后过滤：

```sql
SELECT * FROM documents
WHERE length(content) > 100
ORDER BY content <@> 'search terms'
LIMIT 10;
```

**性能考量**：

- **预过滤的权衡**：如果过滤条件命中很多行（例如 10 万行以上），对所有这些行逐一评分会很昂贵。BM25 索引在能够利用 top-k 优化（`ORDER BY + LIMIT`）避免对每个匹配文档都评分时效率最高。
- **后过滤的权衡**：索引会在过滤之前先返回 top-k 结果。如果你的 `WHERE` 子句会筛掉大部分结果，最终返回的行数可能少于请求值。可以适当增大 `LIMIT`，然后在应用层再次限制返回条数。
- **最佳情况**：先用高选择性的条件做预过滤（匹配行数低于 10%），再让 BM25 对缩小后的结果集配合 `ORDER BY + LIMIT` 进行评分。

这与 [pgvector 中的过滤行为](https://github.com/pgvector/pgvector?tab=readme-ov-file#filtering) 类似，近似索引也会在索引扫描后再应用过滤。

## 建立索引

在文本列上创建 BM25 索引：

```sql
CREATE INDEX ON documents USING bm25(content) WITH (text_config='english');
```

### 索引选项

- `text_config` - 使用的 PostgreSQL 文本搜索配置（必填）
- `k1` - 词频饱和参数（默认 1.2）
- `b` - 长度归一化参数（默认 0.75）

```sql
CREATE INDEX ON documents USING bm25(content) WITH (text_config='english', k1=1.5, b=0.8);
```

也支持不同的文本搜索配置：

```sql
-- 对英文文档做词干提取
CREATE INDEX docs_en_idx ON documents USING bm25(content) WITH (text_config='english');

-- 不做词干提取的简单文本处理
CREATE INDEX docs_simple_idx ON documents USING bm25(content) WITH (text_config='simple');

-- 语言专用配置
CREATE INDEX docs_fr_idx ON french_docs USING bm25(content) WITH (text_config='french');
CREATE INDEX docs_de_idx ON german_docs USING bm25(content) WITH (text_config='german');
```

### 表达式索引

可以为表达式而不是普通列建立索引，这对 JSONB 字段、多列拼接和文本变换很有用：

```sql
-- 提取 JSONB 字段
CREATE INDEX ON events USING bm25 ((data->>'description'))
    WITH (text_config='english');

SELECT * FROM events
ORDER BY (data->>'description') <@> to_bm25query('network error', 'events_expr_idx')
LIMIT 10;

-- 多列搜索
CREATE INDEX ON articles USING bm25 ((coalesce(title, '') || ' ' || coalesce(body, '')))
    WITH (text_config='english');

-- 文本变换
CREATE INDEX ON docs USING bm25 ((lower(content)))
    WITH (text_config='simple');
```

表达式必须求值为 `text`，并且只能使用 IMMUTABLE 函数。查询时必须在 `ORDER BY` 子句中重复完全相同的表达式。

### 部分索引

通过添加 `WHERE` 子句为部分行建立索引。当查询总是只针对某个子集时，部分索引更小也更快：

```sql
CREATE INDEX ON docs USING bm25 (content)
    WITH (text_config='english')
    WHERE status = 'published';

SELECT * FROM docs
WHERE status = 'published'
ORDER BY content <@> to_bm25query('search terms', 'docs_content_idx')
LIMIT 10;
```

部分索引需要通过 `to_bm25query()` 显式指定索引名，隐式的 `text <@> 'query'` 语法会跳过它们。

表达式索引和部分索引可以组合使用：

```sql
CREATE INDEX ON events USING bm25 ((data->>'message'))
    WITH (text_config='english')
    WHERE (data->>'severity') = 'error';
```

### 多语言表

对于存放多种语言文档的表，可以为每种语言建立一个部分索引，并为每个索引使用对应的文本搜索配置：

```sql
ALTER TABLE docs ADD COLUMN lang CHAR(2) NOT NULL DEFAULT 'en';

CREATE INDEX docs_en_idx ON docs USING bm25 (content)
    WITH (text_config='english') WHERE lang = 'en';
CREATE INDEX docs_de_idx ON docs USING bm25 (content)
    WITH (text_config='german')  WHERE lang = 'de';
CREATE INDEX docs_fr_idx ON docs USING bm25 (content)
    WITH (text_config='french')  WHERE lang = 'fr';
```

每个索引都会应用该语言对应的词干提取和停用词规则。查询时请带上匹配的谓词和索引名：

```sql
SELECT * FROM docs
WHERE lang = 'en'
ORDER BY content <@> to_bm25query('databases', 'docs_en_idx')
LIMIT 10;
```

## 数据类型

### bm25query

`bm25query` 类型用于表示 BM25 评分查询，并可选择性附带索引上下文：

```sql
-- 创建带索引名的 bm25query（WHERE 子句和独立评分场景需要）
SELECT to_bm25query('search query text', 'docs_idx');
-- 返回：docs_idx:search query text

-- 内嵌索引名语法（使用类型转换的另一种写法）
SELECT 'docs_idx:search query text'::bm25query;
-- 返回：docs_idx:search query text

-- 创建不带索引名的 bm25query（仅适用于 ORDER BY + 索引扫描）
SELECT to_bm25query('search query text');
-- 返回：search query text
```

**注意**：在 PostgreSQL 18 中，使用单冒号（`:`）的内嵌索引名语法，可以让查询规划器即使在提前计算 `SELECT` 子句表达式时，也能确定索引名。这可确保不同查询求值策略下的兼容性。

#### bm25query 函数

Function | Description
--- | ---
to_bm25query(text) → bm25query | 创建不带索引名的 bm25query（仅用于 ORDER BY）
to_bm25query(text, text) → bm25query | 根据查询文本和索引名创建 bm25query
text <@> bm25query → double precision | BM25 评分运算符（返回负分）
bm25query = bm25query → boolean | 相等比较

## 性能

pg_textsearch 索引使用基于磁盘分页的 memtable（LSM 的 L0 层）来提高写入效率。memtable 在标准 buffer lock 保护下被修改，并通过 `GenericXLog` 写入 WAL。与其他索引类型一样，在数据导入完成后再建索引会更快。

```sql
-- 先导入数据
INSERT INTO documents (content) VALUES (...);

-- 再创建索引
CREATE INDEX docs_idx ON documents USING bm25(content) WITH (text_config='english');
```

### 并行索引构建

pg_textsearch 支持并行建索引，以加快大表建索引速度。Postgres 会根据表大小和配置自动使用并行 worker。

```sql
-- 配置并行 worker（可选，否则使用服务器默认值）
SET max_parallel_maintenance_workers = 4;
SET maintenance_work_mem = '256MB';  -- 并行构建至少需要 64MB

-- 创建索引（对大表会自动使用并行 worker）
CREATE INDEX docs_idx ON documents USING bm25(content) WITH (text_config='english');
```

**注意：** 规划器要求 `maintenance_work_mem >= 64MB` 才会启用并行索引构建。内存不足时会静默回退到串行模式。

当使用并行构建时，你会看到类似提示：

```
NOTICE:  parallel index build: launched 4 of 4 requested workers
```

对于分区表，每个分区都会独立构建各自的索引；如果某个分区足够大，也会使用并行 worker。这使得超大分区数据集也能高效建立索引。

### 性能调优

#### 强制合并 segment

索引会在多个 level 中存储多个 segment（类似 LSM 树）。在批量导入或持续增量插入之后，可能累积出多个 segment；将它们合并成一个可以减少扫描的 segment 数量，从而提升查询速度：

```sql
SELECT bm25_force_merge('docs_idx');
```

这类似于 Lucene 的 `forceMerge(1)`。它会将所有 segment 重写为单个 segment，并回收释放的页面。最适合在大批量插入之后使用，而不是在持续写入期间使用。

#### 在 ORDER BY 中配合 LIMIT 使用

top-k 查询（`ORDER BY ... LIMIT n`）会启用 Block-Max WAND 优化，从而跳过那些不可能进入前几名结果的 posting block。没有 `LIMIT` 子句时，索引会退回到对最多 `pg_textsearch.default_limit` 个匹配文档进行评分。

```sql
-- 更快：BMW 跳过没有竞争力的块
SELECT * FROM documents ORDER BY content <@> 'search terms' LIMIT 10;

-- 更慢：会对最多 default_limit 个文档评分
SELECT * FROM documents ORDER BY content <@> 'search terms';
```

#### Segment 压缩

压缩默认开启，通常同时改善索引大小和查询性能（需要读取的页面更少）。只有在你确认自己的工作负载中解压开销成为瓶颈时，才建议关闭：

```sql
SET pg_textsearch.compress_segments = off;
```

#### 影响索引构建的 Postgres 设置

Setting | Effect
--- | ---
`max_parallel_maintenance_workers` | `CREATE INDEX` 使用的并行 worker 数量（默认 2）
`maintenance_work_mem` | 每个 worker 的内存；并行构建时必须 >= 64MB

#### pg_textsearch GUC

Setting | Default | Description
--- | --- | ---
`pg_textsearch.default_limit` | 1000 | 未指定 LIMIT 时最多评分的文档数
`pg_textsearch.compress_segments` | on | 对新 segment 中的 posting block 进行压缩
`pg_textsearch.segments_per_level` | 8 | 自动压实前每层允许的 segment 数量（2-64）
`pg_textsearch.bulk_load_threshold` | 100000 | 单事务内达到该词项数时自动 spill（0 = 禁用）
`pg_textsearch.memtable_pages_threshold` | 64 | 链页达到该数量时自动 spill（0 = 禁用）

#### Memtable 架构

从 1.3.0 开始，L0 memtable 直接存储在索引关系内部，表现为一条 doc-record 页面链；其修改受标准 buffer lock 保护，并通过 `GenericXLog` 写入 WAL。这里没有共享内存 memtable、没有自定义 WAL resource manager，也没有 docid-page 恢复脚手架。PostgreSQL 原生 WAL 回放（包括在线页修复工具使用的单页重建辅助机制）可以在无需加载 `pg_textsearch.so` 的情况下重建所有页面。规范见 [docs/memtable_v2.md](docs/memtable_v2.md)。

自动 spill 由两个互补触发器控制：

- `memtable_pages_threshold`：每次插入后检查；当链长度超过配置的页数时触发。默认 64 页（8 KB block 时约 512 KB），可以因为链保持较小而控制查询延迟。
- `bulk_load_threshold`：在 `COMMIT` 时触发；当单个事务在 memtable 中累积了大量词项时生效，对 COPY / 批量 INSERT 很有用，可限制链页增长。

```sql
-- 手动 spill（将当前链强制写出为新的 L0 segment）
SELECT bm25_spill_index('docs_idx');
```

VACUUM（包括 autovacuum 基于插入阈值的路径）运行时也会 spill memtable，因此在 `CREATE INDEX` 与下一次服务器重启之间，未 spill 的状态量会保持有界。

**崩溃恢复**：磁盘上的 memtable 链本身就是持久记录。崩溃后，PostgreSQL 原生回放会恢复每一页；首次 backend 打开时无需重建。

**流复制**：所有页面修改都会通过标准 WAL 流复制。standby 会原生地重建每一页。

## 监控

SELECT schemaname, tablename, indexname, idx_scan, idx_tup_r
```sql
-- 检查索引使用情况ead, idx_tup_fetch
FROM pg_stat_user_indexes
WHERE indexrelid::regclass::text ~ 'pg_textsearch';
```

## 示例

### 基础搜索

```sql
CREATE TABLE articles (id serial PRIMARY KEY, title text, content text);
CREATE INDEX articles_idx ON articles USING bm25(content) WITH (text_config='english');

INSERT INTO articles (title, content) VALUES
    ('Database Systems', 'PostgreSQL is a powerful relational database system'),
    ('Search Technology', 'Full text search enables finding relevant documents quickly'),
    ('Information Retrieval', 'BM25 is a ranking function used in search engines');

-- 查找相关文档
SELECT title, content <@> 'database search' as score
FROM articles
ORDER BY score;
```

也支持不同语言和自定义参数：

```sql
-- 不同语言
CREATE INDEX fr_idx ON french_articles USING bm25(content) WITH (text_config='french');
CREATE INDEX de_idx ON german_articles USING bm25(content) WITH (text_config='german');

-- 自定义参数
CREATE INDEX custom_idx ON documents USING bm25(content)
    WITH (text_config='english', k1=2.0, b=0.9);
```

## 限制

### 不支持短语查询

BM25 索引存储词频而不存储词位置，因此无法原生执行类似 `"database system"` 的短语查询。你可以将 BM25 排序与后过滤结合起来模拟短语匹配：

```sql
-- BM25 先对候选文档排序；子查询通过多取一些结果来抵消
-- 后过滤移除非短语匹配带来的损耗
SELECT * FROM (
    SELECT *, content <@> 'database system' AS score
    FROM documents
    ORDER BY score
    LIMIT 100  -- 过量获取
) sub
WHERE content ILIKE '%database system%'
ORDER BY score
LIMIT 10;
```

由于后过滤会筛掉一部分结果，内部的 `LIMIT` 应该大于最终期望的结果数量。

### 不内置分面搜索

pg_textsearch 不提供专门的分面操作符，但标准 Postgres 查询机制已经能处理常见的分面模式：

```sql
-- 按类别过滤（假设 category 上有 B-tree 索引）
SELECT * FROM documents
WHERE category = 'engineering'
ORDER BY content <@> 'search terms'
LIMIT 10;

-- 对搜索结果前若干条计算分面计数
SELECT category, count(*)
FROM (
    SELECT category FROM documents
    ORDER BY content <@> 'search terms'
    LIMIT 100
) matches
GROUP BY category;
```

### 插入/更新性能

memtable 架构旨在支持高效写入，但对于持续的写密集工作负载，目前仍未完全优化。对于初始数据导入，在数据加载完成后再创建索引，比逐条增量插入时建索引更快。这仍是积极开发中的方向。

### 没有后台压实

当前 segment 压实会在 memtable spill 期间同步执行。写密集型工作负载可能会在 spill 时观察到压实延迟。后台压实计划在未来版本中提供。

### 分区表

分区表上的 BM25 索引使用 **分区本地统计信息**。每个分区都会维护自己的：
- 文档数（`total_docs`）
- 平均文档长度（`avg_doc_len`）
- 用于计算 IDF 的每个词项文档频率

这意味着：
- 针对单个分区的查询会使用该分区的统计信息计算出准确的 BM25 分数
- 跨多个分区的查询会分别按各分区独立计算分数，因此不同分区之间的分数不一定可以直接比较

**示例**：如果分区 A 有 1000 篇文档，而分区 B 只有 10 篇文档，则词项 `database` 在两个分区中的 IDF 值会不同。来自两个分区的结果分数会落在不同尺度上。

**建议**：
- 对于按时间分区的数据，当分数可比性重要时，尽量查询单个分区
- 使用那些天然会把查询限制在单个分区上的分区方案
- 在为搜索工作负载设计分区策略时，考虑到这一行为

```sql
-- 查询单个分区（分数在该分区内是准确的）
SELECT * FROM docs
WHERE created_at >= '2024-01-01' AND created_at < '2025-01-01'
ORDER BY content <@> 'search terms'
LIMIT 10;

-- 跨分区查询（分数按分区分别计算）
SELECT * FROM docs
ORDER BY content <@> 'search terms'
LIMIT 10;
```

### 词长度限制

pg_textsearch 继承了 PostgreSQL `tsvector` 的词长度上限，即 2047 个字符。超过该限制的词会在分词过程中被忽略（并伴随一条 INFO 消息）。这个限制由 PostgreSQL 文本搜索实现中的 `MAXSTRLEN` 定义。

对于典型自然语言文本，这个限制几乎不会遇到。它可能影响包含超长 token 的文档，例如 base64 编码数据、超长 URL 或拼接后的标识符。

这种行为与其他搜索引擎类似：
- Elasticsearch：截断 token（可通过 `truncate` filter 配置，默认 10 个字符）
- Tantivy：默认截断到 255 字节

### 大文档与分块分词

pg_textsearch 调用 Postgres 的 `to_tsvector` 对文档文本进行分词。Postgres 将单个 `tsvector` 的 lexeme 字典大小限制为 1 MB（`MAXSTRPOS`）。如果文档的唯一 token 数量超过这个上限，就会在分词前把文档拆成多个块（当前为 256 KB），再将每个块的词频合并起来。

分块边界会选择在每个窗口内最后一个 ASCII 空白字符处。对于以空白分词的文字体系（Latin、Cyrillic、Greek、Arabic 等），这是一种正确处理方式。对于不依赖空白分词的文字体系（CJK、Thai、Lao、Khmer），超大文档仍然可以被索引，但块边界可能落在语言感知分词器认为的“词”中间。实际中这通常可以接受，因为 Postgres 默认文本搜索解析器本身也不会为这些文字体系产生按词切分的 token。如果你使用了某个自定义文本搜索配置，并且其中的解析器会为这些文字体系生成逐词 token，那么超大文档的 lexeme 计数可能会与一次性完整分词略有差异。

**针对大型 CJK（或其他非空白分词）文档的变通方案：** 在应用层将文档拆成更小的片段，并为 `text[]` 列建立索引，而不是为 `text` 列建立索引。pg_textsearch 会逐元素索引数组，BM25 分数与将这些元素拼接成单个 `text` 值时一致，因此你既能保持排序质量，也能控制分块边界。再配合使用支持 CJK 的文本搜索配置扩展，例如 [zhparser](https://github.com/amutu/zhparser)（中文），这样每个块都能执行按词分词：

```sql
CREATE EXTENSION zhparser;
CREATE TEXT SEARCH CONFIGURATION chinese_zh (PARSER = zhparser);
ALTER TEXT SEARCH CONFIGURATION chinese_zh
    ADD MAPPING FOR n,v,a,i,e,l WITH simple;

CREATE TABLE docs (id bigserial PRIMARY KEY, content text[]);
CREATE INDEX docs_bm25 ON docs USING bm25(content)
    WITH (text_config='chinese_zh');
```

### PL/pgSQL 与存储过程

隐式的 `text <@> 'query'` 语法依赖规划器 hook 来自动识别 BM25 索引。这些 hook 在 PL/pgSQL 的 DO 块、函数或存储过程中不会运行。

**在 PL/pgSQL 内部**，请使用带显式索引名的 `to_bm25query()`：

```sql
-- 这在 PL/pgSQL 中无法工作：
-- SELECT * FROM docs ORDER BY content <@> 'search terms' LIMIT 10;

-- 请改用显式索引名：
SELECT * FROM docs
ORDER BY content <@> to_bm25query('search terms', 'docs_idx')
LIMIT 10;
```

普通 SQL 查询（PL/pgSQL 外部）两种写法都支持。

## 故障排查

```sql
-- 列出可用的文本搜索配置
SELECT cfgname FROM pg_ts_config;

-- 列出 BM25 索引
SELECT indexname FROM pg_indexes WHERE indexdef LIKE '%USING bm25%';
```

## 安装说明

如果你的机器上安装了多个 Postgres 版本，请显式指定 `pg_config` 的路径：

```sh
export PG_CONFIG=/Library/PostgreSQL/18/bin/pg_config  # 或 17
make clean && make && make install
```

如果遇到编译错误，请先安装 Postgres 开发文件：

```sh
# Ubuntu/Debian
sudo apt install postgresql-server-dev-17  # 适用于 PostgreSQL 17
sudo apt install postgresql-server-dev-18  # 适用于 PostgreSQL 18
```

## 参考

### 索引选项

Option | Type | Default | Description
--- | --- | --- | ---
text_config | string | required | 使用的 PostgreSQL 文本搜索配置
k1 | real | 1.2 | 词频饱和参数（0.1 到 10.0）
b | real | 0.75 | 长度归一化参数（0.0 到 1.0）

### 文本搜索配置

可用配置取决于你的 Postgres 安装：

```
# SELECT cfgname FROM pg_ts_config;
  cfgname
------------
 simple
 arabic
 armenian
 basque
 catalan
 danish
 dutch
 english
 finnish
 french
 german
 greek
 hindi
 hungarian
 indonesian
 irish
 italian
 lithuanian
 nepali
 norwegian
 portuguese
 romanian
 russian
 serbian
 spanish
 swedish
 tamil
 turkish
 yiddish
(29 rows)
```

可以通过 [zhparser](https://github.com/amutu/zhparser) 等扩展获得更多语言支持。

### 开发函数

这些函数仅供调试和开发使用。它们的接口在未来版本中可能会在不提前通知的情况下变更。带 † 的函数需要 superuser 权限。

Function | Description
--- | ---
bm25_force_merge(index_name) → void | 将所有 segment 合并为一个（提高查询速度）
bm25_spill_index(index_name) → int4 | 强制将 memtable spill 到磁盘 segment
bm25_dump_index(index_name) † → text | 导出内部索引结构（输出会截断）
bm25_summarize_index(index_name) † → text | 在不显示内容的情况下输出索引统计信息

额外的文件写入型调试函数（`bm25_dump_index(text, text)` 和 `bm25_debug_pageviz`）仅在 debug 构建中可用（编译时加上 `-DDEBUG_DUMP_INDEX`）。

```sql
-- 将所有 segment 合并为一个（最适合批量导入后使用）
SELECT bm25_force_merge('docs_idx');

-- 强制 spill 到磁盘（返回 spill 的条目数）
SELECT bm25_spill_index('docs_idx');

-- 快速查看索引统计信息
SELECT bm25_summarize_index('docs_idx');

-- 用于调试的详细导出（输出会截断）
SELECT bm25_dump_index('docs_idx');
```

## 扩展兼容性

pg_textsearch 使用固定的 LWLock tranche ID 1001-1008，以支持大量索引（例如拥有数百个分区的分区表）。如果你同时使用了另一个 Postgres 扩展，并且它也在这个范围内注册了固定 tranche ID，那么 `pg_stat_activity` 中的 wait event 名称可能会不准确。Postgres 核心 tranche 使用的是 100 以下的 ID。如果你遇到冲突，请 [提交 issue](https://github.com/timescale/pg_textsearch/issues)。

## 贡献

开发环境、代码风格以及如何提交 pull request，请参见 [CONTRIBUTING.md](CONTRIBUTING.md)。

- **Bug 报告**：[创建 issue](https://github.com/timescale/pg_textsearch/issues/new?labels=bug&template=bug_report.md)
- **功能请求**：[提交功能建议](https://github.com/timescale/pg_textsearch/issues/new?labels=enhancement&template=feature_request.md)
- **常规讨论**：[发起讨论](https://github.com/timescale/pg_textsearch/discussions)
