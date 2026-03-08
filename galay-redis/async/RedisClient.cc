#include "RedisClient.h"
#include "base/RedisError.h"
#include "base/RedisLog.h"
#include <galay-utils/system/System.hpp>
#include <sys/uio.h>
#include <array>
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

        size_t estimateRespCommandBytes(std::string_view cmd, const std::vector<std::string>& args)
        {
            size_t total = 1 + decimalDigits(1 + args.size()) + 2;
            total += 1 + decimalDigits(cmd.size()) + 2 + cmd.size() + 2;
            for (const auto& arg : args) {
                total += 1 + decimalDigits(arg.size()) + 2 + arg.size() + 2;
            }
            return total;
        }

        size_t estimateRespCommandBytes(std::string_view cmd, std::span<const std::string_view> args)
        {
            size_t total = 1 + decimalDigits(1 + args.size()) + 2;
            total += 1 + decimalDigits(cmd.size()) + 2 + cmd.size() + 2;
            for (const auto& arg : args) {
                total += 1 + decimalDigits(arg.size()) + 2 + arg.size() + 2;
            }
            return total;
        }

#ifdef IOV_MAX
        constexpr int kPipelineWritevMaxIov = IOV_MAX > 0 ? IOV_MAX : 1024;
#else
        constexpr int kPipelineWritevMaxIov = 1024;
