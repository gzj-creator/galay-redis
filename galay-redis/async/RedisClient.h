#ifndef GALAY_REDIS_CLIENT_H
#define GALAY_REDIS_CLIENT_H

#include <galay-kernel/async/TcpSocket.h>
#include <galay-kernel/kernel/IOScheduler.hpp>
#include <galay-kernel/kernel/Task.h>
#include <galay-kernel/kernel/Timeout.hpp>
#include <galay-kernel/common/Host.hpp>
#include <galay-kernel/common/Error.h>
#ifdef GALAY_REDIS_SSL_ENABLED
#include <galay-ssl/async/SslAwaitableCore.h>
#endif
#include <concepts>
#include <memory>
#include <string>
#include <expected>
#include <optional>
#include <vector>
#include <coroutine>
#include <utility>
#include <span>
#include <array>
#include <string_view>
#include <sys/uio.h>
#include "galay-redis/base/RedisError.h"
#include "galay-redis/base/RedisLog.h"
#include "galay-redis/base/RedisValue.h"
#include "galay-redis/protocol/RedisProtocol.h"
#include "galay-redis/protocol/Builder.h"
#include "AsyncRedisConfig.h"
#include "RedisBufferProvider.h"

namespace galay::redis
{
    using galay::async::TcpSocket;
    using galay::kernel::IOScheduler;
    using galay::kernel::Host;
    using galay::kernel::IOError;
    using galay::kernel::IPType;
    using galay::kernel::Task;
    using galay::kernel::TaskRef;

    using Coroutine = Task<void>;

    // 类型别名
    using RedisResult = std::expected<std::vector<RedisValue>, RedisError>;
    using RedisVoidResult = std::expected<void, RedisError>;

    class RedisBorrowedCommand
    {
    public:
        // Internal fast-path packet for trusted callers.
        // encoded must outlive the entire co_await of commandBorrowed/batchBorrowed.
        // Passing temporaries is forbidden at compile-time where possible.
        explicit RedisBorrowedCommand(const std::string& encoded,
                                      size_t expected_replies = 1) noexcept
            : m_encoded(encoded)
            , m_expected_replies(expected_replies)
        {
        }

        RedisBorrowedCommand(std::string&&, size_t = 1) = delete;
        RedisBorrowedCommand(std::string_view, size_t = 1) = delete;

        [[nodiscard]] std::string_view encoded() const noexcept { return m_encoded; }
        [[nodiscard]] size_t expectedReplies() const noexcept { return m_expected_replies; }

    private:
        std::string_view m_encoded;
        size_t m_expected_replies = 1;
    };

    struct RedisConnectOptions
    {
        std::string username;
        std::string password;
        int32_t db_index = 0;
        int version = 2;
    };

    struct RedissClientConfig
    {
        std::string ca_path;
        bool verify_peer = false;
        int verify_depth = 4;
        std::string server_name;
    };

    // 前向声明
    class RedisClient;
    class RedissClient;

    namespace detail
    {
        struct RedissClientImpl;
    }

    class RedisClientBuilder
    {
    public:
        RedisClientBuilder& scheduler(IOScheduler* scheduler)
        {
            m_scheduler = scheduler;
            return *this;
        }

        RedisClientBuilder& config(AsyncRedisConfig config)
        {
            m_config = std::move(config);
            return *this;
        }

        RedisClientBuilder& sendTimeout(std::chrono::milliseconds timeout)
        {
            m_config.send_timeout = timeout;
            return *this;
        }

        RedisClientBuilder& recvTimeout(std::chrono::milliseconds timeout)
        {
            m_config.recv_timeout = timeout;
            return *this;
        }

        RedisClientBuilder& bufferSize(size_t size)
        {
            m_config.buffer_size = size;
            return *this;
        }

        RedisClientBuilder& bufferProvider(std::shared_ptr<RedisBufferProvider> provider)
        {
            m_buffer_provider = std::move(provider);
            return *this;
        }

        RedisClient build() const;

        AsyncRedisConfig buildConfig() const
        {
            return m_config;
        }

