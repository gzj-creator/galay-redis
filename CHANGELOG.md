# CHANGELOG

维护说明：
- 未打 tag 的改动先写入 `## [Unreleased]`。
- 需要发版时，从 `Unreleased` 或“自上次 tag 以来”的累计变更整理出新的版本节。
- 版本号遵循 `major/minor/patch` 规则：大改动升主版本，新功能升次版本，修复与非破坏性维护升修订版本。
- 推荐标题格式为 `## [vX.Y.Z] - YYYY-MM-DD`，正文按 `Added` / `Changed` / `Fixed` / `Docs` / `Chore` 归纳。

## [Unreleased]

## [v1.2.2] - 2026-04-21

### Changed
- 锁定 `galay-utils 1.0.3` 与 `galay-ssl 1.2.2` 的 CMake 依赖版本，避免 TLS 与工具库路径回落到旧前缀。
- 同步更新导出包配置中的 `galay-ssl` 版本约束，确保下游 `rediss://` 能力与当前构建基线一致。
