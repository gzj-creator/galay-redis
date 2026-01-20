#include "RedisClient.h"
#include "base/RedisError.h"
#include "base/RedisLog.h"
#include <galay-utils/system/System.hpp>
#include <regex>

namespace galay::redis
{
    // ======================== RedisClientAwaitable 实现 ========================

    RedisClientAwaitable::RedisClientAwaitable(RedisClient& client,
                                               std::string cmd,
                                               std::vector<std::string> args,
                                               size_t expected_replies)
        : m_client(client)
        , m_cmd(std::move(cmd))
        , m_args(std::move(args))
        , m_expected_replies(expected_replies)
        , m_state(State::Invalid)
        , m_sent(0)
    {
        // 编码命令
        std::vector<std::string> cmd_parts;
        cmd_parts.reserve(1 + m_args.size());  // 预分配内存
        cmd_parts.push_back(m_cmd);
        cmd_parts.insert(cmd_parts.end(), m_args.begin(), m_args.end());
        m_encoded_cmd = m_client.m_encoder.encodeCommand(cmd_parts);

        // 预分配响应值的内存
        m_values.reserve(m_expected_replies);
    }

    bool RedisClientAwaitable::await_suspend(std::coroutine_handle<> handle)
    {
        if (m_state == State::Invalid) {
            // Invalid 状态，开始发送命令
            m_state = State::Sending;
            m_send_awaitable.emplace(m_client.m_socket.send(
                m_encoded_cmd.c_str() + m_sent,
                m_encoded_cmd.size() - m_sent
            ));
            return m_send_awaitable->await_suspend(handle);
        }
        else if (m_state == State::Sending) {
            // 继续发送命令（重新创建 awaitable）
            m_send_awaitable.emplace(m_client.m_socket.send(
                m_encoded_cmd.c_str() + m_sent,
                m_encoded_cmd.size() - m_sent
            ));
            return m_send_awaitable->await_suspend(handle);
        }
        else {
            // Receiving 状态，接收响应（重新创建 awaitable）
            auto iovecs = m_client.m_ring_buffer.getWriteIovecs();
            m_recv_awaitable.emplace(m_client.m_socket.readv(std::move(iovecs)));
            return m_recv_awaitable->await_suspend(handle);
        }
    }

