# RedisClient - 支持超时的异步Redis客户端

## 概述

`RedisClient` 是一个基于 C++20 协程的异步 Redis 客户端，参考 `HttpClientAwaitable` 的设计模式实现，提供了完整的超时支持、错误处理和资源管理功能。

## 主要特性

### ✨ 核心功能

1. **超时支持** - 所有操作都支持 `.timeout()` 方法设置超时时间
2. **完整的错误处理** - 区分超时、连接关闭、网络错误等不同错误类型
3. **Pipeline批处理** - 支持批量命令执行，提高性能
4. **资源自动管理** - 自动清理资源，防止内存泄漏
5. **状态机设计** - 高效的状态机实现，无额外协程开销

### 🎯 设计模式

`RedisClientAwaitable` 遵循与 `HttpClientAwaitable` 相同的设计模式：

```cpp
class RedisClientAwaitable : public galay::kernel::TimeoutSupport<RedisClientAwaitable>
{
public:
    // 标准 awaitable 接口
    bool await_ready() const noexcept;
    bool await_suspend(std::coroutine_handle<> handle);
    std::expected<std::optional<std::vector<RedisValue>>, RedisError> await_resume();

    // 资源管理
    void reset();
    bool isInvalid() const;

public:
    // TimeoutSupport 需要访问此成员
    std::expected<std::optional<std::vector<RedisValue>>, galay::kernel::IOError> m_result;
};
```

## 快速开始

### 基本使用

```cpp
#include "galay-redis/async/RedisClient.h"
#include <galay-kernel/kernel/Runtime.h>

using namespace galay::redis;
using namespace galay::kernel;

Coroutine testRedisClient(IOScheduler* scheduler)
{
    // 创建客户端
    RedisClient client(scheduler);

    // 连接到Redis服务器
    auto connect_result = co_await client.connect("127.0.0.1", 6379);
    if (!connect_result) {
        std::cerr << "Failed to connect: " << connect_result.error().message() << std::endl;
        co_return;
    }

    // 执行SET命令
    auto set_result = co_await client.set("mykey", "myvalue");
    if (set_result && set_result.value()) {
        std::cout << "SET succeeded" << std::endl;
    }

    // 执行GET命令
    auto get_result = co_await client.get("mykey");
    if (get_result && get_result.value()) {
        auto& values = get_result.value().value();
        if (!values.empty() && values[0].isString()) {
            std::cout << "GET result: " << values[0].toString() << std::endl;
        }
    }

    // 关闭连接
    co_await client.close();
}

int main()
{
    Runtime runtime;
    runtime.start();

    auto* scheduler = runtime.getNextIOScheduler();
    scheduler->spawn(testRedisClient(scheduler));

    std::this_thread::sleep_for(std::chrono::seconds(5));
    runtime.stop();

    return 0;
}
```

### 使用超时功能

```cpp
Coroutine testWithTimeout(IOScheduler* scheduler)
{
    RedisClient client(scheduler);

    co_await client.connect("127.0.0.1", 6379);

    // 设置5秒超时
    auto result = co_await client.get("mykey").timeout(std::chrono::seconds(5));

    if (!result) {
        if (result.error().type() == REDIS_ERROR_TYPE_TIMEOUT_ERROR) {
            std::cout << "Operation timed out!" << std::endl;
        } else {
            std::cout << "Error: " << result.error().message() << std::endl;
        }
        co_return;
    }

    // 处理结果
    if (result.value()) {
        auto& values = result.value().value();
        // 使用values...
    }
}
```

### Pipeline批处理

```cpp
Coroutine testPipeline(IOScheduler* scheduler)
{
    RedisClient client(scheduler);

    co_await client.connect("127.0.0.1", 6379);

    // 构建批量命令
    std::vector<std::vector<std::string>> commands = {
        {"SET", "key1", "value1"},
        {"SET", "key2", "value2"},
        {"GET", "key1"},
        {"GET", "key2"},
        {"INCR", "counter"}
    };

    // 执行Pipeline
    auto result = co_await client.pipeline(commands);

    if (result && result.value()) {
        auto& values = result.value().value();
        std::cout << "Pipeline completed, received " << values.size() << " responses" << std::endl;

        for (size_t i = 0; i < values.size(); ++i) {
            if (values[i].isString()) {
                std::cout << "Response " << i << ": " << values[i].toString() << std::endl;
            } else if (values[i].isInteger()) {
                std::cout << "Response " << i << ": " << values[i].toInteger() << std::endl;
            }
        }
    }
}
```

## API 参考

### 连接管理

```cpp
// 连接到Redis服务器
RedisConnectAwaitable& connect(const std::string& ip, int32_t port,
                               const std::string& username = "",
                               const std::string& password = "");

// 使用URL连接
RedisConnectAwaitable& connect(const std::string& url);
// 示例: "redis://:password@127.0.0.1:6379/0"

// 关闭连接
CloseAwaitable close();
```

### String操作

