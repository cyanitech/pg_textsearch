# 发布 pg_textsearch

本文档说明 pg_textsearch 的发布流程。

## 版本方案

我们使用语义化版本：`MAJOR.MINOR.PATCH`。

- 开发版本使用 `-dev` 后缀（例如 `1.2.0-dev`）。
- 正式发布版本去掉该后缀（例如 `1.2.0`）。

## 发布一个版本

### 1. 审核升级 SQL 脚本

这是发布流程中唯一无法自动化的部分，也是最容易出错的部分。请仔细过一遍。

升级脚本 `sql/pg_textsearch--PREV--CURRENT-dev.sql` 必须确保：对于仍停留在
`PREV` 版本的安装，它能够重新创建当前主 SQL 文件中存在、但上一个发布版本的主 SQL
文件中不存在的每一个 catalog 对象。要枚举升级脚本必须覆盖的内容，请比较这两个主 SQL 文件：

```sh
git show vPREV:sql/pg_textsearch--PREV.sql > /tmp/prev.sql
diff /tmp/prev.sql sql/pg_textsearch--CURRENT-dev.sql
```

对于当前主文件中每一条新增语句，都要确认升级脚本里有对应语句：

| 主文件中的新增内容 | 升级脚本中的必需内容 |
|--------------------|----------------------|
| `CREATE FUNCTION` | `CREATE FUNCTION` |
| `CREATE OPERATOR` | `CREATE OPERATOR` |
| `CREATE OPERATOR CLASS` | `CREATE OPERATOR CLASS` |
| `CREATE TYPE` | `CREATE TYPE` |
| `CREATE CAST` | `CREATE CAST` |
| `ALTER OPERATOR FAMILY` | `ALTER OPERATOR FAMILY` |
| Catalog `UPDATE pg_catalog.*` | 相同的 `UPDATE` |

如果对象被重命名、签名发生变化、或被删除，升级脚本中也需要配套的 `DROP` / `ALTER`
语句。

CI 中的 `upgrade-tests` 工作流会针对其 `old_version` 矩阵中的每个版本执行升级测试，
这是发现遗漏的主要安全网。但它只能捕获回归测试实际覆盖到的问题。一个没有测试覆盖的新
operator 仍然可能通过 CI，然后以损坏状态发布。人工审核能补上工作流发现不了的缺口。

### 2. 运行版本提升脚本

```sh
./scripts/bump-version.sh CURRENT-dev CURRENT
```

这会重命名 SQL 文件，并更新整个代码树中的所有版本引用。脚本会提示还需要人工完成哪些步骤。

### 3. 更新发布横幅图片

将 `images/tapir_and_friends_vCURRENT-dev.png` 替换为新的发布横幅图
`images/tapir_and_friends_vCURRENT.png`。（README 中的引用已经由 bump 脚本更新。）

### 4. 把上一个发布版本加入 upgrade-tests 矩阵

在 `.github/workflows/upgrade-tests.yml` 中，把 `PREV` 加入 `old_version`
矩阵，这样后续发布时会测试从该版本升级的兼容性。

### 5. 创建 PR

```sh
git checkout -b release-CURRENT
git add -A
git commit -m "Release vCURRENT"
gh pr create --draft --title "Release vCURRENT"
```

CI 会运行完整测试套件，包括针对矩阵中每个版本的 upgrade-tests。

### 6. PR 合并后

`release.yml` 工作流会在标题匹配 `Release v*` 的 PR 合并后触发。它会给提交打 tag，
在 Linux/macOS 上为 PG17/PG18 构建发布产物（amd64/arm64），并发布 GitHub Release。

## 提升到下一个开发版本

发布完成后，创建一个后续 PR：

```sh
git checkout main && git pull
git checkout -b bump-to-NEXT-dev
./scripts/bump-version.sh CURRENT NEXT-dev
git add -A
git commit -m "chore: bump version to NEXT-dev"
gh pr create --draft --title "chore: bump version to NEXT-dev"
```

该脚本会创建新的主 SQL 文件（由上一个发布版本的主 SQL 文件重命名而来），创建一个只包含
库版本检查的升级文件骨架 `sql/pg_textsearch--CURRENT--NEXT-dev.sql`，并更新其余所有
版本引用。随后，开发者会随着开发周期中新功能的落地，持续向这个升级文件追加内容。