    std::expected<std::optional<std::vector<RedisValue>>, RedisError>
    RedisClientAwaitable::await_resume()
    {
        // 首先检查是否有超时错误（由 TimeoutSupport 设置）
        if (!m_result.has_value()) {
            // m_result 已被 TimeoutSupport 设置为 IOError，需要转换为 RedisError
            auto& io_error = m_result.error();
            RedisLogDebug(m_client.m_logger, "command failed with IO error: {}", io_error.message());

            // 将 IOError 转换为 RedisError
            RedisErrorType redis_error_type;
            if (io_error.code() == galay::kernel::kTimeout) {
                redis_error_type = RedisErrorType::REDIS_ERROR_TYPE_TIMEOUT_ERROR;
            } else if (io_error.code() == galay::kernel::kDisconnectError) {
                redis_error_type = RedisErrorType::REDIS_ERROR_TYPE_CONNECTION_CLOSED;
            } else {
                redis_error_type = RedisErrorType::REDIS_ERROR_TYPE_RECV_ERROR;
            }

            reset();
            return std::unexpected(RedisError(redis_error_type, io_error.message()));
        }

        if (m_state == State::Sending) {
            // 检查发送结果
            auto send_result = m_send_awaitable->await_resume();

            if (!send_result) {
                // 发送错误，清理资源并重置为 Invalid 状态
                RedisLogDebug(m_client.m_logger, "send command failed: {}", send_result.error().message());
                reset();
                return std::unexpected(RedisError(RedisErrorType::REDIS_ERROR_TYPE_SEND_ERROR,
                                                 send_result.error().message()));
            }

            m_sent += send_result.value();

            if (m_sent < m_encoded_cmd.size()) {
                // 发送未完成，保持 Sending 状态
                RedisLogDebug(m_client.m_logger, "send command incomplete, continue sending");
                return std::nullopt;
            }

            // 发送完成，立刻切换到 Receiving 状态
            RedisLogDebug(m_client.m_logger, "send command completed, start receiving response");
            m_state = State::Receiving;
            m_send_awaitable.reset();
            return std::nullopt;
        }
        else if (m_state == State::Receiving) {
            // Receiving 状态，检查接收结果
            auto recv_result = m_recv_awaitable->await_resume();

            if (!recv_result) {
                // 接收错误，清理资源并重置为 Invalid 状态
                RedisLogDebug(m_client.m_logger, "receive response failed: {}", recv_result.error().message());
                reset();
                return std::unexpected(RedisError(RedisErrorType::REDIS_ERROR_TYPE_RECV_ERROR,
                                                 recv_result.error().message()));
            }

            size_t n = recv_result.value();
            if (n == 0) {
                // 连接关闭
                RedisLogDebug(m_client.m_logger, "connection closed by peer");
                reset();
                return std::unexpected(RedisError(RedisErrorType::REDIS_ERROR_TYPE_CONNECTION_CLOSED,
                                                 "Connection closed"));
            }

            m_client.m_ring_buffer.produce(n);

            // 解析响应
            while (m_values.size() < m_expected_replies) {
                auto read_iovecs = m_client.m_ring_buffer.getReadIovecs();
                if (read_iovecs.empty()) {
                    // 需要继续接收
                    RedisLogDebug(m_client.m_logger, "response incomplete, continue receiving");
                    return std::nullopt;
                }

                const char* data = static_cast<const char*>(read_iovecs[0].iov_base);
                size_t len = read_iovecs[0].iov_len;
                if (read_iovecs.size() > 1) {
                    len += read_iovecs[1].iov_len;
                }

                auto parse_result = m_client.m_parser.parse(data, len);

                if (parse_result) {
                    auto [consumed, value] = parse_result.value();
                    m_client.m_ring_buffer.consume(consumed);
                    m_values.push_back(RedisValue(value));
                } else if (parse_result.error() == protocol::ParseError::Incomplete) {
                    // 数据不完整，需要继续接收
                    RedisLogDebug(m_client.m_logger, "parse incomplete, continue receiving");
                    return std::nullopt;
                } else {
                    // 解析错误
                    RedisLogDebug(m_client.m_logger, "parse error");
                    reset();
                    return std::unexpected(RedisError(RedisErrorType::REDIS_ERROR_TYPE_PARSE_ERROR,
                                                     "Parse error"));
                }
            }

            // 所有响应都已接收完成，立刻重置为 Invalid 状态
            RedisLogDebug(m_client.m_logger, "receive response completed, reset to Invalid state");
            auto values = std::move(m_values);
            reset();  // 清理所有资源
            return values;
        }
        else {
            // Invalid 状态，不应该被调用
            RedisLogError(m_client.m_logger, "await_resume called in Invalid state");
            reset();
            return std::unexpected(RedisError(RedisErrorType::REDIS_ERROR_TYPE_INTERNAL_ERROR,
                                             "RedisClientAwaitable in Invalid state"));
        }
    }

    // ======================== RedisPipelineAwaitable 实现 ========================

    RedisPipelineAwaitable::RedisPipelineAwaitable(RedisClient& client,
                                                   std::vector<std::vector<std::string>> commands)
        : m_client(client)
        , m_commands(std::move(commands))
        , m_state(State::Invalid)
        , m_sent(0)
    {
        // 预分配响应值的内存
        m_values.reserve(m_commands.size());

        // 编码所有命令
        for (const auto& cmd_parts : m_commands) {
            m_encoded_batch += m_client.m_encoder.encodeCommand(cmd_parts);
        }
    }

    bool RedisPipelineAwaitable::await_suspend(std::coroutine_handle<> handle)
    {
        if (m_state == State::Invalid) {
            // Invalid 状态，开始发送
            m_state = State::Sending;
            m_send_awaitable.emplace(m_client.m_socket.send(
                m_encoded_batch.c_str() + m_sent,
                m_encoded_batch.size() - m_sent
            ));
            return m_send_awaitable->await_suspend(handle);
        }
        else if (m_state == State::Sending) {
            // 继续发送
            m_send_awaitable.emplace(m_client.m_socket.send(
                m_encoded_batch.c_str() + m_sent,
                m_encoded_batch.size() - m_sent
            ));
            return m_send_awaitable->await_suspend(handle);
        }
        else {
            // Receiving 状态
            auto iovecs = m_client.m_ring_buffer.getWriteIovecs();
            m_recv_awaitable.emplace(m_client.m_socket.readv(std::move(iovecs)));
            return m_recv_awaitable->await_suspend(handle);
        }
    }

