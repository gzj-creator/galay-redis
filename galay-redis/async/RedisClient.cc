#include "RedisClient.h"
#include "base/RedisError.h"
#include "base/RedisLog.h"
#include <galay-utils/system/System.hpp>
#include <sys/uio.h>
#include <regex>

namespace galay::redis
{
    namespace
    {
        RedisErrorType mapIoErrorToRedisType(const IOError& io_error, RedisErrorType fallback)
        {
            if (IOError::contains(io_error.code(), galay::kernel::kTimeout)) {
                return RedisErrorType::REDIS_ERROR_TYPE_TIMEOUT_ERROR;
            }
            if (IOError::contains(io_error.code(), galay::kernel::kDisconnectError)) {
                return RedisErrorType::REDIS_ERROR_TYPE_CONNECTION_CLOSED;
            }
            return fallback;
        }

        size_t decimalDigits(size_t value)
        {
            size_t digits = 1;
            while (value >= 10) {
                value /= 10;
                ++digits;
            }
            return digits;
        }

        size_t estimateRespCommandBytes(const std::vector<std::string>& cmd_parts)
        {
            size_t total = 1 + decimalDigits(cmd_parts.size()) + 2;
            for (const auto& part : cmd_parts) {
                total += 1 + decimalDigits(part.size()) + 2 + part.size() + 2;
            }
            return total;
        }

        const char* prepareParseInput(const std::vector<struct iovec>& read_iovecs,
                                      std::string& parse_buffer,
                                      size_t& len)
        {
            if (read_iovecs.empty()) {
                len = 0;
                return nullptr;
            }

            if (read_iovecs.size() == 1) {
                len = read_iovecs[0].iov_len;
                return static_cast<const char*>(read_iovecs[0].iov_base);
            }

            len = 0;
            for (const auto& iov : read_iovecs) {
                len += iov.iov_len;
            }

            parse_buffer.clear();
            parse_buffer.reserve(len);
            for (const auto& iov : read_iovecs) {
                parse_buffer.append(static_cast<const char*>(iov.iov_base), iov.iov_len);
            }
            return parse_buffer.data();
        }
    }

    // ======================== RedisClientAwaitable 实现 ========================

    RedisClientAwaitable::ProtocolSendAwaitable::ProtocolSendAwaitable(RedisClientAwaitable* owner)
        : SendAwaitable(owner && owner->m_client ? owner->m_client->m_socket.controller() : nullptr,
                        nullptr, 0)
        , m_owner(owner)
    {
        rebind(owner);
    }

    void RedisClientAwaitable::ProtocolSendAwaitable::rebind(RedisClientAwaitable* owner)
    {
        m_owner = owner;
        m_controller = (m_owner && m_owner->m_client) ? m_owner->m_client->m_socket.controller() : nullptr;
        if (m_owner) {
            m_buffer = m_owner->m_encoded_cmd.data();
            m_length = m_owner->m_encoded_cmd.size();
        } else {
            m_buffer = nullptr;
            m_length = 0;
        }
    }

#ifdef USE_IOURING
    bool RedisClientAwaitable::ProtocolSendAwaitable::handleComplete(struct io_uring_cqe* cqe, GHandle)
    {
        if (m_length == 0) {
            return true;
        }
        if (cqe == nullptr) {
            return false;
        }

        auto result = galay::kernel::io::handleSend(cqe);
        if (!result && IOError::contains(result.error().code(), galay::kernel::kNotReady)) {
            return false;
        }
        if (!result) {
            m_owner->setSendError(result.error());
            return true;
        }

        const size_t sent = result.value();
        if (sent == 0) {
            return false;
        }

        m_buffer += sent;
        m_length -= sent;
        return m_length == 0;
    }
#else
    bool RedisClientAwaitable::ProtocolSendAwaitable::handleComplete(GHandle handle)
    {
        while (m_length > 0) {
            auto result = galay::kernel::io::handleSend(handle, m_buffer, m_length);
            if (!result && IOError::contains(result.error().code(), galay::kernel::kNotReady)) {
                return false;
            }
            if (!result) {
                m_owner->setSendError(result.error());
                return true;
            }

            const size_t sent = result.value();
            if (sent == 0) {
                return false;
            }

            m_buffer += sent;
            m_length -= sent;
        }
        return true;
    }
#endif

    RedisClientAwaitable::ProtocolRecvAwaitable::ProtocolRecvAwaitable(RedisClientAwaitable* owner)
        : RecvAwaitable(owner && owner->m_client ? owner->m_client->m_socket.controller() : nullptr,
                        nullptr, 0)
        , m_owner(owner)
    {
        rebind(owner);
    }

