# pg_textsearch 短语检索与模糊检索设计

本文档把 pg_textsearch 的查询语法收敛为两条用户可见能力：

- 精确短语检索：`SELECT * FROM a WHERE a.b == '词汇混合';`
- 精确短语检索并按相关度排序：`SELECT * FROM a WHERE a.b == '词汇混合' ORDER BY score;`
- 模糊检索并按相关度排序：`SELECT * FROM a WHERE a.b %= '词汇混合' ORDER BY score;`

本文档只讨论当前 pg_textsearch 项目上可落地的实现方案，不再沿用旧文档里的双引号短语语法作为主入口。

## 1. 设计目标

本次设计的目标不是再引入一套复杂查询语言，而是在当前项目上补齐两类能力：

- `==` 表示“分词后短语精确匹配”
- `%=` 表示“分词后模糊召回 + BM25 排序”

统一约束如下：

- 查询语义基于 PostgreSQL text config 分词后的 lexeme，而不是原始字符串子串匹配
- `score` 统一表示 BM25 相关度分数
- `==` 和 `%=` 都尽量复用现有 `<@>` 打分路径、planner hook 和 BM25 access method
- 短语命中只负责过滤，不在一期引入 phrase boost
保守版：一期直接不支持同位词/多 lexeme 展开，遇到这种查询就报错。先实现这种
宽松版：任一展开分支命中就算短语命中。
严格版：只认主 lexeme，其它扩展不参与 phrase match。

## 2. 目标语法与语义

### 2.1 精确短语检索

目标语法：

```sql
SELECT * FROM a WHERE a.b == '词汇混合';
SELECT * FROM a WHERE a.b == '词汇混合' ORDER BY score;
```

语义定义：

- 将 `'词汇混合'` 按索引绑定的 text config 分词
- 若分词后得到带 position 的 lexeme 序列 `[(t1, qpos1), (t2, qpos2), ...]`
- 文档必须在同一字段中按相同顺序命中这些 lexeme
- 文档中相邻命中 lexeme 的 position 差必须与查询侧的相对 position 差一致
- 只有满足整段短语连续命中的文档，`==` 才返回 `true`

这里的“精确”是基于 PostgreSQL text config 归一化后的 lexeme + position
语义，而不是原始字符串逐字符子串匹配。也就是说，短语匹配必须和
`to_tsvector` / `to_tsquery` 风格的位置信息保持一致，不能简单写死为
所有相邻词都满足 `+1`。

例子：

- `== '词汇混合'` 命中 `词汇混合`
- 不命中 `混合词汇`
- 不命中 `词汇 和 混合`
- 在会过滤 stop-word 的配置下，若查询经过分词后形成 position gap，
    则文档也必须匹配相同 gap；例如查询侧等价于 `[(bank,1),(china,3)]`
    时，验证条件应是 `doc_pos(china) = doc_pos(bank) + 2`，而不是 `+1`

### 2.2 模糊检索

目标语法：

```sql
SELECT * FROM a WHERE a.b %= '词汇混合' ORDER BY score;
```

语义定义：

- `%=` 仍表示“进入 BM25 召回路径”
- 查询文本按同一 text config 分词
- 命中文档按 BM25 排序
- 不要求词序连续，也不要求全部词必须相邻

对当前项目来说，`%=` 本质上还是对现有 BM25 查询能力的语法糖，不需要依赖 positions。

### 2.3 `score` 的定义

`score` 统一定义为 BM25 分数，不单独为短语命中增加 boost。

这意味着：

- `WHERE a.b == '词汇混合' ORDER BY score` 的执行语义是“先做短语过滤，再按 BM25 分数排序”
- `WHERE a.b %= '词汇混合' ORDER BY score` 的执行语义是“按 BM25 直接召回并排序”

## 3. 现状与缺口

从当前仓库实现看，模糊检索路径已经有基础，但短语检索还缺少底层能力。

### 3.1 已有基础

当前项目已经有三块可复用能力：

- `text <@> bm25query` 的原生打分入口，见 [src/types/query.c](../src/types/query.c)
- `%=` 语法糖改写为 `ORDER BY <@>` 的 planner 路径，见 [src/planner/hooks.c](../src/planner/hooks.c)
- `bm25_get_current_score()` 作为 `score` 暴露桩函数，见 [src/types/query.c](../src/types/query.c)

也就是说，模糊检索的排序基础已经存在，新增工作主要集中在：

- 新增 `==` 入口
- 为 phrase verification 提供 position 数据
- 让 `==` 和 `ORDER BY score` 能走进同一条索引执行路径

### 3.2 当前缺口

当前项目不能直接支持短语检索，根因是索引只存词频，不存词位置：

