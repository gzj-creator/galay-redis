# Release Note

按时间顺序追加版本记录，避免覆盖历史发布说明。

## v1.2.2 - 2026-04-21

- 版本级别：小版本（patch）
- Git 提交消息：`chore: 发布 v1.2.2`
- Git Tag：`v1.2.2`
- 自述摘要：
  - 锁定 `galay-utils 1.0.3` 与 `galay-ssl 1.2.2` 的依赖版本，确保 `galay-redis` 在最新基础库前缀下稳定构建。
  - 同步导出包配置里的 TLS 依赖版本约束，减少下游 `find_package(galay-redis)` 时命中旧 `galay-ssl` 的风险。

## v2.0.0 - 2026-04-29

- 版本级别：大版本（major）
- Git 提交消息：`refactor: 统一源码文件命名规范`
- Git Tag：`v2.0.0`
- 自述摘要：
  - 将源码、头文件、测试、示例与 benchmark 文件统一重命名为 lower_snake_case，编号前缀同步改为小写下划线形式。
  - 同步更新 CMake/Bazel 构建描述、模块入口、README/docs、脚本和所有项目内 include 路径引用。
  - 移除项目内相对 include，统一使用基于公开 include 根或模块根的非相对路径。

## v2.0.1 - 2026-05-11

- 版本级别：小版本（patch）
- Git 提交消息：`chore: 移除 benchmark compare 目录`
- Git Tag：`v2.0.1`
- 自述摘要：
  - 移除 `benchmark/compare` 目录并收紧忽略规则，避免误提交对比基准测试代码与构建产物。

## v2.0.2 - 2026-05-17

- 版本级别：小版本（patch）
- Git 提交消息：`fix: 修复 redis 示例协程编译兼容性`
- Git Tag：`v2.0.2`
- 自述摘要：
  - 将 Redis examples 与 README/快速开始中的协程入口从兼容别名 `Coroutine` 迁移为显式 `Task<void>`，避免诊断继续暴露旧命名。
  - 将 `e3_pubsub` 的大型协程拆分为 pub/sub、master-slave 与 cluster 三段 `Task<bool>` 子流程，并在顶层使用 `auto result = co_await ...; if (!result)` 的保守写法，降低 GCC 13 coroutine frame 清理路径触发 ICE 的概率。
  - 新增接口回归检查，防止 examples 目录再次使用 `Coroutine` 返回类型；同步将 CMake project 版本提升到 `2.0.2`。

## v2.0.3 - 2026-05-18

- 版本级别：小版本（patch）
- Git 提交消息：`chore: 调整 spdlog 头文件依赖与导出命名`
- Git Tag：`v2.0.3`
- 自述摘要：
  - 将 `spdlog` 从 `galay-redis` 的公开 CMake 链接依赖调整为头文件 include 查找，安装包不再导出 `spdlog::` link dependency。
  - `galay-redis-config.cmake` 在消费端通过 `find_path` 补充 spdlog 头文件 include 路径，保持公开日志头文件可消费。
  - 将安装导出的 CMake targets 文件改为 `galayRedisConfigTargets.cmake`，构建树导出文件改为 `galayRedisConfigTargets-build.cmake`，Release 安装生成 `galayRedisConfigTargets-release.cmake`。
  - 接口回归检查新增 `spdlog::` 链接导出断言，并将 CMake project 版本提升到 `2.0.3`。
