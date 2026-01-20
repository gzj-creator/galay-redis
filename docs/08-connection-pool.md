# Redis 连接池使用指南

## 概述

`RedisConnectionPool` 提供了一个高性能、线程安全的 Redis 连接池实现，支持连接复用、自动扩缩容、健康检查等功能。

## 核心特性

### 1. 连接管理
- ✅ 最小/最大连接数控制
- ✅ 连接复用
- ✅ 自动扩容（按需创建连接）
- ✅ 手动扩容/缩容
- ✅ RAII 风格的连接管理

### 2. 连接重试机制
- ✅ 自动重连（可配置）
- ✅ 重连次数限制
- ✅ 重连统计

### 3. 连接验证
- ✅ 获取时验证（可选）
- ✅ 归还时验证（可选）
- ✅ 验证失败统计

### 4. 性能监控
- ✅ 平均获取连接时间
- ✅ 最大获取连接时间
- ✅ 峰值活跃连接数
- ✅ 详细的操作统计

### 5. 维护功能
- ✅ 健康检查
- ✅ 空闲连接清理
- ✅ 不健康连接清理
- ✅ 连接池预热

## 快速开始

### 基本用法

```cpp
#include "galay-redis/async/RedisConnectionPool.h"

using namespace galay::redis;

Coroutine example(IOScheduler* scheduler)
{
    // 1. 创建连接池配置
    auto config = ConnectionPoolConfig::create("127.0.0.1", 6379, 2, 10);

    // 2. 创建连接池
    RedisConnectionPool pool(scheduler, config);

    // 3. 初始化连接池
    auto init_result = co_await pool.initialize();
    if (!init_result) {
        std::cerr << "Failed to initialize pool: "
                  << init_result.error().message() << std::endl;
        co_return;
    }

    // 4. 获取连接
    auto conn_result = co_await pool.acquire();
    if (!conn_result) {
        std::cerr << "Failed to acquire connection: "
                  << conn_result.error().message() << std::endl;
        co_return;
    }

    auto conn = conn_result.value();

    // 5. 使用连接
    auto result = co_await conn->get()->set("key", "value");
    if (result && result.value()) {
        std::cout << "SET succeeded" << std::endl;
    }

    // 6. 归还连接
    pool.release(conn);

    // 7. 关闭连接池
    pool.shutdown();
}
```

### RAII 风格用法

```cpp
Coroutine exampleRAII(IOScheduler* scheduler)
{
    RedisConnectionPool pool(scheduler, ConnectionPoolConfig::defaultConfig());

    auto init_result = co_await pool.initialize();
    if (!init_result) {
        co_return;
    }

    // 使用 ScopedConnection 自动管理连接生命周期
    {
        auto conn_result = co_await pool.acquire();
        if (conn_result) {
            ScopedConnection scoped(pool, conn_result.value());

            // 使用连接
            auto result = co_await scoped->set("key", "value");

            // 离开作用域时自动归还连接
        }
    }

    pool.shutdown();
}
```

## 配置选项

### ConnectionPoolConfig

```cpp
struct ConnectionPoolConfig
{
    // 连接参数
    std::string host = "127.0.0.1";
    int32_t port = 6379;
    std::string username = "";
    std::string password = "";
    int32_t db_index = 0;

    // 连接池大小
    size_t min_connections = 2;      // 最小连接数
    size_t max_connections = 10;     // 最大连接数
    size_t initial_connections = 2;  // 初始连接数

    // 超时配置
    std::chrono::milliseconds acquire_timeout = std::chrono::seconds(5);
    std::chrono::milliseconds idle_timeout = std::chrono::minutes(5);
    std::chrono::milliseconds connect_timeout = std::chrono::seconds(3);

    // 健康检查
    bool enable_health_check = true;
    std::chrono::milliseconds health_check_interval = std::chrono::seconds(30);

    // 重连配置
    bool enable_auto_reconnect = true;
    int max_reconnect_attempts = 3;

    // 连接验证配置
    bool enable_connection_validation = true;
    bool validate_on_acquire = false;  // 每次获取时验证（性能开销较大）
    bool validate_on_return = false;   // 归还时验证
};
```

### 创建自定义配置

```cpp
// 方式1：使用默认配置
auto config = ConnectionPoolConfig::defaultConfig();

// 方式2：使用便捷方法
auto config = ConnectionPoolConfig::create("127.0.0.1", 6379, 2, 10);

// 方式3：完全自定义
ConnectionPoolConfig config;
config.host = "127.0.0.1";
config.port = 6379;
config.min_connections = 5;
config.max_connections = 20;
config.initial_connections = 5;
config.acquire_timeout = std::chrono::seconds(10);
config.enable_auto_reconnect = true;
config.max_reconnect_attempts = 5;
```

## 高级功能

### 1. 连接池预热

在应用启动时预先创建连接，避免首次请求的延迟：

