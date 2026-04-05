# galay-redis

`galay-redis` 是 `galay` 生态中的 Redis 客户端库。当前仓库里，经过 `examples/`、`test/`、`benchmark/` 实际覆盖的主路径已经统一到：

- 单命令（常规 owning 路径）：`RedisCommandBuilder` + `RedisClient::command(...)`
- Plain 零拷贝快路径（内部 / benchmark / 可信调用方）：`RedisBorrowedCommand` + `RedisClient::commandBorrowed(...)` / `RedisClient::batchBorrowed(...)`
- TLS 单命令：`RedissClientBuilder` + `RedissClient::connect("rediss://...")`
- 批量发送（常规 owning 路径）：`RedisCommandBuilder::append(...)` + `RedisClient::batch(...)`
- 拓扑访问：`RedisMasterSlaveClient::execute(...)` / `RedisClusterClient::execute(...)`
- TLS 拓扑访问：`RedissMasterSlaveClient::refreshFromSentinel()` / `RedissClusterClient::refreshSlots()`

本文档按如下真相顺序维护：公开头文件与导出目标 → 实现行为 → 示例 → 测试 → benchmark → Markdown 说明。

## 特性概览

- 异步协程客户端：`RedisClient`、`RedissClient`、`RedisMasterSlaveClient`、`RedissMasterSlaveClient`、`RedisClusterClient`、`RedissClusterClient`
- 统一命令构建：`RedisCommandBuilder` 提供 `command`、`append`、便捷命令封装
- 批量/流水线：`RedisClient::batch(std::span<const RedisCommandView>)`，plain 内部快路径可用 `batchBorrowed(...)`
- 连接池：`RedisConnectionPool`、`RedissConnectionPool`
- 拓扑能力：主从读写、Sentinel 刷新、Cluster 槽路由、MOVED/ASK 自动处理
- TLS / `rediss://`：支持 SNI、CA 校验、`verify_peer`、默认端口 `6380`
- C++23 模块接口：可选目标 `galay-redis-modules`

## 文档入口

- [文档索引](docs/README.md)
- [00-快速开始](docs/00-快速开始.md)
- [01-架构设计](docs/01-架构设计.md)
- [02-API参考](docs/02-API参考.md)
- [03-使用指南](docs/03-使用指南.md)
- [04-示例代码](docs/04-示例代码.md)
- [05-性能测试](docs/05-性能测试.md)
- [06-高级主题](docs/06-高级主题.md)
- [07-常见问题](docs/07-常见问题.md)

## 构建前提

以下要求直接来自 `CMakeLists.txt`、`cmake/option.cmake` 和 `galay-redis/CMakeLists.txt`：

- CMake 最低版本：`3.20`
- C++ 标准：`C++23`
- 外部依赖：`OpenSSL`、`spdlog`
- 内部依赖：`galay-kernel`、`galay-utils`
- TLS 额外依赖：`galay-ssl`（当 `GALAY_REDIS_ENABLE_SSL=ON` 时）
- 可选构建开关：`BUILD_EXAMPLES`、`BUILD_TESTING`、`BUILD_BENCHMARKS`、`GALAY_REDIS_ENABLE_SSL`
- 兼容开关：`BUILD_TESTS` 仍可用，但只作为 `BUILD_TESTING` 的旧别名

如果 `galay-kernel` / `galay-utils` 没有安装到默认搜索路径，需要通过 `CMAKE_PREFIX_PATH` 或各自的 package config 让 `find_package(...)` 可见。

monorepo 联调时，推荐把共享前缀放在 `CMAKE_PREFIX_PATH` 首位：

```bash
-DCMAKE_PREFIX_PATH=/Users/gongzhijie/Desktop/projects/git/.galay-prefix/latest
```

当前 `galay-kernel` 对齐基线是 `3.4.4+`。

`examples/`、`test/`、`benchmark/` 现在不再写死 `/usr/local/include` 或 `/opt/homebrew`。如果依赖安装在自定义前缀，请在配置阶段通过 `CMAKE_PREFIX_PATH`、`OpenSSL_ROOT_DIR` 或对应 package config 暴露它们。

下文统一用 `<build-dir>` 表示你的 CMake binary dir；它可以是 `build`、`build-release`、`build-doccheck` 等任意目录名。

```bash
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DBUILD_EXAMPLES=ON \
  -DBUILD_TESTING=ON \
  -DBUILD_BENCHMARKS=ON
cmake --build build --parallel
```