    void RedisClientAwaitable::ProtocolRecvAwaitable::rebind(RedisClientAwaitable* owner)
    {
        m_owner = owner;
        m_controller = (m_owner && m_owner->m_client) ? m_owner->m_client->m_socket.controller() : nullptr;
        m_buffer = nullptr;
        m_length = 0;
    }

    bool RedisClientAwaitable::ProtocolRecvAwaitable::prepareRecvWindow()
    {
        auto write_iovecs = m_owner->m_client->m_ring_buffer.getWriteIovecs();
        if (write_iovecs.empty()) {
            return false;
        }

        m_buffer = static_cast<char*>(write_iovecs[0].iov_base);
        m_length = write_iovecs[0].iov_len;
        return m_length > 0;
    }

#ifdef USE_IOURING
    bool RedisClientAwaitable::ProtocolRecvAwaitable::handleComplete(struct io_uring_cqe* cqe, GHandle)
    {
        if (m_owner->parseResponsesFromRingBuffer()) {
            return true;
        }

        if (cqe == nullptr) {
            if (!prepareRecvWindow()) {
                m_owner->setBufferOverflowError();
                return true;
            }
            return false;
        }

        auto result = galay::kernel::io::handleRecv(cqe, m_buffer);
        if (!result && IOError::contains(result.error().code(), galay::kernel::kNotReady)) {
            return false;
        }
        if (!result) {
            m_owner->setRecvError(result.error());
            return true;
        }

        const size_t recv_bytes = result.value().size();
        if (recv_bytes == 0) {
            m_owner->setConnectionClosedError();
            return true;
        }

        m_owner->m_client->m_ring_buffer.produce(recv_bytes);
        if (m_owner->parseResponsesFromRingBuffer()) {
            return true;
        }

        if (!prepareRecvWindow()) {
            m_owner->setBufferOverflowError();
            return true;
        }
        return false;
    }
#else
    bool RedisClientAwaitable::ProtocolRecvAwaitable::handleComplete(GHandle handle)
    {
        if (m_owner->parseResponsesFromRingBuffer()) {
            return true;
        }

        while (true) {
            if (!prepareRecvWindow()) {
                m_owner->setBufferOverflowError();
                return true;
            }

            auto result = galay::kernel::io::handleRecv(handle, m_buffer, m_length);
            if (!result && IOError::contains(result.error().code(), galay::kernel::kNotReady)) {
                return false;
            }
            if (!result) {
                m_owner->setRecvError(result.error());
                return true;
            }

            const size_t recv_bytes = result.value().size();
            if (recv_bytes == 0) {
                m_owner->setConnectionClosedError();
                return true;
            }

            m_owner->m_client->m_ring_buffer.produce(recv_bytes);
            if (m_owner->parseResponsesFromRingBuffer()) {
                return true;
            }
        }
    }
#endif

    RedisClientAwaitable::RedisClientAwaitable(RedisClient& client,
                                               std::string cmd,
                                               std::vector<std::string> args,
                                               size_t expected_replies,
                                               bool recv_only)
        : CustomAwaitable(client.m_socket.controller())
        , m_client(&client)
        , m_cmd(std::move(cmd))
        , m_args(std::move(args))
        , m_expected_replies(expected_replies)
        , m_recv_only(recv_only)
        , m_state(State::Running)
        , m_send_awaitable(this)
        , m_recv_awaitable(this)
        , m_result(std::nullopt)
    {
        if (!m_recv_only) {
            std::vector<std::string> cmd_parts;
            cmd_parts.reserve(1 + m_args.size());
            cmd_parts.push_back(m_cmd);
            cmd_parts.insert(cmd_parts.end(), m_args.begin(), m_args.end());
            m_encoded_cmd = m_client->m_encoder.encodeCommand(cmd_parts);
        }

        m_values.reserve(m_expected_replies);
        m_send_awaitable.rebind(this);
        m_recv_awaitable.rebind(this);
        initTaskQueue();
    }

    RedisClientAwaitable::RedisClientAwaitable(RedisClientAwaitable&& other) noexcept
        : CustomAwaitable(other.m_client ? other.m_client->m_socket.controller() : nullptr)
        , m_client(nullptr)
        , m_expected_replies(0)
        , m_recv_only(false)
        , m_state(State::Invalid)
        , m_send_awaitable(this)
        , m_recv_awaitable(this)
    {
        moveFrom(std::move(other));
    }

    RedisClientAwaitable& RedisClientAwaitable::operator=(RedisClientAwaitable&& other) noexcept
    {
        if (this != &other) {
            moveFrom(std::move(other));
        }
        return *this;
    }