```cpp
RedisConnectionPool pool(scheduler, config);
auto init_result = co_await pool.initialize();

// 预热到最小连接数
pool.warmup();
```

### 2. 手动扩容/缩容

根据负载动态调整连接池大小：

```cpp
// 扩容：增加 5 个连接
size_t created = pool.expandPool(5);
std::cout << "Created " << created << " connections" << std::endl;

// 缩容：缩减到 10 个连接
size_t removed = pool.shrinkPool(10);
std::cout << "Removed " << removed << " connections" << std::endl;
```

### 3. 健康检查

定期检查连接健康状态，自动移除不健康的连接：

```cpp
// 手动触发健康检查
pool.triggerHealthCheck();

// 清理所有不健康的连接
size_t removed = pool.cleanupUnhealthyConnections();
std::cout << "Removed " << removed << " unhealthy connections" << std::endl;
```

### 4. 空闲连接清理

清理长时间未使用的连接，释放资源：

```cpp
// 手动触发空闲连接清理
pool.triggerIdleCleanup();
```

### 5. 统计信息

获取连接池的详细统计信息：

```cpp
auto stats = pool.getStats();

std::cout << "Total connections: " << stats.total_connections << std::endl;
std::cout << "Available: " << stats.available_connections << std::endl;
std::cout << "Active: " << stats.active_connections << std::endl;
std::cout << "Waiting requests: " << stats.waiting_requests << std::endl;

// 操作统计
std::cout << "Total acquired: " << stats.total_acquired << std::endl;
std::cout << "Total released: " << stats.total_released << std::endl;
std::cout << "Total created: " << stats.total_created << std::endl;
std::cout << "Total destroyed: " << stats.total_destroyed << std::endl;

// 性能指标
std::cout << "Avg acquire time: " << stats.avg_acquire_time_ms << " ms" << std::endl;
std::cout << "Max acquire time: " << stats.max_acquire_time_ms << " ms" << std::endl;
std::cout << "Peak active connections: " << stats.peak_active_connections << std::endl;

// 健康检查和重连统计
std::cout << "Health check failures: " << stats.health_check_failures << std::endl;
std::cout << "Reconnect attempts: " << stats.reconnect_attempts << std::endl;
std::cout << "Reconnect successes: " << stats.reconnect_successes << std::endl;
std::cout << "Validation failures: " << stats.validation_failures << std::endl;
```

## 最佳实践

### 1. 连接池大小设置

```cpp
// 根据应用负载设置合适的连接池大小
// 一般建议：
// - min_connections: 核心并发数（如 CPU 核心数）
// - max_connections: 峰值并发数的 1.5-2 倍
// - initial_connections: 等于 min_connections

auto config = ConnectionPoolConfig::create(
    "127.0.0.1", 6379,
    4,   // min: 4 个核心连接
    20   // max: 最多 20 个连接
);
```

### 2. 超时设置

```cpp
ConnectionPoolConfig config;
config.acquire_timeout = std::chrono::seconds(5);    // 获取连接超时
config.idle_timeout = std::chrono::minutes(5);       // 空闲连接超时
config.connect_timeout = std::chrono::seconds(3);    // 连接超时
```

### 3. 错误处理

```cpp
auto conn_result = co_await pool.acquire();
if (!conn_result) {
    auto& error = conn_result.error();

    switch (error.type()) {
        case RedisErrorType::REDIS_ERROR_TYPE_TIMEOUT_ERROR:
            // 处理超时错误
            std::cerr << "Acquire timeout" << std::endl;
            break;

        case RedisErrorType::REDIS_ERROR_TYPE_CONNECTION_ERROR:
            // 处理连接错误
            std::cerr << "Connection error: " << error.message() << std::endl;
            break;

        default:
            std::cerr << "Unknown error: " << error.message() << std::endl;
            break;
    }

    co_return;
}
```

### 4. 连接验证

```cpp
// 对于关键业务，启用连接验证
ConnectionPoolConfig config;
config.enable_connection_validation = true;
config.validate_on_acquire = true;  // 获取时验证（推荐）
config.validate_on_return = false;  // 归还时验证（可选）
```

### 5. 资源清理

```cpp
// 应用关闭时正确清理资源
void cleanup(RedisConnectionPool& pool)
{
    // 1. 停止接受新请求
    // 2. 等待所有活跃连接归还
    // 3. 关闭连接池
    pool.shutdown();
}
```

## 性能优化建议

### 1. 连接池预热

```cpp
// 在应用启动时预热连接池
auto init_result = co_await pool.initialize();
if (init_result) {
    pool.warmup();  // 创建到最小连接数
}
```

### 2. 避免频繁创建/销毁

```cpp
// 设置合理的 min_connections，避免连接频繁创建和销毁
config.min_connections = 预期的平均并发数;
config.max_connections = 预期的峰值并发数;
```

### 3. 禁用不必要的验证

```cpp
// 对于高性能场景，可以禁用获取时验证
config.validate_on_acquire = false;
config.validate_on_return = false;

// 通过定期健康检查来保证连接质量
config.enable_health_check = true;
```