    std::expected<std::optional<std::vector<RedisValue>>, RedisError>
    RedisPipelineAwaitable::await_resume()
    {
        // 首先检查是否有超时错误（由 TimeoutSupport 设置）
        if (!m_result.has_value()) {
            // m_result 已被 TimeoutSupport 设置为 IOError，需要转换为 RedisError
            auto& io_error = m_result.error();
            RedisLogDebug(m_client.m_logger, "pipeline failed with IO error: {}", io_error.message());

            // 将 IOError 转换为 RedisError
            RedisErrorType redis_error_type;
            if (io_error.code() == galay::kernel::kTimeout) {
                redis_error_type = RedisErrorType::REDIS_ERROR_TYPE_TIMEOUT_ERROR;
            } else if (io_error.code() == galay::kernel::kDisconnectError) {
                redis_error_type = RedisErrorType::REDIS_ERROR_TYPE_CONNECTION_CLOSED;
            } else {
                redis_error_type = RedisErrorType::REDIS_ERROR_TYPE_RECV_ERROR;
            }

            reset();
            return std::unexpected(RedisError(redis_error_type, io_error.message()));
        }

        if (m_state == State::Sending) {
            auto send_result = m_send_awaitable->await_resume();

            if (!send_result) {
                RedisLogDebug(m_client.m_logger, "send pipeline failed: {}", send_result.error().message());
                reset();
                return std::unexpected(RedisError(RedisErrorType::REDIS_ERROR_TYPE_SEND_ERROR,
                                                 send_result.error().message()));
            }

            m_sent += send_result.value();

            if (m_sent < m_encoded_batch.size()) {
                RedisLogDebug(m_client.m_logger, "send pipeline incomplete, continue sending");
                return std::nullopt;
            }

            RedisLogDebug(m_client.m_logger, "send pipeline completed, start receiving responses");
            m_state = State::Receiving;
            m_send_awaitable.reset();
            return std::nullopt;
        }
        else if (m_state == State::Receiving) {
            auto recv_result = m_recv_awaitable->await_resume();

            if (!recv_result) {
                RedisLogDebug(m_client.m_logger, "receive pipeline responses failed: {}", recv_result.error().message());
                reset();
                return std::unexpected(RedisError(RedisErrorType::REDIS_ERROR_TYPE_RECV_ERROR,
                                                 recv_result.error().message()));
            }

            size_t n = recv_result.value();
            if (n == 0) {
                RedisLogDebug(m_client.m_logger, "connection closed by peer");
                reset();
                return std::unexpected(RedisError(RedisErrorType::REDIS_ERROR_TYPE_CONNECTION_CLOSED,
                                                 "Connection closed"));
            }

            m_client.m_ring_buffer.produce(n);

            // 解析所有响应
            while (m_values.size() < m_commands.size()) {
                auto read_iovecs = m_client.m_ring_buffer.getReadIovecs();
                if (read_iovecs.empty()) {
                    RedisLogDebug(m_client.m_logger, "pipeline responses incomplete, continue receiving");
                    return std::nullopt;
                }

                const char* data = static_cast<const char*>(read_iovecs[0].iov_base);
                size_t len = read_iovecs[0].iov_len;
                if (read_iovecs.size() > 1) {
                    len += read_iovecs[1].iov_len;
                }

                auto parse_result = m_client.m_parser.parse(data, len);

                if (parse_result) {
                    auto [consumed, value] = parse_result.value();
                    m_client.m_ring_buffer.consume(consumed);
                    m_values.push_back(RedisValue(value));
                } else if (parse_result.error() == protocol::ParseError::Incomplete) {
                    RedisLogDebug(m_client.m_logger, "parse incomplete, continue receiving");
                    return std::nullopt;
                } else {
                    RedisLogDebug(m_client.m_logger, "parse error");
                    reset();
                    return std::unexpected(RedisError(RedisErrorType::REDIS_ERROR_TYPE_PARSE_ERROR,
                                                     "Parse error"));
                }
            }

            RedisLogDebug(m_client.m_logger, "receive pipeline responses completed, reset to Invalid state");
            auto values = std::move(m_values);
            reset();  // 清理所有资源
            return values;
        }
        else {
            // Invalid 状态，不应该被调用
            RedisLogError(m_client.m_logger, "await_resume called in Invalid state");
            reset();
            return std::unexpected(RedisError(RedisErrorType::REDIS_ERROR_TYPE_INTERNAL_ERROR,
                                             "RedisPipelineAwaitable in Invalid state"));
        }
    }

    // ======================== RedisConnectAwaitable 实现 ========================