    void RedisClientAwaitable::moveFrom(RedisClientAwaitable&& other) noexcept
    {
        m_controller = other.m_controller;
        m_waker = std::move(other.m_waker);

        m_client = other.m_client;
        m_cmd = std::move(other.m_cmd);
        m_args = std::move(other.m_args);
        m_encoded_cmd = std::move(other.m_encoded_cmd);
        m_parse_buffer = std::move(other.m_parse_buffer);
        m_expected_replies = other.m_expected_replies;
        m_recv_only = other.m_recv_only;
        m_values = std::move(other.m_values);
        m_state = other.m_state;
        m_internal_error = std::move(other.m_internal_error);
        m_result = std::move(other.m_result);

        m_send_awaitable.rebind(this);
        m_recv_awaitable.rebind(this);
        if (m_state == State::Running) {
            initTaskQueue();
        } else {
            m_tasks.clear();
            m_cursor = 0;
        }

        other.markMovedFrom();
    }

    void RedisClientAwaitable::markMovedFrom() noexcept
    {
        m_state = State::Invalid;
        m_client = nullptr;
        m_tasks.clear();
        m_cursor = 0;
        m_internal_error.reset();
        m_values.clear();
        m_parse_buffer.clear();
        m_recv_only = false;
        m_result = std::nullopt;
        m_send_awaitable.rebind(this);
        m_recv_awaitable.rebind(this);
    }

    void RedisClientAwaitable::initTaskQueue()
    {
        m_tasks.clear();
        m_cursor = 0;
        if (!m_recv_only) {
            addTask(IOEventType::SEND, &m_send_awaitable);
        }
        addTask(IOEventType::RECV, &m_recv_awaitable);
    }

    bool RedisClientAwaitable::parseResponsesFromRingBuffer()
    {
        while (m_values.size() < m_expected_replies) {
            auto read_iovecs = m_client->m_ring_buffer.getReadIovecs();
            if (read_iovecs.empty()) {
                return false;
            }

            size_t len = 0;
            const char* data = prepareParseInput(read_iovecs, m_parse_buffer, len);
            if (data == nullptr) {
                return false;
            }

            auto parse_result = m_client->m_parser.parse(data, len);
            if (parse_result) {
                auto [consumed, value] = parse_result.value();
                m_client->m_ring_buffer.consume(consumed);
                m_values.push_back(RedisValue(value));
                continue;
            }

            if (parse_result.error() == protocol::ParseError::Incomplete) {
                return false;
            }

            setParseError();
            return true;
        }
        return true;
    }

    void RedisClientAwaitable::setSendError(const IOError& io_error)
    {
        m_internal_error = RedisError(RedisErrorType::REDIS_ERROR_TYPE_SEND_ERROR, io_error.message());
    }

    void RedisClientAwaitable::setRecvError(const IOError& io_error)
    {
        if (IOError::contains(io_error.code(), galay::kernel::kDisconnectError)) {
            setConnectionClosedError();
            return;
        }
        m_internal_error = RedisError(RedisErrorType::REDIS_ERROR_TYPE_RECV_ERROR, io_error.message());
    }

    void RedisClientAwaitable::setParseError()
    {
        m_internal_error = RedisError(RedisErrorType::REDIS_ERROR_TYPE_PARSE_ERROR, "Parse error");
    }

    void RedisClientAwaitable::setConnectionClosedError()
    {
        m_internal_error = RedisError(RedisErrorType::REDIS_ERROR_TYPE_CONNECTION_CLOSED, "Connection closed");
    }

    void RedisClientAwaitable::setBufferOverflowError()
    {
        m_internal_error = RedisError(RedisErrorType::REDIS_ERROR_TYPE_BUFFER_OVERFLOW_ERROR,
                                      "Ring buffer exhausted before parsing complete response");
    }

    std::expected<std::optional<std::vector<RedisValue>>, RedisError> RedisClientAwaitable::await_resume()
    {
        onCompleted();

        if (m_state == State::Invalid) {
            return std::unexpected(RedisError(RedisErrorType::REDIS_ERROR_TYPE_INTERNAL_ERROR,
                                              "RedisClientAwaitable in Invalid state"));
        }

        if (!m_result.has_value()) {
            auto& io_error = m_result.error();
            const auto redis_error_type = mapIoErrorToRedisType(
                io_error, RedisErrorType::REDIS_ERROR_TYPE_RECV_ERROR);
            RedisLogDebug(m_client->m_logger, "command failed with IO error: {}", io_error.message());
            reset();
            return std::unexpected(RedisError(redis_error_type, io_error.message()));
        }

        if (m_internal_error.has_value()) {
            auto error = std::move(m_internal_error.value());
            reset();  // 清理所有资源
            return std::unexpected(std::move(error));
        }

        auto values = std::move(m_values);
        reset();
        return std::optional<std::vector<RedisValue>>(std::move(values));
    }

    // ======================== RedisPipelineAwaitable 实现 ========================