```cpp
RedisClientAwaitable& get(const std::string& key);
RedisClientAwaitable& set(const std::string& key, const std::string& value);
RedisClientAwaitable& setex(const std::string& key, int64_t seconds, const std::string& value);
RedisClientAwaitable& del(const std::string& key);
RedisClientAwaitable& exists(const std::string& key);
RedisClientAwaitable& incr(const std::string& key);
RedisClientAwaitable& decr(const std::string& key);
```

### Hash操作

```cpp
RedisClientAwaitable& hget(const std::string& key, const std::string& field);
RedisClientAwaitable& hset(const std::string& key, const std::string& field, const std::string& value);
RedisClientAwaitable& hdel(const std::string& key, const std::string& field);
RedisClientAwaitable& hgetAll(const std::string& key);
```

### List操作

```cpp
RedisClientAwaitable& lpush(const std::string& key, const std::string& value);
RedisClientAwaitable& rpush(const std::string& key, const std::string& value);
RedisClientAwaitable& lpop(const std::string& key);
RedisClientAwaitable& rpop(const std::string& key);
RedisClientAwaitable& llen(const std::string& key);
RedisClientAwaitable& lrange(const std::string& key, int64_t start, int64_t stop);
```

### Set操作

```cpp
RedisClientAwaitable& sadd(const std::string& key, const std::string& member);
RedisClientAwaitable& srem(const std::string& key, const std::string& member);
RedisClientAwaitable& smembers(const std::string& key);
RedisClientAwaitable& scard(const std::string& key);
```

### Sorted Set操作

```cpp
RedisClientAwaitable& zadd(const std::string& key, double score, const std::string& member);
RedisClientAwaitable& zrem(const std::string& key, const std::string& member);
RedisClientAwaitable& zrange(const std::string& key, int64_t start, int64_t stop);
RedisClientAwaitable& zscore(const std::string& key, const std::string& member);
```

### Pipeline批处理

```cpp
RedisPipelineAwaitable& pipeline(const std::vector<std::vector<std::string>>& commands);
```

## 错误处理

### 错误类型

```cpp
enum RedisErrorType
{
    REDIS_ERROR_TYPE_SUCCESS,                   // 成功
    REDIS_ERROR_TYPE_TIMEOUT_ERROR,             // 超时错误
    REDIS_ERROR_TYPE_CONNECTION_CLOSED,         // 连接已关闭
    REDIS_ERROR_TYPE_SEND_ERROR,                // 发送数据错误
    REDIS_ERROR_TYPE_RECV_ERROR,                // 接收数据错误
    REDIS_ERROR_TYPE_PARSE_ERROR,               // 协议解析错误
    REDIS_ERROR_TYPE_NETWORK_ERROR,             // 网络错误
    REDIS_ERROR_TYPE_INTERNAL_ERROR,            // 内部错误
    // ... 其他错误类型
};
```

### 错误处理示例

```cpp
auto result = co_await client.get("key").timeout(std::chrono::seconds(5));

if (!result) {
    auto& error = result.error();

    switch (error.type()) {
        case REDIS_ERROR_TYPE_TIMEOUT_ERROR:
            std::cout << "Operation timed out" << std::endl;
            break;
        case REDIS_ERROR_TYPE_CONNECTION_CLOSED:
            std::cout << "Connection closed" << std::endl;
            break;
        case REDIS_ERROR_TYPE_NETWORK_ERROR:
            std::cout << "Network error: " << error.message() << std::endl;
            break;
        default:
            std::cout << "Error: " << error.message() << std::endl;
            break;
    }
    co_return;
}

// 处理成功结果
if (result.value()) {
    auto& values = result.value().value();
    // 使用values...
}
```

## 返回值说明

所有命令操作返回 `std::expected<std::optional<std::vector<RedisValue>>, RedisError>`：

- **成功且有数据**: `result.value()` 包含 `std::vector<RedisValue>`
- **成功但需要继续**: `result.value()` 为 `std::nullopt`（内部状态，用户通常不会遇到）
- **失败**: `result.error()` 包含 `RedisError`

### RedisValue 类型

```cpp
class RedisValue
{
public:
    // 类型检查
    bool isNull();
    bool isString();
    bool isInteger();
    bool isArray();
    bool isError();

    // 值获取
    std::string toString();
    int64_t toInteger();
    std::vector<RedisValue> toArray();
    std::string toError();

    // RESP3 支持
    bool isDouble();
    double toDouble();
    bool isBool();
    bool toBool();
    bool isMap();
    std::map<std::string, RedisValue> toMap();
};
```

## 性能测试

### 运行基准测试

```bash
# 基本测试：10个客户端，每个执行100次操作
./test_redis_client_benchmark

# 自定义参数：客户端数量 操作次数
./test_redis_client_benchmark 50 200

# Pipeline模式：客户端数量 操作次数 模式 批大小
./test_redis_client_benchmark 10 1000 pipeline 100
```

### 性能指标

典型性能指标（取决于硬件和网络）：

- **普通模式**: 10,000-50,000 ops/sec
- **Pipeline模式**: 50,000-200,000 ops/sec
- **超时开销**: < 1% 性能影响

## 并发使用

### 多客户端并发

