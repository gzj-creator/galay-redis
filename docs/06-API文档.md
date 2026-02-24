# API 文档

本文档详细介绍 galay-redis 的所有公开 API。

## Base 模块

### RedisError / RedisErrorType

定义位置：`galay-redis/base/RedisError.h`

```cpp
enum RedisErrorType {
    REDIS_ERROR_TYPE_SUCCESS,                   // 成功
    REDIS_ERROR_TYPE_URL_INVALID_ERROR,         // URL 无效
    REDIS_ERROR_TYPE_HOST_INVALID_ERROR,        // 主机无效
    REDIS_ERROR_TYPE_PORT_INVALID_ERROR,        // 端口无效
    REDIS_ERROR_TYPE_DB_INDEX_INVALID_ERROR,    // 数据库索引无效
    REDIS_ERROR_TYPE_ADDRESS_TYPE_INVALID_ERROR, // 地址类型无效
    REDIS_ERROR_TYPE_VERSION_INVALID_ERROR,     // 版本无效
    REDIS_ERROR_TYPE_CONNECTION_ERROR,          // 连接错误
    REDIS_ERROR_TYPE_FREE_REDISOBJ_ERROR,       // 释放对象失败
    REDIS_ERROR_TYPE_COMMAND_ERROR,             // 命令错误
    REDIS_ERROR_TYPE_TIMEOUT_ERROR,             // 超时
    REDIS_ERROR_TYPE_AUTH_ERROR,                // 认证失败
    REDIS_ERROR_TYPE_INVALID_ERROR,             // 无效错误
    REDIS_ERROR_TYPE_UNKNOWN_ERROR,             // 未知错误
    REDIS_ERROR_TYPE_PARSE_ERROR,               // 协议解析错误
    REDIS_ERROR_TYPE_SEND_ERROR,                // 发送数据错误
    REDIS_ERROR_TYPE_RECV_ERROR,                // 接收数据错误
    REDIS_ERROR_TYPE_BUFFER_OVERFLOW_ERROR,     // 缓冲区溢出
    REDIS_ERROR_TYPE_NETWORK_ERROR,             // 网络错误
    REDIS_ERROR_TYPE_CONNECTION_CLOSED,         // 连接已关闭
    REDIS_ERROR_TYPE_INTERNAL_ERROR,            // 内部错误
};

class RedisError {
public:
    RedisError(RedisErrorType type);
    RedisError(RedisErrorType type, std::string extra_msg);

    RedisErrorType type() const;
    std::string message() const;
};
```

### RedisValue

定义位置：`galay-redis/base/RedisValue.h`

RedisValue 封装 Redis 响应值，支持 RESP2 和 RESP3 的所有数据类型。

#### RESP2 类型

```cpp
class RedisValue {
public:
    // 类型检查
    bool isNull() const;
    bool isStatus() const;
    bool isError() const;
    bool isInteger() const;
    bool isString() const;
    bool isArray() const;

    // 值获取
    std::string toStatus() const;
    std::string toError() const;
    int64_t toInteger() const;
    std::string toString() const;
    std::vector<RedisValue> toArray() const;

    // 静态工厂方法
    static RedisValue fromError(const std::string& error_msg);
};
```

#### RESP3 扩展类型

```cpp
class RedisValue {
public:
    // RESP3 类型检查
    bool isDouble() const;
    bool isBool() const;
    bool isMap() const;
    bool isSet() const;
    bool isAttr() const;
    bool isPush() const;
    bool isBigNumber() const;
    bool isVerb() const;

    // RESP3 值获取
    double toDouble() const;
    bool toBool() const;
    std::map<std::string, RedisValue> toMap() const;
    std::vector<RedisValue> toSet() const;
    std::vector<RedisValue> toPush() const;
    std::string toBigNumber() const;
    std::string toVerb() const;
};
```