- 构建阶段只把 `tsvector` 的位置信息用于计数，没有落盘位置流
- `bm25vector` / memtable doc record 不包含每个 lexeme 的 position list
- segment posting 只覆盖 `doc_id`、`frequency`、`fieldnorm` 等打分字段
- 查询对象 `bm25query` 只有原始文本和索引引用，没有 phrase constraint 表达

因此，`==` 不是加一个操作符就能完成的功能，而是需要把索引从“文档级词频倒排”升级为“带 positions 的倒排”。

## 4. 推荐总体方案

推荐把整套能力设计成“三个操作符 + 一条统一打分内核”：

- `==`：布尔操作符，表示短语精确匹配
- `%=`：布尔操作符或布尔语法糖，表示模糊召回入口
- `<@>`：float8 打分操作符，继续作为底层 BM25 排序操作符

统一执行内核：

1. 把用户查询解析成内部 `TpParsedQuery`
2. 生成召回词项集合
3. 运行现有 BMW / BM25 召回
4. 如果存在 phrase constraint，则做 phrase verification
5. 输出命中文档并填充 `score`

这样做有两个好处：

- 模糊检索可以最大化复用现有实现
- 精确短语只是在现有召回结果上增加一层位置验证，不需要推翻现有架构

## 5. 语法层设计

### 5.1 `==` 的 SQL 合约

建议定义：

```sql
CREATE OPERATOR == (
  LEFTARG = text,
  RIGHTARG = text,
  PROCEDURE = bm25_phrase_match
);
```

返回值为 `boolean`，用于 `WHERE` 子句。

它的职责只有一个：表达“短语必须命中”。它不是排序操作符，不直接返回分数。

### 5.2 `%=` 的 SQL 合约

保留当前 `%=` 的整体思路，但语义文档化为“模糊召回入口”。

建议维持：

- `WHERE a.b %= '词汇混合' ORDER BY score` 作为主用户语法
- 其内部仍重写到 `<@>` 排序路径
- `%=` 本身不承担真正评分职责

### 5.3 `ORDER BY score` 的实现方式

`ORDER BY score` 不是一个独立索引能力，它只是 `<@>` 打分结果的名字。

推荐内部改写目标：

```sql
SELECT *
FROM a
WHERE a.b == '词汇混合'
ORDER BY score;
```

内部等价到：

```sql
SELECT *, bm25_get_current_score() AS score
FROM a
WHERE a.b == '词汇混合'
ORDER BY a.b <@> to_bm25query('词汇混合', 'idx');
```

对 `%=` 也是同样思路。

### 5.4 关于 `ORDER BY score` 的解析风险

这里有一个必须正面说明的工程点：

- 当前 `%=` 改写逻辑位于 [src/planner/hooks.c](../src/planner/hooks.c) 的 `post_parse_analyze_hook`
- `ORDER BY score` 这种名字解析是否总能在这个阶段安全补齐，需要先做原型验证

因此设计上建议分成两档：

- 目标语法档：直接支持 `ORDER BY score`
- 保底语法档：若 parser 阶段验证发现限制，则保底支持 `ORDER BY a.b <@> ...` 或显式 `SELECT ..., bm25_get_current_score() AS score`

如果必须严格保证字面语法 `ORDER BY score`，则应把这部分改写做成比当前 hook 更早的语法改写层，或在 PostgreSQL 核心侧补一个更早的 parse hook。对当前仓库而言，先做原型验证再决定是否需要 core patch，是更稳妥的路线。

## 6. 查询内部表示

当前 `TpQuery` 不足以表达短语约束，建议新增执行期解析结果：

```c
typedef enum TpQueryMode
{
    TP_QUERY_FUZZY,
    TP_QUERY_PHRASE
} TpQueryMode;

typedef struct TpPhraseNode
{
    int term_count;
    char **terms;
    int *query_positions;
} TpPhraseNode;

typedef struct TpParsedQuery
{
    TpQueryMode mode;
    int scoring_term_count;
    char **scoring_terms;
    int phrase_count;
    TpPhraseNode *phrases;
} TpParsedQuery;
```

设计要求：

- `== '词汇混合'` 解析为 `TP_QUERY_PHRASE`
- `%= '词汇混合'` 解析为 `TP_QUERY_FUZZY`
- phrase 节点必须保留查询侧 position，而不仅是 term 列表，否则在
    stop-word、词典归一化或同位词场景下无法做正确验证
- 外部 SQL 类型 `bm25query` 可以先不变，避免立刻破坏二进制兼容
- phrase constraint 只在执行期缓存，不要求立刻改动用户可见类型格式

## 7. 存储层改造

### 7.1 必须保存 positions

短语检索的根前提是每个 `(term, doc)` 都能拿到位置列表。

建议新增或升级内部向量格式，使每个 lexeme 记录：