    private:
        IOScheduler* m_scheduler = nullptr;
        AsyncRedisConfig m_config = AsyncRedisConfig::noTimeout();
        std::shared_ptr<RedisBufferProvider> m_buffer_provider;
    };

    class RedissClientBuilder
    {
    public:
        RedissClientBuilder& scheduler(IOScheduler* scheduler)
        {
            m_scheduler = scheduler;
            return *this;
        }

        RedissClientBuilder& config(AsyncRedisConfig config)
        {
            m_config = std::move(config);
            return *this;
        }

        RedissClientBuilder& tlsConfig(RedissClientConfig config)
        {
            m_tls_config = std::move(config);
            return *this;
        }

        RedissClientBuilder& sendTimeout(std::chrono::milliseconds timeout)
        {
            m_config.send_timeout = timeout;
            return *this;
        }

        RedissClientBuilder& recvTimeout(std::chrono::milliseconds timeout)
        {
            m_config.recv_timeout = timeout;
            return *this;
        }

        RedissClientBuilder& bufferSize(size_t size)
        {
            m_config.buffer_size = size;
            return *this;
        }

        RedissClientBuilder& bufferProvider(std::shared_ptr<RedisBufferProvider> provider)
        {
            m_buffer_provider = std::move(provider);
            return *this;
        }

        RedissClientBuilder& caPath(std::string path)
        {
            m_tls_config.ca_path = std::move(path);
            return *this;
        }

        RedissClientBuilder& verifyPeer(bool verify_peer)
        {
            m_tls_config.verify_peer = verify_peer;
            return *this;
        }

        RedissClientBuilder& verifyDepth(int verify_depth)
        {
            m_tls_config.verify_depth = verify_depth;
            return *this;
        }

        RedissClientBuilder& serverName(std::string server_name)
        {
            m_tls_config.server_name = std::move(server_name);
            return *this;
        }

        RedissClient build() const;

        AsyncRedisConfig buildConfig() const
        {
            return m_config;
        }

        RedissClientConfig buildTlsConfig() const
        {
            return m_tls_config;
        }

    private:
        IOScheduler* m_scheduler = nullptr;
        AsyncRedisConfig m_config = AsyncRedisConfig::noTimeout();
        RedissClientConfig m_tls_config;
        std::shared_ptr<RedisBufferProvider> m_buffer_provider;
    };

    namespace detail
    {
        using RedisExchangeResult =
            std::expected<std::optional<std::vector<RedisValue>>, RedisError>;

        struct RedisExchangeSharedState
        {
            enum class Phase : uint8_t {
                Invalid,
                Start,
                Send,
                Parse,
                Done
            };

            RedisExchangeSharedState(RedisClient& client,
                                     std::string encoded_command,
                                     size_t expected_replies,
                                     bool recv_only);
            RedisExchangeSharedState(RedisClient& client,
                                     std::string_view encoded_command,
                                     size_t expected_replies,
                                     bool recv_only);
            RedisExchangeSharedState(RedisClient& client,
                                     std::span<const RedisCommandView> commands);

            RedisClient* client = nullptr;
            std::string encoded_cmd;
            std::string_view encoded_view;
            size_t expected_replies = 0;
            bool recv_only = false;
            size_t sent = 0;
            Phase phase = Phase::Start;
            std::vector<RedisValue> values;
            std::string parse_buffer;
            std::array<struct iovec, 2> read_iovecs{};
            size_t read_iov_count = 0;
            std::optional<RedisExchangeResult> result;
        };

        struct RedisExchangeMachine
        {
            using result_type = RedisExchangeResult;
            static constexpr galay::kernel::SequenceOwnerDomain kSequenceOwnerDomain =
                galay::kernel::SequenceOwnerDomain::ReadWrite;

            explicit RedisExchangeMachine(std::shared_ptr<RedisExchangeSharedState> state);

            galay::kernel::MachineAction<result_type> advance();
            void onRead(std::expected<size_t, IOError> result);
            void onWrite(std::expected<size_t, IOError> result);