**注意**：`toArray()`、`toMap()`、`toSet()` 返回的容器生命周期不能超过 RedisValue 本身。

## Async 模块

### RedisClient

定义位置：`galay-redis/async/RedisClient.h`

RedisClient 是核心异步客户端，提供所有 Redis 命令的协程接口。

#### 构造与连接

```cpp
class RedisClient {
public:
    // 构造函数
    RedisClient(IOScheduler* scheduler, AsyncRedisConfig config = AsyncRedisConfig::noTimeout());

    // 支持移动语义
    RedisClient(RedisClient&& other) noexcept;
    RedisClient& operator=(RedisClient&& other) noexcept;

    // 禁止拷贝
    RedisClient(const RedisClient&) = delete;
    RedisClient& operator=(const RedisClient&) = delete;

    // 连接方法
    RedisConnectAwaitable& connect(const std::string& url);
    RedisConnectAwaitable& connect(const std::string& ip, int32_t port,
                                   const std::string& username = "",
                                   const std::string& password = "");
    RedisConnectAwaitable& connect(const std::string& ip, int32_t port,
                                   const std::string& username,
                                   const std::string& password,
                                   int32_t db_index);
    RedisConnectAwaitable& connect(const std::string& ip, int32_t port,
                                   const std::string& username,
                                   const std::string& password,
                                   int32_t db_index, int version);

    // 关闭连接
    auto close();
    bool isClosed() const;
};
```

#### String 命令

```cpp
class RedisClient {
public:
    RedisClientAwaitable& get(const std::string& key);
    RedisClientAwaitable& set(const std::string& key, const std::string& value);
    RedisClientAwaitable& setex(const std::string& key, int64_t seconds, const std::string& value);
    RedisClientAwaitable& del(const std::string& key);
    RedisClientAwaitable& exists(const std::string& key);
    RedisClientAwaitable& incr(const std::string& key);
    RedisClientAwaitable& decr(const std::string& key);
};
```

#### Hash 命令

```cpp
class RedisClient {
public:
    RedisClientAwaitable& hget(const std::string& key, const std::string& field);
    RedisClientAwaitable& hset(const std::string& key, const std::string& field, const std::string& value);
    RedisClientAwaitable& hdel(const std::string& key, const std::string& field);
    RedisClientAwaitable& hgetAll(const std::string& key);
};
```

#### List 命令

```cpp
class RedisClient {
public:
    RedisClientAwaitable& lpush(const std::string& key, const std::string& value);
    RedisClientAwaitable& rpush(const std::string& key, const std::string& value);
    RedisClientAwaitable& lpop(const std::string& key);
    RedisClientAwaitable& rpop(const std::string& key);
    RedisClientAwaitable& llen(const std::string& key);
    RedisClientAwaitable& lrange(const std::string& key, int64_t start, int64_t stop);
};
```

#### Set 命令

```cpp
class RedisClient {
public:
    RedisClientAwaitable& sadd(const std::string& key, const std::string& member);
    RedisClientAwaitable& srem(const std::string& key, const std::string& member);
    RedisClientAwaitable& smembers(const std::string& key);
    RedisClientAwaitable& scard(const std::string& key);
};
```

#### Sorted Set 命令

```cpp
class RedisClient {
public:
    RedisClientAwaitable& zadd(const std::string& key, double score, const std::string& member);
    RedisClientAwaitable& zrem(const std::string& key, const std::string& member);
    RedisClientAwaitable& zrange(const std::string& key, int64_t start, int64_t stop);
    RedisClientAwaitable& zscore(const std::string& key, const std::string& member);
};
```

#### Pub/Sub 命令

