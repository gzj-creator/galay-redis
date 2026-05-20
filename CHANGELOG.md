# CHANGELOG

维护说明：
- 未打 tag 的改动先写入 `

## [Unreleased]

## [v2.1.0] - 2026-05-20

### Added
- 新增 `galay::redis::log::set/get` 库级日志入口，使用 `galay-kernel` 的 `BaseLogger` 和独立 logger 槽位。
- 新增 `REDIS_LOG_*` 埋点宏，并在 async 客户端、TLS 客户端、连接池和遗留同步会话路径补充日志埋点。
- 新增 `t26_log` 回归测试，验证未设置 logger 和级别过滤时不会求值日志格式化参数。

### Changed
- 移除旧的 `spdlog` / `RedisLog` / `setLogger` / `logger()` 公开日志入口，Redis 日志改为只通过库级 `BaseLogger` 注入启用。
- 将 `galay-kernel` 依赖提升到 `5.0.0`，将 TLS 路径的 `galay-ssl` 依赖提升到 `2.1.0`。
- 当前发布版本提升到 `2.1.0`，并同步 CMake package 版本。

## [v2.0.3] - 2026-05-18

### Changed
- `spdlog` 从公开 CMake 链接依赖改为头文件 include 查找，`galay-redis` 不再导出 `spdlog::` link dependency。
- 安装包 config 在消费端通过 `find_path` 补充 spdlog 头文件 include 路径，保持公开头文件可用。
- 将安装导出的 CMake targets 文件改为 `galayRedisConfigTargets.cmake`，构建树导出文件改为 `galayRedisConfigTargets-build.cmake`，Release 安装生成 `galayRedisConfigTargets-release.cmake`。
- 将 CMake project 版本提升到 `2.0.3`，对齐本次发布 tag。

### Chore
- 接口回归检查新增断言，禁止 `galay-redis` 再次导出 `spdlog::` 链接依赖。


## [v2.0.2] - 2026-05-17

### Fixed
- 将 Redis 示例协程入口从兼容别名 `Coroutine` 迁移为显式 `Task<void>`，避免编译器诊断继续暴露旧命名。
- 拆分 `e3_pubsub` 的大型协程为 pub/sub、master-slave 与 cluster 三段 `Task<bool>` 子流程，降低 GCC 13 在 coroutine frame 清理路径上触发 ICE 的概率。

### Changed
- README 与快速开始示例同步使用 `Task<void>`，保持文档和当前公开任务模型一致。
- 新增接口回归检查，阻止 examples 目录再次暴露 `Coroutine` 返回类型。
- 将 CMake project 版本提升到 `2.0.2`，对齐本次发布 tag。

## [v2.0.1] - 2026-05-11

### Chore
- 移除 `benchmark/compare` 目录，避免误提交对比基准测试代码与构建产物。

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
