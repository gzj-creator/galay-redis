#include "AsyncRedisSession.h"
#include "base/RedisError.h"
#include "base/RedisLog.h"
#include <galay-utils/system/System.hpp>
#include <regex>

namespace galay::redis
{
    // ======================== ExecuteAwaitable 实现 ========================

    ExecuteAwaitable::ExecuteAwaitable(AsyncRedisSession& session,
                                               std::string cmd,
                                               std::vector<std::string> args,
                                               size_t expected_replies)
        : m_session(session)
        , m_cmd(std::move(cmd))
        , m_args(std::move(args))
        , m_expected_replies(expected_replies)
        , m_state(State::Invalid)
        , m_sent(0)
    {
        // 编码命令
        std::vector<std::string> cmd_parts = {m_cmd};
        cmd_parts.insert(cmd_parts.end(), m_args.begin(), m_args.end());
        m_encoded_cmd = m_session.m_encoder.encodeCommand(cmd_parts);
    }

    bool ExecuteAwaitable::await_suspend(std::coroutine_handle<> handle)
    {
        if (m_state == State::Invalid) {
            // Invalid 状态，开始发送命令
            m_state = State::Sending;
            m_send_awaitable.emplace(m_session.m_socket.send(
                m_encoded_cmd.c_str() + m_sent,
                m_encoded_cmd.size() - m_sent
            ));
            return m_send_awaitable->await_suspend(handle);
        }
        else if (m_state == State::Sending) {
            // 继续发送命令（重新创建 awaitable）
            m_send_awaitable.emplace(m_session.m_socket.send(
                m_encoded_cmd.c_str() + m_sent,
                m_encoded_cmd.size() - m_sent
            ));
            return m_send_awaitable->await_suspend(handle);
        }
        else {
            // Receiving 状态，接收响应（重新创建 awaitable）
            auto iovecs = m_session.m_ring_buffer.getWriteIovecs();
            m_recv_awaitable.emplace(m_session.m_socket.readv(std::move(iovecs)));
            return m_recv_awaitable->await_suspend(handle);
        }
    }

    std::expected<std::optional<std::vector<RedisValue>>, RedisError>
    ExecuteAwaitable::await_resume()
    {
        if (m_state == State::Sending) {
            // 检查发送结果
            auto send_result = m_send_awaitable->await_resume();

            if (!send_result) {
                // 发送错误，重置为 Invalid 状态
                RedisLogDebug(m_session.m_logger, "send command failed: {}", send_result.error().message());
                m_state = State::Invalid;
                m_send_awaitable.reset();
                return std::unexpected(RedisError(RedisErrorType::REDIS_ERROR_TYPE_SEND_ERROR,
                                                 send_result.error().message()));
            }

            m_sent += send_result.value();

            if (m_sent < m_encoded_cmd.size()) {
                // 发送未完成，保持 Sending 状态
                RedisLogDebug(m_session.m_logger, "send command incomplete, continue sending");
                return std::nullopt;
            }

            // 发送完成，立刻切换到 Receiving 状态
            RedisLogDebug(m_session.m_logger, "send command completed, start receiving response");
            m_state = State::Receiving;
            m_send_awaitable.reset();
            return std::nullopt;
        }
        else {
            // Receiving 状态，检查接收结果
            auto recv_result = m_recv_awaitable->await_resume();

            if (!recv_result) {
                // 接收错误，重置为 Invalid 状态
                RedisLogDebug(m_session.m_logger, "receive response failed: {}", recv_result.error().message());
                m_state = State::Invalid;
                m_recv_awaitable.reset();
                return std::unexpected(RedisError(RedisErrorType::REDIS_ERROR_TYPE_RECV_ERROR,
                                                 recv_result.error().message()));
            }

            size_t n = recv_result.value();
            if (n == 0) {
                // 连接关闭
                RedisLogDebug(m_session.m_logger, "connection closed by peer");
                m_state = State::Invalid;
                m_recv_awaitable.reset();
                return std::unexpected(RedisError(RedisErrorType::REDIS_ERROR_TYPE_CONNECTION_CLOSED,
                                                 "Connection closed"));
            }

            m_session.m_ring_buffer.produce(n);

            // 解析响应
            while (m_values.size() < m_expected_replies) {
                auto read_iovecs = m_session.m_ring_buffer.getReadIovecs();
                if (read_iovecs.empty()) {
                    // 需要继续接收
                    RedisLogDebug(m_session.m_logger, "response incomplete, continue receiving");
                    return std::nullopt;
                }

                const char* data = static_cast<const char*>(read_iovecs[0].iov_base);
                size_t len = read_iovecs[0].iov_len;
                if (read_iovecs.size() > 1) {
                    len += read_iovecs[1].iov_len;
                }

                auto parse_result = m_session.m_parser.parse(data, len);

                if (parse_result) {
                    auto [consumed, value] = parse_result.value();
                    m_session.m_ring_buffer.consume(consumed);
                    m_values.push_back(RedisValue(value));
                } else if (parse_result.error() == protocol::ParseError::Incomplete) {
                    // 数据不完整，需要继续接收
                    RedisLogDebug(m_session.m_logger, "parse incomplete, continue receiving");
                    return std::nullopt;
                } else {
                    // 解析错误
                    RedisLogDebug(m_session.m_logger, "parse error");
                    m_state = State::Invalid;
                    m_recv_awaitable.reset();
                    return std::unexpected(RedisError(RedisErrorType::REDIS_ERROR_TYPE_PARSE_ERROR,
                                                     "Parse error"));
                }
            }

            // 所有响应都已接收完成，立刻重置为 Invalid 状态
            RedisLogDebug(m_session.m_logger, "receive response completed, reset to Invalid state");
            m_state = State::Invalid;
            m_recv_awaitable.reset();
            return std::move(m_values);
        }
    }