#endif

        std::array<struct iovec, 1>& emptyIovecs()
        {
            static std::array<struct iovec, 1> empty{};
            return empty;
        }

        const char* prepareParseInput(const struct iovec* read_iovecs,
                                      size_t read_iovec_count,
                                      std::string& parse_buffer,
                                      size_t& len)
        {
            if (read_iovecs == nullptr || read_iovec_count == 0) {
                len = 0;
                return nullptr;
            }

            if (read_iovec_count == 1) {
                len = read_iovecs[0].iov_len;
                return static_cast<const char*>(read_iovecs[0].iov_base);
            }

            len = 0;
            for (size_t i = 0; i < read_iovec_count; ++i) {
                len += read_iovecs[i].iov_len;
            }

            parse_buffer.clear();
            parse_buffer.reserve(len);
            for (size_t i = 0; i < read_iovec_count; ++i) {
                parse_buffer.append(
                    static_cast<const char*>(read_iovecs[i].iov_base),
                    read_iovecs[i].iov_len
                );
            }
            return parse_buffer.data();
        }

        enum class ParseChunkState : uint8_t
        {
            Done,
            NeedMore,
            ParseError
        };

        struct ParseChunkResult
        {
            size_t consumed = 0;
            ParseChunkState state = ParseChunkState::NeedMore;
        };

        ParseChunkResult parseRepliesFromChunk(protocol::RespParser& parser,
                                               const char* data,
                                               size_t len,
                                               size_t expected_replies,
                                               std::vector<RedisValue>& values)
        {
            ParseChunkResult result;
            while (values.size() < expected_replies && result.consumed < len) {
                protocol::RedisReply reply;
                auto parse_result =
                    parser.parseFast(data + result.consumed, len - result.consumed, &reply);
                if (parse_result) {
                    result.consumed += parse_result.value();
                    values.emplace_back(std::move(reply));
                    continue;
                }

                if (parse_result.error() == protocol::ParseError::Incomplete) {
                    result.state = ParseChunkState::NeedMore;
                } else {
                    result.state = ParseChunkState::ParseError;
                }
                return result;
            }

            if (values.size() >= expected_replies) {
                result.state = ParseChunkState::Done;
            } else {
                result.state = ParseChunkState::NeedMore;
            }
            return result;
        }

        bool parseRepliesFromRingBuffer(RedisBufferProvider& ring_buffer,
                                        protocol::RespParser& parser,
                                        std::string& parse_buffer,
                                        size_t expected_replies,
                                        std::vector<RedisValue>& values,
                                        bool& parse_error)
        {
            struct iovec read_iovecs[2];
            while (values.size() < expected_replies) {
                const size_t read_iovec_count = ring_buffer.getReadIovecs(read_iovecs, 2);
                if (read_iovec_count == 0) {
                    return false;
                }

                const char* first_data = static_cast<const char*>(read_iovecs[0].iov_base);
                const size_t first_len = read_iovecs[0].iov_len;
                if (first_data == nullptr || first_len == 0) {
                    return false;
                }

                const auto first_chunk =
                    parseRepliesFromChunk(parser, first_data, first_len, expected_replies, values);
                if (first_chunk.consumed > 0) {
                    ring_buffer.consume(first_chunk.consumed);
                }

                if (first_chunk.state == ParseChunkState::ParseError) {
                    parse_error = true;
                    return true;
                }
                if (values.size() >= expected_replies) {
                    return true;
                }

                // Fast path: first iovec fully consumed, continue without any copy.
                if (first_chunk.consumed == first_len) {
                    if (first_chunk.consumed == 0) {
                        return false;
                    }
                    continue;
                }

                // Not enough data in first iovec and no wrapped second iovec available.
                if (read_iovec_count < 2) {
                    return false;
                }

                const char* second_data = static_cast<const char*>(read_iovecs[1].iov_base);
                const size_t second_len = read_iovecs[1].iov_len;
                if (second_data == nullptr || second_len == 0) {
                    return false;
                }

                // Only stitch the unparsed tail + wrapped head instead of copying full readable bytes.
                const size_t first_tail_offset = first_chunk.consumed;
                const size_t first_tail_len = first_len - first_tail_offset;
                parse_buffer.clear();
                parse_buffer.reserve(first_tail_len + second_len);
                parse_buffer.append(first_data + first_tail_offset, first_tail_len);
                parse_buffer.append(second_data, second_len);

                const auto stitched_chunk = parseRepliesFromChunk(parser,
                                                                  parse_buffer.data(),
                                                                  parse_buffer.size(),
                                                                  expected_replies,
                                                                  values);
                const size_t stitched_consume = stitched_chunk.consumed;
                if (stitched_consume > 0) {
                    ring_buffer.consume(stitched_consume);
                }

                if (stitched_chunk.state == ParseChunkState::ParseError) {
                    parse_error = true;
                    return true;
                }
                if (values.size() >= expected_replies) {
                    return true;
                }
                if (stitched_consume == 0) {
                    return false;
                }
            }
            return true;
        }
    }

    // ======================== RedisClientAwaitable 实现 ========================

    RedisClientAwaitable::ProtocolSendAwaitable::ProtocolSendAwaitable(RedisClientAwaitable* owner)
        : WritevIOContext(emptyIovecs(), 0)
        , m_owner(owner)
    {
        rebind(owner);
    }

    void RedisClientAwaitable::ProtocolSendAwaitable::syncContextIovecs()
    {
        WritevIOContext::m_iovecs = std::span<const struct iovec>(m_iovecs.data(), m_iovecs.size());
    }

    void RedisClientAwaitable::ProtocolSendAwaitable::rebind(RedisClientAwaitable* owner)
    {
        m_owner = owner;
        m_iovecs.clear();
        if (!m_owner || m_owner->m_encoded_cmd.empty()) {
            syncContextIovecs();
            return;
        }

        struct iovec iov;
        iov.iov_base = const_cast<char*>(m_owner->m_encoded_cmd.data());
        iov.iov_len = m_owner->m_encoded_cmd.size();
        m_iovecs.push_back(iov);
        syncContextIovecs();
    }

    int RedisClientAwaitable::ProtocolSendAwaitable::pendingIovCount()
    {
        while (!m_iovecs.empty() && m_iovecs.front().iov_len == 0) {
            m_iovecs.erase(m_iovecs.begin());
        }
        syncContextIovecs();
        return static_cast<int>(m_iovecs.size());
    }

    bool RedisClientAwaitable::ProtocolSendAwaitable::advanceAfterWrite(size_t sent_bytes)
    {
        size_t remaining = sent_bytes;
        while (remaining > 0 && !m_iovecs.empty()) {
            auto& iov = m_iovecs.front();
            if (remaining < iov.iov_len) {
                iov.iov_base = static_cast<char*>(iov.iov_base) + remaining;
                iov.iov_len -= remaining;
                syncContextIovecs();
                return true;
            }

            remaining -= iov.iov_len;
            m_iovecs.erase(m_iovecs.begin());
        }
        syncContextIovecs();
        return remaining == 0;
    }

#ifdef USE_IOURING
    bool RedisClientAwaitable::ProtocolSendAwaitable::handleComplete(struct io_uring_cqe* cqe, GHandle)
    {
        if (pendingIovCount() == 0) {
            return true;
        }
        if (cqe == nullptr) {
            return false;
        }

        auto result = galay::kernel::io::handleWritev(cqe);
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

        if (!advanceAfterWrite(sent)) {
            m_owner->setSendError(IOError(galay::kernel::kSendFailed, 0));
            return true;
        }
        return pendingIovCount() == 0;
    }