```cpp
class RedisClient {
public:
    RedisClientAwaitable& publish(const std::string& channel, const std::string& message);
    RedisClientAwaitable& subscribe(const std::string& channel);
    RedisClientAwaitable& subscribe(const std::vector<std::string>& channels);
    RedisClientAwaitable& unsubscribe(const std::string& channel);
    RedisClientAwaitable& unsubscribe(const std::vector<std::string>& channels);
    RedisClientAwaitable& psubscribe(const std::string& pattern);
    RedisClientAwaitable& psubscribe(const std::vector<std::string>& patterns);
    RedisClientAwaitable& punsubscribe(const std::string& pattern);
    RedisClientAwaitable& punsubscribe(const std::vector<std::string>& patterns);

    // 接收订阅消息
    RedisClientAwaitable& receive(size_t expected_replies = 1);
};
```

#### 通用命令

```cpp
class RedisClient {
public:
    // 执行任意命令
    RedisClientAwaitable& execute(const std::string& cmd, const std::vector<std::string>& args);
    RedisClientAwaitable& execute(const std::string& cmd,
                                  const std::vector<std::string>& args,
                                  size_t expected_replies);

    // Pipeline 批处理
    RedisPipelineAwaitable& pipeline(const std::vector<std::vector<std::string>>& commands);

    // 认证与数据库选择
    RedisClientAwaitable& auth(const std::string& password);
    RedisClientAwaitable& auth(const std::string& username, const std::string& password);
    RedisClientAwaitable& select(int32_t db_index);

    // 服务器命令
    RedisClientAwaitable& ping();
    RedisClientAwaitable& echo(const std::string& message);

    // 集群/主从命令
    RedisClientAwaitable& role();
    RedisClientAwaitable& replicaof(const std::string& host, int32_t port);
    RedisClientAwaitable& readonly();
    RedisClientAwaitable& readwrite();
    RedisClientAwaitable& clusterInfo();
    RedisClientAwaitable& clusterNodes();
    RedisClientAwaitable& clusterSlots();
};
```

### Awaitable 返回语义

所有命令方法返回 Awaitable 引用，支持 `.timeout()` 设置超时：

```cpp
// 基本用法
auto result = co_await client.get("key");

// 带超时
auto result = co_await client.get("key").timeout(std::chrono::seconds(5));
```

返回值类型：`std::expected<std::optional<std::vector<RedisValue>>, RedisError>`

- `!result`：失败，读取 `result.error()`
- `result && !result->has_value()`：未完成，继续 `co_await`
- `result && result->has_value()`：完成，读取 `result->value()`

典型用法：

```cpp
auto result = co_await client.get("key").timeout(std::chrono::seconds(5));

if (!result) {
    // 错误处理
    std::cerr << "错误: " << result.error().message() << '\n';
    co_return;
}

if (result.value()) {
    // 成功获取值
    auto& values = result.value().value();
    if (!values.empty() && values[0].isString()) {
        std::cout << "值: " << values[0].toString() << '\n';
    }
}
```

### RedisConnectionPool

定义位置：`galay-redis/async/RedisConnectionPool.h`

连接池提供线程安全的连接管理，支持自动扩缩容、健康检查。

#### 配置

```cpp
struct ConnectionPoolConfig {
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

    // 连接验证
    bool enable_connection_validation = true;
    bool validate_on_acquire = false;  // 每次获取时验证（性能开销大）
    bool validate_on_return = false;   // 归还时验证

    // 工厂方法
    static ConnectionPoolConfig defaultConfig();
    static ConnectionPoolConfig create(const std::string& host, int32_t port,
                                      size_t min_conn = 2, size_t max_conn = 10);
};
```

#### 连接池接口