    RedisConnectAwaitable::RedisConnectAwaitable(RedisClient& client,
                                                 std::string ip,
                                                 int32_t port,
                                                 std::string username,
                                                 std::string password,
                                                 int32_t db_index,
                                                 int version)
        : m_client(client)
        , m_ip(std::move(ip))
        , m_port(port)
        , m_username(std::move(username))
        , m_password(std::move(password))
        , m_db_index(db_index)
        , m_version(version)
        , m_state(State::Invalid)
    {
    }

    bool RedisConnectAwaitable::await_suspend(std::coroutine_handle<> handle)
    {
        if (m_state == State::Invalid) {
            // 开始连接
            m_state = State::Connecting;

            // 创建 Host 对象并连接
            Host host(m_version == 4 ? IPType::IPV4 : IPType::IPV6, m_ip, m_port);
            m_connect_awaitable.emplace(m_client.m_socket.connect(host));
            return m_connect_awaitable->await_suspend(handle);
        }
        else if (m_state == State::Connecting) {
            // 连接已建立，检查是否需要认证
            if (m_username.empty() && m_password.empty()) {
                // 不需要认证，直接跳到数据库选择
                m_state = State::SelectingDB;
                return true;  // 立即继续
            }

            // 需要认证
            m_state = State::Authenticating;

            // 编码认证命令
            std::vector<std::string> auth_cmd;
            if (m_username.empty()) {
                auth_cmd = {"AUTH", m_password};
            } else {
                auth_cmd = {"AUTH", m_username, m_password};
            }

            m_encoded_cmd = m_client.m_encoder.encodeCommand(auth_cmd);
            m_sent = 0;

            // 发送认证命令
            m_send_awaitable.emplace(m_client.m_socket.send(
                m_encoded_cmd.c_str(),
                m_encoded_cmd.size()
            ));
            return m_send_awaitable->await_suspend(handle);
        }
        else if (m_state == State::Authenticating) {
            // 继续发送认证命令（如果未完成）
            m_send_awaitable.emplace(m_client.m_socket.send(
                m_encoded_cmd.c_str() + m_sent,
                m_encoded_cmd.size() - m_sent
            ));
            return m_send_awaitable->await_suspend(handle);
        }
        else if (m_state == State::SelectingDB) {
            // 需要选择数据库
            if (m_db_index == 0) {
                // 不需要选择数据库，直接完成
                m_state = State::Done;
                return false;
            }

            // 发送 SELECT 命令
            std::vector<std::string> select_cmd = {"SELECT", std::to_string(m_db_index)};
            m_encoded_cmd = m_client.m_encoder.encodeCommand(select_cmd);
            m_sent = 0;

            m_send_awaitable.emplace(m_client.m_socket.send(
                m_encoded_cmd.c_str(),
                m_encoded_cmd.size()
            ));
            return m_send_awaitable->await_suspend(handle);
        }

        return false;
    }

