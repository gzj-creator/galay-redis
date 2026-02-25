#ifndef GALAY_REDIS_CLIENT_H
#define GALAY_REDIS_CLIENT_H

#include <galay-kernel/async/TcpSocket.h>
#include <galay-kernel/kernel/IOScheduler.hpp>
#include <galay-kernel/kernel/Coroutine.h>
#include <galay-kernel/kernel/Timeout.hpp>
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
    using galay::kernel::Coroutine;
    using galay::kernel::Host;
    using galay::kernel::IOError;
    using galay::kernel::IPType;
    using galay::kernel::RingBuffer;
    using galay::kernel::CustomAwaitable;
    using galay::kernel::SendAwaitable;
    using galay::kernel::RecvAwaitable;
    using galay::kernel::ReadvAwaitable;
    using galay::kernel::ConnectAwaitable;

    // 类型别名
    using RedisResult = std::expected<std::vector<RedisValue>, RedisError>;
    using RedisVoidResult = std::expected<void, RedisError>;

    // 前向声明
    class RedisClient;

    /**
     * @brief Redis客户端等待体
     * @details 自动处理完整的命令发送和响应接收流程
     *          返回 std::expected<std::optional<std::vector<RedisValue>>, RedisError>
     *          - std::vector<RedisValue>: 命令和响应全部完成
     *          - std::nullopt: 需要继续调用（数据未完全发送或接收）
     *          - RedisError: 发生错误
     *
     * @note 支持超时设置：
     * @code
     * auto result = co_await client.get("key").timeout(std::chrono::seconds(5));
     * @endcode
     */
    class RedisClientAwaitable : public galay::kernel::CustomAwaitable,
                                 public galay::kernel::TimeoutSupport<RedisClientAwaitable>
    {
    public:
        class ProtocolSendAwaitable : public SendAwaitable
        {
        public:
            explicit ProtocolSendAwaitable(RedisClientAwaitable* owner);

#ifdef USE_IOURING
            bool handleComplete(struct io_uring_cqe* cqe, GHandle) override;
#else
            bool handleComplete(GHandle handle) override;
#endif

            void rebind(RedisClientAwaitable* owner);

        private:
            RedisClientAwaitable* m_owner;
        };

        class ProtocolRecvAwaitable : public RecvAwaitable
        {
        public:
            explicit ProtocolRecvAwaitable(RedisClientAwaitable* owner);

#ifdef USE_IOURING
            bool handleComplete(struct io_uring_cqe* cqe, GHandle) override;
#else
            bool handleComplete(GHandle handle) override;
#endif

            void rebind(RedisClientAwaitable* owner);

        private:
            bool prepareRecvWindow();

            RedisClientAwaitable* m_owner;
        };

        /**
         * @brief 构造函数
         * @param client RedisClient引用
         * @param cmd 命令名称
         * @param args 命令参数
         * @param expected_replies 期望的响应数量（Pipeline时>1）
         */
        RedisClientAwaitable(RedisClient& client,
                            std::string cmd,
                            std::vector<std::string> args,
                            size_t expected_replies = 1,
                            bool recv_only = false);

        RedisClientAwaitable(const RedisClientAwaitable&) = delete;
        RedisClientAwaitable& operator=(const RedisClientAwaitable&) = delete;
        RedisClientAwaitable(RedisClientAwaitable&& other) noexcept;
        RedisClientAwaitable& operator=(RedisClientAwaitable&& other) noexcept;

        bool await_ready() const noexcept {
            return false;
        }

        using galay::kernel::CustomAwaitable::await_suspend;

        std::expected<std::optional<std::vector<RedisValue>>, RedisError> await_resume();

        /**
         * @brief 检查状态是否为 Invalid
         * @return true 如果状态为 Invalid
         */
        bool isInvalid() const noexcept {
            return m_state == State::Invalid;
        }

        /**
         * @brief 重置状态并清理资源
         * @details 在错误发生时调用，确保资源正确清理
         */
        void reset() noexcept {
            m_state = State::Invalid;
            m_send_awaitable.rebind(this);
            m_recv_awaitable.rebind(this);
            m_values.clear();
            m_parse_buffer.clear();
            m_internal_error.reset();
            m_result = std::nullopt;  // 重置为 nullopt
        }

    private:
        enum class State {
            Running,           // 正在执行链式 SEND -> RECV
            Invalid            // 无效状态，可以重新创建
        };

        void initTaskQueue();
        void moveFrom(RedisClientAwaitable&& other) noexcept;
        void markMovedFrom() noexcept;
        bool parseResponsesFromRingBuffer();
        void setSendError(const IOError& io_error);
        void setRecvError(const IOError& io_error);
        void setParseError();
        void setConnectionClosedError();
        void setBufferOverflowError();

        RedisClient* m_client;
        std::string m_cmd;
        std::vector<std::string> m_args;
        std::string m_encoded_cmd;
        std::string m_parse_buffer;
        size_t m_expected_replies;
        bool m_recv_only;
        std::vector<RedisValue> m_values;
        State m_state;
        ProtocolSendAwaitable m_send_awaitable;
        ProtocolRecvAwaitable m_recv_awaitable;
        std::optional<RedisError> m_internal_error;

    public:
        // TimeoutSupport 需要访问此成员来设置超时错误
        // 注意：这里使用 IOError 类型，因为 TimeoutSupport 会设置 IOError
        std::expected<std::optional<std::vector<RedisValue>>, galay::kernel::IOError> m_result;
    };

    /**
     * @brief Redis Pipeline等待体
     * @details 处理批量命令的发送和接收
     *
     * @note 支持超时设置：
     * @code
     * auto result = co_await client.pipeline(commands).timeout(std::chrono::seconds(10));
     * @endcode
     */
    class RedisPipelineAwaitable : public galay::kernel::CustomAwaitable,
                                   public galay::kernel::TimeoutSupport<RedisPipelineAwaitable>
    {
    public:
        class ProtocolSendAwaitable : public SendAwaitable
        {
        public:
            explicit ProtocolSendAwaitable(RedisPipelineAwaitable* owner);

#ifdef USE_IOURING
            bool handleComplete(struct io_uring_cqe* cqe, GHandle) override;
#else
            bool handleComplete(GHandle handle) override;
#endif

            void rebind(RedisPipelineAwaitable* owner);

        private:
            RedisPipelineAwaitable* m_owner;
        };

        class ProtocolRecvAwaitable : public RecvAwaitable
        {
        public:
            explicit ProtocolRecvAwaitable(RedisPipelineAwaitable* owner);

#ifdef USE_IOURING
            bool handleComplete(struct io_uring_cqe* cqe, GHandle) override;
#else
            bool handleComplete(GHandle handle) override;
#endif

            void rebind(RedisPipelineAwaitable* owner);

        private:
            bool prepareRecvWindow();

            RedisPipelineAwaitable* m_owner;
        };

        RedisPipelineAwaitable(RedisClient& client,
                              std::vector<std::vector<std::string>> commands);

        RedisPipelineAwaitable(const RedisPipelineAwaitable&) = delete;
        RedisPipelineAwaitable& operator=(const RedisPipelineAwaitable&) = delete;
        RedisPipelineAwaitable(RedisPipelineAwaitable&& other) noexcept;
        RedisPipelineAwaitable& operator=(RedisPipelineAwaitable&& other) noexcept;

        bool await_ready() const noexcept {
            return false;
        }

        using galay::kernel::CustomAwaitable::await_suspend;

        std::expected<std::optional<std::vector<RedisValue>>, RedisError> await_resume();

        bool isInvalid() const noexcept {
            return m_state == State::Invalid;
        }

        /**
         * @brief 重置状态并清理资源
         * @details 在错误发生时调用，确保资源正确清理
         */
        void reset() noexcept {
            m_state = State::Invalid;
            m_send_awaitable.rebind(this);
            m_recv_awaitable.rebind(this);
            m_values.clear();
            m_parse_buffer.clear();
            m_internal_error.reset();
            m_result = std::nullopt;
        }

    private:
        enum class State {
            Running,
            Invalid
        };

        void initTaskQueue();
        void moveFrom(RedisPipelineAwaitable&& other) noexcept;
        void markMovedFrom() noexcept;
        bool parseResponsesFromRingBuffer();
        void setSendError(const IOError& io_error);
        void setRecvError(const IOError& io_error);
        void setParseError();
        void setConnectionClosedError();
        void setBufferOverflowError();

        RedisClient* m_client;
        std::vector<std::vector<std::string>> m_commands;
        std::string m_encoded_batch;
        std::string m_parse_buffer;
        std::vector<RedisValue> m_values;
        State m_state;
        ProtocolSendAwaitable m_send_awaitable;
        ProtocolRecvAwaitable m_recv_awaitable;
        std::optional<RedisError> m_internal_error;

    public:
        // TimeoutSupport 需要访问此成员来设置超时错误
        std::expected<std::optional<std::vector<RedisValue>>, galay::kernel::IOError> m_result;
    };

    /**
     * @brief Redis连接等待体
     * @details 处理连接、认证、选择数据库的完整流程
     */
    class RedisConnectAwaitable : public galay::kernel::TimeoutSupport<RedisConnectAwaitable>
    {
    public:
        RedisConnectAwaitable(RedisClient& client,
                             std::string ip,
                             int32_t port,
                             std::string username,
                             std::string password,
                             int32_t db_index,
                             int version);

        bool await_ready() const noexcept {
            return false;
        }

        bool await_suspend(std::coroutine_handle<> handle);

        RedisVoidResult await_resume();

        bool isInvalid() const {
            return m_state == State::Invalid;
        }

    private:
        enum class State {
            Invalid,
            Connecting,
            Authenticating,
            SelectingDB,
            Done
        };

        RedisClient* m_client;
        std::string m_ip;
        int32_t m_port;
        std::string m_username;
        std::string m_password;
        int32_t m_db_index;
        int m_version;
        State m_state;

        std::optional<galay::kernel::ConnectAwaitable> m_connect_awaitable;
        std::optional<SendAwaitable> m_send_awaitable;
        std::optional<ReadvAwaitable> m_recv_awaitable;
        std::vector<RedisValue> m_temp_values;
        std::string m_encoded_cmd;
        std::string m_parse_buffer;
        size_t m_sent;

    public:
        // TimeoutSupport 需要访问此成员来设置超时错误
        std::expected<void, galay::kernel::IOError> m_result;
    };

    /**
     * @brief Redis客户端类
     * @details 提供异步Redis客户端功能，采用Awaitable模式
     */
    class RedisClient
    {
    public:
        RedisClient(IOScheduler* scheduler, AsyncRedisConfig config = AsyncRedisConfig::noTimeout());

        /**
         * @brief 移动构造函数
         * @warning 不要在操作进行中移动 RedisClient
         * @warning 确保所有 awaitable 都处于 Invalid 状态
         */
        RedisClient(RedisClient&& other) noexcept;

        /**
         * @brief 移动赋值运算符
         * @warning 不要在操作进行中移动 RedisClient
         * @warning 确保所有 awaitable 都处于 Invalid 状态
         */
        RedisClient& operator=(RedisClient&& other) noexcept;

        // 禁止拷贝
        RedisClient(const RedisClient&) = delete;
        RedisClient& operator=(const RedisClient&) = delete;

        // ======================== 连接方法 ========================

        /**
         * @brief 连接到Redis服务器
         * @return RedisConnectAwaitable 连接等待体
         */
        RedisConnectAwaitable connect(const std::string& url);
        RedisConnectAwaitable connect(const std::string& ip, int32_t port,
                                      const std::string& username = "",
                                      const std::string& password = "");
        RedisConnectAwaitable connect(const std::string& ip, int32_t port,
                                      const std::string& username,
                                      const std::string& password,
                                      int32_t db_index);
        RedisConnectAwaitable connect(const std::string& ip, int32_t port,
                                      const std::string& username,
                                      const std::string& password,
                                      int32_t db_index, int version);

        // ======================== 基础Redis命令 ========================

        RedisClientAwaitable execute(const std::string& cmd, const std::vector<std::string>& args);
        RedisClientAwaitable execute(const std::string& cmd,
                                     const std::vector<std::string>& args,
                                     size_t expected_replies);
        RedisClientAwaitable receive(size_t expected_replies = 1);
        RedisClientAwaitable auth(const std::string& password);
        RedisClientAwaitable auth(const std::string& username, const std::string& password);
        RedisClientAwaitable select(int32_t db_index);
        RedisClientAwaitable ping();
        RedisClientAwaitable echo(const std::string& message);

        // ======================== 发布订阅 ========================

        RedisClientAwaitable publish(const std::string& channel, const std::string& message);
        RedisClientAwaitable subscribe(const std::string& channel);
        RedisClientAwaitable subscribe(const std::vector<std::string>& channels);
        RedisClientAwaitable unsubscribe(const std::string& channel);
        RedisClientAwaitable unsubscribe(const std::vector<std::string>& channels);
        RedisClientAwaitable psubscribe(const std::string& pattern);
        RedisClientAwaitable psubscribe(const std::vector<std::string>& patterns);
        RedisClientAwaitable punsubscribe(const std::string& pattern);
        RedisClientAwaitable punsubscribe(const std::vector<std::string>& patterns);

        // ======================== 集群/主从命令 ========================

        RedisClientAwaitable role();
        RedisClientAwaitable replicaof(const std::string& host, int32_t port);
        RedisClientAwaitable readonly();
        RedisClientAwaitable readwrite();
        RedisClientAwaitable clusterInfo();
        RedisClientAwaitable clusterNodes();
        RedisClientAwaitable clusterSlots();

        // ======================== String操作 ========================

        RedisClientAwaitable get(const std::string& key);
        RedisClientAwaitable set(const std::string& key, const std::string& value);
        RedisClientAwaitable setex(const std::string& key, int64_t seconds, const std::string& value);
        RedisClientAwaitable del(const std::string& key);
        RedisClientAwaitable exists(const std::string& key);
        RedisClientAwaitable incr(const std::string& key);
        RedisClientAwaitable decr(const std::string& key);

        // ======================== Hash操作 ========================

        RedisClientAwaitable hget(const std::string& key, const std::string& field);
        RedisClientAwaitable hset(const std::string& key, const std::string& field, const std::string& value);
        RedisClientAwaitable hdel(const std::string& key, const std::string& field);
        RedisClientAwaitable hgetAll(const std::string& key);

        // ======================== List操作 ========================

        RedisClientAwaitable lpush(const std::string& key, const std::string& value);
        RedisClientAwaitable rpush(const std::string& key, const std::string& value);
        RedisClientAwaitable lpop(const std::string& key);
        RedisClientAwaitable rpop(const std::string& key);
        RedisClientAwaitable llen(const std::string& key);
        RedisClientAwaitable lrange(const std::string& key, int64_t start, int64_t stop);

        // ======================== Set操作 ========================

        RedisClientAwaitable sadd(const std::string& key, const std::string& member);
        RedisClientAwaitable srem(const std::string& key, const std::string& member);
        RedisClientAwaitable smembers(const std::string& key);
        RedisClientAwaitable scard(const std::string& key);

        // ======================== Sorted Set操作 ========================

        RedisClientAwaitable zadd(const std::string& key, double score, const std::string& member);
        RedisClientAwaitable zrem(const std::string& key, const std::string& member);
        RedisClientAwaitable zrange(const std::string& key, int64_t start, int64_t stop);
        RedisClientAwaitable zscore(const std::string& key, const std::string& member);

        // ======================== Pipeline批量操作 ========================

        RedisPipelineAwaitable pipeline(const std::vector<std::vector<std::string>>& commands);

        // ======================== 连接管理 ========================

        auto close() {
            return m_socket.close();
        }

        bool isClosed() const { return m_is_closed; }

        ~RedisClient() = default;

    private:
        friend class RedisClientAwaitable;
        friend class RedisPipelineAwaitable;
        friend class RedisConnectAwaitable;

        // 成员变量
        bool m_is_closed = false;
        TcpSocket m_socket;
        IOScheduler* m_scheduler;
        protocol::RespEncoder m_encoder;
        protocol::RespParser m_parser;
        AsyncRedisConfig m_config;
        RingBuffer m_ring_buffer;

        std::shared_ptr<spdlog::logger> m_logger;
    };

}

#endif // GALAY_REDIS_CLIENT_H
