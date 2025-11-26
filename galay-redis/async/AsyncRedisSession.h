#ifndef GALAY_REDIS_ASYNC_SESSION_H
#define GALAY_REDIS_ASYNC_SESSION_H

#include <galay/kernel/async/Socket.h>
#include <galay/kernel/coroutine/CoSchedulerHandle.hpp>
#include <galay/kernel/coroutine/Result.hpp>
#include <galay/common/Base.h>
#include <string>
#include <expected>
#include <vector>
#include "../base/RedisError.h"
#include "../base/RedisValue.h"
#include "../protocol/RedisProtocol.h"
#include "galay/common/Log.h"

namespace galay::redis
{
    // 使用galay框架的类型
    using galay::AsyncTcpSocket;
    using galay::CoSchedulerHandle;
    using galay::AsyncResult;
    using galay::Coroutine;
    using galay::nil;
    using galay::Host;
    using galay::Bytes;
    using galay::error::CommonError;

    // 异步Redis会话类 - 类似 HttpConnection 的设计模式
    class AsyncRedisSession
    {
    public:
        // 构造函数 - 接受已连接的socket
        AsyncRedisSession(AsyncTcpSocket&& socket, CoSchedulerHandle handle);
        AsyncRedisSession(AsyncRedisSession&& other);
        AsyncRedisSession(const AsyncRedisSession&) = delete;
        AsyncRedisSession& operator=(const AsyncRedisSession&) = delete;

        // 通用命令执行接口
        AsyncResult<std::expected<RedisValue, RedisError>> execute(const std::string& cmd);
        AsyncResult<std::expected<RedisValue, RedisError>> execute(const std::string& cmd,
                                                                const std::vector<std::string>& args);
        AsyncResult<std::expected<RedisValue, RedisError>> execute(const std::vector<std::string>& cmd_parts);

        // 基础Redis命令
        AsyncResult<std::expected<void, RedisError>> auth(const std::string& password);
        AsyncResult<std::expected<void, RedisError>> auth(const std::string& username, const std::string& password);
        AsyncResult<std::expected<void, RedisError>> select(int32_t db_index);
        AsyncResult<std::expected<RedisValue, RedisError>> ping();
        AsyncResult<std::expected<RedisValue, RedisError>> echo(const std::string& message);

        // String操作
        AsyncResult<std::expected<RedisValue, RedisError>> get(const std::string& key);
        AsyncResult<std::expected<RedisValue, RedisError>> set(const std::string& key, const std::string& value);
        AsyncResult<std::expected<RedisValue, RedisError>> setex(const std::string& key, int64_t seconds, const std::string& value);
        AsyncResult<std::expected<RedisValue, RedisError>> del(const std::string& key);
        AsyncResult<std::expected<RedisValue, RedisError>> exists(const std::string& key);
        AsyncResult<std::expected<RedisValue, RedisError>> incr(const std::string& key);
        AsyncResult<std::expected<RedisValue, RedisError>> decr(const std::string& key);

        // Hash操作
        AsyncResult<std::expected<RedisValue, RedisError>> hget(const std::string& key, const std::string& field);
        AsyncResult<std::expected<RedisValue, RedisError>> hset(const std::string& key, const std::string& field, const std::string& value);
        AsyncResult<std::expected<RedisValue, RedisError>> hdel(const std::string& key, const std::string& field);
        AsyncResult<std::expected<RedisValue, RedisError>> hgetAll(const std::string& key);

        // List操作
        AsyncResult<std::expected<RedisValue, RedisError>> lpush(const std::string& key, const std::string& value);
        AsyncResult<std::expected<RedisValue, RedisError>> rpush(const std::string& key, const std::string& value);
        AsyncResult<std::expected<RedisValue, RedisError>> lpop(const std::string& key);
        AsyncResult<std::expected<RedisValue, RedisError>> rpop(const std::string& key);
        AsyncResult<std::expected<RedisValue, RedisError>> llen(const std::string& key);
        AsyncResult<std::expected<RedisValue, RedisError>> lrange(const std::string& key, int64_t start, int64_t stop);

        // Set操作
        AsyncResult<std::expected<RedisValue, RedisError>> sadd(const std::string& key, const std::string& member);
        AsyncResult<std::expected<RedisValue, RedisError>> srem(const std::string& key, const std::string& member);
        AsyncResult<std::expected<RedisValue, RedisError>> smembers(const std::string& key);
        AsyncResult<std::expected<RedisValue, RedisError>> scard(const std::string& key);

        // Sorted Set操作
        AsyncResult<std::expected<RedisValue, RedisError>> zadd(const std::string& key, double score, const std::string& member);
        AsyncResult<std::expected<RedisValue, RedisError>> zrem(const std::string& key, const std::string& member);
        AsyncResult<std::expected<RedisValue, RedisError>> zrange(const std::string& key, int64_t start, int64_t stop);
        AsyncResult<std::expected<RedisValue, RedisError>> zscore(const std::string& key, const std::string& member);

        // 关闭连接
        AsyncResult<std::expected<void, CommonError>> close();
        bool isClosed() const;
        void markClosed();

        ~AsyncRedisSession() = default;

    private:
        // 发送编码的命令
        AsyncResult<std::expected<void, RedisError>> sendCommand(const std::string& encoded_cmd);

        // 接收并解析Redis响应
        AsyncResult<std::expected<protocol::RedisReply, RedisError>> receiveReply();

        // 执行命令的内部实现
        AsyncResult<std::expected<RedisValue, RedisError>> executeCommand(std::string&& encoded_cmd);

        // 成员变量
        bool m_is_closed = false;
        AsyncTcpSocket m_socket;
        CoSchedulerHandle m_handle;
        protocol::RespEncoder m_encoder;
        protocol::RespParser m_parser;
        std::vector<char> m_recv_buffer;
        size_t m_buffer_pos = 0;  // 缓冲区当前位置
        static constexpr size_t BUFFER_SIZE = 8192;
        Logger::uptr m_logger = Logger::createStdoutLoggerMT("AsyncRedisLogger");
    };

}

#endif // GALAY_REDIS_ASYNC_SESSION_H