    RedisVoidResult RedisConnectAwaitable::await_resume()
    {
        if (m_state == State::Connecting) {
            // 检查连接结果
            auto connect_result = m_connect_awaitable->await_resume();

            if (!connect_result) {
                // 连接失败
                RedisLogDebug(m_client.m_logger, "Connection to {}:{} failed: {}",
                              m_ip, m_port, connect_result.error().message());
                m_state = State::Invalid;
                m_connect_awaitable.reset();
                return std::unexpected(RedisError(RedisErrorType::REDIS_ERROR_TYPE_CONNECTION_ERROR,
                                                         "Connection failed: " + connect_result.error().message()));
            }

            // 连接成功，检查是否需要认证
            if (m_username.empty() && m_password.empty()) {
                // 不需要认证，直接检查是否需要选择数据库
                if (m_db_index == 0) {
                    m_state = State::Done;
                    return {};
                }
                m_state = State::SelectingDB;
            } else {
                m_state = State::Authenticating;
            }

            m_connect_awaitable.reset();
            return {};
        }

        if (m_state == State::Authenticating) {
            // 检查认证命令发送结果
            auto send_result = m_send_awaitable->await_resume();

            if (!send_result) {
                RedisLogDebug(m_client.m_logger, "Send AUTH command failed: {}", send_result.error().message());
                m_state = State::Invalid;
                m_send_awaitable.reset();
                return std::unexpected(RedisError(RedisErrorType::REDIS_ERROR_TYPE_SEND_ERROR,
                                                         "Send AUTH failed: " + send_result.error().message()));
            }

            m_sent += send_result.value();

            if (m_sent < m_encoded_cmd.size()) {
                // 发送未完成，继续发送
                return {};
            }

            // 发送完成，接收认证响应
            auto iovecs = m_client.m_ring_buffer.getWriteIovecs();
            m_recv_awaitable.emplace(m_client.m_socket.readv(std::move(iovecs)));
            m_recv_awaitable->await_suspend(std::coroutine_handle<>::from_address(nullptr));
            m_recv_awaitable->await_resume();

            auto recv_result = m_recv_awaitable->await_resume();

            if (!recv_result) {
                RedisLogDebug(m_client.m_logger, "Receive AUTH response failed: {}", recv_result.error().message());
                m_state = State::Invalid;
                m_recv_awaitable.reset();
                return std::unexpected(RedisError(RedisErrorType::REDIS_ERROR_TYPE_RECV_ERROR,
                                                         "Receive AUTH response failed"));
            }

            size_t n = recv_result.value();
            if (n == 0) {
                RedisLogDebug(m_client.m_logger, "Connection closed during AUTH");
                m_state = State::Invalid;
                m_recv_awaitable.reset();
                return std::unexpected(RedisError(RedisErrorType::REDIS_ERROR_TYPE_CONNECTION_CLOSED,
                                                         "Connection closed"));
            }

            m_client.m_ring_buffer.produce(n);

            // 解析认证响应
            auto read_iovecs = m_client.m_ring_buffer.getReadIovecs();
            if (read_iovecs.empty()) {
                RedisLogDebug(m_client.m_logger, "AUTH response incomplete");
                return {};  // 继续接收
            }

            const char* data = static_cast<const char*>(read_iovecs[0].iov_base);
            size_t len = read_iovecs[0].iov_len;
            if (read_iovecs.size() > 1) {
                len += read_iovecs[1].iov_len;
            }

            auto parse_result = m_client.m_parser.parse(data, len);

            if (!parse_result) {
                if (parse_result.error() == protocol::ParseError::Incomplete) {
                    return {};  // 继续接收
                }
                RedisLogDebug(m_client.m_logger, "Parse AUTH response error");
                m_state = State::Invalid;
                m_recv_awaitable.reset();
                return std::unexpected(RedisError(RedisErrorType::REDIS_ERROR_TYPE_PARSE_ERROR,
                                                         "Parse AUTH response error"));
            }

            auto [consumed, value] = parse_result.value();
            m_client.m_ring_buffer.consume(consumed);

            // 检查认证结果
            if (value.isError()) {
                RedisLogDebug(m_client.m_logger, "AUTH failed: {}", value.asString());
                m_state = State::Invalid;
                m_recv_awaitable.reset();
                return std::unexpected(RedisError(RedisErrorType::REDIS_ERROR_TYPE_AUTH_ERROR,
                                                         "AUTH failed: " + value.asString()));
            }

            RedisLogDebug(m_client.m_logger, "AUTH succeeded");

            // 认证成功，检查是否需要选择数据库
            if (m_db_index == 0) {
                m_state = State::Done;
                return {};
            }

            m_state = State::SelectingDB;
            m_recv_awaitable.reset();
            return {};
        }

        if (m_state == State::SelectingDB) {
            // 检查 SELECT 命令发送结果
            auto send_result = m_send_awaitable->await_resume();

            if (!send_result) {
                RedisLogDebug(m_client.m_logger, "Send SELECT command failed: {}", send_result.error().message());
                m_state = State::Invalid;
                m_send_awaitable.reset();
                return std::unexpected(RedisError(RedisErrorType::REDIS_ERROR_TYPE_SEND_ERROR,
                                                         "Send SELECT failed"));
            }

            m_sent += send_result.value();

            if (m_sent < m_encoded_cmd.size()) {
                // 发送未完成，继续发送
                return {};
            }

            // 发送完成，接收 SELECT 响应
            auto iovecs = m_client.m_ring_buffer.getWriteIovecs();
            m_recv_awaitable.emplace(m_client.m_socket.readv(std::move(iovecs)));
            m_recv_awaitable->await_suspend(std::coroutine_handle<>::from_address(nullptr));
            m_recv_awaitable->await_resume();

            auto recv_result = m_recv_awaitable->await_resume();

            if (!recv_result) {
                RedisLogDebug(m_client.m_logger, "Receive SELECT response failed: {}", recv_result.error().message());
                m_state = State::Invalid;
                m_recv_awaitable.reset();
                return std::unexpected(RedisError(RedisErrorType::REDIS_ERROR_TYPE_RECV_ERROR,
                                                         "Receive SELECT response failed"));
            }

            size_t n = recv_result.value();
            if (n == 0) {
                RedisLogDebug(m_client.m_logger, "Connection closed during SELECT");
                m_state = State::Invalid;
                m_recv_awaitable.reset();
                return std::unexpected(RedisError(RedisErrorType::REDIS_ERROR_TYPE_CONNECTION_CLOSED,
                                                         "Connection closed"));
            }

            m_client.m_ring_buffer.produce(n);

            // 解析 SELECT 响应
            auto read_iovecs = m_client.m_ring_buffer.getReadIovecs();
            if (read_iovecs.empty()) {
                RedisLogDebug(m_client.m_logger, "SELECT response incomplete");
                return {};  // 继续接收
            }

            const char* data = static_cast<const char*>(read_iovecs[0].iov_base);
            size_t len = read_iovecs[0].iov_len;
            if (read_iovecs.size() > 1) {
                len += read_iovecs[1].iov_len;
            }

            auto parse_result = m_client.m_parser.parse(data, len);

            if (!parse_result) {
                if (parse_result.error() == protocol::ParseError::Incomplete) {
                    return {};  // 继续接收
                }
                RedisLogDebug(m_client.m_logger, "Parse SELECT response error");
                m_state = State::Invalid;
                m_recv_awaitable.reset();
                return std::unexpected(RedisError(RedisErrorType::REDIS_ERROR_TYPE_PARSE_ERROR,
                                                         "Parse SELECT response error"));
            }

            auto [consumed, value] = parse_result.value();
            m_client.m_ring_buffer.consume(consumed);

            // 检查 SELECT 结果
            if (value.isError()) {
                RedisLogDebug(m_client.m_logger, "SELECT failed: {}", value.asString());
                m_state = State::Invalid;
                m_recv_awaitable.reset();
                return std::unexpected(RedisError(RedisErrorType::REDIS_ERROR_TYPE_INVALID_ERROR,
                                                         "SELECT failed: " + value.asString()));
            }

            RedisLogDebug(m_client.m_logger, "SELECT succeeded, db_index: {}", m_db_index);

            // 完成
            m_state = State::Done;
            m_recv_awaitable.reset();
            return {};
        }

        if (m_state == State::Done) {
            m_state = State::Invalid;
            return {};
        }

        // Invalid 状态
        RedisLogError(m_client.m_logger, "await_resume called in Invalid state");
        m_state = State::Invalid;
        return std::unexpected(RedisError(RedisErrorType::REDIS_ERROR_TYPE_INTERNAL_ERROR,
                                                 "RedisConnectAwaitable in Invalid state"));
    }