```cpp
Coroutine worker(IOScheduler* scheduler, int worker_id)
{
    RedisClient client(scheduler);
    co_await client.connect("127.0.0.1", 6379);

    for (int i = 0; i < 100; ++i) {
        std::string key = "worker_" + std::to_string(worker_id) + "_" + std::to_string(i);
        co_await client.set(key, "value");
    }

    co_await client.close();
}

int main()
{
    Runtime runtime;
    runtime.start();

    auto* scheduler = runtime.getNextIOScheduler();

    // 启动多个并发客户端
    for (int i = 0; i < 10; ++i) {
        scheduler->spawn(worker(scheduler, i));
    }

    std::this_thread::sleep_for(std::chrono::seconds(10));
    runtime.stop();

    return 0;
}
```

## 最佳实践

### 1. 超时设置

```cpp
// 短操作使用较短超时
auto result = co_await client.get("key").timeout(std::chrono::seconds(1));

// 长操作使用较长超时
auto result = co_await client.pipeline(large_commands).timeout(std::chrono::seconds(30));
```

### 2. 错误重试

```cpp
int max_retries = 3;
for (int retry = 0; retry < max_retries; ++retry) {
    auto result = co_await client.get("key").timeout(std::chrono::seconds(5));

    if (result && result.value()) {
        // 成功
        break;
    }

    if (!result && result.error().type() == REDIS_ERROR_TYPE_TIMEOUT_ERROR) {
        std::cout << "Retry " << (retry + 1) << " after timeout" << std::endl;
        continue;
    }

    // 其他错误，不重试
    break;
}
```

### 3. Pipeline优化

```cpp
// 批量操作使用Pipeline
std::vector<std::vector<std::string>> commands;
for (int i = 0; i < 1000; ++i) {
    commands.push_back({"SET", "key" + std::to_string(i), "value"});
}

// 分批执行，避免单次批量过大
const int batch_size = 100;
for (size_t i = 0; i < commands.size(); i += batch_size) {
    auto end = std::min(i + batch_size, commands.size());
    std::vector<std::vector<std::string>> batch(
        commands.begin() + i,
        commands.begin() + end
    );

    auto result = co_await client.pipeline(batch);
    // 处理结果...
}
```

### 4. 资源管理

```cpp
// 使用RAII模式确保连接关闭
class RedisConnection
{
public:
    RedisConnection(IOScheduler* scheduler) : m_client(scheduler) {}

    Coroutine connect(const std::string& host, int port) {
        auto result = co_await m_client.connect(host, port);
        m_connected = result.has_value();
        co_return result;
    }

    ~RedisConnection() {
        if (m_connected) {
            // 注意：析构函数中不能使用co_await
            // 需要在外部显式调用close()
        }
    }

    RedisClient& client() { return m_client; }

private:
    RedisClient m_client;
    bool m_connected = false;
};
```

## 与 AsyncRedisSession 的对比

| 特性 | RedisClient | AsyncRedisSession |
|------|-------------|-------------------|
| 超时支持 | ✅ 完整支持 | ❌ 不支持 |
| TimeoutSupport | ✅ 继承 | ❌ 无 |
| 错误处理 | ✅ 完善 | ⚠️ 基础 |
| 资源管理 | ✅ reset()方法 | ⚠️ 手动 |
| Pipeline | ✅ 支持 | ✅ 支持 |
| 设计模式 | HttpClientAwaitable | 自定义 |
| 推荐使用 | ✅ 新项目 | ⚠️ 兼容旧代码 |

## 迁移指南

### 从 AsyncRedisSession 迁移到 RedisClient

```cpp
// 旧代码 (AsyncRedisSession)
AsyncRedisSession session(scheduler);
auto result = co_await session.get("key");

// 新代码 (RedisClient)
RedisClient client(scheduler);
auto result = co_await client.get("key").timeout(std::chrono::seconds(5));
```

主要变化：
1. 类名从 `AsyncRedisSession` 改为 `RedisClient`
2. 可以使用 `.timeout()` 方法
3. 错误处理更完善
4. 返回类型相同，无需修改结果处理代码

## 故障排查

### 常见问题

1. **连接失败**
   ```cpp
   // 检查Redis服务器是否运行
   // 检查IP和端口是否正确
   // 检查防火墙设置
   ```

2. **超时错误**
   ```cpp
   // 增加超时时间
   auto result = co_await client.get("key").timeout(std::chrono::seconds(10));

   // 检查网络延迟
   // 检查Redis服务器负载
   ```

3. **解析错误**
   ```cpp
   // 检查Redis协议版本
   // 检查返回数据格式
   ```

## 示例代码

完整示例代码位于：
- `test/test_redis_client_timeout.cc` - 超时功能演示
- `test/test_redis_client_benchmark.cc` - 性能测试
- `test/test_async.cc` - 基本功能测试

## 许可证

与 galay-redis 项目相同的许可证。

## 贡献

欢迎提交 Issue 和 Pull Request！

## 更新日志

### v1.0.0 (2026-01-19)
- ✨ 实现 RedisClientAwaitable，参考 HttpClientAwaitable 设计
- ✨ 添加完整的超时支持
- ✨ 改进错误处理和资源管理
- ✨ 添加性能测试工具
- 📝 完善文档和示例代码