        private:
            bool prepareReadWindow();
            std::expected<bool, RedisError> tryParseReplies();
            void setError(RedisError error) noexcept;
            void setSendError(const IOError& io_error) noexcept;
            void setRecvError(const IOError& io_error) noexcept;

            std::shared_ptr<RedisExchangeSharedState> m_state;
        };

        struct RedisConnectSharedState
        {
            enum class Phase : uint8_t {
                Invalid,
                Connect,
                Send,
                Parse,
                Done
            };

            enum class PendingCommand : uint8_t {
                None,
                Auth,
                Select
            };

            RedisConnectSharedState(RedisClient& client,
                                    std::string ip,
                                    int32_t port,
                                    std::string username,
                                    std::string password,
                                    int32_t db_index,
                                    int version);

            RedisClient* client = nullptr;
            std::string ip;
            int32_t port = 0;
            std::string username;
            std::string password;
            int32_t db_index = 0;
            int version = 2;
            galay::kernel::Host host;
            size_t sent = 0;
            bool auth_sent = false;
            bool select_sent = false;
            PendingCommand pending_command = PendingCommand::None;
            std::string encoded_cmd;
            std::string parse_buffer;
            std::vector<RedisValue> values;
            Phase phase = Phase::Connect;
            std::array<struct iovec, 2> read_iovecs{};
            size_t read_iov_count = 0;
            std::optional<RedisVoidResult> result;
        };

        struct RedisConnectMachine
        {
            using result_type = RedisVoidResult;
            static constexpr galay::kernel::SequenceOwnerDomain kSequenceOwnerDomain =
                galay::kernel::SequenceOwnerDomain::ReadWrite;

            explicit RedisConnectMachine(std::shared_ptr<RedisConnectSharedState> state);

            galay::kernel::MachineAction<result_type> advance();
            void onConnect(std::expected<void, IOError> result);
            void onRead(std::expected<size_t, IOError> result);
            void onWrite(std::expected<size_t, IOError> result);

        private:
            bool prepareReadWindow();
            bool prepareNextCommand();
            std::expected<bool, RedisError> tryParseReply();
            void setError(RedisError error) noexcept;
            void setConnectError(const IOError& io_error) noexcept;
            void setSendError(const IOError& io_error) noexcept;
            void setRecvError(const IOError& io_error) noexcept;

            std::shared_ptr<RedisConnectSharedState> m_state;
        };

        using RedisExchangeOperation =
            galay::kernel::StateMachineAwaitable<RedisExchangeMachine>;
        using RedisConnectOperation =
            galay::kernel::StateMachineAwaitable<RedisConnectMachine>;
    } // namespace detail

    using RedisExchangeOperation = detail::RedisExchangeOperation;
    using RedisConnectOperation = detail::RedisConnectOperation;

#ifdef GALAY_REDIS_SSL_ENABLED
    namespace detail
    {
        using RedissCommandResult =
            std::expected<std::optional<std::vector<RedisValue>>, RedisError>;

        struct RedissExchangeSharedState
        {
            enum class Phase : uint8_t {
                Invalid,
                Start,
                Send,
                Parse,
                Done
            };

            RedissExchangeSharedState(RedissClientImpl* impl,
                                      std::string encoded_command,
                                      size_t expected_replies,
                                      bool recv_only);

            RedissClientImpl* impl = nullptr;
            std::string encoded_cmd;
            size_t expected_replies = 0;
            bool recv_only = false;
            size_t sent = 0;
            Phase phase = Phase::Start;
            std::vector<RedisValue> values;
            std::string parse_buffer;
            std::array<struct iovec, 2> read_iovecs{};
            size_t read_iov_count = 0;
            char* read_buffer = nullptr;
            size_t read_length = 0;
            std::optional<RedissCommandResult> result;
        };

        struct RedissExchangeMachine
        {
            using result_type = RedissCommandResult;

            explicit RedissExchangeMachine(std::shared_ptr<RedissExchangeSharedState> state);