启用 TLS / `rediss://` 时额外打开：

```bash
cmake -S . -B build-ssl \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DGALAY_REDIS_ENABLE_SSL=ON \
  -DBUILD_TESTING=ON
cmake --build build-ssl --parallel
```

标准 CTest 入口已经注册到 `test/CMakeLists.txt`。常用命令：

```bash
ctest --test-dir build --output-on-failure -L unit
ctest --test-dir build --output-on-failure -L redis
GALAY_IT_ENABLE=1 ctest --test-dir build --output-on-failure -L integration
```

如果你只想确认当前 awaitable / 连接池公开 surface 与源码一致，可直接运行：

```bash
ctest --test-dir build --output-on-failure -R T15-awaitable_surface
```

## 在你的项目中接入

当前仓库已经提供可安装的 CMake package config。推荐的企业接入方式是：

```cmake
find_package(galay-redis CONFIG REQUIRED)
target_link_libraries(your_app PRIVATE galay-redis::galay-redis)
```

如果 `galay-kernel`、`spdlog` 或可选的 `galay-ssl` 安装在自定义前缀，请把对应前缀加入 `CMAKE_PREFIX_PATH`。`galay-utils` 仍然是本仓库的构建依赖，但不再作为安装消费方必须显式重建的公开链接依赖。在源码树内联调时，也仍然可以使用 `add_subdirectory(...)`：

```cmake
add_subdirectory(external/galay-redis)
target_link_libraries(your_app PRIVATE galay-redis)
```

如果工具链满足模块条件，额外可用目标为 `galay-redis-modules`；详细限制见 `docs/06-高级主题.md`。

## 30 秒上手

来源：`examples/include/E1-async_basic_demo.cc`

- 目标：`E1-async_basic_demo`
- 运行：`./<build-dir>/examples/E1-async_basic_demo 127.0.0.1 6379`
- 环境变量：无

```cpp
#include "galay-redis/async/RedisClient.h"
#include <galay-kernel/kernel/Runtime.h>
#include <chrono>
#include <iostream>

using namespace galay::kernel;
using namespace galay::redis;

Coroutine demo(IOScheduler* scheduler)
{
    auto client = RedisClientBuilder().scheduler(scheduler).build();
    RedisCommandBuilder builder;

    auto connect_result = co_await client.connect("127.0.0.1", 6379).timeout(std::chrono::seconds(5));
    if (!connect_result) {
        std::cerr << "connect failed: " << connect_result.error().message() << '\n';
        co_return;
    }

    auto set_result = co_await client.command(builder.set("demo:key", "hello")).timeout(std::chrono::seconds(5));
    if (!set_result || !set_result.value()) {
        std::cerr << "SET failed\n";
        (void)co_await client.close();
        co_return;
    }

    auto get_result = co_await client.command(builder.get("demo:key")).timeout(std::chrono::seconds(5));
    if (get_result && get_result.value()) {
        const auto& values = get_result.value().value();
        if (!values.empty() && values[0].isString()) {
            std::cout << values[0].toString() << '\n';
        }
    }

    (void)co_await client.close();
}
```

## 示例与测试入口

| 场景 | 源文件 | 目标 | 运行命令 | 环境 |
|---|---|---|---|---|
| 基础异步命令 | `examples/include/E1-async_basic_demo.cc` | `E1-async_basic_demo` | `./<build-dir>/examples/E1-async_basic_demo 127.0.0.1 6379` | 本地 Redis |
| 批量发送 | `examples/include/E2-pipeline_demo.cc` | `E2-pipeline_demo` | `./<build-dir>/examples/E2-pipeline_demo 127.0.0.1 6379 demo:pipeline: 20` | 本地 Redis |
| Pub/Sub 与拓扑 API 形状 | `examples/include/E3-topology_pubsub_demo.cc` | `E3-topology_pubsub_demo` | `./<build-dir>/examples/E3-topology_pubsub_demo 127.0.0.1 6379` | 单节点 Redis；非真实 failover |
| `rediss://` TLS smoke | `test/T17-rediss_client_tls.cc` | `T17-rediss_client_tls` | `GALAY_REDIS_TLS_URL=rediss://... ./<build-dir>/test/T17-rediss_client_tls` | TLS Redis，可选 `GALAY_REDIS_TLS_CA` / `GALAY_REDIS_TLS_VERIFY_PEER` / `GALAY_REDIS_TLS_SERVER_NAME` |
| TLS pool + topology smoke | `test/T19-rediss_pool_and_topology.cc` | `T19-rediss_pool_and_topology` | `GALAY_REDIS_TLS_URL=rediss://... ./<build-dir>/test/T19-rediss_pool_and_topology` | TLS Redis，可选 Sentinel / Cluster 环境变量 |
| timeout 行为 | `test/T5-redis_client_timeout.cc` | `T5-redis_client_timeout` | `./<build-dir>/test/T5-redis_client_timeout` | 本地 Redis |
| raw command API | `test/T10-redis_raw_command_api.cc` | `T10-redis_raw_command_api` | `./<build-dir>/test/T10-redis_raw_command_api` | 无 |
| awaitable / pool surface 回归 | `test/T15-awaitable_surface.cc` | `T15-awaitable_surface` | `./<build-dir>/test/T15-awaitable_surface` | 无 |
| topology + pubsub | `test/T11-topology_and_pubsub.cc` | `T11-topology_and_pubsub` | `./<build-dir>/test/T11-topology_and_pubsub` | 本地 Redis |
| real cluster + sentinel | `test/T13-integration_cluster_sentinel.cc` | `T13-integration_cluster_sentinel` | `test/integration/run_cluster_sentinel_integration.sh --build-dir <build-dir>` | Docker、`redis-cli` |