    // ======================== RedisClient 实现 ========================

    RedisClient::RedisClient(IOScheduler* scheduler, AsyncRedisConfig config)
        : m_scheduler(scheduler), m_config(config), m_ring_buffer(config.buffer_size)
    {
        try {
            m_logger = spdlog::get("AsyncRedisLogger");
            if (!m_logger) {
                m_logger = spdlog::stdout_color_mt("AsyncRedisLogger");
            }
        } catch (const spdlog::spdlog_ex& ex) {
            // 尝试获取已存在的 logger
            m_logger = spdlog::get("AsyncRedisLogger");
            if (!m_logger) {
                // 最后的备选方案：使用默认 logger
                m_logger = spdlog::default_logger();
                if (m_logger) {
                    m_logger->warn("Failed to create AsyncRedisLogger, using default logger: {}", ex.what());
                }
            }
        }

        // 确保 logger 不为空
        if (!m_logger) {
            throw std::runtime_error("Failed to initialize logger for RedisClient");
        }
    }

    RedisClient::RedisClient(RedisClient&& other) noexcept
        : m_is_closed(other.m_is_closed)
        , m_socket(std::move(other.m_socket))
        , m_scheduler(other.m_scheduler)
        , m_encoder(std::move(other.m_encoder))
        , m_parser(std::move(other.m_parser))
        , m_config(other.m_config)
        , m_ring_buffer(std::move(other.m_ring_buffer))
        , m_cmd_awaitable(std::move(other.m_cmd_awaitable))
        , m_pipeline_awaitable(std::move(other.m_pipeline_awaitable))
        , m_connect_awaitable(std::move(other.m_connect_awaitable))
        , m_logger(std::move(other.m_logger))
    {
        other.m_is_closed = true;
    }