## SQL 升级路径要求

**升级脚本必须形成一条单一线性链路。** 每个版本只能连接到唯一的下一个版本，不能通过捷径
跳过中间步骤。这样可以让升级脚本数量最少，并使升级路径可预测。

```
... → 0.5.0 → 0.5.1 → 0.6.0 → 0.6.1 → 1.0.0 → 1.1.0
```

每个发布版本都必须提供从上一个稳定版本升级的路径（例如 `1.0.0--1.1.0.sql`）。不支持从
开发版本直接升级；使用开发版本的用户应重新安装该扩展。

## 升级兼容性

并非所有版本升级都兼容 `ALTER EXTENSION UPDATE`。以下破坏性变更会要求重建索引或重启服务器：

| 变更类型 | 影响 | 用户需要执行的操作 |
|----------|------|--------------------|
| Metapage 版本提升 | 现有索引不兼容 | `REINDEX` 或重建索引 |
| 共享内存结构变更 | 新旧版本混用时可能导致服务器崩溃 | 升级后重启 Postgres |
| Segment 格式变更 | 旧 segment 不可读 | 重建索引 |

**当前兼容性矩阵：**

| 起始版本 | 目标版本 | 兼容？ | 说明 |
|-----------|----------|--------|------|
| 0.5.x | 0.6.0 | ⚠️ REINDEX | Segment 格式 v3→v4（uint32→uint64 offsets） |
| 0.3.0 | 0.4.0 | ❌ 否 | 为压缩支持将 Segment 格式从 v2→v3 |
| 0.2.0 | 0.4.0 | ❌ 否 | Segment 格式 v2→v3 |
| 0.2.0 | 0.3.0 | ✅ 是 | 可直接升级 |
| 0.1.0 | 0.3.0 | ❌ 否 | Metapage v4→v5，shmem 大小变更 |
| < 0.1.0 | 0.3.0 | ❌ 否 | 多项破坏性变更 |

当发布一个包含破坏性变更的版本时：

1. 更新 `.github/workflows/upgrade-tests.yml`，排除不兼容版本。
2. 在发布说明中注明用户必须重建索引。
3. 对于主版本升级，考虑提供迁移指南。

## 磁盘格式版本

如果某个发布周期中任一磁盘格式发生变化，请提升对应的版本常量：

| 常量 | 头文件 | 用途 |
|------|--------|------|
| `TP_METAPAGE_VERSION` | `src/constants.h` | 索引 metapage 格式 |
| `TP_DOCID_PAGE_VERSION` | `src/constants.h` | 文档 ID 页格式 |
| `TP_PAGE_INDEX_VERSION` | `src/constants.h` | 页索引格式 |
| `TP_SEGMENT_FORMAT_VERSION` | `src/segment/segment.h` | Segment 块存储格式 |
| `TPQUERY_VERSION` | `src/types/query.h` | bm25query 二进制格式 |

版本号提升会破坏升级兼容性，因此需要在 upgrade-tests 矩阵中排除不兼容的旧版本，并在发布说明中
记录这一破坏性变更。

## 自动化工作流

| 工作流 | 触发条件 | 用途 |
|--------|----------|------|
| `ci.yml` | PR、push 到 main | 在 PG17/18 上构建并测试 |
| `upgrade-tests.yml` | PR（sql/control 变更）、每周 | 测试扩展升级 |
| `release.yml` | 标题包含 "Release v" 的 PR 合并 | 创建发布和产物 |
| `benchmark.yml` | 每周、手动 | 性能回归测试 |

## 故障排查

### Postgres share 目录中的旧 SQL 文件

如果测试因旧版本相关信息失败，请检查是否存在陈旧文件：

```sh
ls ~/pg18/share/postgresql/extension/pg_textsearch*
```

删除那些不应再安装的旧开发版本：

```sh
rm ~/pg18/share/postgresql/extension/pg_textsearch--X.Y.Z-dev.sql
```

### 扩展无法升级

如果 `ALTER EXTENSION pg_textsearch UPDATE` 失败，请检查：

1. share 目录中是否存在升级 SQL 文件。
2. control 文件中的 `default_version` 是否正确。
3. 升级路径是否存在（检查 `pg_extension_update_paths('pg_textsearch')`）。