            galay::ssl::SslMachineAction<result_type> advance();
            void onHandshake(std::expected<void, galay::ssl::SslError> result);
            void onRecv(std::expected<galay::kernel::Bytes, galay::ssl::SslError> result);
            void onSend(std::expected<size_t, galay::ssl::SslError> result);
            void onShutdown(std::expected<void, galay::ssl::SslError> result);

        private:
            bool prepareReadWindow();
            std::expected<bool, RedisError> tryParseReplies();
            void setError(RedisError error) noexcept;
            void setSendError(const galay::ssl::SslError& ssl_error) noexcept;
            void setRecvError(const galay::ssl::SslError& ssl_error) noexcept;

            std::shared_ptr<RedissExchangeSharedState> m_state;
        };

        struct RedissConnectSharedState
        {
            enum class Phase : uint8_t {
                Invalid,
                Connect,
                Handshake,
                Send,
                Parse,
                Done
            };

            enum class PendingCommand : uint8_t {
                None,
                Auth,
                Select
            };

            RedissConnectSharedState(RedissClientImpl* impl,
                                     std::string ip,
                                     int32_t port,
                                     RedisConnectOptions options);

            RedissClientImpl* impl = nullptr;
            std::string ip;
            int32_t port = 0;
            RedisConnectOptions options;
            galay::kernel::Host host;
            size_t sent = 0;
            bool auth_sent = false;
            bool select_sent = false;
            PendingCommand pending_command = PendingCommand::None;
            std::string encoded_cmd;
            std::string parse_buffer;
            std::vector<RedisValue> values;
            Phase phase = Phase::Connect;
            std::array<struct iovec, 2> read_iovecs{};
            size_t read_iov_count = 0;
            char* read_buffer = nullptr;
            size_t read_length = 0;
            std::optional<RedisVoidResult> result;
        };

        struct RedissConnectMachine
        {
            using result_type = RedisVoidResult;
            static constexpr galay::kernel::SequenceOwnerDomain kSequenceOwnerDomain =
                galay::kernel::SequenceOwnerDomain::ReadWrite;

            explicit RedissConnectMachine(std::shared_ptr<RedissConnectSharedState> state);

            galay::kernel::MachineAction<result_type> advance();
            void onConnect(std::expected<void, IOError> result);
            void onRead(std::expected<size_t, IOError> result);
            void onWrite(std::expected<size_t, IOError> result);

        private:
            bool prepareReadWindow();
            bool prepareNextCommand();
            std::expected<bool, RedisError> tryParseReply();
            galay::kernel::MachineAction<result_type> advanceSsl();
            void setError(RedisError error) noexcept;
            void setConnectError(const IOError& io_error) noexcept;
            void setSendError(const galay::ssl::SslError& ssl_error) noexcept;
            void setRecvError(const galay::ssl::SslError& ssl_error) noexcept;
            void handleHandshakeResult(std::expected<void, galay::ssl::SslError> result);
            void handleSendResult(std::expected<size_t, galay::ssl::SslError> result);
            void handleRecvResult(std::expected<galay::kernel::Bytes, galay::ssl::SslError> result);

            std::shared_ptr<RedissConnectSharedState> m_state;
            galay::ssl::SslOperationDriver m_driver;
            bool m_ssl_active = false;
        };
        using RedissExchangeOperation =
            galay::ssl::SslStateMachineAwaitable<RedissExchangeMachine>;
        using RedissConnectOperation =
            galay::kernel::StateMachineAwaitable<RedissConnectMachine>;
    } // namespace detail
#else
    namespace detail
    {
        using RedissCommandResult =
            std::expected<std::optional<std::vector<RedisValue>>, RedisError>;
        using RedissExchangeOperation =
            galay::kernel::ReadyAwaitable<RedissCommandResult>;
        using RedissConnectOperation =
            galay::kernel::ReadyAwaitable<RedisVoidResult>;
    } // namespace detail
#endif

    /**
     * @brief Redis客户端类
     * @details 提供异步Redis客户端功能，采用Awaitable模式
     */
    class RedisClient
    {
    public:
        RedisClient(IOScheduler* scheduler,
                    AsyncRedisConfig config = AsyncRedisConfig::noTimeout(),
                    std::shared_ptr<RedisBufferProvider> buffer_provider = nullptr);

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
         * @return RedisConnectOperation 连接操作
         */
        RedisConnectOperation connect(const std::string& url);
        RedisConnectOperation connect(const std::string& ip,
                                      int32_t port,
                                      RedisConnectOptions options = {});