#else
    bool RedisClientAwaitable::ProtocolSendAwaitable::handleComplete(GHandle handle)
    {
        while (true) {
            const int iov_count = pendingIovCount();
            if (iov_count == 0) {
                return true;
            }

            auto result = galay::kernel::io::handleWritev(handle, m_iovecs.data(), iov_count);
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

            if (!advanceAfterWrite(sent)) {
                m_owner->setSendError(IOError(galay::kernel::kSendFailed, 0));
                return true;
            }
        }
    }
#endif

    RedisClientAwaitable::ProtocolRecvAwaitable::ProtocolRecvAwaitable(RedisClientAwaitable* owner)
        : ReadvIOContext(emptyIovecs(), 0)
        , m_owner(owner)
    {
        m_iovecs.reserve(2);
        rebind(owner);
    }

    void RedisClientAwaitable::ProtocolRecvAwaitable::syncContextIovecs()
    {
        ReadvIOContext::m_iovecs = std::span<const struct iovec>(m_iovecs.data(), m_iovecs.size());
    }

    void RedisClientAwaitable::ProtocolRecvAwaitable::rebind(RedisClientAwaitable* owner)
    {
        m_owner = owner;
        m_iovecs.clear();
        syncContextIovecs();
    }

    bool RedisClientAwaitable::ProtocolRecvAwaitable::prepareRecvWindow()
    {
        if (!m_owner || !m_owner->m_client) {
            return false;
        }

        struct iovec write_iovecs[2];
        const size_t write_iovec_count =
            m_owner->m_client->m_buffer_provider->getWriteIovecs(write_iovecs, 2);
        if (write_iovec_count == 0) {
            return false;
        }

        m_iovecs.clear();
        for (size_t i = 0; i < write_iovec_count; ++i) {
            if (write_iovecs[i].iov_len == 0) {
                continue;
            }
            m_iovecs.push_back(write_iovecs[i]);
        }
        syncContextIovecs();
        return !m_iovecs.empty();
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

        auto result = galay::kernel::io::handleReadv(cqe);
        if (!result && IOError::contains(result.error().code(), galay::kernel::kNotReady)) {
            return false;
        }
        if (!result) {
            m_owner->setRecvError(result.error());
            return true;
        }

        const size_t recv_bytes = result.value();
        if (recv_bytes == 0) {
            m_owner->setConnectionClosedError();
            return true;
        }

        m_owner->m_client->m_buffer_provider->produce(recv_bytes);
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

            auto result = galay::kernel::io::handleReadv(handle,
                                                         m_iovecs.data(),
                                                         static_cast<int>(m_iovecs.size()));
            if (!result && IOError::contains(result.error().code(), galay::kernel::kNotReady)) {
                return false;
            }
            if (!result) {
                m_owner->setRecvError(result.error());
                return true;
            }

            const size_t recv_bytes = result.value();
            if (recv_bytes == 0) {
                m_owner->setConnectionClosedError();
                return true;
            }

            m_owner->m_client->m_buffer_provider->produce(recv_bytes);
            if (m_owner->parseResponsesFromRingBuffer()) {
                return true;
            }
        }
    }
