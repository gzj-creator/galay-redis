# RedisClient vs AsyncRedisSession 对比分析

## 架构对比

### RedisClient (新实现)

```
RedisClient
    ├── RedisClientAwaitable (继承 TimeoutSupport)
    │   ├── await_ready()
    │   ├── await_suspend()
    │   ├── await_resume()
    │   ├── reset()
    │   └── m_result (用于超时错误)
    ├── RedisPipelineAwaitable
    └── RedisConnectAwaitable
```

**设计模式**: 参考 `HttpClientAwaitable`
- 继承 `TimeoutSupport<RedisClientAwaitable>`
- 完整的资源管理（reset方法）
- 统一的错误处理流程

### AsyncRedisSession (旧实现)

```
AsyncRedisSession
    ├── ExecuteAwaitable
    │   ├── await_ready()
    │   ├── await_suspend()
    │   └── await_resume()
    ├── PipelineAwaitable
    └── ConnectAwaitable
```

**设计模式**: 自定义实现
- 无超时支持
- 基础的错误处理
- 手动资源管理

## 功能对比表

| 功能特性 | RedisClient | AsyncRedisSession | 说明 |
|---------|-------------|-------------------|------|
| **超时支持** | ✅ | ❌ | RedisClient支持`.timeout()`方法 |
| **TimeoutSupport继承** | ✅ | ❌ | 统一的超时处理机制 |
| **错误类型转换** | ✅ | ⚠️ | IOError → RedisError自动转换 |
| **资源自动清理** | ✅ | ⚠️ | reset()方法确保资源释放 |
| **状态机设计** | ✅ | ✅ | 两者都使用状态机 |
| **Pipeline支持** | ✅ | ✅ | 批量命令执行 |
| **连接管理** | ✅ | ✅ | 支持认证和数据库选择 |
| **移动语义** | ✅ | ✅ | 支持移动构造和赋值 |
| **日志支持** | ✅ | ✅ | 使用spdlog |

## 代码对比

### 1. 基本使用

#### RedisClient (新)
```cpp
RedisClient client(scheduler);
co_await client.connect("127.0.0.1", 6379);

// 支持超时
auto result = co_await client.get("key").timeout(std::chrono::seconds(5));

if (!result) {
    if (result.error().type() == REDIS_ERROR_TYPE_TIMEOUT_ERROR) {
        std::cout << "Timeout!" << std::endl;
    }
}
```

#### AsyncRedisSession (旧)
```cpp
AsyncRedisSession session(scheduler);
co_await session.connect("127.0.0.1", 6379);

// 不支持超时
auto result = co_await session.get("key");

if (!result) {
    std::cout << "Error: " << result.error().message() << std::endl;
}
```

### 2. 错误处理

#### RedisClient (新)
```cpp
auto result = co_await client.get("key").timeout(std::chrono::seconds(5));

if (!result) {
    auto& error = result.error();

    // 详细的错误类型判断
    switch (error.type()) {
        case REDIS_ERROR_TYPE_TIMEOUT_ERROR:
            // 超时错误
            break;
        case REDIS_ERROR_TYPE_CONNECTION_CLOSED:
            // 连接关闭
            break;
        case REDIS_ERROR_TYPE_NETWORK_ERROR:
            // 网络错误
            break;
        case REDIS_ERROR_TYPE_INTERNAL_ERROR:
            // 内部错误
            break;
        default:
            // 其他错误
            break;
    }
}
```

#### AsyncRedisSession (旧)
```cpp
auto result = co_await session.get("key");

if (!result) {
    // 只能获取错误消息
    std::cout << "Error: " << result.error().message() << std::endl;
}
```

### 3. 内部实现对比

#### RedisClientAwaitable::await_resume() (新)
```cpp
std::expected<std::optional<std::vector<RedisValue>>, RedisError>
RedisClientAwaitable::await_resume()
{
    // 1. 首先检查超时错误（由TimeoutSupport设置）
    if (!m_result.has_value()) {
        auto& io_error = m_result.error();

        // 将IOError转换为RedisError
        RedisErrorType redis_error_type;
        if (io_error.code() == galay::kernel::kTimeout) {
            redis_error_type = REDIS_ERROR_TYPE_TIMEOUT_ERROR;
        } else if (io_error.code() == galay::kernel::kDisconnectError) {
            redis_error_type = REDIS_ERROR_TYPE_CONNECTION_CLOSED;
        } else {
            redis_error_type = REDIS_ERROR_TYPE_RECV_ERROR;
        }

        reset();  // 清理资源
        return std::unexpected(RedisError(redis_error_type, io_error.message()));
    }

    // 2. 处理发送状态
    if (m_state == State::Sending) {
        auto send_result = m_send_awaitable->await_resume();
        if (!send_result) {
            reset();  // 错误时清理资源
            return std::unexpected(RedisError(...));
        }
        // ... 状态转换
    }

    // 3. 处理接收状态
    else if (m_state == State::Receiving) {
        auto recv_result = m_recv_awaitable->await_resume();
        if (!recv_result) {
            reset();  // 错误时清理资源
            return std::unexpected(RedisError(...));
        }
        // ... 解析响应

        // 完成后清理资源
        auto values = std::move(m_values);
        reset();
        return values;
    }

    // 4. Invalid状态错误
    else {
        reset();
        return std::unexpected(RedisError(REDIS_ERROR_TYPE_INTERNAL_ERROR, ...));
    }
}
```