        // ======================== 命令执行 ========================

        RedisExchangeOperation command(RedisEncodedCommand command_packet);
        // Internal plain fast path: zero-copy send from caller-owned bytes.
        // packet must outlive the awaited operation and must not be a temporary.
        RedisExchangeOperation commandBorrowed(const RedisBorrowedCommand& packet);
        RedisExchangeOperation commandBorrowed(RedisBorrowedCommand&& packet) = delete;
        RedisExchangeOperation receive(size_t expected_replies = 1);

        // ======================== Pipeline批量操作 ========================

        RedisExchangeOperation batch(std::span<const RedisCommandView> commands);
        // Internal plain fast path for pre-encoded pipeline payloads.
        // encoded must remain valid until the awaited exchange completes.
        RedisExchangeOperation batchBorrowed(const std::string& encoded, size_t expected_replies);
        RedisExchangeOperation batchBorrowed(std::string&& encoded, size_t expected_replies) = delete;

        // ======================== 连接管理 ========================

        RedisLoggerPtr& logger() { return m_logger; }
        void setLogger(RedisLoggerPtr logger) { m_logger = std::move(logger); }
        TcpSocket& socket() { return m_socket; }
        protocol::RespParser& parser() { return m_parser; }
        RedisBufferProvider& bufferProvider() { return *m_buffer_provider; }
        const AsyncRedisConfig& asyncConfig() const { return m_config; }
        void setClosed(bool closed) { m_is_closed = closed; }

        auto close() {
            return m_socket.close();
        }

        bool isClosed() const { return m_is_closed; }

        ~RedisClient() = default;

    private:
        // 成员变量
        bool m_is_closed = false;
        TcpSocket m_socket;
        IOScheduler* m_scheduler;
        protocol::RespParser m_parser;
        AsyncRedisConfig m_config;
        std::shared_ptr<RedisBufferProvider> m_buffer_provider;

        RedisLoggerPtr m_logger;
    };

    class RedissClient
    {
    public:
        RedissClient(IOScheduler* scheduler,
                     AsyncRedisConfig config = AsyncRedisConfig::noTimeout(),
                     RedissClientConfig tls_config = {},
                     std::shared_ptr<RedisBufferProvider> buffer_provider = nullptr);
        RedissClient(RedissClient&& other) noexcept;
        RedissClient& operator=(RedissClient&& other) noexcept;
        RedissClient(const RedissClient&) = delete;
        RedissClient& operator=(const RedissClient&) = delete;
        ~RedissClient();

        detail::RedissConnectOperation connect(const std::string& url);
        detail::RedissConnectOperation connect(const std::string& ip,
                                              int32_t port,
                                              RedisConnectOptions options = {});

        detail::RedissExchangeOperation command(RedisEncodedCommand command_packet);
        detail::RedissExchangeOperation receive(size_t expected_replies = 1);
        detail::RedissExchangeOperation batch(std::span<const RedisCommandView> commands);

        RedisLoggerPtr& logger();
        void setLogger(RedisLoggerPtr logger);
        const AsyncRedisConfig& asyncConfig() const;
        const RedissClientConfig& tlsConfig() const;
        bool isClosed() const;
        void setClosed(bool closed);
        galay::kernel::CloseAwaitable close();

    private:
        std::unique_ptr<detail::RedissClientImpl> m_impl;
    };

    inline galay::redis::RedisClient galay::redis::RedisClientBuilder::build() const
    {
        return RedisClient(m_scheduler, m_config, m_buffer_provider);
    }

    inline galay::redis::RedissClient galay::redis::RedissClientBuilder::build() const
    {
        return RedissClient(m_scheduler, m_config, m_tls_config, m_buffer_provider);
    }

}

#endif // GALAY_REDIS_CLIENT_H