    RedisPipelineAwaitable::ProtocolSendAwaitable::ProtocolSendAwaitable(RedisPipelineAwaitable* owner)
        : SendAwaitable(owner && owner->m_client ? owner->m_client->m_socket.controller() : nullptr,
                        nullptr, 0)
        , m_owner(owner)
    {
        rebind(owner);
    }

    void RedisPipelineAwaitable::ProtocolSendAwaitable::rebind(RedisPipelineAwaitable* owner)
    {
        m_owner = owner;
        m_controller = (m_owner && m_owner->m_client) ? m_owner->m_client->m_socket.controller() : nullptr;
        if (m_owner) {
            m_buffer = m_owner->m_encoded_batch.data();
            m_length = m_owner->m_encoded_batch.size();
        } else {
            m_buffer = nullptr;
            m_length = 0;
        }
    }

#ifdef USE_IOURING
    bool RedisPipelineAwaitable::ProtocolSendAwaitable::handleComplete(struct io_uring_cqe* cqe, GHandle)
    {
        if (m_length == 0) {
            return true;
        }
        if (cqe == nullptr) {
            return false;
        }

        auto result = galay::kernel::io::handleSend(cqe);
        if (!result && IOError::contains(result.error().code(), galay::kernel::kNotReady)) {
            return false;
        }
        if (!result) {
            m_owner->setSendError(result.error());
            return true;
        }

        const size_t sent = result.value();
        if (sent == 0) {
            return false;
        }

        m_buffer += sent;
        m_length -= sent;
        return m_length == 0;
    }
#else
    bool RedisPipelineAwaitable::ProtocolSendAwaitable::handleComplete(GHandle handle)
    {
        while (m_length > 0) {
            auto result = galay::kernel::io::handleSend(handle, m_buffer, m_length);
            if (!result && IOError::contains(result.error().code(), galay::kernel::kNotReady)) {
                return false;
            }
            if (!result) {
                m_owner->setSendError(result.error());
                return true;
            }

            const size_t sent = result.value();
            if (sent == 0) {
                return false;
            }

            m_buffer += sent;
            m_length -= sent;
        }
        return true;
    }
#endif

    RedisPipelineAwaitable::ProtocolRecvAwaitable::ProtocolRecvAwaitable(RedisPipelineAwaitable* owner)
        : RecvAwaitable(owner && owner->m_client ? owner->m_client->m_socket.controller() : nullptr,
                        nullptr, 0)
        , m_owner(owner)
    {
        rebind(owner);
    }

    void RedisPipelineAwaitable::ProtocolRecvAwaitable::rebind(RedisPipelineAwaitable* owner)
    {
        m_owner = owner;
        m_controller = (m_owner && m_owner->m_client) ? m_owner->m_client->m_socket.controller() : nullptr;
        m_buffer = nullptr;
        m_length = 0;
    }

    bool RedisPipelineAwaitable::ProtocolRecvAwaitable::prepareRecvWindow()
    {
        auto write_iovecs = m_owner->m_client->m_ring_buffer.getWriteIovecs();
        if (write_iovecs.empty()) {
            return false;
        }

        m_buffer = static_cast<char*>(write_iovecs[0].iov_base);
        m_length = write_iovecs[0].iov_len;
        return m_length > 0;
    }

#ifdef USE_IOURING
    bool RedisPipelineAwaitable::ProtocolRecvAwaitable::handleComplete(struct io_uring_cqe* cqe, GHandle)
    {
        if (m_owner->parseResponsesFromRingBuffer()) {
            return true;
        }

        if (cqe == nullptr) {
            if (!prepareRecvWindow()) {
                m_owner->setBufferOverflowError();
                return true;
            }
            return false;
        }

        auto result = galay::kernel::io::handleRecv(cqe, m_buffer);
        if (!result && IOError::contains(result.error().code(), galay::kernel::kNotReady)) {
            return false;
        }
        if (!result) {
            m_owner->setRecvError(result.error());
            return true;
        }

        const size_t recv_bytes = result.value().size();
        if (recv_bytes == 0) {
            m_owner->setConnectionClosedError();
            return true;
        }

        m_owner->m_client->m_ring_buffer.produce(recv_bytes);
        if (m_owner->parseResponsesFromRingBuffer()) {
            return true;
        }

        if (!prepareRecvWindow()) {
            m_owner->setBufferOverflowError();
            return true;
        }
        return false;
    }
#else
    bool RedisPipelineAwaitable::ProtocolRecvAwaitable::handleComplete(GHandle handle)
    {
        if (m_owner->parseResponsesFromRingBuffer()) {
            return true;
        }

        while (true) {
            if (!prepareRecvWindow()) {
                m_owner->setBufferOverflowError();
                return true;
            }

            auto result = galay::kernel::io::handleRecv(handle, m_buffer, m_length);
            if (!result && IOError::contains(result.error().code(), galay::kernel::kNotReady)) {
                return false;
            }
            if (!result) {
                m_owner->setRecvError(result.error());
                return true;
            }

            const size_t recv_bytes = result.value().size();
            if (recv_bytes == 0) {
                m_owner->setConnectionClosedError();
                return true;
            }

            m_owner->m_client->m_ring_buffer.produce(recv_bytes);
            if (m_owner->parseResponsesFromRingBuffer()) {
                return true;
            }
        }
    }
#endif