    RedisClient& RedisClient::operator=(RedisClient&& other) noexcept
    {
        if (this != &other) {
            m_is_closed = other.m_is_closed;
            m_socket = std::move(other.m_socket);
            m_scheduler = other.m_scheduler;
            m_encoder = std::move(other.m_encoder);
            m_parser = std::move(other.m_parser);
            m_config = other.m_config;
            m_ring_buffer = std::move(other.m_ring_buffer);

            // 手动处理optional成员，因为awaitable不可复制
            m_cmd_awaitable.reset();
            m_pipeline_awaitable.reset();
            m_connect_awaitable.reset();

            m_logger = std::move(other.m_logger);
            other.m_is_closed = true;
        }
        return *this;
    }

    // ======================== 命令方法 ========================

    RedisClientAwaitable& RedisClient::execute(const std::string& cmd, const std::vector<std::string>& args)
    {
        // 只有当 awaitable 不存在或状态为 Invalid 时，才创建新的
        if (!m_cmd_awaitable.has_value() || m_cmd_awaitable->isInvalid()) {
            m_cmd_awaitable.emplace(*this, cmd, args, 1);
        }
        return *m_cmd_awaitable;
    }

    RedisClientAwaitable& RedisClient::auth(const std::string& password) {
        return execute("AUTH", {password});
    }

    RedisClientAwaitable& RedisClient::auth(const std::string& username, const std::string& password) {
        return execute("AUTH", {username, password});
    }

    RedisClientAwaitable& RedisClient::select(int32_t db_index) {
        return execute("SELECT", {std::to_string(db_index)});
    }

    RedisClientAwaitable& RedisClient::ping() {
        return execute("PING", {});
    }

    RedisClientAwaitable& RedisClient::echo(const std::string& message) {
        return execute("ECHO", {message});
    }

    RedisClientAwaitable& RedisClient::get(const std::string& key) {
        return execute("GET", {key});
    }

    RedisClientAwaitable& RedisClient::set(const std::string& key, const std::string& value) {
        return execute("SET", {key, value});
    }

    RedisClientAwaitable& RedisClient::setex(const std::string& key, int64_t seconds, const std::string& value) {
        return execute("SETEX", {key, std::to_string(seconds), value});
    }

    RedisClientAwaitable& RedisClient::del(const std::string& key) {
        return execute("DEL", {key});
    }

    RedisClientAwaitable& RedisClient::exists(const std::string& key) {
        return execute("EXISTS", {key});
    }

    RedisClientAwaitable& RedisClient::incr(const std::string& key) {
        return execute("INCR", {key});
    }

    RedisClientAwaitable& RedisClient::decr(const std::string& key) {
        return execute("DECR", {key});
    }

    RedisClientAwaitable& RedisClient::hget(const std::string& key, const std::string& field) {
        return execute("HGET", {key, field});
    }

    RedisClientAwaitable& RedisClient::hset(const std::string& key, const std::string& field, const std::string& value) {
        return execute("HSET", {key, field, value});
    }

    RedisClientAwaitable& RedisClient::hdel(const std::string& key, const std::string& field) {
        return execute("HDEL", {key, field});
    }

    RedisClientAwaitable& RedisClient::hgetAll(const std::string& key) {
        return execute("HGETALL", {key});
    }

    RedisClientAwaitable& RedisClient::lpush(const std::string& key, const std::string& value) {
        return execute("LPUSH", {key, value});
    }

    RedisClientAwaitable& RedisClient::rpush(const std::string& key, const std::string& value) {
        return execute("RPUSH", {key, value});
    }

    RedisClientAwaitable& RedisClient::lpop(const std::string& key) {
        return execute("LPOP", {key});
    }

    RedisClientAwaitable& RedisClient::rpop(const std::string& key) {
        return execute("RPOP", {key});
    }

    RedisClientAwaitable& RedisClient::llen(const std::string& key) {
        return execute("LLEN", {key});
    }

    RedisClientAwaitable& RedisClient::lrange(const std::string& key, int64_t start, int64_t stop) {
        return execute("LRANGE", {key, std::to_string(start), std::to_string(stop)});
    }

    RedisClientAwaitable& RedisClient::sadd(const std::string& key, const std::string& member) {
        return execute("SADD", {key, member});
    }

    RedisClientAwaitable& RedisClient::srem(const std::string& key, const std::string& member) {
        return execute("SREM", {key, member});
    }

