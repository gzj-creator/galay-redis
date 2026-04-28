# CHANGELOG

维护说明：
- 未打 tag 的改动先写入 `

## [Unreleased]

## [v2.0.0] - 2026-04-29

### Changed
- 统一源码、头文件、测试、示例与 benchmark 文件命名为 `lower_snake_case`，编号前缀同步使用 `t<number>_`、`e<number>_` 与 `b<number>_` 风格。
- 同步更新构建脚本、模块入口、示例、测试、文档与脚本中的文件路径引用。
- 将项目内头文件包含调整为基于公开 include 根或模块根的非相对路径。

### Release
- 按大版本发布要求提升版本到 `v2.0.0`。

## [v1.2.2] - 2026-04-21

### Changed
- 锁定 `galay-utils 1.0.3` 与 `galay-ssl 1.2.2` 的 CMake 依赖版本，避免 TLS 与工具库路径回落到旧前缀。
- 同步更新导出包配置中的 `galay-ssl` 版本约束，确保下游 `rediss://` 能力与当前构建基线一致。
