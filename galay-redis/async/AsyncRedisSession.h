#ifndef GALAY_REDIS_ASYNC_SESSION_H
#define GALAY_REDIS_ASYNC_SESSION_H

#include <cstdint>
#include <galay-kernel/async/TcpSocket.h>
#include <galay-kernel/kernel/IOScheduler.hpp>
#include <galay-kernel/common/Host.hpp>
#include <galay-kernel/common/Buffer.h>
#include <galay-kernel/common/Error.h>
#include <memory>
#include <string>
#include <expected>
#include <optional>
#include <vector>
#include <coroutine>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include "galay-redis/base/RedisError.h"
#include "galay-redis/base/RedisValue.h"
#include "galay-redis/protocol/RedisProtocol.h"
#include "AsyncRedisConfig.h"

namespace galay::redis
{
    using galay::async::TcpSocket;
    using galay::kernel::IOScheduler;
    using galay::kernel::Host;
    using galay::kernel::IOError;
    using galay::kernel::IPType;
    using galay::kernel::RingBuffer;
    using galay::kernel::SendAwaitable;
    using galay::kernel::ReadvAwaitable;

    // 类型别名
    using RedisResult = std::expected<std::optional<std::vector<RedisValue>>, RedisError>;
    using RedisVoidResult = std::expected<void, RedisError>;

    // 前向声明
    class AsyncRedisSession;

    /**
     * @brief Redis命令执行等待体
     * @details 自动处理完整的命令发送和响应接收流程
     */
    class ExecuteAwaitable
    {
    public:
        ExecuteAwaitable(AsyncRedisSession& session,
                        std::string cmd,
                        std::vector<std::string> args,
                        size_t expected_replies = 1);

        bool await_ready() const noexcept { return false; }
        bool await_suspend(std::coroutine_handle<> handle);
        RedisResult await_resume();

        bool isInvalid() const { return m_state == State::Invalid; }

    private:
        enum class State {
            Invalid,
            Sending,
            Receiving
        };

        AsyncRedisSession& m_session;
        std::string m_cmd;
        std::vector<std::string> m_args;
        std::string m_encoded_cmd;
        size_t m_expected_replies;
        std::vector<RedisValue> m_values;
        State m_state;
        size_t m_sent;

        std::optional<SendAwaitable> m_send_awaitable;
        std::optional<ReadvAwaitable> m_recv_awaitable;
    };

    /**
     * @brief Redis Pipeline等待体
     */
    class PipelineAwaitable
    {
    public:
        PipelineAwaitable(AsyncRedisSession& session,
                         std::vector<std::vector<std::string>> commands);

        bool await_ready() const noexcept { return false; }
        bool await_suspend(std::coroutine_handle<> handle);
        RedisResult await_resume();

        bool isInvalid() const { return m_state == State::Invalid; }

    private:
        enum class State {
            Invalid,
            Sending,
            Receiving
        };

        AsyncRedisSession& m_session;
        std::vector<std::vector<std::string>> m_commands;
        std::string m_encoded_batch;
        std::vector<RedisValue> m_values;
        State m_state;
        size_t m_sent;

        std::optional<SendAwaitable> m_send_awaitable;
        std::optional<ReadvAwaitable> m_recv_awaitable;
    };

    /**
     * @brief Redis连接等待体
     */
    class ConnectAwaitable
    {
    public:
        ConnectAwaitable(AsyncRedisSession& session,
                        std::string ip,
                        int32_t port,
                        std::string username,
                        std::string password,
                        int32_t db_index,
                        int version);

        bool await_ready() const noexcept { return false; }
        bool await_suspend(std::coroutine_handle<> handle);
        RedisVoidResult await_resume();

        bool isInvalid() const { return m_state == State::Invalid; }

    private:
        enum class State {
            Invalid,
            Connecting,
            Authenticating,
            SelectingDB,
            Done
        };

        AsyncRedisSession& m_session;
        std::string m_ip;
        int32_t m_port;
        std::string m_username;
        std::string m_password;
        int32_t m_db_index;
        int m_version;
        State m_state;

        // 连接相关的 awaitable
        std::optional<SendAwaitable> m_send_awaitable;
        std::optional<ReadvAwaitable> m_recv_awaitable;
        std::vector<RedisValue> m_temp_values;
        std::string m_encoded_cmd;
        size_t m_sent;
    };

    /**
     * @brief 异步Redis会话类 - 使用Awaitable模式
     * @details 完全采用状态机实现，无Coroutine开销
     */
    class AsyncRedisSession
    {
    public:
        AsyncRedisSession(IOScheduler* scheduler, AsyncRedisConfig config = AsyncRedisConfig::noTimeout());

