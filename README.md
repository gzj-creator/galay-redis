# galay-redis

基于 C++20 协程的高性能异步 Redis 客户端库，属于 galay 生态。

## 特性

- 全异步协程 API，所有操作支持 `.timeout()` 超时控制
- Pipeline 批处理，百万级 QPS
- 连接池，自动扩缩容、健康检查
- 主从读写分离（支持 Sentinel 自动故障转移）
- Cluster 集群路由（MOVED/ASK 自动重定向）
- Pub/Sub 发布订阅
- 统一的 `std::expected` 错误处理

## 文档导航

建议按以下顺序阅读：

1. [快速开始](docs/01-快速开始.md) - 编译、基本用法、API 速览、错误处理
2. [使用示例](docs/02-使用示例.md) - 各类操作示例、连接池、主从、集群、实战场景
3. [模块介绍](docs/03-模块介绍.md) - 核心模块、连接池、拓扑客户端等模块详解
4. [运行原理](docs/04-运行原理.md) - 协程状态机、超时机制、RESP 编解码、路由原理
5. [性能分析](docs/05-性能分析.md) - 基准数据、瓶颈分析、优化建议
6. [API 文档](docs/06-API文档.md) - 完整的 API 参考文档
7. [架构设计](docs/07-架构设计.md) - 整体架构、模块职责、设计原理
8. [高级主题](docs/08-高级主题.md) - 性能优化、连接池、主从、集群、事务、Pub/Sub
9. [常见问题](docs/09-常见问题.md) - 编译、连接、查询、性能等常见问题解答

## 依赖

- C++20 编译器（GCC 11+ / Clang 14+）
- CMake 3.20+
- [galay-kernel](https://github.com/gzj-creator/galay-kernel) — 协程运行时
- [galay-utils](https://github.com/gzj-creator/galay-utils) — 工具库
- spdlog
- OpenSSL

## 依赖安装（macOS / Homebrew）

```bash
brew install cmake spdlog openssl
```

## 依赖安装（Ubuntu / Debian）

```bash
sudo apt-get update
sudo apt-get install -y cmake g++ libspdlog-dev libssl-dev
```

## 编译安装

```bash
git clone https://github.com/gzj-creator/galay-kernel.git
git clone https://github.com/gzj-creator/galay-utils.git
git clone https://github.com/gzj-creator/galay-redis.git
cd galay-redis
mkdir build && cd build
cmake ..
cmake --build . --parallel
```

在你的 CMakeLists.txt 中链接：

```cmake
find_package(galay-redis REQUIRED)
target_link_libraries(your_target PRIVATE galay-redis)
```

## 快速开始

```cpp
#include "galay-redis/async/RedisClient.h"
#include <galay-kernel/kernel/Runtime.h>

using namespace galay::redis;
using namespace galay::kernel;

Coroutine example(IOScheduler* scheduler)
{
    RedisClient client(scheduler);
    co_await client.connect("127.0.0.1", 6379);

    co_await client.set("key", "value").timeout(std::chrono::seconds(5));

    auto r = co_await client.get("key");
    if (r && r.value()) {
        auto& vals = r.value().value();
        if (!vals.empty() && vals[0].isString()) {
            std::cout << vals[0].toString() << std::endl;
        }
    }

    co_await client.close();
}

int main()
{
    Runtime runtime;
    runtime.start();
    auto* scheduler = runtime.getNextIOScheduler();
    scheduler->spawn(example(scheduler));
    std::this_thread::sleep_for(std::chrono::seconds(5));
    runtime.stop();
    return 0;
}
```

## 运行测试

```bash
# 功能测试
./build/test/test_redis_client_timeout

# 连接池/协议等其他测试
./build/test/test_async
```

## 运行示例（example/E*）

```bash
# E1：基础异步 SET/GET/DEL
./build/example/E1-AsyncBasicDemo 127.0.0.1 6379

# E2：Pipeline 示例
./build/example/E2-PipelineDemo 127.0.0.1 6379 demo:pipeline: 20

# E3：拓扑 + Pub/Sub 示例（单机也可演示）
./build/example/E3-TopologyPubSubDemo 127.0.0.1 6379
```

## 运行压测（benchmark/B*）

```bash
# 普通模式
./build/benchmark/B1-RedisClientBench -h 127.0.0.1 -p 6379 -c 10 -n 100 -m normal

# Pipeline 性能测试
./build/benchmark/B1-RedisClientBench -h 127.0.0.1 -p 6379 -c 20 -n 5000 -m pipeline -b 100 -q

# 连接池压测
./build/benchmark/B2-ConnectionPoolBench -h 127.0.0.1 -p 6379 -c 20 -n 300 -m 4 -x 20 -q
```

## 性能

| 场景 | QPS | 成功率 |
|------|-----|--------|
| 普通模式 10x100 | 26,666 | 100% |
| 普通模式 50x500 | 112,359 | 100% |
| Pipeline 20x5000 batch=100 | 1,098,901 | 100% |

## C++23 模块支持更新（2026-02）

本次模块接口已统一为：

- `module;`
- `#include "galay-redis/module/ModulePrelude.hpp"`
- `export module galay.redis;`
- `export { #include ... }`

对应文件：

- `galay-redis/module/galay.redis.cppm`
- `galay-redis/module/ModulePrelude.hpp`

推荐构建（Clang 20 + Ninja）：

```bash
cmake -S . -B build-mod -G Ninja \
  -DCMAKE_CXX_COMPILER=/opt/homebrew/opt/llvm@20/bin/clang++
cmake --build build-mod --target galay-redis-modules --parallel
```

## 文档

详细文档见 [docs/](docs/)：

| 文档 | 说明 |
|------|------|
| [01-快速开始](docs/01-快速开始.md) | 编译、基本用法、API 速览、错误处理 |
| [02-使用示例](docs/02-使用示例.md) | 各类操作示例、连接池、主从、集群、实战场景 |
| [03-模块介绍](docs/03-模块介绍.md) | 核心模块、连接池、拓扑客户端等模块详解 |
| [04-运行原理](docs/04-运行原理.md) | 协程状态机、超时机制、RESP 编解码、路由原理 |
| [05-性能分析](docs/05-性能分析.md) | 基准数据、瓶颈分析、优化建议 |
| [06-API文档](docs/06-API文档.md) | 完整的 API 参考文档 |
| [07-架构设计](docs/07-架构设计.md) | 整体架构、模块职责、设计原理 |
| [08-高级主题](docs/08-高级主题.md) | 性能优化、连接池、主从、集群、事务、Pub/Sub |
| [09-常见问题](docs/09-常见问题.md) | 编译、连接、查询、性能等常见问题解答 |

## 许可证

与 galay 项目相同的许可证。