    RedisClientAwaitable& RedisClient::smembers(const std::string& key) {
        return execute("SMEMBERS", {key});
    }

    RedisClientAwaitable& RedisClient::scard(const std::string& key) {
        return execute("SCARD", {key});
    }

    RedisClientAwaitable& RedisClient::zadd(const std::string& key, double score, const std::string& member) {
        return execute("ZADD", {key, std::to_string(score), member});
    }

    RedisClientAwaitable& RedisClient::zrem(const std::string& key, const std::string& member) {
        return execute("ZREM", {key, member});
    }

    RedisClientAwaitable& RedisClient::zrange(const std::string& key, int64_t start, int64_t stop) {
        return execute("ZRANGE", {key, std::to_string(start), std::to_string(stop)});
    }

    RedisClientAwaitable& RedisClient::zscore(const std::string& key, const std::string& member) {
        return execute("ZSCORE", {key, member});
    }

    RedisPipelineAwaitable& RedisClient::pipeline(const std::vector<std::vector<std::string>>& commands) {
        // 只有当 awaitable 不存在或状态为 Invalid 时，才创建新的
        if (!m_pipeline_awaitable.has_value() || m_pipeline_awaitable->isInvalid()) {
            m_pipeline_awaitable.emplace(*this, commands);
        }
        return *m_pipeline_awaitable;
    }

    // ======================== 连接方法 ========================

    RedisConnectAwaitable& RedisClient::connect(const std::string& url)
    {
        // URL解析逻辑
        std::regex pattern(R"(^redis://(?:([^:@]*)(?::([^@]*))?@)?([a-zA-Z0-9\-\.]+)(?::(\d+))?(?:/(\d+))?$)");
        std::smatch matches;
        std::string username, password, host;
        int32_t port = 6379, db_index = 0;

        if (std::regex_match(url, matches, pattern)) {
            if (matches.size() > 1 && !matches[1].str().empty()) username = matches[1];
            if (matches.size() > 2 && !matches[2].str().empty()) password = matches[2];
            if (matches.size() > 3 && !matches[3].str().empty()) host = matches[3];
            if (matches.size() > 4 && !matches[4].str().empty()) {
                try {
                    port = std::stoi(matches[4]);
                } catch(const std::exception& e) {
                    RedisLogWarn(m_logger, "Failed to parse port from URL, using default 6379: {}", e.what());
                    port = 6379;
                }
            }
            if (matches.size() > 5 && !matches[5].str().empty()) {
                try {
                    db_index = std::stoi(matches[5]);
                } catch(const std::exception& e) {
                    RedisLogWarn(m_logger, "Failed to parse db_index from URL, using default 0: {}", e.what());
                    db_index = 0;
                }
            }
        }

        using namespace galay::utils;
        std::string ip;
        switch (System::checkAddressType(host)) {
        case System::AddressType::IPv4:
            ip = host;
            break;
        case System::AddressType::Domain:
            ip = System::resolveHostIPv4(host);
            break;
        default:
            ip = host;
        }

        if (!m_connect_awaitable.has_value() || m_connect_awaitable->isInvalid()) {
            m_connect_awaitable.emplace(*this, ip, port, username, password, db_index, 2);
        }
        return *m_connect_awaitable;
    }

    RedisConnectAwaitable& RedisClient::connect(const std::string& ip, int32_t port,
                                               const std::string& username, const std::string& password)
    {
        if (!m_connect_awaitable.has_value() || m_connect_awaitable->isInvalid()) {
            m_connect_awaitable.emplace(*this, ip, port, username, password, 0, 2);
        }
        return *m_connect_awaitable;
    }

    RedisConnectAwaitable& RedisClient::connect(const std::string& ip, int32_t port,
                                               const std::string& username, const std::string& password,
                                               int32_t db_index)
    {
        if (!m_connect_awaitable.has_value() || m_connect_awaitable->isInvalid()) {
            m_connect_awaitable.emplace(*this, ip, port, username, password, db_index, 2);
        }
        return *m_connect_awaitable;
    }

    RedisConnectAwaitable& RedisClient::connect(const std::string& ip, int32_t port,
                                               const std::string& username, const std::string& password,
                                               int32_t db_index, int version)
    {
        if (!m_connect_awaitable.has_value() || m_connect_awaitable->isInvalid()) {
            m_connect_awaitable.emplace(*this, ip, port, username, password, db_index, version);
        }
        return *m_connect_awaitable;
    }
}