#endif

    RedisClientAwaitable::RedisClientAwaitable(RedisClient& client,
                                               std::string encoded_command,
                                               size_t expected_replies,
                                               bool recv_only)
        : CustomAwaitable(client.m_socket.controller())
        , m_client(&client)
        , m_encoded_cmd(std::move(encoded_command))
        , m_expected_replies(expected_replies)
        , m_recv_only(recv_only)
        , m_state(State::Running)
        , m_send_awaitable(this)
        , m_recv_awaitable(this)
        , m_result(std::nullopt)
    {
        if (m_recv_only) {
            m_encoded_cmd.clear();
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
            addTask(IOEventType::WRITEV, &m_send_awaitable);
        }
        addTask(IOEventType::READV, &m_recv_awaitable);
    }

    bool RedisClientAwaitable::parseResponsesFromRingBuffer()
    {
        bool parse_error = false;
        const bool done = parseRepliesFromRingBuffer(
            *m_client->m_buffer_provider,
            m_client->m_parser,
            m_parse_buffer,
            m_expected_replies,
            m_values,
            parse_error);
        if (parse_error) {
            setParseError();
            return true;
        }
        return done;
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
        : WritevIOContext(emptyIovecs(), 0)
        , m_owner(owner)
    {
        rebind(owner);
    }

    void RedisPipelineAwaitable::ProtocolSendAwaitable::syncContextIovecs()
    {
        if (m_iov_cursor >= m_iovecs.size()) {
            WritevIOContext::m_iovecs = std::span<const struct iovec>();
            return;
        }
        WritevIOContext::m_iovecs = std::span<const struct iovec>(
            m_iovecs.data() + m_iov_cursor,
            m_iovecs.size() - m_iov_cursor);
    }

    void RedisPipelineAwaitable::ProtocolSendAwaitable::rebind(RedisPipelineAwaitable* owner)
    {
        m_owner = owner;
        m_iov_cursor = 0;
        m_next_command_index = 0;
        m_iovecs.clear();
        if (!m_owner) {
            syncContextIovecs();
            return;
        }

        const size_t reserve_hint = m_owner->m_encoded_slices.size() <
                                            static_cast<size_t>(kPipelineWritevMaxIov)
                                        ? m_owner->m_encoded_slices.size()
                                        : static_cast<size_t>(kPipelineWritevMaxIov);
        m_iovecs.reserve(reserve_hint);
        refillIovWindow();
        syncContextIovecs();
    }

    void RedisPipelineAwaitable::ProtocolSendAwaitable::refillIovWindow()
    {
        if (!m_owner) {
            m_iovecs.clear();
            m_iov_cursor = 0;
            syncContextIovecs();
            return;
        }

        if (m_iov_cursor > 0) {
            m_iovecs.erase(
                m_iovecs.begin(),
                m_iovecs.begin() + static_cast<std::vector<struct iovec>::difference_type>(m_iov_cursor));
            m_iov_cursor = 0;
        }

        while (m_iovecs.size() < static_cast<size_t>(kPipelineWritevMaxIov) &&
               m_next_command_index < m_owner->m_encoded_slices.size()) {
            const auto encoded_slice = m_owner->m_encoded_slices[m_next_command_index++];
            if (encoded_slice.length == 0) {
                continue;
            }

            struct iovec iov;
            iov.iov_base = const_cast<char*>(m_owner->m_encoded_buffer.data() + encoded_slice.offset);
            iov.iov_len = encoded_slice.length;
            m_iovecs.push_back(iov);
        }
        syncContextIovecs();
    }

    int RedisPipelineAwaitable::ProtocolSendAwaitable::pendingIovCount()
    {
        while (m_iov_cursor < m_iovecs.size() && m_iovecs[m_iov_cursor].iov_len == 0) {
            ++m_iov_cursor;
        }

        if (m_iov_cursor >= m_iovecs.size()) {
            refillIovWindow();
            while (m_iov_cursor < m_iovecs.size() && m_iovecs[m_iov_cursor].iov_len == 0) {
                ++m_iov_cursor;
            }
        }

        if (m_iov_cursor >= m_iovecs.size()) {
            syncContextIovecs();
            return 0;
        }

        syncContextIovecs();
        return static_cast<int>(m_iovecs.size() - m_iov_cursor);
    }

    bool RedisPipelineAwaitable::ProtocolSendAwaitable::advanceAfterWrite(size_t sent_bytes)
    {
        size_t remaining = sent_bytes;
        while (remaining > 0 && m_iov_cursor < m_iovecs.size()) {
            auto& iov = m_iovecs[m_iov_cursor];
            if (iov.iov_len == 0) {
                ++m_iov_cursor;
                continue;
            }

            if (remaining < iov.iov_len) {
                iov.iov_base = static_cast<char*>(iov.iov_base) + remaining;
                iov.iov_len -= remaining;
                syncContextIovecs();
                return true;
            }

            remaining -= iov.iov_len;
            iov.iov_len = 0;
            ++m_iov_cursor;
        }

        if (remaining != 0) {
            return false;
        }

        if (m_iov_cursor >= m_iovecs.size()) {
            refillIovWindow();
        }
        syncContextIovecs();
        return true;
    }

#ifdef USE_IOURING
    bool RedisPipelineAwaitable::ProtocolSendAwaitable::handleComplete(struct io_uring_cqe* cqe, GHandle)
    {
        if (pendingIovCount() == 0) {
            return true;
        }
        if (cqe == nullptr) {
            return false;
        }

        auto result = galay::kernel::io::handleWritev(cqe);
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

        if (!advanceAfterWrite(sent)) {
            m_owner->setSendError(IOError(galay::kernel::kSendFailed, 0));
            return true;
        }
        return pendingIovCount() == 0;
    }
#else
    bool RedisPipelineAwaitable::ProtocolSendAwaitable::handleComplete(GHandle handle)
    {
        while (true) {
            const int iov_count = pendingIovCount();
            if (iov_count == 0) {
                return true;
            }

            auto result = galay::kernel::io::handleWritev(handle,
                                                          m_iovecs.data() + m_iov_cursor,
                                                          iov_count);
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

            if (!advanceAfterWrite(sent)) {
                m_owner->setSendError(IOError(galay::kernel::kSendFailed, 0));
                return true;
            }
        }
    }
#endif

    RedisPipelineAwaitable::ProtocolRecvAwaitable::ProtocolRecvAwaitable(RedisPipelineAwaitable* owner)
        : ReadvIOContext(emptyIovecs(), 0)
        , m_owner(owner)
    {
        m_iovecs.reserve(2);
        rebind(owner);
    }

    void RedisPipelineAwaitable::ProtocolRecvAwaitable::syncContextIovecs()
    {
        ReadvIOContext::m_iovecs = std::span<const struct iovec>(m_iovecs.data(), m_iovecs.size());
    }

    void RedisPipelineAwaitable::ProtocolRecvAwaitable::rebind(RedisPipelineAwaitable* owner)
    {
        m_owner = owner;
        m_iovecs.clear();
        syncContextIovecs();
    }

    bool RedisPipelineAwaitable::ProtocolRecvAwaitable::prepareRecvWindow()
    {
        if (!m_owner || !m_owner->m_client) {
            return false;
        }

        struct iovec write_iovecs[2];
        const size_t write_iovec_count =
            m_owner->m_client->m_buffer_provider->getWriteIovecs(write_iovecs, 2);
        if (write_iovec_count == 0) {
            return false;
        }

        m_iovecs.clear();
        for (size_t i = 0; i < write_iovec_count; ++i) {
            if (write_iovecs[i].iov_len == 0) {
                continue;
            }
            m_iovecs.push_back(write_iovecs[i]);
        }
        syncContextIovecs();
        return !m_iovecs.empty();
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

        auto result = galay::kernel::io::handleReadv(cqe);
        if (!result && IOError::contains(result.error().code(), galay::kernel::kNotReady)) {
            return false;
        }
        if (!result) {
            m_owner->setRecvError(result.error());
            return true;
        }

        const size_t recv_bytes = result.value();
        if (recv_bytes == 0) {
            m_owner->setConnectionClosedError();
            return true;
        }

        m_owner->m_client->m_buffer_provider->produce(recv_bytes);
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

            auto result = galay::kernel::io::handleReadv(handle,
                                                         m_iovecs.data(),
                                                         static_cast<int>(m_iovecs.size()));
            if (!result && IOError::contains(result.error().code(), galay::kernel::kNotReady)) {
                return false;
            }
            if (!result) {
                m_owner->setRecvError(result.error());
                return true;
            }

            const size_t recv_bytes = result.value();
            if (recv_bytes == 0) {
                m_owner->setConnectionClosedError();
                return true;
            }

            m_owner->m_client->m_buffer_provider->produce(recv_bytes);
            if (m_owner->parseResponsesFromRingBuffer()) {
                return true;
            }
        }
    }