    // ======================== PipelineAwaitable 实现 ========================

    PipelineAwaitable::PipelineAwaitable(AsyncRedisSession& session,
                                                   std::vector<std::vector<std::string>> commands)
        : m_session(session)
        , m_commands(std::move(commands))
        , m_state(State::Invalid)
        , m_sent(0)
    {
        // 编码所有命令
        for (const auto& cmd_parts : m_commands) {
            m_encoded_batch += m_session.m_encoder.encodeCommand(cmd_parts);
        }
    }

    bool PipelineAwaitable::await_suspend(std::coroutine_handle<> handle)
    {
        if (m_state == State::Invalid) {
            // Invalid 状态，开始发送
            m_state = State::Sending;
            m_send_awaitable.emplace(m_session.m_socket.send(
                m_encoded_batch.c_str() + m_sent,
                m_encoded_batch.size() - m_sent
            ));
            return m_send_awaitable->await_suspend(handle);
        }
        else if (m_state == State::Sending) {
            // 继续发送
            m_send_awaitable.emplace(m_session.m_socket.send(
                m_encoded_batch.c_str() + m_sent,
                m_encoded_batch.size() - m_sent
            ));
            return m_send_awaitable->await_suspend(handle);
        }
        else {
            // Receiving 状态
            auto iovecs = m_session.m_ring_buffer.getWriteIovecs();
            m_recv_awaitable.emplace(m_session.m_socket.readv(std::move(iovecs)));
            return m_recv_awaitable->await_suspend(handle);
        }
    }

