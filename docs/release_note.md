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