    RedisPipelineAwaitable::RedisPipelineAwaitable(RedisClient& client,
                                                   std::vector<std::vector<std::string>> commands)
        : CustomAwaitable(client.m_socket.controller())
        , m_client(&client)
        , m_commands(std::move(commands))
        , m_state(State::Running)
        , m_send_awaitable(this)
        , m_recv_awaitable(this)
        , m_result(std::nullopt)
    {
        m_values.reserve(m_commands.size());
        size_t estimated_batch_size = 0;
        for (const auto& cmd_parts : m_commands) {
            estimated_batch_size += estimateRespCommandBytes(cmd_parts);
        }
        m_encoded_batch.reserve(estimated_batch_size);

        for (const auto& cmd_parts : m_commands) {
            m_encoded_batch += m_client->m_encoder.encodeCommand(cmd_parts);
        }

        m_send_awaitable.rebind(this);
        m_recv_awaitable.rebind(this);
        initTaskQueue();
    }

    RedisPipelineAwaitable::RedisPipelineAwaitable(RedisPipelineAwaitable&& other) noexcept
        : CustomAwaitable(other.m_client ? other.m_client->m_socket.controller() : nullptr)
        , m_client(nullptr)
        , m_state(State::Invalid)
        , m_send_awaitable(this)
        , m_recv_awaitable(this)
    {
        moveFrom(std::move(other));
    }

    RedisPipelineAwaitable& RedisPipelineAwaitable::operator=(RedisPipelineAwaitable&& other) noexcept
    {
        if (this != &other) {
            moveFrom(std::move(other));
        }
        return *this;
    }

    void RedisPipelineAwaitable::moveFrom(RedisPipelineAwaitable&& other) noexcept
    {
        m_controller = other.m_controller;
        m_waker = std::move(other.m_waker);

        m_client = other.m_client;
        m_commands = std::move(other.m_commands);
        m_encoded_batch = std::move(other.m_encoded_batch);
        m_parse_buffer = std::move(other.m_parse_buffer);
        m_values = std::move(other.m_values);
        m_state = other.m_state;
        m_internal_error = std::move(other.m_internal_error);
        m_result = std::move(other.m_result);

        m_send_awaitable.rebind(this);
        m_recv_awaitable.rebind(this);
        if (m_state == State::Running) {
            initTaskQueue();
        } else {
            m_tasks.clear();
            m_cursor = 0;
        }

        other.markMovedFrom();
    }

    void RedisPipelineAwaitable::markMovedFrom() noexcept
    {
        m_state = State::Invalid;
        m_client = nullptr;
        m_tasks.clear();
        m_cursor = 0;
        m_internal_error.reset();
        m_values.clear();
        m_parse_buffer.clear();
        m_result = std::nullopt;
        m_send_awaitable.rebind(this);
        m_recv_awaitable.rebind(this);
    }

    void RedisPipelineAwaitable::initTaskQueue()
    {
        m_tasks.clear();
        m_cursor = 0;
        addTask(IOEventType::SEND, &m_send_awaitable);
        addTask(IOEventType::RECV, &m_recv_awaitable);
    }

    bool RedisPipelineAwaitable::parseResponsesFromRingBuffer()
    {
        while (m_values.size() < m_commands.size()) {
            auto read_iovecs = m_client->m_ring_buffer.getReadIovecs();
            if (read_iovecs.empty()) {
                return false;
            }

            size_t len = 0;
            const char* data = prepareParseInput(read_iovecs, m_parse_buffer, len);
            if (data == nullptr) {
                return false;
            }

            auto parse_result = m_client->m_parser.parse(data, len);
            if (parse_result) {
                auto [consumed, value] = parse_result.value();
                m_client->m_ring_buffer.consume(consumed);
                m_values.push_back(RedisValue(value));
                continue;
            }

            if (parse_result.error() == protocol::ParseError::Incomplete) {
                return false;
            }

            setParseError();
            return true;
        }
        return true;
    }

    void RedisPipelineAwaitable::setSendError(const IOError& io_error)
    {
        m_internal_error = RedisError(RedisErrorType::REDIS_ERROR_TYPE_SEND_ERROR, io_error.message());
    }