- lexeme
- frequency
- position_count
- positions[]

推荐方向：升级现有 `bm25vector` 内部格式，而不是在 memtable 外挂一套 position sidecar。原因是 spill、cache build、chain source 和 standalone scoring 都需要同一份真值来源。

### 7.2 segment 格式升级

建议引入新的 segment format version，并增加：

- posting entry 中的 `position_count`
- 指向 position stream 的 offset
- segment header 中的 capability bit，例如 `has_positions`

兼容策略：

- 旧 segment：只支持模糊检索和普通 BM25 排序
- 新 segment：支持 `==` 精确短语
- 对旧索引执行 `==` 时直接报错，不允许静默降级

### 7.3 memtable / cache / spill

需要同步改造：

- [src/access/build.c](../src/access/build.c)：从 `tsvector` 提取位置数组
- memtable doc record：保存 positions
- chain source / cache source：提供 positions 读取接口
- spill：把 positions 一起写入 segment
- merge：合并 posting 时保留 positions
- VACUUM：清理 dead doc 时同步清理 positions

## 8. 执行层设计

### 8.1 模糊检索路径

`%=` 的执行路径尽量不改核心逻辑：

1. 将 `%=` 改写为底层 `<@>` 排序路径
2. 运行现有 BM25 / BMW
3. 通过 `bm25_get_current_score()` 暴露 `score`

这个路径只需要少量语法层收敛，不依赖 positions。

### 8.2 精确短语路径

`==` 的执行路径建议采用“两段式”：

1. 用短语里的所有 lexeme 参与 BM25 召回
2. 对候选文档做 phrase verification

phrase verification 接口建议抽象为：

- `tp_source_get_positions(term, doc_ref)`
- `tp_verify_phrase(doc_ref, phrase_node)`

验证算法从最简单的 positional intersection 开始：

1. 取第一个词的位置列表
2. 用查询侧相对位置差做校验，即对第 `i` 个词检查
    `doc_pos[i] = doc_pos[0] + (query_pos[i] - query_pos[0])`
3. 把满足条件的位置链继续与后续词做同样的 gap 验证
4. 任一完整链成立，则短语命中

如果实现里把所有相邻词都简化成 `+1`，则在 stop-word 被过滤但 position
gap 仍保留的 text config 下会产生漏检。这一点必须在一期设计里直接规避。

### 8.3 排序策略

一期排序策略保持简单：

- `==` 命中后，仍按普通 BM25 分数排序
- 不做 phrase boost
- 不做 phrase-aware BMW 上界估计

这样可以先把 correctness 做稳定，再看是否需要为短语命中增加加权。

## 9. 访问方法与 planner 设计

### 9.1 access method 侧

建议把 `==` 作为真正的 Search key，把 `<@>` 继续作为 ORDER BY key。

对应查询：

```sql
SELECT * FROM a WHERE a.b == '词汇混合' ORDER BY score;
```

逻辑上等价于：

- Search key：`a.b == '词汇混合'`
- Order key：`a.b <@> to_bm25query('词汇混合', idx)`

`amrescan()` 需要能同时抽取：

- phrase/fuzzy 查询文本
- 查询模式
- order by 的打分 query

`amgettuple()` 需要能同时处理：

- 候选召回
- phrase verification
- `xs_orderbyvals` 填充分数

### 9.2 planner hook 侧

建议新增 `==` 的 query rewrite，方式和当前 `%=` 保持同一风格：

- 识别 `WHERE col == '短语'`
- 若没有 `ORDER BY`，生成 Search-only 的索引扫描路径
- 若有 `ORDER BY score`，补齐底层 `<@>` 排序表达式
- 若目标索引不支持 positions，直接报错

对 `%=` 则沿用现有 rewrite 思路，只把文档说明和错误处理补齐。

## 10. 索引能力声明与兼容策略

建议为索引增加显式能力位，例如：

```sql
CREATE INDEX idx_docs_bm25 ON docs
USING bm25 (content)
WITH (text_config = 'simple', store_positions = true);
```

建议规则：

- `store_positions = false` 或旧索引：仅支持 `%=` 和 `<@>`
- `store_positions = true`：支持 `==`
- 对不支持 positions 的索引执行 `==`，报明确错误并提示 `REINDEX`

这样可以避免无感知地增大所有索引体积，也便于平滑迁移旧数据。

## 11. 分阶段实施计划

### 阶段 0：语法原型

目标：先验证语法层没有走偏。

工作项：

- 注册 `== (text, text) -> boolean`
- 验证 `WHERE a.b == '词汇混合'` 的 planner / executor 接线方式
- 验证 `ORDER BY score` 是否能沿用当前 hook 风格稳定改写
- 如果不稳定，立即收敛为“纯扩展保底语法”和“需要 core patch 的目标语法”两档