### 4. 监控和调优

```cpp
// 定期检查统计信息，根据实际情况调整配置
auto stats = pool.getStats();

// 如果 waiting_requests 经常 > 0，考虑增加 max_connections
// 如果 peak_active_connections 远小于 max_connections，考虑减小 max_connections
// 如果 avg_acquire_time_ms 过高，考虑增加连接数或优化 Redis 性能
```

## 注意事项

1. **连接创建**：当前实现中 `getConnectionSync()` 方法创建的是未连接的客户端，实际连接会在第一次使用时建立。

2. **线程安全**：连接池本身是线程安全的，但单个连接不是线程安全的，不要在多个协程间共享同一个连接。

3. **资源清理**：使用 `ScopedConnection` 可以自动管理连接生命周期，避免忘记归还连接。

4. **健康检查**：健康检查和空闲连接清理需要手动触发，建议在应用中定期调用。

5. **性能监控**：定期检查统计信息，根据实际负载调整连接池配置。

## 完整示例

```cpp
#include "galay-redis/async/RedisConnectionPool.h"
#include <galay-kernel/kernel/Runtime.h>
#include <iostream>

using namespace galay::redis;
using namespace galay::kernel;

Coroutine workerTask(IOScheduler* scheduler, RedisConnectionPool& pool, int worker_id)
{
    for (int i = 0; i < 10; ++i) {
        // 获取连接
        auto conn_result = co_await pool.acquire();
        if (!conn_result) {
            std::cerr << "Worker " << worker_id << " failed to acquire connection" << std::endl;
            continue;
        }

        // 使用 RAII 风格管理连接
        ScopedConnection conn(pool, conn_result.value());

        // 执行 Redis 操作
        std::string key = "worker_" + std::to_string(worker_id) + "_" + std::to_string(i);
        auto result = co_await conn->set(key, "value");

        if (result && result.value()) {
            std::cout << "Worker " << worker_id << " SET " << key << " succeeded" << std::endl;
        }

        // 连接自动归还
    }
}

Coroutine mainTask(IOScheduler* scheduler)
{
    // 创建连接池
    auto config = ConnectionPoolConfig::create("127.0.0.1", 6379, 5, 20);
    config.enable_auto_reconnect = true;
    config.max_reconnect_attempts = 3;

    RedisConnectionPool pool(scheduler, config);

    // 初始化
    auto init_result = co_await pool.initialize();
    if (!init_result) {
        std::cerr << "Failed to initialize pool" << std::endl;
        co_return;
    }

    // 预热连接池
    pool.warmup();

    // 启动多个工作协程
    for (int i = 0; i < 5; ++i) {
        scheduler->spawn(workerTask(scheduler, pool, i));
    }

    // 等待一段时间
    // 注意：这里需要使用适当的等待机制

    // 打印统计信息
    auto stats = pool.getStats();
    std::cout << "\n=== Connection Pool Statistics ===" << std::endl;
    std::cout << "Total connections: " << stats.total_connections << std::endl;
    std::cout << "Available: " << stats.available_connections << std::endl;
    std::cout << "Active: " << stats.active_connections << std::endl;
    std::cout << "Total acquired: " << stats.total_acquired << std::endl;
    std::cout << "Avg acquire time: " << stats.avg_acquire_time_ms << " ms" << std::endl;

    // 清理
    pool.shutdown();
}

int main()
{
    Runtime runtime;
    runtime.start();

    auto* scheduler = runtime.getNextIOScheduler();
    scheduler->spawn(mainTask(scheduler));

    runtime.stop();
    return 0;
}
```

## 故障排查

### 问题1：获取连接超时

**原因**：
- 连接池已满，所有连接都在使用中
- max_connections 设置过小

**解决方案**：
```cpp
// 增加最大连接数
config.max_connections = 50;

// 或增加超时时间
config.acquire_timeout = std::chrono::seconds(10);
```

### 问题2：连接频繁创建和销毁

**原因**：
- min_connections 设置过小
- idle_timeout 设置过短

**解决方案**：
```cpp
// 增加最小连接数
config.min_connections = 10;

// 增加空闲超时时间
config.idle_timeout = std::chrono::minutes(10);
```

### 问题3：健康检查失败过多

**原因**：
- Redis 服务器不稳定
- 网络问题
- 连接超时设置过短

**解决方案**：
```cpp
// 启用自动重连
config.enable_auto_reconnect = true;
config.max_reconnect_attempts = 5;

// 定期清理不健康的连接
pool.cleanupUnhealthyConnections();
```

## 总结

Redis 连接池提供了完整的连接管理功能，包括：
- ✅ 自动连接管理和复用
- ✅ 灵活的配置选项
- ✅ 完善的错误处理
- ✅ 详细的性能监控
- ✅ 便捷的维护功能

通过合理配置和使用连接池，可以显著提升应用的性能和稳定性。