```cpp
class RedisConnectionPool {
public:
    RedisConnectionPool(IOScheduler* scheduler,
                       ConnectionPoolConfig config = ConnectionPoolConfig::defaultConfig());

    // 初始化连接池
    PoolInitializeAwaitable& initialize();

    // 获取连接
    PoolAcquireAwaitable& acquire();

    // 归还连接
    void release(std::shared_ptr<PooledConnection> conn);

    // 统计信息
    struct PoolStats {
        size_t total_connections;
        size_t available_connections;
        size_t active_connections;
        size_t waiting_requests;
        uint64_t total_acquired;
        uint64_t total_released;
        uint64_t total_created;
        uint64_t total_destroyed;
        uint64_t health_check_failures;
        uint64_t reconnect_attempts;
        uint64_t reconnect_successes;
        uint64_t validation_failures;
        double avg_acquire_time_ms;
        double max_acquire_time_ms;
        size_t peak_active_connections;
        uint64_t total_acquire_time_ms;
    };

    PoolStats getStats() const;

    // 维护操作
    void warmup();                          // 预热到最小连接数
    size_t expandPool(size_t count);        // 手动扩容，返回实际创建数
    size_t shrinkPool(size_t target_size);  // 手动缩容，返回实际移除数
    void triggerHealthCheck();              // 触发健康检查
    void triggerIdleCleanup();              // 清理空闲连接
    size_t cleanupUnhealthyConnections();   // 清理不健康连接，返回清理数
    void shutdown();                        // 关闭连接池
};
```

#### ScopedConnection

RAII 风格的连接管理：

```cpp
class ScopedConnection {
public:
    ScopedConnection(RedisConnectionPool& pool, std::shared_ptr<PooledConnection> conn);
    ~ScopedConnection();

    // 禁止拷贝
    ScopedConnection(const ScopedConnection&) = delete;
    ScopedConnection& operator=(const ScopedConnection&) = delete;

    // 支持移动
    ScopedConnection(ScopedConnection&& other) noexcept;
    ScopedConnection& operator=(ScopedConnection&& other) noexcept;

    RedisClient* get();
    const RedisClient* get() const;
    RedisClient* operator->();
    const RedisClient* operator->() const;
    RedisClient& operator*();
    const RedisClient& operator*() const;
    explicit operator bool() const;
    void release();
};
```

使用示例：

```cpp
auto conn_result = co_await pool.acquire().timeout(std::chrono::seconds(2));
if (conn_result && conn_result.value()) {
    auto conn = conn_result.value();
    ScopedConnection scoped(pool, conn);
    co_await scoped->set("key", "value");
    auto r = co_await scoped->get("key");
    // 离开作用域自动归还连接
}
```

### RedisTopologyClient

定义位置：`galay-redis/async/RedisTopologyClient.h`

提供主从读写分离和集群路由能力。

#### RedisMasterSlaveClient

```cpp
class RedisMasterSlaveClient {
public:
    RedisMasterSlaveClient(IOScheduler* scheduler,
                          AsyncRedisConfig config = AsyncRedisConfig::noTimeout());

    // 连接管理
    RedisConnectAwaitable& connectMaster(const RedisNodeAddress& master);
    RedisConnectAwaitable& addReplica(const RedisNodeAddress& replica);

    // 基本操作
    RedisClientAwaitable& executeWrite(const std::string& cmd,
                                       const std::vector<std::string>& args);
    RedisPipelineAwaitable& pipelineWrite(const std::vector<std::vector<std::string>>& commands);
    RedisClientAwaitable& executeRead(const std::string& cmd,
                                      const std::vector<std::string>& args);
    RedisPipelineAwaitable& pipelineRead(const std::vector<std::vector<std::string>>& commands);

    // 自动模式（失败重试 + 拓扑刷新）
    RedisCommandResultAwaitable executeWriteAuto(const std::string& cmd,
                                                 const std::vector<std::string>& args);
    RedisCommandResultAwaitable executeReadAuto(const std::string& cmd,
                                                const std::vector<std::string>& args);

    // Sentinel 支持
    RedisConnectAwaitable& addSentinel(const RedisNodeAddress& sentinel);
    void setSentinelMasterName(std::string master_name);
    void setAutoRetryAttempts(size_t attempts) noexcept;
    RedisCommandResultAwaitable refreshFromSentinel();

    // 访问底层客户端
    RedisClient& master();
    std::optional<std::reference_wrapper<RedisClient>> replica(size_t index);
    size_t replicaCount() const noexcept;
};
```