        // 移动构造和赋值
        AsyncRedisSession(AsyncRedisSession&& other) noexcept;
        AsyncRedisSession& operator=(AsyncRedisSession&& other) noexcept;

        // 禁止拷贝
        AsyncRedisSession(const AsyncRedisSession&) = delete;
        AsyncRedisSession& operator=(const AsyncRedisSession&) = delete;

        // ======================== 连接方法 ========================

        ConnectAwaitable& connect(const std::string& url);
        ConnectAwaitable& connect(const std::string& ip, int32_t port,
                                 const std::string& username = "",
                                 const std::string& password = "");
        ConnectAwaitable& connect(const std::string& ip, int32_t port,
                                 const std::string& username,
                                 const std::string& password,
                                 int32_t db_index);
        ConnectAwaitable& connect(const std::string& ip, int32_t port,
                                 const std::string& username,
                                 const std::string& password,
                                 int32_t db_index, int version);

        // ======================== 基础Redis命令 ========================

        ExecuteAwaitable& execute(const std::string& cmd, const std::vector<std::string>& args);
        ExecuteAwaitable& auth(const std::string& password);
        ExecuteAwaitable& auth(const std::string& username, const std::string& password);
        ExecuteAwaitable& select(int32_t db_index);
        ExecuteAwaitable& ping();
        ExecuteAwaitable& echo(const std::string& message);

        // ======================== String操作 ========================

        ExecuteAwaitable& get(const std::string& key);
        ExecuteAwaitable& set(const std::string& key, const std::string& value);
        ExecuteAwaitable& setex(const std::string& key, int64_t seconds, const std::string& value);
        ExecuteAwaitable& del(const std::string& key);
        ExecuteAwaitable& exists(const std::string& key);
        ExecuteAwaitable& incr(const std::string& key);
        ExecuteAwaitable& decr(const std::string& key);

        // ======================== Hash操作 ========================

        ExecuteAwaitable& hget(const std::string& key, const std::string& field);
        ExecuteAwaitable& hset(const std::string& key, const std::string& field, const std::string& value);
        ExecuteAwaitable& hdel(const std::string& key, const std::string& field);
        ExecuteAwaitable& hgetAll(const std::string& key);

        // ======================== List操作 ========================

        ExecuteAwaitable& lpush(const std::string& key, const std::string& value);
        ExecuteAwaitable& rpush(const std::string& key, const std::string& value);
        ExecuteAwaitable& lpop(const std::string& key);
        ExecuteAwaitable& rpop(const std::string& key);
        ExecuteAwaitable& llen(const std::string& key);
        ExecuteAwaitable& lrange(const std::string& key, int64_t start, int64_t stop);

        // ======================== Set操作 ========================

        ExecuteAwaitable& sadd(const std::string& key, const std::string& member);
        ExecuteAwaitable& srem(const std::string& key, const std::string& member);
        ExecuteAwaitable& smembers(const std::string& key);
        ExecuteAwaitable& scard(const std::string& key);

        // ======================== Sorted Set操作 ========================

        ExecuteAwaitable& zadd(const std::string& key, double score, const std::string& member);
        ExecuteAwaitable& zrem(const std::string& key, const std::string& member);
        ExecuteAwaitable& zrange(const std::string& key, int64_t start, int64_t stop);
        ExecuteAwaitable& zscore(const std::string& key, const std::string& member);

        // ======================== Pipeline批量操作 ========================

        PipelineAwaitable& pipeline(const std::vector<std::vector<std::string>>& commands);

        // ======================== 连接管理 ========================

        auto close() {
            return m_socket.close();
        }

        bool isClosed() const { return m_is_closed; }

        ~AsyncRedisSession() = default;

    private:
        friend class ExecuteAwaitable;
        friend class PipelineAwaitable;
        friend class ConnectAwaitable;

        // 成员变量
        bool m_is_closed = false;
        TcpSocket m_socket;
        IOScheduler* m_scheduler;
        protocol::RespEncoder m_encoder;
        protocol::RespParser m_parser;
        AsyncRedisConfig m_config;
        RingBuffer m_ring_buffer;

        // 存储 awaitable 对象
        std::optional<ExecuteAwaitable> m_execute_awaitable;
        std::optional<PipelineAwaitable> m_pipeline_awaitable;
        std::optional<ConnectAwaitable> m_connect_awaitable;

        std::shared_ptr<spdlog::logger> m_logger;
    };

}

#endif // GALAY_REDIS_ASYNC_SESSION_H