    void RedisPipelineAwaitable::setRecvError(const IOError& io_error)
    {
        if (IOError::contains(io_error.code(), galay::kernel::kDisconnectError)) {
            setConnectionClosedError();
            return;
        }
        m_internal_error = RedisError(RedisErrorType::REDIS_ERROR_TYPE_RECV_ERROR, io_error.message());
    }

    void RedisPipelineAwaitable::setParseError()
    {
        m_internal_error = RedisError(RedisErrorType::REDIS_ERROR_TYPE_PARSE_ERROR, "Parse error");
    }

    void RedisPipelineAwaitable::setConnectionClosedError()
    {
        m_internal_error = RedisError(RedisErrorType::REDIS_ERROR_TYPE_CONNECTION_CLOSED, "Connection closed");
    }

    void RedisPipelineAwaitable::setBufferOverflowError()
    {
        m_internal_error = RedisError(RedisErrorType::REDIS_ERROR_TYPE_BUFFER_OVERFLOW_ERROR,
                                      "Ring buffer exhausted before parsing complete pipeline responses");
    }

    std::expected<std::optional<std::vector<RedisValue>>, RedisError> RedisPipelineAwaitable::await_resume()
    {
        onCompleted();

        if (m_state == State::Invalid) {
            return std::unexpected(RedisError(RedisErrorType::REDIS_ERROR_TYPE_INTERNAL_ERROR,
                                              "RedisPipelineAwaitable in Invalid state"));
        }

        if (!m_result.has_value()) {
            auto& io_error = m_result.error();
            const auto redis_error_type = mapIoErrorToRedisType(
                io_error, RedisErrorType::REDIS_ERROR_TYPE_RECV_ERROR);
            RedisLogDebug(m_client->m_logger, "pipeline failed with IO error: {}", io_error.message());
            reset();
            return std::unexpected(RedisError(redis_error_type, io_error.message()));
        }

        if (m_internal_error.has_value()) {
            auto error = std::move(m_internal_error.value());
            reset();  // 清理所有资源
            return std::unexpected(std::move(error));
        }

        auto values = std::move(m_values);
        reset();
        return std::optional<std::vector<RedisValue>>(std::move(values));
    }

    // ======================== RedisConnectAwaitable 实现 ========================

    RedisConnectAwaitable::RedisConnectAwaitable(RedisClient& client,
                                                 std::string ip,
                                                 int32_t port,
                                                 std::string username,
                                                 std::string password,
                                                 int32_t db_index,
                                                 int version)
        : m_client(&client)
        , m_ip(std::move(ip))
        , m_port(port)
        , m_username(std::move(username))
        , m_password(std::move(password))
        , m_db_index(db_index)
        , m_version(version)
        , m_state(State::Invalid)
        , m_result({})
    {
    }