#endif

    RedisPipelineAwaitable::RedisPipelineAwaitable(RedisClient& client,
                                                   std::span<const RedisCommandView> commands)
        : CustomAwaitable(client.m_socket.controller())
        , m_client(&client)
        , m_expected_replies(commands.size())
        , m_state(State::Running)
        , m_send_awaitable(this)
        , m_recv_awaitable(this)
        , m_result(std::nullopt)
    {
        m_values.reserve(m_expected_replies);
        static thread_local protocol::RespEncoder encoder;
        size_t encoded_bytes = 0;
        for (const auto& cmd_view : commands) {
            if (!cmd_view.encoded.empty()) {
                encoded_bytes += cmd_view.encoded.size();
            } else {
                encoded_bytes += estimateRespCommandBytes(cmd_view.command, cmd_view.args);
            }
        }

        m_encoded_buffer.reserve(encoded_bytes);
        m_encoded_slices.reserve(commands.size());
        for (const auto& cmd_view : commands) {
            const size_t offset = m_encoded_buffer.size();
            if (!cmd_view.encoded.empty()) {
                m_encoded_buffer.append(cmd_view.encoded.data(), cmd_view.encoded.size());
            } else {
                encoder.appendCommandFast(m_encoded_buffer, cmd_view.command, cmd_view.args);
            }
            m_encoded_slices.push_back(EncodedSlice{offset, m_encoded_buffer.size() - offset});
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
        m_expected_replies = other.m_expected_replies;
        m_encoded_buffer = std::move(other.m_encoded_buffer);
        m_encoded_slices = std::move(other.m_encoded_slices);
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
        m_expected_replies = 0;
        m_tasks.clear();
        m_cursor = 0;
        m_internal_error.reset();
        m_values.clear();
        m_encoded_buffer.clear();
        m_encoded_slices.clear();
        m_parse_buffer.clear();
        m_result = std::nullopt;
        m_send_awaitable.rebind(this);
        m_recv_awaitable.rebind(this);
    }

    void RedisPipelineAwaitable::initTaskQueue()
    {
        m_tasks.clear();
        m_cursor = 0;
        addTask(IOEventType::WRITEV, &m_send_awaitable);
        addTask(IOEventType::READV, &m_recv_awaitable);
    }

    bool RedisPipelineAwaitable::parseResponsesFromRingBuffer()
    {
        bool parse_error = false;
        const bool done = parseRepliesFromRingBuffer(
            *m_client->m_buffer_provider,
            m_client->m_parser,
            m_parse_buffer,
            m_expected_replies,
            m_values,
            parse_error);
        if (parse_error) {
            setParseError();
            return true;
        }
        return done;
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
        , m_result(std::in_place)
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

            // 编码认证命令（避免构造临时 vector）
            RedisCommandBuilder builder;
            if (m_username.empty()) {
                m_encoded_cmd = std::move(builder.auth(m_password).encoded);
            } else {
                m_encoded_cmd = std::move(builder.auth(m_username, m_password).encoded);
            }
            m_sent = 0;

            // 发送认证命令
            m_send_iovec[0].iov_base = const_cast<char*>(m_encoded_cmd.data());
            m_send_iovec[0].iov_len = m_encoded_cmd.size();
            m_send_awaitable.emplace(
                m_client->m_socket.writev(m_send_iovec, 1));
            return m_send_awaitable->await_suspend(handle);
        }
        else if (m_state == State::Authenticating) {
            // 继续发送认证命令（如果未完成）
            m_send_iovec[0].iov_base = const_cast<char*>(m_encoded_cmd.data() + m_sent);
            m_send_iovec[0].iov_len = m_encoded_cmd.size() - m_sent;
            m_send_awaitable.emplace(
                m_client->m_socket.writev(m_send_iovec, 1));
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
            RedisCommandBuilder builder;
            m_encoded_cmd = std::move(builder.select(m_db_index).encoded);
            m_sent = 0;

            m_send_iovec[0].iov_base = const_cast<char*>(m_encoded_cmd.data());
            m_send_iovec[0].iov_len = m_encoded_cmd.size();
            m_send_awaitable.emplace(
                m_client->m_socket.writev(m_send_iovec, 1));
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
            const size_t recv_iovec_count =
                m_client->m_buffer_provider->getWriteIovecs(m_recv_iovecs.data(), m_recv_iovecs.size());
            if (recv_iovec_count == 0) {
                RedisLogDebug(m_client->m_logger, "Ring buffer has no writable iovec for AUTH response");
                m_state = State::Invalid;
                m_send_awaitable.reset();
                return std::unexpected(RedisError(RedisErrorType::REDIS_ERROR_TYPE_BUFFER_OVERFLOW_ERROR,
                                                         "No writable iovec for AUTH response"));
            }
            m_recv_awaitable.emplace(
                m_client->m_socket.readv(m_recv_iovecs, recv_iovec_count));
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

            m_client->m_buffer_provider->produce(n);

            // 解析认证响应
            struct iovec read_iovecs[2];
            const size_t read_iovec_count = m_client->m_buffer_provider->getReadIovecs(read_iovecs, 2);
            if (read_iovec_count == 0) {
                RedisLogDebug(m_client->m_logger, "AUTH response incomplete");
                return {};  // 继续接收
            }

            size_t len = 0;
            const char* data = prepareParseInput(read_iovecs, read_iovec_count, m_parse_buffer, len);
            if (data == nullptr) {
                RedisLogDebug(m_client->m_logger, "AUTH response parse buffer unavailable");
                return {};
            }

            protocol::RedisReply value;
            auto parse_result = m_client->m_parser.parseFast(data, len, &value);
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

            m_client->m_buffer_provider->consume(parse_result.value());

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
            const size_t recv_iovec_count =
                m_client->m_buffer_provider->getWriteIovecs(m_recv_iovecs.data(), m_recv_iovecs.size());
            if (recv_iovec_count == 0) {
                RedisLogDebug(m_client->m_logger, "Ring buffer has no writable iovec for SELECT response");
                m_state = State::Invalid;
                m_send_awaitable.reset();
                return std::unexpected(RedisError(RedisErrorType::REDIS_ERROR_TYPE_BUFFER_OVERFLOW_ERROR,
                                                         "No writable iovec for SELECT response"));
            }
            m_recv_awaitable.emplace(
                m_client->m_socket.readv(m_recv_iovecs, recv_iovec_count));
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

            m_client->m_buffer_provider->produce(n);

            // 解析 SELECT 响应
            struct iovec read_iovecs[2];
            const size_t read_iovec_count = m_client->m_buffer_provider->getReadIovecs(read_iovecs, 2);
            if (read_iovec_count == 0) {
                RedisLogDebug(m_client->m_logger, "SELECT response incomplete");
                return {};  // 继续接收
            }

            size_t len = 0;
            const char* data = prepareParseInput(read_iovecs, read_iovec_count, m_parse_buffer, len);
            if (data == nullptr) {
                RedisLogDebug(m_client->m_logger, "SELECT response parse buffer unavailable");
                return {};
            }

            protocol::RedisReply value;
            auto parse_result = m_client->m_parser.parseFast(data, len, &value);
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

            m_client->m_buffer_provider->consume(parse_result.value());

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

    RedisClient::RedisClient(IOScheduler* scheduler,
                             AsyncRedisConfig config,
                             std::shared_ptr<RedisBufferProvider> buffer_provider)
        : m_scheduler(scheduler)
        , m_config(config)
        , m_buffer_provider(std::move(buffer_provider))
    {
        if (!m_buffer_provider) {
            m_buffer_provider = std::make_shared<RedisRingBufferProvider>(config.buffer_size);
        }
        m_logger = RedisLog::getInstance()->getLogger();
    }

    RedisClient::RedisClient(RedisClient&& other) noexcept
        : m_is_closed(other.m_is_closed)
        , m_socket(std::move(other.m_socket))
        , m_scheduler(other.m_scheduler)
        , m_parser(std::move(other.m_parser))
        , m_config(other.m_config)
        , m_buffer_provider(std::move(other.m_buffer_provider))
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
            m_parser = std::move(other.m_parser);
            m_config = other.m_config;
            m_buffer_provider = std::move(other.m_buffer_provider);
            m_logger = std::move(other.m_logger);
            other.m_is_closed = true;
        }
        return *this;
    }

    // ======================== 命令方法 ========================

    RedisClientAwaitable RedisClient::command(RedisEncodedCommand command_packet)
    {
        return RedisClientAwaitable(
            *this,
            std::move(command_packet.encoded),
            command_packet.expected_replies,
            false);
    }

    RedisClientAwaitable RedisClient::receive(size_t expected_replies)
    {
        return RedisClientAwaitable(*this, std::string(), expected_replies, true);
    }

    RedisPipelineAwaitable RedisClient::batch(std::span<const RedisCommandView> commands)
    {
        return RedisPipelineAwaitable(*this, commands);
    }

    // ======================== 连接方法 ========================

    RedisConnectAwaitable RedisClient::connect(const std::string& url)
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

        RedisConnectOptions options;
        options.username = std::move(username);
        options.password = std::move(password);
        options.db_index = db_index;
        options.version = 2;
        return connect(ip, port, std::move(options));
    }

    RedisConnectAwaitable RedisClient::connect(const std::string& ip,
                                               int32_t port,
                                               RedisConnectOptions options)
    {
        return RedisConnectAwaitable(
            *this,
            ip,
            port,
            std::move(options.username),
            std::move(options.password),
            options.db_index,
            options.version);
    }
}