#### ExecuteAwaitable::await_resume() (旧)
```cpp
std::expected<std::optional<std::vector<RedisValue>>, RedisError>
ExecuteAwaitable::await_resume()
{
    // 没有超时检查

    if (m_state == State::Sending) {
        auto send_result = m_send_awaitable->await_resume();
        if (!send_result) {
            // 手动重置状态
            m_state = State::Invalid;
            m_send_awaitable.reset();
            return std::unexpected(RedisError(...));
        }
        // ... 状态转换
    }
    else {
        auto recv_result = m_recv_awaitable->await_resume();
        if (!recv_result) {
            // 手动重置状态
            m_state = State::Invalid;
            m_recv_awaitable.reset();
            return std::unexpected(RedisError(...));
        }
        // ... 解析响应

        // 手动重置状态
        m_state = State::Invalid;
        m_recv_awaitable.reset();
        return std::move(m_values);
    }
}
```

## 性能对比

### 理论分析

| 指标 | RedisClient | AsyncRedisSession | 差异 |
|------|-------------|-------------------|------|
| **内存开销** | +8字节 | 基准 | m_result成员 |
| **CPU开销** | +1-2% | 基准 | 超时检查 |
| **延迟** | 相同 | 相同 | 状态机效率相同 |
| **吞吐量** | 相同 | 相同 | 网络IO为瓶颈 |

### 实际测试

```bash
# 测试命令
./test_redis_client_benchmark 10 1000        # 普通模式
./test_redis_client_benchmark 10 1000 pipeline 100  # Pipeline模式
```

预期结果：
- **普通模式**: 20,000-50,000 ops/sec
- **Pipeline模式**: 100,000-200,000 ops/sec
- **超时开销**: < 1% 性能影响

## 迁移建议

### 何时使用 RedisClient

✅ **推荐使用场景**:
1. 新项目开发
2. 需要超时控制的场景
3. 需要详细错误处理的场景
4. 需要与HttpClient统一风格的项目

### 何时保留 AsyncRedisSession

⚠️ **可以继续使用**:
1. 已有稳定运行的代码
2. 不需要超时功能
3. 性能极度敏感（省略超时检查）

### 迁移步骤

1. **替换类名**
   ```cpp
   // 旧
   AsyncRedisSession session(scheduler);

   // 新
   RedisClient client(scheduler);
   ```

2. **添加超时（可选）**
   ```cpp
   // 旧
   auto result = co_await session.get("key");

   // 新（添加超时）
   auto result = co_await client.get("key").timeout(std::chrono::seconds(5));
   ```

3. **更新错误处理**
   ```cpp
   // 新增超时错误处理
   if (!result && result.error().type() == REDIS_ERROR_TYPE_TIMEOUT_ERROR) {
       // 处理超时
   }
   ```

## 兼容性说明

### API兼容性

✅ **完全兼容**:
- 所有命令方法签名相同
- 返回类型相同
- 连接方法相同

⚠️ **需要注意**:
- 类名不同（AsyncRedisSession vs RedisClient）
- 新增了超时功能（可选使用）
- 错误类型更详细

### 二进制兼容性

❌ **不兼容**:
- 类布局不同（新增m_result成员）
- 虚函数表不同（继承TimeoutSupport）
- 需要重新编译

## 最佳实践

### 1. 统一使用 RedisClient

```cpp
// 推荐：统一使用新实现
RedisClient client(scheduler);
auto result = co_await client.get("key").timeout(std::chrono::seconds(5));
```

### 2. 合理设置超时

```cpp
// 短操作：1-5秒
auto result = co_await client.get("key").timeout(std::chrono::seconds(2));

// 长操作：10-30秒
auto result = co_await client.pipeline(commands).timeout(std::chrono::seconds(30));

// 不设置超时（使用默认行为）
auto result = co_await client.get("key");  // 无超时限制
```

### 3. 错误处理模式

```cpp
auto result = co_await client.get("key").timeout(std::chrono::seconds(5));

if (!result) {
    auto& error = result.error();

    // 可重试的错误
    if (error.type() == REDIS_ERROR_TYPE_TIMEOUT_ERROR ||
        error.type() == REDIS_ERROR_TYPE_NETWORK_ERROR) {
        // 重试逻辑
    }

    // 不可重试的错误
    else if (error.type() == REDIS_ERROR_TYPE_CONNECTION_CLOSED) {
        // 重新连接
    }

    // 其他错误
    else {
        // 记录日志并返回
    }
}
```

## 总结

### RedisClient 的优势

1. ✅ **完整的超时支持** - 所有操作都可以设置超时
2. ✅ **统一的设计模式** - 与HttpClientAwaitable保持一致
3. ✅ **更好的错误处理** - 详细的错误类型和自动转换
4. ✅ **自动资源管理** - reset()方法确保资源正确释放
5. ✅ **更好的可维护性** - 代码结构清晰，易于扩展

### 性能影响

- **内存**: +8字节/实例（m_result成员）
- **CPU**: +1-2%（超时检查开销）
- **延迟**: 无明显影响
- **吞吐量**: 无明显影响

### 推荐

🎯 **新项目强烈推荐使用 RedisClient**

对于已有项目：
- 如果需要超时功能 → 迁移到 RedisClient
- 如果不需要超时功能 → 可以继续使用 AsyncRedisSession
- 两者可以共存，逐步迁移

## 参考资料

- [RedisClient 完整文档](README_RedisClient.md)
- [HttpClientAwaitable 设计文档](../galay-http/docs/HttpClient.md)
- [性能测试报告](docs/benchmark_results.md)