    bool RedisConnectAwaitable::await_suspend(std::coroutine_handle<> handle)
    {
        if (m_state == State::Invalid) {
            // 开始连接
            m_state = State::Connecting;

            // version 历史上常被传入 RESP 版本(2/3)，这里只把 6 解释为 IPv6，其余默认 IPv4
            Host host(m_version == 6 ? IPType::IPV6 : IPType::IPV4, m_ip, m_port);
            m_connect_awaitable.emplace(m_client->m_socket.connect(host));
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

            m_encoded_cmd = m_client->m_encoder.encodeCommand(auth_cmd);
            m_sent = 0;

            // 发送认证命令
            m_send_awaitable.emplace(m_client->m_socket.send(
                m_encoded_cmd.c_str(),
                m_encoded_cmd.size()
            ));
            return m_send_awaitable->await_suspend(handle);
        }
        else if (m_state == State::Authenticating) {
            // 继续发送认证命令（如果未完成）
            m_send_awaitable.emplace(m_client->m_socket.send(
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
            m_encoded_cmd = m_client->m_encoder.encodeCommand(select_cmd);
            m_sent = 0;

            m_send_awaitable.emplace(m_client->m_socket.send(
                m_encoded_cmd.c_str(),
                m_encoded_cmd.size()
            ));
            return m_send_awaitable->await_suspend(handle);
        }

        return false;
    }

    RedisVoidResult RedisConnectAwaitable::await_resume()
    {
        if (!m_result.has_value()) {
            auto io_error = m_result.error();
            m_state = State::Invalid;
            m_connect_awaitable.reset();
            m_send_awaitable.reset();
            m_recv_awaitable.reset();
            return std::unexpected(
                RedisError(mapIoErrorToRedisType(io_error, RedisErrorType::REDIS_ERROR_TYPE_TIMEOUT_ERROR),
                           io_error.message()));
        }

        if (m_state == State::Connecting) {
            // 检查连接结果
            auto connect_result = m_connect_awaitable->await_resume();

            if (!connect_result) {
                // 连接失败
                RedisLogDebug(m_client->m_logger, "Connection to {}:{} failed: {}",
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
                RedisLogDebug(m_client->m_logger, "Send AUTH command failed: {}", send_result.error().message());
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
            auto iovecs = m_client->m_ring_buffer.getWriteIovecs();
            m_recv_awaitable.emplace(m_client->m_socket.readv(std::move(iovecs)));
            m_recv_awaitable->await_suspend(std::coroutine_handle<>::from_address(nullptr));
            m_recv_awaitable->await_resume();

            auto recv_result = m_recv_awaitable->await_resume();

            if (!recv_result) {
                RedisLogDebug(m_client->m_logger, "Receive AUTH response failed: {}", recv_result.error().message());
                m_state = State::Invalid;
                m_recv_awaitable.reset();
                return std::unexpected(RedisError(RedisErrorType::REDIS_ERROR_TYPE_RECV_ERROR,
                                                         "Receive AUTH response failed"));
            }

            size_t n = recv_result.value();
            if (n == 0) {
                RedisLogDebug(m_client->m_logger, "Connection closed during AUTH");
                m_state = State::Invalid;
                m_recv_awaitable.reset();
                return std::unexpected(RedisError(RedisErrorType::REDIS_ERROR_TYPE_CONNECTION_CLOSED,
                                                         "Connection closed"));
            }

            m_client->m_ring_buffer.produce(n);

            // 解析认证响应
            auto read_iovecs = m_client->m_ring_buffer.getReadIovecs();
            if (read_iovecs.empty()) {
                RedisLogDebug(m_client->m_logger, "AUTH response incomplete");
                return {};  // 继续接收
            }

            size_t len = 0;
            const char* data = prepareParseInput(read_iovecs, m_parse_buffer, len);
            if (data == nullptr) {
                RedisLogDebug(m_client->m_logger, "AUTH response parse buffer unavailable");
                return {};
            }

            auto parse_result = m_client->m_parser.parse(data, len);

            if (!parse_result) {
                if (parse_result.error() == protocol::ParseError::Incomplete) {
                    return {};  // 继续接收
                }
                RedisLogDebug(m_client->m_logger, "Parse AUTH response error");
                m_state = State::Invalid;
                m_recv_awaitable.reset();
                return std::unexpected(RedisError(RedisErrorType::REDIS_ERROR_TYPE_PARSE_ERROR,
                                                         "Parse AUTH response error"));
            }

            auto [consumed, value] = parse_result.value();
            m_client->m_ring_buffer.consume(consumed);

            // 检查认证结果
            if (value.isError()) {
                RedisLogDebug(m_client->m_logger, "AUTH failed: {}", value.asString());
                m_state = State::Invalid;
                m_recv_awaitable.reset();
                return std::unexpected(RedisError(RedisErrorType::REDIS_ERROR_TYPE_AUTH_ERROR,
                                                         "AUTH failed: " + value.asString()));
            }

            RedisLogDebug(m_client->m_logger, "AUTH succeeded");

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
                RedisLogDebug(m_client->m_logger, "Send SELECT command failed: {}", send_result.error().message());
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
            auto iovecs = m_client->m_ring_buffer.getWriteIovecs();
            m_recv_awaitable.emplace(m_client->m_socket.readv(std::move(iovecs)));
            m_recv_awaitable->await_suspend(std::coroutine_handle<>::from_address(nullptr));
            m_recv_awaitable->await_resume();

            auto recv_result = m_recv_awaitable->await_resume();

            if (!recv_result) {
                RedisLogDebug(m_client->m_logger, "Receive SELECT response failed: {}", recv_result.error().message());
                m_state = State::Invalid;
                m_recv_awaitable.reset();
                return std::unexpected(RedisError(RedisErrorType::REDIS_ERROR_TYPE_RECV_ERROR,
                                                         "Receive SELECT response failed"));
            }

            size_t n = recv_result.value();
            if (n == 0) {
                RedisLogDebug(m_client->m_logger, "Connection closed during SELECT");
                m_state = State::Invalid;
                m_recv_awaitable.reset();
                return std::unexpected(RedisError(RedisErrorType::REDIS_ERROR_TYPE_CONNECTION_CLOSED,
                                                         "Connection closed"));
            }

            m_client->m_ring_buffer.produce(n);

            // 解析 SELECT 响应
            auto read_iovecs = m_client->m_ring_buffer.getReadIovecs();
            if (read_iovecs.empty()) {
                RedisLogDebug(m_client->m_logger, "SELECT response incomplete");
                return {};  // 继续接收
            }

            size_t len = 0;
            const char* data = prepareParseInput(read_iovecs, m_parse_buffer, len);
            if (data == nullptr) {
                RedisLogDebug(m_client->m_logger, "SELECT response parse buffer unavailable");
                return {};
            }

            auto parse_result = m_client->m_parser.parse(data, len);

            if (!parse_result) {
                if (parse_result.error() == protocol::ParseError::Incomplete) {
                    return {};  // 继续接收
                }
                RedisLogDebug(m_client->m_logger, "Parse SELECT response error");
                m_state = State::Invalid;
                m_recv_awaitable.reset();
                return std::unexpected(RedisError(RedisErrorType::REDIS_ERROR_TYPE_PARSE_ERROR,
                                                         "Parse SELECT response error"));
            }

            auto [consumed, value] = parse_result.value();
            m_client->m_ring_buffer.consume(consumed);

            // 检查 SELECT 结果
            if (value.isError()) {
                RedisLogDebug(m_client->m_logger, "SELECT failed: {}", value.asString());
                m_state = State::Invalid;
                m_recv_awaitable.reset();
                return std::unexpected(RedisError(RedisErrorType::REDIS_ERROR_TYPE_INVALID_ERROR,
                                                         "SELECT failed: " + value.asString()));
            }

            RedisLogDebug(m_client->m_logger, "SELECT succeeded, db_index: {}", m_db_index);

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
        RedisLogError(m_client->m_logger, "await_resume called in Invalid state");
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
        return execute(cmd, args, 1);
    }

    RedisClientAwaitable& RedisClient::execute(const std::string& cmd,
                                               const std::vector<std::string>& args,
                                               size_t expected_replies)
    {
        // 只有当 awaitable 不存在或状态为 Invalid 时，才创建新的
        if (!m_cmd_awaitable.has_value() || m_cmd_awaitable->isInvalid()) {
            m_cmd_awaitable.emplace(*this, cmd, args, expected_replies, false);
        }
        return *m_cmd_awaitable;
    }

    RedisClientAwaitable& RedisClient::receive(size_t expected_replies)
    {
        if (!m_cmd_awaitable.has_value() || m_cmd_awaitable->isInvalid()) {
            m_cmd_awaitable.emplace(*this, "", std::vector<std::string>{}, expected_replies, true);
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

    RedisClientAwaitable& RedisClient::publish(const std::string& channel, const std::string& message)
    {
        return execute("PUBLISH", {channel, message});
    }

    RedisClientAwaitable& RedisClient::subscribe(const std::string& channel)
    {
        return execute("SUBSCRIBE", {channel}, 1);
    }

    RedisClientAwaitable& RedisClient::subscribe(const std::vector<std::string>& channels)
    {
        if (channels.empty()) {
            return execute("SUBSCRIBE", {}, 1);
        }
        return execute("SUBSCRIBE", channels, channels.size());
    }

    RedisClientAwaitable& RedisClient::unsubscribe(const std::string& channel)
    {
        return execute("UNSUBSCRIBE", {channel}, 1);
    }

    RedisClientAwaitable& RedisClient::unsubscribe(const std::vector<std::string>& channels)
    {
        if (channels.empty()) {
            return execute("UNSUBSCRIBE", {}, 1);
        }
        return execute("UNSUBSCRIBE", channels, channels.size());
    }

    RedisClientAwaitable& RedisClient::psubscribe(const std::string& pattern)
    {
        return execute("PSUBSCRIBE", {pattern}, 1);
    }

    RedisClientAwaitable& RedisClient::psubscribe(const std::vector<std::string>& patterns)
    {
        if (patterns.empty()) {
            return execute("PSUBSCRIBE", {}, 1);
        }
        return execute("PSUBSCRIBE", patterns, patterns.size());
    }

    RedisClientAwaitable& RedisClient::punsubscribe(const std::string& pattern)
    {
        return execute("PUNSUBSCRIBE", {pattern}, 1);
    }

    RedisClientAwaitable& RedisClient::punsubscribe(const std::vector<std::string>& patterns)
    {
        if (patterns.empty()) {
            return execute("PUNSUBSCRIBE", {}, 1);
        }
        return execute("PUNSUBSCRIBE", patterns, patterns.size());
    }

    RedisClientAwaitable& RedisClient::role()
    {
        return execute("ROLE", {});
    }

    RedisClientAwaitable& RedisClient::replicaof(const std::string& host, int32_t port)
    {
        return execute("REPLICAOF", {host, std::to_string(port)});
    }

    RedisClientAwaitable& RedisClient::readonly()
    {
        return execute("READONLY", {});
    }

    RedisClientAwaitable& RedisClient::readwrite()
    {
        return execute("READWRITE", {});
    }

    RedisClientAwaitable& RedisClient::clusterInfo()
    {
        return execute("CLUSTER", {"INFO"});
    }

    RedisClientAwaitable& RedisClient::clusterNodes()
    {
        return execute("CLUSTER", {"NODES"});
    }

    RedisClientAwaitable& RedisClient::clusterSlots()
    {
        return execute("CLUSTER", {"SLOTS"});
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
