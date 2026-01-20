# RedisClient 使用示例

本文档提供了 RedisClient 的实用示例代码。

## 目录

1. [基础示例](#基础示例)
2. [超时控制](#超时控制)
3. [错误处理](#错误处理)
4. [Pipeline批处理](#pipeline批处理)
5. [并发操作](#并发操作)
6. [实战场景](#实战场景)

## 基础示例

### 简单的GET/SET操作

```cpp
#include "galay-redis/async/RedisClient.h"
#include <galay-kernel/kernel/Runtime.h>

using namespace galay::redis;
using namespace galay::kernel;

Coroutine simpleExample(IOScheduler* scheduler)
{
    RedisClient client(scheduler);

    // 连接
    auto conn = co_await client.connect("127.0.0.1", 6379);
    if (!conn) {
        std::cerr << "Connect failed: " << conn.error().message() << std::endl;
        co_return;
    }

    // SET
    auto set_result = co_await client.set("name", "Alice");
    if (set_result && set_result.value()) {
        std::cout << "SET succeeded" << std::endl;
    }

    // GET
    auto get_result = co_await client.get("name");
    if (get_result && get_result.value()) {
        auto& values = get_result.value().value();
        if (!values.empty() && values[0].isString()) {
            std::cout << "Name: " << values[0].toString() << std::endl;
        }
    }

    co_await client.close();
}
```

## 超时控制

### 设置不同的超时时间

```cpp
Coroutine timeoutExample(IOScheduler* scheduler)
{
    RedisClient client(scheduler);
    co_await client.connect("127.0.0.1", 6379);

    // 短超时（1秒）
    auto result1 = co_await client.get("key1").timeout(std::chrono::seconds(1));

    // 中等超时（5秒）
    auto result2 = co_await client.get("key2").timeout(std::chrono::seconds(5));

    // 长超时（30秒）
    auto result3 = co_await client.pipeline(commands).timeout(std::chrono::seconds(30));

    // 不设置超时（无限等待）
    auto result4 = co_await client.get("key3");

    co_await client.close();
}
```

### 超时重试

```cpp
Coroutine retryOnTimeout(IOScheduler* scheduler)
{
    RedisClient client(scheduler);
    co_await client.connect("127.0.0.1", 6379);

    const int max_retries = 3;
    std::optional<std::vector<RedisValue>> result;

    for (int retry = 0; retry < max_retries; ++retry) {
        auto res = co_await client.get("important_key").timeout(std::chrono::seconds(5));

        if (res && res.value()) {
            result = std::move(res.value().value());
            break;
        }

        if (!res && res.error().type() == REDIS_ERROR_TYPE_TIMEOUT_ERROR) {
            std::cout << "Timeout, retry " << (retry + 1) << "/" << max_retries << std::endl;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        // 其他错误，不重试
        std::cerr << "Error: " << res.error().message() << std::endl;
        break;
    }

    if (result) {
        std::cout << "Success after retries" << std::endl;
    }

    co_await client.close();
}
```

## 错误处理

### 完整的错误处理

```cpp
Coroutine errorHandlingExample(IOScheduler* scheduler)
{
    RedisClient client(scheduler);

    auto conn = co_await client.connect("127.0.0.1", 6379);
    if (!conn) {
        std::cerr << "Connection failed" << std::endl;
        co_return;
    }

    auto result = co_await client.get("key").timeout(std::chrono::seconds(5));

    if (!result) {
        auto& error = result.error();

        switch (error.type()) {
            case REDIS_ERROR_TYPE_TIMEOUT_ERROR:
                std::cerr << "Operation timed out after 5 seconds" << std::endl;
                // 可以选择重试
                break;

            case REDIS_ERROR_TYPE_CONNECTION_CLOSED:
                std::cerr << "Connection was closed" << std::endl;
                // 需要重新连接
                break;

            case REDIS_ERROR_TYPE_NETWORK_ERROR:
                std::cerr << "Network error: " << error.message() << std::endl;
                // 检查网络状态
                break;

            case REDIS_ERROR_TYPE_PARSE_ERROR:
                std::cerr << "Protocol parse error" << std::endl;
                // 可能是Redis版本不兼容
                break;

            default:
                std::cerr << "Unknown error: " << error.message() << std::endl;
                break;
        }

        co_await client.close();
        co_return;
    }

    // 处理成功结果
    if (result.value()) {
        auto& values = result.value().value();
        std::cout << "Got " << values.size() << " values" << std::endl;
    }

    co_await client.close();
}
```

## Pipeline批处理

### 基本Pipeline

```cpp
Coroutine pipelineExample(IOScheduler* scheduler)
{
    RedisClient client(scheduler);
    co_await client.connect("127.0.0.1", 6379);

    // 构建批量命令
    std::vector<std::vector<std::string>> commands = {
        {"SET", "user:1:name", "Alice"},
        {"SET", "user:1:age", "25"},
        {"SET", "user:2:name", "Bob"},
        {"SET", "user:2:age", "30"},
        {"GET", "user:1:name"},
        {"GET", "user:2:name"}
    };

    // 执行Pipeline
    auto result = co_await client.pipeline(commands);

    if (result && result.value()) {
        auto& values = result.value().value();
        std::cout << "Pipeline completed with " << values.size() << " responses" << std::endl;

        // 处理每个响应
        for (size_t i = 0; i < values.size(); ++i) {
            if (values[i].isString()) {
                std::cout << "Response " << i << ": " << values[i].toString() << std::endl;
            } else if (values[i].isInteger()) {
                std::cout << "Response " << i << ": " << values[i].toInteger() << std::endl;
            }
        }
    }

    co_await client.close();
}
```

### 分批Pipeline

```cpp
Coroutine batchPipelineExample(IOScheduler* scheduler)
{
    RedisClient client(scheduler);
    co_await client.connect("127.0.0.1", 6379);

    // 大量数据
    const int total_items = 10000;
    const int batch_size = 100;

    for (int start = 0; start < total_items; start += batch_size) {
        std::vector<std::vector<std::string>> batch;

        // 构建一批命令
        for (int i = start; i < start + batch_size && i < total_items; ++i) {
            batch.push_back({
                "SET",
                "item:" + std::to_string(i),
                "value_" + std::to_string(i)
            });
        }

        // 执行这一批
        auto result = co_await client.pipeline(batch).timeout(std::chrono::seconds(10));

        if (!result || !result.value()) {
            std::cerr << "Batch " << (start / batch_size) << " failed" << std::endl;
            break;
        }

        std::cout << "Batch " << (start / batch_size) << " completed" << std::endl;
    }

    co_await client.close();
}
```

## 并发操作

### 多客户端并发

```cpp
Coroutine worker(IOScheduler* scheduler, int worker_id, int tasks)
{
    RedisClient client(scheduler);

    auto conn = co_await client.connect("127.0.0.1", 6379);
    if (!conn) {
        std::cerr << "Worker " << worker_id << " failed to connect" << std::endl;
        co_return;
    }

    std::cout << "Worker " << worker_id << " started" << std::endl;

    for (int i = 0; i < tasks; ++i) {
        std::string key = "worker:" + std::to_string(worker_id) + ":task:" + std::to_string(i);
        std::string value = "result_" + std::to_string(i);

        auto result = co_await client.set(key, value).timeout(std::chrono::seconds(5));

        if (!result || !result.value()) {
            std::cerr << "Worker " << worker_id << " task " << i << " failed" << std::endl;
        }
    }

    std::cout << "Worker " << worker_id << " completed" << std::endl;
    co_await client.close();
}

int main()
{
    Runtime runtime;
    runtime.start();

    auto* scheduler = runtime.getNextIOScheduler();

    // 启动10个并发worker
    const int num_workers = 10;
    const int tasks_per_worker = 100;

    for (int i = 0; i < num_workers; ++i) {
        scheduler->spawn(worker(scheduler, i, tasks_per_worker));
    }

    std::this_thread::sleep_for(std::chrono::seconds(30));
    runtime.stop();

    return 0;
}
```

## 实战场景

### 场景1: 用户会话管理

```cpp
class SessionManager
{
public:
    SessionManager(IOScheduler* scheduler) : m_client(scheduler) {}

    Coroutine init(const std::string& host, int port)
    {
        auto result = co_await m_client.connect(host, port);
        co_return result;
    }

    // 创建会话
    Coroutine createSession(const std::string& user_id, const std::string& token)
    {
        std::string key = "session:" + user_id;
        auto result = co_await m_client.setex(key, 3600, token).timeout(std::chrono::seconds(5));
        co_return result;
    }

    // 验证会话
    Coroutine validateSession(const std::string& user_id)
    {
        std::string key = "session:" + user_id;
        auto result = co_await m_client.get(key).timeout(std::chrono::seconds(2));

        if (result && result.value()) {
            auto& values = result.value().value();
            if (!values.empty() && values[0].isString()) {
                co_return std::optional<std::string>(values[0].toString());
            }
        }

        co_return std::optional<std::string>();
    }

    // 删除会话
    Coroutine deleteSession(const std::string& user_id)
    {
        std::string key = "session:" + user_id;
        auto result = co_await m_client.del(key).timeout(std::chrono::seconds(2));
        co_return result;
    }

private:
    RedisClient m_client;
};
```

### 场景2: 计数器服务

```cpp
class CounterService
{
public:
    CounterService(IOScheduler* scheduler) : m_client(scheduler) {}

    Coroutine init(const std::string& host, int port)
    {
        co_return co_await m_client.connect(host, port);
    }

    // 增加计数
    Coroutine increment(const std::string& counter_name)
    {
        std::string key = "counter:" + counter_name;
        auto result = co_await m_client.incr(key).timeout(std::chrono::seconds(2));

        if (result && result.value()) {
            auto& values = result.value().value();
            if (!values.empty() && values[0].isInteger()) {
                co_return std::optional<int64_t>(values[0].toInteger());
            }
        }

        co_return std::optional<int64_t>();
    }

    // 获取计数
    Coroutine getCount(const std::string& counter_name)
    {
        std::string key = "counter:" + counter_name;
        auto result = co_await m_client.get(key).timeout(std::chrono::seconds(2));

        if (result && result.value()) {
            auto& values = result.value().value();
            if (!values.empty() && values[0].isString()) {
                try {
                    co_return std::optional<int64_t>(std::stoll(values[0].toString()));
                } catch (...) {
                    co_return std::optional<int64_t>();
                }
            }
        }

        co_return std::optional<int64_t>();
    }

    // 批量获取多个计数器
    Coroutine getMultipleCounters(const std::vector<std::string>& counter_names)
    {
        std::vector<std::vector<std::string>> commands;

        for (const auto& name : counter_names) {
            commands.push_back({"GET", "counter:" + name});
        }

        auto result = co_await m_client.pipeline(commands).timeout(std::chrono::seconds(5));

        std::map<std::string, int64_t> counters;

        if (result && result.value()) {
            auto& values = result.value().value();

            for (size_t i = 0; i < values.size() && i < counter_names.size(); ++i) {
                if (values[i].isString()) {
                    try {
                        counters[counter_names[i]] = std::stoll(values[i].toString());
                    } catch (...) {
                        counters[counter_names[i]] = 0;
                    }
                }
            }
        }

        co_return counters;
    }

private:
    RedisClient m_client;
};
```

### 场景3: 缓存服务

```cpp
template<typename T>
class CacheService
{
public:
    CacheService(IOScheduler* scheduler) : m_client(scheduler) {}

    Coroutine init(const std::string& host, int port)
    {
        co_return co_await m_client.connect(host, port);
    }

    // 设置缓存（带过期时间）
    Coroutine set(const std::string& key, const std::string& value, int ttl_seconds)
    {
        auto result = co_await m_client.setex(key, ttl_seconds, value)
                                       .timeout(std::chrono::seconds(5));
        co_return result;
    }

    // 获取缓存
    Coroutine get(const std::string& key)
    {
        auto result = co_await m_client.get(key).timeout(std::chrono::seconds(2));

        if (result && result.value()) {
            auto& values = result.value().value();
            if (!values.empty() && values[0].isString()) {
                co_return std::optional<std::string>(values[0].toString());
            }
        }

        co_return std::optional<std::string>();
    }

    // 批量获取缓存
    Coroutine multiGet(const std::vector<std::string>& keys)
    {
        std::vector<std::vector<std::string>> commands;

        for (const auto& key : keys) {
            commands.push_back({"GET", key});
        }

        auto result = co_await m_client.pipeline(commands).timeout(std::chrono::seconds(5));

        std::map<std::string, std::string> cache_data;

        if (result && result.value()) {
            auto& values = result.value().value();

            for (size_t i = 0; i < values.size() && i < keys.size(); ++i) {
                if (values[i].isString()) {
                    cache_data[keys[i]] = values[i].toString();
                }
            }
        }

        co_return cache_data;
    }

    // 删除缓存
    Coroutine remove(const std::string& key)
    {
        co_return co_await m_client.del(key).timeout(std::chrono::seconds(2));
    }

private:
    RedisClient m_client;
};
```

### 场景4: 排行榜服务

```cpp
class LeaderboardService
{
public:
    LeaderboardService(IOScheduler* scheduler) : m_client(scheduler) {}

    Coroutine init(const std::string& host, int port)
    {
        co_return co_await m_client.connect(host, port);
    }

    // 更新分数
    Coroutine updateScore(const std::string& leaderboard, const std::string& player, double score)
    {
        auto result = co_await m_client.zadd(leaderboard, score, player)
                                       .timeout(std::chrono::seconds(2));
        co_return result;
    }

    // 获取排名
    Coroutine getTopPlayers(const std::string& leaderboard, int count)
    {
        // 注意：这里需要使用ZREVRANGE来获取从高到低的排名
        // 当前实现的zrange是从低到高，实际使用时需要调整
        auto result = co_await m_client.zrange(leaderboard, 0, count - 1)
                                       .timeout(std::chrono::seconds(5));

        std::vector<std::string> players;

        if (result && result.value()) {
            auto& values = result.value().value();

            for (const auto& value : values) {
                if (value.isString()) {
                    players.push_back(value.toString());
                }
            }
        }

        co_return players;
    }

    // 获取玩家分数
    Coroutine getPlayerScore(const std::string& leaderboard, const std::string& player)
    {
        auto result = co_await m_client.zscore(leaderboard, player)
                                       .timeout(std::chrono::seconds(2));

        if (result && result.value()) {
            auto& values = result.value().value();
            if (!values.empty() && values[0].isString()) {
                try {
                    co_return std::optional<double>(std::stod(values[0].toString()));
                } catch (...) {
                    co_return std::optional<double>();
                }
            }
        }

        co_return std::optional<double>();
    }

private:
    RedisClient m_client;
};
```

## 编译和运行

### 编译示例

```bash
cd build
cmake ..
make test_redis_client_timeout
make test_redis_client_benchmark
```

### 运行示例

```bash
# 基本功能测试
./test/test_redis_client_timeout

# 性能测试
./test/test_redis_client_benchmark 10 100

# Pipeline性能测试
./test/test_redis_client_benchmark 10 1000 pipeline 100
```

## 注意事项

1. **连接管理**: 确保在使用前调用 `connect()`
2. **超时设置**: 根据操作类型合理设置超时时间
3. **错误处理**: 始终检查返回值并处理错误
4. **资源清理**: 使用完毕后调用 `close()`
5. **并发控制**: 每个客户端实例不是线程安全的，多线程使用时需要为每个线程创建独立的客户端

## 更多资源

- [完整API文档](README_RedisClient.md)
- [性能对比](COMPARISON.md)
- [源代码](galay-redis/async/RedisClient.h)