### 阶段 1：positions 存储打底

工作项：

- 扩展 `bm25vector` 内部格式
- 改造 build / memtable / spill / merge / vacuum
- 完成 segment format version 升级

验收标准：

- memtable 和 segment 都能返回每个 term 的位置列表
- 新旧 segment 能正确区分能力位

### 阶段 2：phrase verification

工作项：

- 实现 source 级 positions 访问接口
- 实现 `tp_verify_phrase()`
- 把 `==` 接入索引扫描和 standalone 路径

验收标准：

- `WHERE a.b == '词汇混合'` 返回正确结果
- memtable 命中和 spill 后命中结果一致

### 阶段 3：score 与排序接线

工作项：

- 把 `== ... ORDER BY score` 接到 `<@>` 打分路径
- 统一 `%=` 和 `==` 的 `score` 暴露方式
- 补充错误信息和 planner 诊断日志

验收标准：

- `WHERE a.b == '词汇混合' ORDER BY score` 返回正确排序
- `WHERE a.b %= '词汇混合' ORDER BY score` 保持现有行为不回退

### 阶段 4：性能与优化

二期可选项：

- phrase boost
- phrase-aware BMW
- 高频词短语首词锚定
- 更紧凑的 position 压缩布局

## 12. 测试计划

至少覆盖以下场景：

- 双词短语、三词短语、重复词短语
- 中文分词、英文词干化、stop-word 位置影响
- stop-word 过滤后仍保留查询侧 gap 的短语，验证不会因为写死 `+1`
    而漏检
- 词典归一化或同位词导致多个 lexeme / position 解释时，短语语义有明
    确且稳定的行为
- `%=` 模糊检索与 `<@>` 现有排序结果一致
- `==` 在 memtable 命中、spill 后命中、merge 后命中结果一致
- `VACUUM`、crash recovery、standby replay 后短语结果一致
- 对旧索引执行 `==` 报错
- `ORDER BY score` 在 `==` 和 `%=` 两条路径都能得到稳定计划

## 13. 需要改动的主要文件

预计会涉及这些模块：

- [src/types/query.h](../src/types/query.h)
- [src/types/query.c](../src/types/query.c)
- [src/types/vector.h](../src/types/vector.h)
- [src/types/vector.c](../src/types/vector.c)
- [src/planner/hooks.c](../src/planner/hooks.c)
- [src/access/build.c](../src/access/build.c)
- [src/access/scan.c](../src/access/scan.c)
- [src/index/source.h](../src/index/source.h)
- [src/memtable](../src/memtable)
- [src/segment](../src/segment)
- [sql/pg_textsearch--1.4.0-dev.sql](../sql/pg_textsearch--1.4.0-dev.sql)
- [test/sql](../test/sql)
- [test/expected](../test/expected)

## 14. 结论

如果要在当前 pg_textsearch 项目上支持你要求的语法，真正的根改动有两件：

- 语法层：把 `==` 定义成短语精确匹配入口，把 `%=` 定义成模糊召回入口，并统一接入 `score`
- 存储层：把索引从“只存词频”升级到“存词频 + positions”

其中，`%=` 主要是语法和 planner 收敛问题，技术风险较低；`==` 的核心难点是 positions 存储和 phrase verification。只要把 positions 打通，`==` 本质上就是在现有 BM25 召回路径上叠加一层短语过滤，而不需要推翻当前架构。
- 恢复一致性：crash recovery / standby replay 后短语结果一致
- 兼容错误：对旧索引执行短语查询时报错
- 性能回归：大词典、高频词、长文档场景

## 14. 风险清单

主要风险有四类：

- 体积风险：位置列表会显著放大 segment 大小
- 写放大风险：spill/merge 成本上升
- 语义风险：`tsvector` 位置与用户直觉中的“原始字符串短语”不完全等价
- 复杂度风险：BMW、cache、segment merge、VACUUM 都要跟着升级

建议的控制策略是：

- 一期只做 phrase filter，不做 phrase boost
- 一期不做并行短语扫描
- 用 capability/index option 隔离新旧索引
- 用新的 segment format version 做清晰边界

## 15. 建议结论

如果项目要支持短语精确查询，根本前提不是改 SQL 语法，而是让索引从“词频倒排”升级为“带位置的倒排”。

最稳妥的实施路线是：

1. 保持现有 BM25/BMW 主流程不变
2. 给 memtable 和 segment 增加 positions 存储
3. 给查询执行增加 phrase verification 过滤层
4. 用新格式和新索引选项隔离兼容性

这样可以在不推翻当前架构的情况下，把短语能力作为现有 BM25 排序系统上的一个增量特性落地。