更多映射见 [docs/04-示例代码.md](docs/04-示例代码.md)。

模块工具链可用时，`examples/import/` 里还会生成与 `E1`~`E3` 对应的 `*-import` 目标，用来验证 `import galay.redis;` 的消费路径。

## Benchmark 现状

仓库中的真实 benchmark 目标名是：

- `B1-redis_client_bench`
- `B2-connection_pool_bench`

旧文档中的 `B1-RedisClientBench` / `B2-ConnectionPoolBench` 已废弃。当前 README 不再内嵌历史 QPS 数字；如果你需要复现实验命令、输出字段和注意事项，请直接看 [docs/05-性能测试.md](docs/05-性能测试.md)。

`B1` 当前的 plain 模式口径还需要特别注意：

- `normal` 复用预编码 RESP 字节，并通过 `RedisClient::commandBorrowed(...)` 发送
- `pipeline` 复用 `RedisCommandBuilder::encoded()`，并通过 `RedisClient::batchBorrowed(...)` 发送
- `normal-batch` 仍走常规 `RedisClient::batch(...)`

因此 plain `normal` / `pipeline` 的数字表示“当前内部 borrowed fast path 的吞吐”，不是泛化到所有 owning API 调用路径的基准值。

## TLS 快速入口

单节点 TLS：

```cpp
RedissClientConfig tls_config;
tls_config.ca_path = "/path/to/ca.pem";
tls_config.verify_peer = true;
tls_config.server_name = "redis.example.com";

auto client = RedissClientBuilder()
    .scheduler(scheduler)
    .tlsConfig(tls_config)
    .build();

auto connect_result = co_await client.connect("rediss://redis.example.com:6380/0").timeout(std::chrono::seconds(5));
auto ping_result = co_await client.command(RedisCommandBuilder().ping()).timeout(std::chrono::seconds(5));
```

拓扑与连接池的 TLS 版本和 TCP 版本保持同构：

- `RedissConnectionPool`
- `RedissMasterSlaveClientBuilder`
- `RedissClusterClientBuilder`

## 已知限制

- 面向业务代码的主文档流仍以 `RedisCommandBuilder` + `command` / `batch` / `execute` 为主；`commandBorrowed` / `batchBorrowed` 只在 API 参考与 benchmark 口径里单独说明，按内部 trusted fast path 对待
- `sync/RedisSession.*` 仍保留在源码树中做遗留同步路径维护，但当前安装规则与模块合同都已将它排除在公开消费面之外
- `RedisClient` 支持移动，但头文件明确要求不要在 awaitable 进行中移动对象
- `RedisConnectOptions::version` 在 async 连接路径里目前被用作 IPv4/IPv6 选择提示，`6` 表示 IPv6；它不是本文档里的 RESP3 开关
- `E3-topology_pubsub_demo` 主要演示 API 形状；真实 MOVED/ASK 与 Sentinel failover 请看 `T13-integration_cluster_sentinel`
- 模块导入构建存在额外工具链限制，不是所有 Clang/GCC 组合都可用

## 仓库内相关索引

- [docs/README.md](docs/README.md)

## 许可证

与 `galay` 项目保持一致。