    std::expected<std::optional<std::vector<RedisValue>>, RedisError>
    PipelineAwaitable::await_resume()
    {
        if (m_state == State::Sending) {
            auto send_result = m_send_awaitable->await_resume();

            if (!send_result) {
                RedisLogDebug(m_session.m_logger, "send pipeline failed: {}", send_result.error().message());
                m_state = State::Invalid;
                m_send_awaitable.reset();
                return std::unexpected(RedisError(RedisErrorType::REDIS_ERROR_TYPE_SEND_ERROR,
                                                 send_result.error().message()));
            }

            m_sent += send_result.value();

            if (m_sent < m_encoded_batch.size()) {
                RedisLogDebug(m_session.m_logger, "send pipeline incomplete, continue sending");
                return std::nullopt;
            }

            RedisLogDebug(m_session.m_logger, "send pipeline completed, start receiving responses");
            m_state = State::Receiving;
            m_send_awaitable.reset();
            return std::nullopt;
        }
        else {
            auto recv_result = m_recv_awaitable->await_resume();

            if (!recv_result) {
                RedisLogDebug(m_session.m_logger, "receive pipeline responses failed: {}", recv_result.error().message());
                m_state = State::Invalid;
                m_recv_awaitable.reset();
                return std::unexpected(RedisError(RedisErrorType::REDIS_ERROR_TYPE_RECV_ERROR,
                                                 recv_result.error().message()));
            }

            size_t n = recv_result.value();
            if (n == 0) {
                RedisLogDebug(m_session.m_logger, "connection closed by peer");
                m_state = State::Invalid;
                m_recv_awaitable.reset();
                return std::unexpected(RedisError(RedisErrorType::REDIS_ERROR_TYPE_CONNECTION_CLOSED,
                                                 "Connection closed"));
            }

            m_session.m_ring_buffer.produce(n);

            // 解析所有响应
            while (m_values.size() < m_commands.size()) {
                auto read_iovecs = m_session.m_ring_buffer.getReadIovecs();
                if (read_iovecs.empty()) {
                    RedisLogDebug(m_session.m_logger, "pipeline responses incomplete, continue receiving");
                    return std::nullopt;
                }

                const char* data = static_cast<const char*>(read_iovecs[0].iov_base);
                size_t len = read_iovecs[0].iov_len;
                if (read_iovecs.size() > 1) {
                    len += read_iovecs[1].iov_len;
                }

                auto parse_result = m_session.m_parser.parse(data, len);

                if (parse_result) {
                    auto [consumed, value] = parse_result.value();
                    m_session.m_ring_buffer.consume(consumed);
                    m_values.push_back(RedisValue(value));
                } else if (parse_result.error() == protocol::ParseError::Incomplete) {
                    RedisLogDebug(m_session.m_logger, "parse incomplete, continue receiving");
                    return std::nullopt;
                } else {
                    RedisLogDebug(m_session.m_logger, "parse error");
                    m_state = State::Invalid;
                    m_recv_awaitable.reset();
                    return std::unexpected(RedisError(RedisErrorType::REDIS_ERROR_TYPE_PARSE_ERROR,
                                                     "Parse error"));
                }
            }

            RedisLogDebug(m_session.m_logger, "receive pipeline responses completed, reset to Invalid state");
            m_state = State::Invalid;
            m_recv_awaitable.reset();
            return std::move(m_values);
        }
    }

    // ======================== ConnectAwaitable 实现 ========================

    ConnectAwaitable::ConnectAwaitable(AsyncRedisSession& session,
                                                 std::string ip,
                                                 int32_t port,
                                                 std::string username,
                                                 std::string password,
                                                 int32_t db_index,
                                                 int version)
        : m_session(session)
        , m_ip(std::move(ip))
        , m_port(port)
        , m_username(std::move(username))
        , m_password(std::move(password))
        , m_db_index(db_index)
        , m_version(version)
        , m_state(State::Invalid)
    {
    }

    bool ConnectAwaitable::await_suspend(std::coroutine_handle<> handle)
    {
        if (m_state == State::Invalid) {
            // 开始连接
            m_state = State::Connecting;
            // 这里需要实现连接逻辑，暂时简化
            return false;
        }
        else if (m_state == State::Authenticating) {
            // 发送认证命令
            return false;
        }
        else if (m_state == State::SelectingDB) {
            // 发送SELECT命令
            return false;
        }

        return false;
    }

    RedisVoidResult ConnectAwaitable::await_resume()
    {
        // 简化实现，实际应该是完整的状态机
        m_state = State::Invalid;
        return {};
    }

    // ======================== AsyncRedisSession 实现 ========================

    AsyncRedisSession::AsyncRedisSession(IOScheduler* scheduler, AsyncRedisConfig config)
        : m_scheduler(scheduler), m_config(config), m_ring_buffer(config.buffer_size)
    {
        try {
            m_logger = spdlog::get("AsyncRedisLogger");
            if (!m_logger) {
                m_logger = spdlog::stdout_color_mt("AsyncRedisLogger");
            }
        } catch (const spdlog::spdlog_ex&) {
            m_logger = spdlog::get("AsyncRedisLogger");
        }
    }

