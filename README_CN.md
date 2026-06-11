# pg_textsearch

[![CI](https://github.com/timescale/pg_textsearch/actions/workflows/ci.yml/badge.svg)](https://github.com/timescale/pg_textsearch/actions/workflows/ci.yml)
[![Benchmarks](https://github.com/timescale/pg_textsearch/actions/workflows/benchmark.yml/badge.svg)](https://timescale.github.io/pg_textsearch/benchmarks/)
[![Coverity Scan](https://scan.coverity.com/projects/32822/badge.svg)](https://scan.coverity.com/projects/pg_textsearch)

Postgres 的现代排序文本搜索。

- 简洁语法：`ORDER BY content <@> '搜索词'`
- BM25 排序，支持可配置参数（k1, b）
- 兼容 Postgres 文本搜索配置（english, french, german 等）
- 支持 JSONB 字段、多列搜索和文本转换的表达式索引
- 支持范围搜索和多语言表的条件索引
- 通过 Block-Max WAND 优化实现快速的 top-k 查询
- 大表的并行索引构建
- 支持分区表
- 一流的性能和可扩展性

🚀 **状态**: v1.4.0-dev - 生产就绪。

![Tapir and Friends](images/tapir_and_friends_v1.3.0.png)

## 历史说明

项目的原始名称是 Tapir —— **T**extual **A**nalysis for **P**ostgres **I**nformation **R**etrieval（Postgres 信息检索的文本分析）。我们仍然使用 tapir（貘）作为吉祥物，该名称在源代码中多处出现。

## PostgreSQL 版本兼容性

pg_textsearch 支持 PostgreSQL 17 和 18。

## 安装

### 预编译二进制包

从 [Releases 页面](https://github.com/timescale/pg_textsearch/releases) 下载预编译的二进制包。
适用于 Linux 和 macOS（amd64 和 arm64），PostgreSQL 17 和 18。

### 从源码构建

```sh
cd /tmp
git clone https://github.com/timescale/pg_textsearch
cd pg_textsearch
make
make install # 可能需要 sudo
```

## 快速开始

pg_textsearch 必须通过 `shared_preload_libraries` 加载。在 `postgresql.conf` 中添加以下内容并重启服务器：

```
shared_preload_libraries = 'pg_textsearch'  # 如有需要，添加到现有列表中
```

然后在每个数据库中启用扩展：

```sql
CREATE EXTENSION pg_textsearch;
```

创建包含文本内容的表：

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

使用 `<@>` 操作符获取最相关的文档：

```sql
SELECT * FROM documents
ORDER BY content <@> 'database system'
LIMIT 5;
```

注意：`<@>` 返回的是负的 BM25 分数，因为 Postgres 仅支持操作符上的 `ASC` 顺序索引扫描。分数越低表示匹配度越好。

索引会自动从列中检测。如果需要显式指定索引：

```sql
SELECT * FROM documents
ORDER BY content <@> to_bm25query('database system', 'docs_idx')
LIMIT 5;
```

支持的操作：
- `text <@> 'query'` - 根据查询对文本评分（自动检测索引）
- `text <@> bm25query` - 通过显式指定索引对文本评分

### 验证索引使用

使用 EXPLAIN 检查查询计划：

```sql
EXPLAIN SELECT * FROM documents
ORDER BY content <@> 'database system'
LIMIT 5;
```

对于小数据集，PostgreSQL 可能倾向于使用顺序扫描。强制使用索引：

```sql
SET enable_seqscan = off;
```

注意：即使 EXPLAIN 显示顺序扫描，`<@>` 和 `to_bm25query` 始终使用索引来获取 BM25 评分所需的语料库统计信息（文档数、平均长度）。

### 使用 WHERE 子句进行过滤

过滤与 BM25 索引扫描的交互有两种方式：

**预过滤** 使用单独的索引（B-tree 等）在评分前减少行数：

```sql
-- 在过滤列上创建索引
CREATE INDEX ON documents (category_id);

-- 查询先过滤，然后对匹配行评分
SELECT * FROM documents
WHERE category_id = 123
ORDER BY content <@> 'search terms'
LIMIT 10;
```

**后过滤** 先执行 BM25 索引扫描，再进行过滤。没有独立索引的列在 BM25 扫描之后进行过滤：

```sql
SELECT * FROM documents
WHERE length(content) > 100
ORDER BY content <@> 'search terms'
LIMIT 10;
```

**性能考虑**：

- **预过滤的权衡**：如果过滤匹配的行数很多（例如 100K+），对所有行评分会非常昂贵。BM25 索引在能够使用 top-k 优化（ORDER BY + LIMIT）来避免对每个匹配文档评分时效率最高。

- **后过滤的权衡**：索引在过滤*之前*返回 top-k 结果。如果你的 WHERE 子句过滤掉了大部分结果，可能会得到少于请求数量的行。可以通过增加 LIMIT 来补偿，然后在应用程序代码中重新限制。

- **最佳情况**：使用选择性过滤条件（匹配行数 < 10%），然后通过 BM25 配合 ORDER BY + LIMIT 对减少后的集合进行评分。

这类似于 [pgvector 中的过滤行为](https://github.com/pgvector/pgvector?tab=readme-ov-file#filtering)，其中近似索引也在索引扫描之后进行过滤。

## 索引

在文本列上创建 BM25 索引：

```sql
CREATE INDEX ON documents USING bm25(content) WITH (text_config='english');
```

### 索引选项

- `text_config` - 要使用的 PostgreSQL 文本搜索配置（必填）
- `k1` - 词频饱和参数（默认 1.2）
- `b` - 长度归一化参数（默认 0.75）

```sql
CREATE INDEX ON documents USING bm25(content) WITH (text_config='english', k1=1.5, b=0.8);
```

也支持不同的文本搜索配置：

```sql
-- 带词干提取的英文文档
CREATE INDEX docs_en_idx ON documents USING bm25(content) WITH (text_config='english');

-- 不带词干提取的简单文本处理
CREATE INDEX docs_simple_idx ON documents USING bm25(content) WITH (text_config='simple');

-- 语言特定的配置
CREATE INDEX docs_fr_idx ON french_docs USING bm25(content) WITH (text_config='french');
CREATE INDEX docs_de_idx ON german_docs USING bm25(content) WITH (text_config='german');
```

### 表达式索引

对表达式而非纯列创建索引 —— 适用于 JSONB 字段、多列拼接和文本转换：

```sql
-- JSONB 字段提取
CREATE INDEX ON events USING bm25 ((data->>'description'))
    WITH (text_config='english');

SELECT * FROM events
ORDER BY (data->>'description') <@> to_bm25query('network error', 'events_expr_idx')
LIMIT 10;

-- 多列搜索
CREATE INDEX ON articles USING bm25 ((coalesce(title, '') || ' ' || coalesce(body, '')))
    WITH (text_config='english');

-- 文本转换
CREATE INDEX ON docs USING bm25 ((lower(content)))
    WITH (text_config='simple');
```

表达式必须求值为 `text` 类型，且只能使用 IMMUTABLE 函数。查询时必须在 `ORDER BY` 子句中重复相同的表达式。

### 条件索引（Partial Indexes）

通过添加 `WHERE` 子句对部分行进行索引。条件索引更小，当查询始终针对特定子集时更快：

```sql
CREATE INDEX ON docs USING bm25 (content)
    WITH (text_config='english')
    WHERE status = 'published';

SELECT * FROM docs
WHERE status = 'published'
ORDER BY content <@> to_bm25query('search terms', 'docs_content_idx')
LIMIT 10;
```

条件索引需要通过 `to_bm25query()` 显式指定索引名称 —— 隐式的 `text <@> 'query'` 语法会跳过它们。

表达式索引和条件索引可以组合使用：

```sql
CREATE INDEX ON events USING bm25 ((data->>'message'))
    WITH (text_config='english')
    WHERE (data->>'severity') = 'error';
```

### 多语言表

对于包含多种语言文档的表，可以为每种语言创建一个条件索引，每个索引使用相应的文本搜索配置：

```sql
ALTER TABLE docs ADD COLUMN lang CHAR(2) NOT NULL DEFAULT 'en';

CREATE INDEX docs_en_idx ON docs USING bm25 (content)
    WITH (text_config='english') WHERE lang = 'en';
CREATE INDEX docs_de_idx ON docs USING bm25 (content)
    WITH (text_config='german')  WHERE lang = 'de';
CREATE INDEX docs_fr_idx ON docs USING bm25 (content)
    WITH (text_config='french')  WHERE lang = 'fr';
```

每个索引使用对应语言的词干提取和停用词。查询时带上匹配的条件和索引名称：

```sql
SELECT * FROM docs
WHERE lang = 'en'
ORDER BY content <@> to_bm25query('databases', 'docs_en_idx')
LIMIT 10;
```

## 数据类型

### bm25query

`bm25query` 类型表示带有可选索引上下文的 BM25 评分查询：

```sql
-- 创建带索引名称的 bm25query（WHERE 子句和独立评分需要）
SELECT to_bm25query('search query text', 'docs_idx');
-- 返回: docs_idx:search query text

-- 嵌入式索引名称语法（使用类型转换的替代形式）
SELECT 'docs_idx:search query text'::bm25query;
-- 返回: docs_idx:search query text

-- 创建不带索引名称的 bm25query（仅在 ORDER BY 配合索引扫描时有效）
SELECT to_bm25query('search query text');
-- 返回: search query text
```

**注意**：在 PostgreSQL 18 中，使用单冒号（`:`）的嵌入式索引名称语法允许查询规划器即使在提前计算 SELECT 子句表达式时也能确定索引名称。这确保了不同查询评估策略之间的兼容性。

#### bm25query 函数

| 函数 | 描述 |
|--- | --- |
| to_bm25query(text) → bm25query | 创建不带索引名称的 bm25query（仅用于 ORDER BY） |
| to_bm25query(text, text) → bm25query | 创建带查询文本和索引名称的 bm25query |
| text <@> bm25query → double precision | BM25 评分操作符（返回负分数） |
| bm25query = bm25query → boolean | 相等性比较 |

## 性能

pg_textsearch 索引使用基于磁盘的分页内存表（LSM 的 L0 层）来实现高效的写入。内存表在标准缓冲区锁下进行修改，并通过 `GenericXLog` 进行 WAL 日志记录。与其他索引类型一样，在加载数据之后再创建索引会更快。

```sql
-- 先加载数据
INSERT INTO documents (content) VALUES (...);

-- 再创建索引
CREATE INDEX docs_idx ON documents USING bm25(content) WITH (text_config='english');
```

### 并行索引构建

pg_textsearch 支持并行索引构建，以便更快地对大表进行索引。Postgres 会根据表大小和配置自动使用并行工作进程。

```sql
-- 配置并行工作进程（可选，否则使用服务器默认值）
SET max_parallel_maintenance_workers = 4;
SET maintenance_work_mem = '256MB';  -- 并行构建至少需要 64MB

-- 创建索引（大表自动使用并行工作进程）
CREATE INDEX docs_idx ON documents USING bm25(content) WITH (text_config='english');
```

**注意：** 规划器要求 `maintenance_work_mem >= 64MB` 才能启用并行索引构建。内存不足时，构建会静默地回退到串行模式。

使用并行构建时，你会看到如下通知：

```
NOTICE:  parallel index build: launched 4 of 4 requested workers
```

对于分区表，如果分区足够大，每个分区独立地使用并行工作进程构建其索引。这使得可以对非常大的分区数据集进行高效索引。

### 性能调优

#### 强制合并段（Force-merging segments）

索引将数据存储在多个层级的多个段中（类似于 LSM 树）。在批量加载或持续的增量插入之后，可能会积累多个段；将它们合并为一个可以减少扫描的段数，从而提高查询速度：

```sql
SELECT bm25_force_merge('docs_idx');
```

这类似于 Lucene 的 `forceMerge(1)`。它将所有段重写为单个段，并回收释放的页面。最适合在大型批量插入后使用，而不是在持续的写入流量中使用。

#### 使用 LIMIT 配合 ORDER BY

Top-k 查询（`ORDER BY ... LIMIT n`）启用 Block-Max WAND 优化，它可以跳过无法参与顶级结果的 posting 块。如果没有 LIMIT 子句，索引会回退到对最多 `pg_textsearch.default_limit` 个匹配文档评分。

```sql
-- 快速：BMW 跳过无竞争力的块
SELECT * FROM documents ORDER BY content <@> 'search terms' LIMIT 10;

-- 较慢：对最多 default_limit 个文档评分
SELECT * FROM documents ORDER BY content <@> 'search terms';
```

#### 段压缩

压缩默认开启，通常可以提高索引尺寸和查询性能（需要读取更少的页面）。仅当你观察到解压开销成为工作负载瓶颈时才禁用它：

```sql
SET pg_textsearch.compress_segments = off;
```

#### 影响索引构建的 Postgres 设置

| 设置 | 效果 |
|--- | --- |
| `max_parallel_maintenance_workers` | CREATE INDEX 的并行工作进程数（默认 2） |
| `maintenance_work_mem` | 每个工作进程的内存；并行构建必须 >= 64MB |

#### pg_textsearch GUC 参数

| 设置 | 默认值 | 描述 |
|--- | --- | --- |
| `pg_textsearch.default_limit` | 1000 | 在缺少 LIMIT 子句时评分文档的最大数量 |
| `pg_textsearch.compress_segments` | on | 压缩新段中的 posting 块 |
| `pg_textsearch.segments_per_level` | 8 | 每层自动压缩之前的段数（2-64） |
| `pg_textsearch.bulk_load_threshold` | 100000 | 每事务自动溢出前的词条数（0 = 禁用） |
| `pg_textsearch.memtable_pages_threshold` | 64 | 自动溢出前的链页面数（0 = 禁用） |

#### 内存表架构

从 1.3.0 开始，L0 内存表存在于索引关系本身中，作为文档记录页面的链，在标准缓冲区锁下进行修改，并通过 `GenericXLog` 进行 WAL 日志记录。没有共享内存中的内存表，没有自定义的 WAL 资源管理器，也没有 docid-page 恢复脚手架。PostgreSQL 自带的 WAL 重放（包括在线页面修复工具使用的单页重建辅助程序）无需加载 `pg_textsearch.so` 即可重建每个页面。详见 [`docs/memtable_v2.md`](docs/memtable_v2.md) 规范。

自动溢出由两个互补的触发器控制：

- `memtable_pages_threshold` — 每次插入后，当链增长超过配置的页面数时触发。默认 64 页（约 512 KB，8 KB 块大小），由于链保持较小，查询延迟受到限制。
- `bulk_load_threshold` — 在 COMMIT 时，当单个事务在内存表中积累了较多词条时触发；适用于 COPY/批量 INSERT，以限制链页增长。

```sql
-- 手动溢出（强制当前链生成一个新的 L0 段）
SELECT bm25_spill_index('docs_idx');
```

VACUUM（包括 autovacuum 的 insert-threshold 路径）在运行时也会溢出内存表，因此 `CREATE INDEX` 和下次服务器重启之间的未溢出状态量是有界的。

**崩溃恢复**：磁盘上的内存表链本身就是持久化记录。崩溃后，PostgreSQL 自带的 WAL 重放会恢复每个页面；首次打开后端时无需重建。

**流复制**：所有页面修改都通过标准 WAL 流进行复制。从节点本地重建每个页面。

## 监控

```sql
-- 检查索引使用情况
SELECT schemaname, tablename, indexname, idx_scan, idx_tup_read, idx_tup_fetch
FROM pg_stat_user_indexes
WHERE indexrelid::regclass::text ~ 'pg_textsearch';
```

## 示例

### 基本搜索

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

BM25 索引存储了词频但不存储词的位置，因此它无法原生评估像 `"database system"` 这样的短语查询。你可以通过将 BM25 排序与后过滤结合来模拟短语匹配：

```sql
-- BM25 对候选项排序；子查询进行过度获取，以
-- 适应后过滤消除非短语匹配的情况
SELECT * FROM (
    SELECT *, content <@> 'database system' AS score
    FROM documents
    ORDER BY score
    LIMIT 100  -- 过度获取
) sub
WHERE content ILIKE '%database system%'
ORDER BY score
LIMIT 10;
```

由于后过滤会消除一些结果，内层的 LIMIT 应该大于期望的结果数。

### 没有内置的分面搜索

pg_textsearch 不提供专用的分面操作符，但标准的 Postgres 查询机制可以处理常见的分面模式：

```sql
-- 按类别过滤（假设 category 上有 B-tree 索引）
SELECT * FROM documents
WHERE category = 'engineering'
ORDER BY content <@> 'search terms'
LIMIT 10;

-- 对顶级搜索结果计算分面统计
SELECT category, count(*)
FROM (
    SELECT category FROM documents
    ORDER BY content <@> 'search terms'
    LIMIT 100
) matches
GROUP BY category;
```

### 插入/更新性能

内存表架构旨在支持高效的写入，但持续的写入密集型工作负载尚未完全优化。对于初始数据加载，加载数据后再创建索引比增量插入更快。这是当前活跃的开发领域。

### 无后台压缩

段压缩目前同步发生在内存表溢出操作期间。写入密集型工作负载可能会在溢出时观察到压缩延迟。后台压缩计划在未来的版本中实现。

### 分区表

分区表上的 BM25 索引使用**分区本地统计**。每个分区维护自己的：
- 文档数（`total_docs`）
- 平均文档长度（`avg_doc_len`）
- 用于 IDF 计算的每个词条的文档频率

这意味着：
- 针对单个分区的查询使用该分区的统计信息计算准确的 BM25 分数
- 跨多个分区的查询返回按分区独立计算的分数，这些分数在分区之间可能无法直接比较

**示例**：如果分区 A 有 1000 个文档，分区 B 有 10 个文档，那么词条 "database" 在每个分区中会有不同的 IDF 值。来自两个分区的结果的分数会处于不同的量级。

**建议**：
- 对于按时间分区的数据，当分数可比性很重要时，针对单个分区进行查询
- 使用查询自然针对单个分区的分区方案
- 在为搜索工作负载设计分区策略时，考虑此行为

```sql
-- 查询单个分区（分区内分数准确）
SELECT * FROM docs
WHERE created_at >= '2024-01-01' AND created_at < '2025-01-01'
ORDER BY content <@> 'search terms'
LIMIT 10;

-- 跨分区查询（按分区计算分数）
SELECT * FROM docs
ORDER BY content <@> 'search terms'
LIMIT 10;
```

### 单词长度限制

pg_textsearch 继承了 PostgreSQL 的 tsvector 单词长度限制 2047 个字符。超过此限制的单词在分词过程中会被忽略（并产生 INFO 消息）。此限制由 PostgreSQL 文本搜索实现中的 `MAXSTRLEN` 定义。

对于典型的自然语言文本，此限制不会被触发。它可能影响包含超长 token 的文档，例如 base64 编码数据、长 URL 或拼接的标识符。

此行为与其他搜索引擎类似：
- Elasticsearch：截断 token（可通过 `truncate` 过滤器配置，默认 10 个字符）
- Tantivy：默认截断到 255 字节

### 大文档和分块分词

pg_textsearch 调用 Postgres 的 `to_tsvector` 来对文档文本进行分词。Postgres 将单个 `tsvector` 的词素词典限制为 1 MB（`MAXSTRPOS`）。唯一 token 数量可能超过该上限的文档会在分词前被拆分为块（当前为 256 KB），然后合并每个块的词频。

块边界选择在每个窗口内最后一个 ASCII 空白字符处。这对于空白分隔的脚本（拉丁、西里尔、希腊、阿拉伯等）是正确的。对于非空白分隔的脚本（CJK、泰文、老挝文、高棉文），超大文档仍然可以被索引，但块边界可能落在语言感知分词器视为一个词的中间位置。在实践中这是可以接受的，因为 Postgres 的默认文本搜索解析器无论如何都不会为这些脚本产生每个词的 token。如果你使用自定义的文本搜索配置，且该配置具有能为此类脚本产生词级 token 的解析器，那么非常大的文档可能会产生与单次分词略有不同的词素计数。

**超大 CJK（或其他非空白分隔）文档的变通方案**：在应用程序层面将文档拆分为更小的片段，然后索引 `text[]` 列而不是 `text`。pg_textsearch 逐元素索引数组，BM25 分数与将元素拼接为单个 `text` 值的结果相匹配，因此你可以在保持排序质量的同时控制块边界位置。可结合使用来自扩展（如 [zhparser](https://github.com/amutu/zhparser)（中文））的 CJK 感知文本搜索配置，使每个块获得词级分词：

```sql
CREATE EXTENSION zhparser;
CREATE TEXT SEARCH CONFIGURATION chinese_zh (PARSER = zhparser);
ALTER TEXT SEARCH CONFIGURATION chinese_zh
    ADD MAPPING FOR n,v,a,i,e,l WITH simple;

CREATE TABLE docs (id bigserial PRIMARY KEY, content text[]);
CREATE INDEX docs_bm25 ON docs USING bm25(content)
    WITH (text_config='chinese_zh');
```

### PL/pgSQL 和存储过程

隐式的 `text <@> 'query'` 语法依赖于规划器钩子来自动检测 BM25 索引。这些钩子在 PL/pgSQL 的 DO 块、函数或存储过程中不会运行。

**在 PL/pgSQL 中**，使用带有 `to_bm25query()` 的显式索引名称：

```sql
-- 这在 PL/pgSQL 中不起作用：
-- SELECT * FROM docs ORDER BY content <@> 'search terms' LIMIT 10;

-- 请使用显式索引名称：
SELECT * FROM docs
ORDER BY content <@> to_bm25query('search terms', 'docs_idx')
LIMIT 10;
```

普通的 SQL 查询（在 PL/pgSQL 之外）支持两种形式。

## 故障排除

```sql
-- 列出可用的文本搜索配置
SELECT cfgname FROM pg_ts_config;

-- 列出 BM25 索引
SELECT indexname FROM pg_indexes WHERE indexdef LIKE '%USING bm25%';
```

## 安装说明

如果你的机器上安装了多个 Postgres 实例，请指定 `pg_config` 的路径：

```sh
export PG_CONFIG=/Library/PostgreSQL/18/bin/pg_config  # 或 17
make clean && make && make install
```

如果你遇到编译错误，请安装 Postgres 开发文件：

```sh
# Ubuntu/Debian
sudo apt install postgresql-server-dev-17  # 适用于 PostgreSQL 17
sudo apt install postgresql-server-dev-18  # 适用于 PostgreSQL 18
```

## 参考

### 索引选项

| 选项 | 类型 | 默认值 | 描述 |
|--- | --- | --- | --- |
| text_config | string | 必填 | 要使用的 PostgreSQL 文本搜索配置 |
| k1 | real | 1.2 | 词频饱和参数（0.1 到 10.0） |
| b | real | 0.75 | 长度归一化参数（0.0 到 1.0） |

### 文本搜索配置

可用的配置取决于你的 Postgres 安装：

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

更多语言支持可通过扩展获得，如 [zhparser](https://github.com/amutu/zhparser)。

### 开发函数

以下函数仅用于调试和开发目的。其接口可能在未来的版本中不做通知而更改。带有 † 标记的函数需要超级用户权限。

| 函数 | 描述 |
|--- | --- |
| bm25_force_merge(index_name) → void | 将所有段合并为一个（提高查询速度） |
| bm25_spill_index(index_name) → int4 | 强制内存表溢出到磁盘段 |
| bm25_dump_index(index_name) † → text | 转储内部索引结构（截断） |
| bm25_summarize_index(index_name) † → text | 显示索引统计信息（不含内容） |

额外的写入文件的调试函数（`bm25_dump_index(text, text)` 和 `bm25_debug_pageviz`）仅在调试构建中可用（使用 `-DDEBUG_DUMP_INDEX` 编译）。

```sql
-- 将所有段合并为一个（最适合批量加载后使用）
SELECT bm25_force_merge('docs_idx');

-- 强制溢出到磁盘（返回溢出的条目数）
SELECT bm25_spill_index('docs_idx');

-- 快速查看索引统计信息
SELECT bm25_summarize_index('docs_idx');

-- 详细的调试转储（截断输出）
SELECT bm25_dump_index('docs_idx');
```

## 扩展兼容性

pg_textsearch 使用固定的 LWLock tranche ID 1001-1008，以支持大量索引（例如，具有数百个分区的分区表）。如果你使用的其他 Postgres 扩展也在此范围内注册固定 tranche ID，`pg_stat_activity` 中的等待事件名称可能不正确。Postgres 核心 tranche 使用 100 以下的 ID。如果遇到冲突，请[提交 issue](https://github.com/timescale/pg_textsearch/issues)。

## 贡献

有关开发环境搭建、代码风格以及如何提交 pull request，请参阅 [CONTRIBUTING.md](CONTRIBUTING.md)。

- **Bug 报告**：[创建 issue](https://github.com/timescale/pg_textsearch/issues/new?labels=bug&template=bug_report.md)
- **功能请求**：[请求功能](https://github.com/timescale/pg_textsearch/issues/new?labels=enhancement&template=feature_request.md)
- **一般讨论**：[发起讨论](https://github.com/timescale/pg_textsearch/discussions)