#### RedisClusterClient

```cpp
class RedisClusterClient {
public:
    RedisClusterClient(IOScheduler* scheduler,
                      AsyncRedisConfig config = AsyncRedisConfig::noTimeout());

    // 节点管理
    RedisConnectAwaitable& addNode(const RedisClusterNodeAddress& node);
    void setSlotRange(size_t node_index, uint16_t slot_start, uint16_t slot_end);
    void setAutoRefreshInterval(std::chrono::milliseconds interval);

    // 基本操作
    RedisClientAwaitable& execute(const std::string& cmd,
                                  const std::vector<std::string>& args);
    RedisClientAwaitable& executeByKey(const std::string& routing_key,
                                       const std::string& cmd,
                                       const std::vector<std::string>& args);
    RedisPipelineAwaitable& pipelineByKey(const std::string& routing_key,
                                          const std::vector<std::vector<std::string>>& commands);

    // 自动模式（MOVED/ASK 重定向 + slots 刷新）
    RedisCommandResultAwaitable refreshSlots();
    RedisCommandResultAwaitable executeAuto(const std::string& cmd,
                                           const std::vector<std::string>& args);
    RedisCommandResultAwaitable executeByKeyAuto(const std::string& routing_key,
                                                 const std::string& cmd,
                                                 const std::vector<std::string>& args);

    // 工具方法
    uint16_t keySlot(const std::string& key) const;
    size_t nodeCount() const noexcept;
    std::optional<std::reference_wrapper<RedisClient>> node(size_t index);
};
```

## 使用注意事项

### 异步客户端

1. 同一个 `RedisClient` 实例应串行使用，不要并发发起多个异步操作
2. Awaitable 引用在下次调用同类方法前有效
3. 支持移动构造和移动赋值，可以在容器中存储
4. 生产环境建议配置超时参数

### 连接池

1. 从池中获取的连接必须通过 `release()` 归还
2. 建议使用 `ScopedConnection` 确保异常安全
3. 当池满时 `acquire()` 会挂起等待，直到有连接归还

### 性能优化

1. 使用 Pipeline 批处理大量命令
2. 使用连接池避免频繁建立连接
3. 合理设置连接池大小
4. 使用事务包裹批量写入

## 常见问题

### Q: 异步客户端可以并发执行多个查询吗？

A: 不可以。同一个 `RedisClient` 实例应串行使用。如需并发，请使用连接池获取多个客户端实例。

### Q: 如何处理超时？

A: 所有 Awaitable 都支持 `.timeout()` 方法：

```cpp
auto r = co_await client.get("key").timeout(std::chrono::seconds(5));
if (!r && r.error().type() == REDIS_ERROR_TYPE_TIMEOUT_ERROR) {
    std::cerr << "操作超时\n";
}
```

### Q: Pipeline 如何使用？

A: 将多个命令组织成二维数组，一次性发送：

```cpp
std::vector<std::vector<std::string>> commands = {
    {"SET", "k1", "v1"},
    {"SET", "k2", "v2"},
    {"GET", "k1"},
    {"GET", "k2"}
};
auto result = co_await client.pipeline(commands).timeout(std::chrono::seconds(10));
```

### Q: 如何判断命令是否返回结果？

A: 检查返回值：

```cpp
auto result = co_await client.get("key");
if (result && result.value()) {
    auto& values = result.value().value();
    if (!values.empty()) {
        // 有结果
    }
}
```

### Q: 连接池的最小和最大连接数如何设置？

A: 根据并发量设置：

- 低并发（< 10 QPS）：min=2, max=5
- 中等并发（10-100 QPS）：min=5, max=20
- 高并发（> 100 QPS）：min=10, max=50

不要超过 Redis 服务器的 `maxclients` 配置。