    AsyncRedisSession::AsyncRedisSession(AsyncRedisSession&& other) noexcept
        : m_is_closed(other.m_is_closed)
        , m_socket(std::move(other.m_socket))
        , m_scheduler(other.m_scheduler)
        , m_encoder(std::move(other.m_encoder))
        , m_parser(std::move(other.m_parser))
        , m_config(other.m_config)
        , m_ring_buffer(std::move(other.m_ring_buffer))
        , m_execute_awaitable(std::move(other.m_execute_awaitable))
        , m_pipeline_awaitable(std::move(other.m_pipeline_awaitable))
        , m_connect_awaitable(std::move(other.m_connect_awaitable))
        , m_logger(std::move(other.m_logger))
    {
        other.m_is_closed = true;
    }

    AsyncRedisSession& AsyncRedisSession::operator=(AsyncRedisSession&& other) noexcept
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
            m_execute_awaitable.reset();
            m_pipeline_awaitable.reset();
            m_connect_awaitable.reset();

            m_logger = std::move(other.m_logger);
            other.m_is_closed = true;
        }
        return *this;
    }

    // ======================== 命令方法 ========================

    ExecuteAwaitable& AsyncRedisSession::execute(const std::string& cmd, const std::vector<std::string>& args)
    {
        // 只有当 awaitable 不存在或状态为 Invalid 时，才创建新的
        if (!m_execute_awaitable.has_value() || m_execute_awaitable->isInvalid()) {
            m_execute_awaitable.emplace(*this, cmd, args, 1);
        }
        return *m_execute_awaitable;
    }

    ExecuteAwaitable& AsyncRedisSession::auth(const std::string& password) {
        return execute("AUTH", {password});
    }

    ExecuteAwaitable& AsyncRedisSession::auth(const std::string& username, const std::string& password) {
        return execute("AUTH", {username, password});
    }

    ExecuteAwaitable& AsyncRedisSession::select(int32_t db_index) {
        return execute("SELECT", {std::to_string(db_index)});
    }

    ExecuteAwaitable& AsyncRedisSession::ping() {
        return execute("PING", {});
    }

    ExecuteAwaitable& AsyncRedisSession::echo(const std::string& message) {
        return execute("ECHO", {message});
    }

    ExecuteAwaitable& AsyncRedisSession::get(const std::string& key) {
        return execute("GET", {key});
    }

    ExecuteAwaitable& AsyncRedisSession::set(const std::string& key, const std::string& value) {
        return execute("SET", {key, value});
    }

    ExecuteAwaitable& AsyncRedisSession::setex(const std::string& key, int64_t seconds, const std::string& value) {
        return execute("SETEX", {key, std::to_string(seconds), value});
    }

    ExecuteAwaitable& AsyncRedisSession::del(const std::string& key) {
        return execute("DEL", {key});
    }

    ExecuteAwaitable& AsyncRedisSession::exists(const std::string& key) {
        return execute("EXISTS", {key});
    }

    ExecuteAwaitable& AsyncRedisSession::incr(const std::string& key) {
        return execute("INCR", {key});
    }

    ExecuteAwaitable& AsyncRedisSession::decr(const std::string& key) {
        return execute("DECR", {key});
    }

    ExecuteAwaitable& AsyncRedisSession::hget(const std::string& key, const std::string& field) {
        return execute("HGET", {key, field});
    }

    ExecuteAwaitable& AsyncRedisSession::hset(const std::string& key, const std::string& field, const std::string& value) {
        return execute("HSET", {key, field, value});
    }

    ExecuteAwaitable& AsyncRedisSession::hdel(const std::string& key, const std::string& field) {
        return execute("HDEL", {key, field});
    }

    ExecuteAwaitable& AsyncRedisSession::hgetAll(const std::string& key) {
        return execute("HGETALL", {key});
    }

    ExecuteAwaitable& AsyncRedisSession::lpush(const std::string& key, const std::string& value) {
        return execute("LPUSH", {key, value});
    }

    ExecuteAwaitable& AsyncRedisSession::rpush(const std::string& key, const std::string& value) {
        return execute("RPUSH", {key, value});
    }

    ExecuteAwaitable& AsyncRedisSession::lpop(const std::string& key) {
        return execute("LPOP", {key});
    }

    ExecuteAwaitable& AsyncRedisSession::rpop(const std::string& key) {
        return execute("RPOP", {key});
    }

    ExecuteAwaitable& AsyncRedisSession::llen(const std::string& key) {
        return execute("LLEN", {key});
    }

    ExecuteAwaitable& AsyncRedisSession::lrange(const std::string& key, int64_t start, int64_t stop) {
        return execute("LRANGE", {key, std::to_string(start), std::to_string(stop)});
    }

    ExecuteAwaitable& AsyncRedisSession::sadd(const std::string& key, const std::string& member) {
        return execute("SADD", {key, member});
    }

    ExecuteAwaitable& AsyncRedisSession::srem(const std::string& key, const std::string& member) {
        return execute("SREM", {key, member});
    }

    ExecuteAwaitable& AsyncRedisSession::smembers(const std::string& key) {
        return execute("SMEMBERS", {key});
    }

    ExecuteAwaitable& AsyncRedisSession::scard(const std::string& key) {
        return execute("SCARD", {key});
    }

    ExecuteAwaitable& AsyncRedisSession::zadd(const std::string& key, double score, const std::string& member) {
        return execute("ZADD", {key, std::to_string(score), member});
    }

    ExecuteAwaitable& AsyncRedisSession::zrem(const std::string& key, const std::string& member) {
        return execute("ZREM", {key, member});
    }

    ExecuteAwaitable& AsyncRedisSession::zrange(const std::string& key, int64_t start, int64_t stop) {
        return execute("ZRANGE", {key, std::to_string(start), std::to_string(stop)});
    }

    ExecuteAwaitable& AsyncRedisSession::zscore(const std::string& key, const std::string& member) {
        return execute("ZSCORE", {key, member});
    }

    PipelineAwaitable& AsyncRedisSession::pipeline(const std::vector<std::vector<std::string>>& commands) {
        // 只有当 awaitable 不存在或状态为 Invalid 时，才创建新的
        if (!m_pipeline_awaitable.has_value() || m_pipeline_awaitable->isInvalid()) {
            m_pipeline_awaitable.emplace(*this, commands);
        }
        return *m_pipeline_awaitable;
    }

    // ======================== 连接方法 ========================

    ConnectAwaitable& AsyncRedisSession::connect(const std::string& url)
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
                try { port = std::stoi(matches[4]); } catch(...) {}
            }
            if (matches.size() > 5 && !matches[5].str().empty()) {
                try { db_index = std::stoi(matches[5]); } catch(...) {}
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

    ConnectAwaitable& AsyncRedisSession::connect(const std::string& ip, int32_t port,
                                               const std::string& username, const std::string& password)
    {
        if (!m_connect_awaitable.has_value() || m_connect_awaitable->isInvalid()) {
            m_connect_awaitable.emplace(*this, ip, port, username, password, 0, 2);
        }
        return *m_connect_awaitable;
    }

    ConnectAwaitable& AsyncRedisSession::connect(const std::string& ip, int32_t port,
                                               const std::string& username, const std::string& password,
                                               int32_t db_index)
    {
        if (!m_connect_awaitable.has_value() || m_connect_awaitable->isInvalid()) {
            m_connect_awaitable.emplace(*this, ip, port, username, password, db_index, 2);
        }
        return *m_connect_awaitable;
    }

    ConnectAwaitable& AsyncRedisSession::connect(const std::string& ip, int32_t port,
                                               const std::string& username, const std::string& password,
                                               int32_t db_index, int version)
    {
        if (!m_connect_awaitable.has_value() || m_connect_awaitable->isInvalid()) {
            m_connect_awaitable.emplace(*this, ip, port, username, password, db_index, version);
        }
        return *m_connect_awaitable;
    }
}
