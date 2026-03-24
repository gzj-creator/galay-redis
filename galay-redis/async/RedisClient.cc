#include "RedisClient.h"

#include "base/RedisError.h"
#include "base/RedisLog.h"

#include <galay-utils/system/System.hpp>

#include <array>
#include <cerrno>
#include <regex>
#include <sys/uio.h>
#include <utility>

namespace galay::redis
{
    namespace detail
    {
        using galay::kernel::IOContextBase;
        using ::GHandle;
        using ::IOEventType;

        using CommandOuterResult = std::expected<RedisClientAwaitable::Result, IOError>;
        using PipelineOuterResult = std::expected<RedisPipelineAwaitable::Result, IOError>;
        using ConnectOuterResult = std::expected<RedisVoidResult, IOError>;

        RedisError mapIoErrorToRedisError(const IOError& io_error, RedisErrorType fallback)
        {
            if (IOError::contains(io_error.code(), galay::kernel::kTimeout)) {
                return RedisError(RedisErrorType::REDIS_ERROR_TYPE_TIMEOUT_ERROR, io_error.message());
            }
            if (IOError::contains(io_error.code(), galay::kernel::kDisconnectError)) {
                return RedisError(RedisErrorType::REDIS_ERROR_TYPE_CONNECTION_CLOSED, io_error.message());
            }
            return RedisError(fallback, io_error.message());
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

        size_t estimateRespCommandBytes(std::string_view cmd, std::span<const std::string_view> args)
        {
            size_t total = 1 + decimalDigits(1 + args.size()) + 2;
            total += 1 + decimalDigits(cmd.size()) + 2 + cmd.size() + 2;
            for (const auto& arg : args) {
                total += 1 + decimalDigits(arg.size()) + 2 + arg.size() + 2;
            }
            return total;
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

            result.state = values.size() >= expected_replies
                               ? ParseChunkState::Done
                               : ParseChunkState::NeedMore;
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
                if (first_chunk.consumed == first_len) {
                    if (first_chunk.consumed == 0) {
                        return false;
                    }
                    continue;
                }
                if (read_iovec_count < 2) {
                    return false;
                }

                const char* second_data = static_cast<const char*>(read_iovecs[1].iov_base);
                const size_t second_len = read_iovecs[1].iov_len;
                if (second_data == nullptr || second_len == 0) {
                    return false;
                }

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
                if (stitched_chunk.consumed > 0) {
                    ring_buffer.consume(stitched_chunk.consumed);
                }

                if (stitched_chunk.state == ParseChunkState::ParseError) {
                    parse_error = true;
                    return true;
                }
                if (values.size() >= expected_replies) {
                    return true;
                }
                if (stitched_chunk.consumed == 0) {
                    return false;
                }
            }
            return true;
        }

    } // namespace detail

    RedisClientAwaitable::RedisClientAwaitable(RedisClient& client,
                                               std::string encoded_command,
                                               size_t expected_replies,
                                               bool recv_only)
        : m_state(std::make_shared<SharedState>(
              client,
              std::move(encoded_command),
              expected_replies,
              recv_only))
        , m_inner(galay::kernel::AwaitableBuilder<Result>::fromStateMachine(
                      client.socket().controller(),
                      Machine(m_state))
                      .build())
    {
    }

    RedisClientAwaitable::SharedState::SharedState(RedisClient& client,
                                                   std::string encoded_command_in,
                                                   size_t expected_replies_in,
                                                   bool recv_only_in)
        : client(&client)
        , encoded_cmd(std::move(encoded_command_in))
        , expected_replies(expected_replies_in)
        , recv_only(recv_only_in)
    {
        if (recv_only) {
            encoded_cmd.clear();
        }
        values.reserve(expected_replies);
        if (expected_replies == 0) {
            result = std::optional<std::vector<RedisValue>>(std::vector<RedisValue>{});
            phase = Phase::Done;
        }
    }

    RedisClientAwaitable::Machine::Machine(std::shared_ptr<SharedState> state)
        : m_state(std::move(state))
    {
    }

    void RedisClientAwaitable::Machine::setError(RedisError error) noexcept
    {
        m_state->result = std::unexpected(std::move(error));
        m_state->phase = Phase::Invalid;
    }

    void RedisClientAwaitable::Machine::setSendError(const IOError& io_error) noexcept
    {
        setError(detail::mapIoErrorToRedisError(
            io_error,
            RedisErrorType::REDIS_ERROR_TYPE_SEND_ERROR));
    }

    void RedisClientAwaitable::Machine::setRecvError(const IOError& io_error) noexcept
    {
        setError(detail::mapIoErrorToRedisError(
            io_error,
            RedisErrorType::REDIS_ERROR_TYPE_RECV_ERROR));
    }

    bool RedisClientAwaitable::Machine::prepareReadWindow()
    {
        m_state->read_iov_count = m_state->client->bufferProvider().getWriteIovecs(
            m_state->read_iovecs.data(),
            m_state->read_iovecs.size());
        if (m_state->read_iov_count == 0) {
            setError(RedisError(
                RedisErrorType::REDIS_ERROR_TYPE_BUFFER_OVERFLOW_ERROR,
                "Ring buffer exhausted before parsing complete response"));
            return false;
        }
        return true;
    }

    std::expected<bool, RedisError> RedisClientAwaitable::Machine::tryParseReplies()
    {
        bool parse_error = false;
        const bool done = detail::parseRepliesFromRingBuffer(
            m_state->client->bufferProvider(),
            m_state->client->parser(),
            m_state->parse_buffer,
            m_state->expected_replies,
            m_state->values,
            parse_error);
        if (parse_error) {
            return std::unexpected(RedisError(
                RedisErrorType::REDIS_ERROR_TYPE_PARSE_ERROR,
                "Parse error"));
        }
        return done;
    }

    galay::kernel::MachineAction<RedisClientAwaitable::Result>
    RedisClientAwaitable::Machine::advance()
    {
        if (m_state->result.has_value()) {
            return galay::kernel::MachineAction<result_type>::complete(std::move(*m_state->result));
        }

        switch (m_state->phase) {
        case Phase::Invalid:
            setError(RedisError(
                RedisErrorType::REDIS_ERROR_TYPE_INTERNAL_ERROR,
                "RedisClientAwaitable in Invalid state"));
            return galay::kernel::MachineAction<result_type>::complete(std::move(*m_state->result));
        case Phase::Start:
            if (m_state->expected_replies == 0) {
                m_state->result = std::optional<std::vector<RedisValue>>(std::vector<RedisValue>{});
                m_state->phase = Phase::Done;
                return galay::kernel::MachineAction<result_type>::continue_();
            }
            m_state->phase = (m_state->recv_only || m_state->encoded_cmd.empty())
                ? Phase::Parse
                : Phase::Send;
            return galay::kernel::MachineAction<result_type>::continue_();
        case Phase::Send:
            if (m_state->sent >= m_state->encoded_cmd.size()) {
                m_state->phase = Phase::Parse;
                return galay::kernel::MachineAction<result_type>::continue_();
            }
            return galay::kernel::MachineAction<result_type>::waitWrite(
                m_state->encoded_cmd.data() + m_state->sent,
                m_state->encoded_cmd.size() - m_state->sent);
        case Phase::Parse: {
            auto parsed = tryParseReplies();
            if (!parsed.has_value()) {
                setError(std::move(parsed.error()));
                return galay::kernel::MachineAction<result_type>::complete(std::move(*m_state->result));
            }
            if (parsed.value()) {
                auto values = std::move(m_state->values);
                m_state->result = std::optional<std::vector<RedisValue>>(std::move(values));
                m_state->phase = Phase::Done;
                return galay::kernel::MachineAction<result_type>::continue_();
            }
            if (!prepareReadWindow()) {
                return galay::kernel::MachineAction<result_type>::complete(std::move(*m_state->result));
            }
            return galay::kernel::MachineAction<result_type>::waitReadv(
                m_state->read_iovecs.data(),
                m_state->read_iov_count);
        }
        case Phase::Done:
            return galay::kernel::MachineAction<result_type>::complete(std::move(*m_state->result));
        }

        setError(RedisError(
            RedisErrorType::REDIS_ERROR_TYPE_INTERNAL_ERROR,
            "Unknown RedisClientAwaitable state"));
        return galay::kernel::MachineAction<result_type>::complete(std::move(*m_state->result));
    }

    void RedisClientAwaitable::Machine::onRead(std::expected<size_t, IOError> result)
    {
        if (m_state->result.has_value()) {
            return;
        }
        if (!result.has_value()) {
            setRecvError(result.error());
            return;
        }
        if (result.value() == 0) {
            setError(RedisError(
                RedisErrorType::REDIS_ERROR_TYPE_CONNECTION_CLOSED,
                "Connection closed"));
            return;
        }

        m_state->client->bufferProvider().produce(result.value());
        m_state->phase = Phase::Parse;
    }

    void RedisClientAwaitable::Machine::onWrite(std::expected<size_t, IOError> result)
    {
        if (m_state->result.has_value()) {
            return;
        }
        if (!result.has_value()) {
            setSendError(result.error());
            return;
        }
        if (result.value() == 0) {
            setError(RedisError(
                RedisErrorType::REDIS_ERROR_TYPE_SEND_ERROR,
                "Send returned 0"));
            return;
        }

        m_state->sent += result.value();
        if (m_state->sent >= m_state->encoded_cmd.size()) {
            m_state->phase = Phase::Parse;
        }
    }

    bool RedisClientAwaitable::isInvalid() const noexcept
    {
        return m_state != nullptr && m_state->phase == Phase::Invalid;
    }

    void RedisClientAwaitable::reset() noexcept
    {
        if (!m_state) {
            return;
        }
        m_state->sent = 0;
        m_state->values.clear();
        m_state->parse_buffer.clear();
        m_state->result = std::unexpected(RedisError(
            RedisErrorType::REDIS_ERROR_TYPE_INTERNAL_ERROR,
            "RedisClientAwaitable reset"));
        m_state->phase = Phase::Invalid;
    }

    RedisPipelineAwaitable::RedisPipelineAwaitable(RedisClient& client,
                                                   std::span<const RedisCommandView> commands)
        : m_state(std::make_shared<SharedState>(client, commands))
        , m_inner(galay::kernel::AwaitableBuilder<Result>::fromStateMachine(
                      client.socket().controller(),
                      Machine(m_state))
                      .build())
    {
    }

    RedisPipelineAwaitable::SharedState::SharedState(RedisClient& client,
                                                     std::span<const RedisCommandView> commands)
        : client(&client)
        , expected_replies(commands.size())
    {
        values.reserve(expected_replies);
        if (expected_replies == 0) {
            result = std::optional<std::vector<RedisValue>>(std::vector<RedisValue>{});
            phase = Phase::Done;
            return;
        }

        static thread_local protocol::RespEncoder encoder;
        size_t encoded_bytes = 0;
        for (const auto& cmd_view : commands) {
            encoded_bytes += !cmd_view.encoded.empty()
                                 ? cmd_view.encoded.size()
                                 : detail::estimateRespCommandBytes(cmd_view.command, cmd_view.args);
        }

        encoded_buffer.reserve(encoded_bytes);
        for (const auto& cmd_view : commands) {
            if (!cmd_view.encoded.empty()) {
                encoded_buffer.append(cmd_view.encoded.data(), cmd_view.encoded.size());
            } else {
                encoder.appendCommandFast(encoded_buffer, cmd_view.command, cmd_view.args);
            }
        }
    }

    RedisPipelineAwaitable::Machine::Machine(std::shared_ptr<SharedState> state)
        : m_state(std::move(state))
    {
    }

    void RedisPipelineAwaitable::Machine::setError(RedisError error) noexcept
    {
        m_state->result = std::unexpected(std::move(error));
        m_state->phase = Phase::Invalid;
    }

    void RedisPipelineAwaitable::Machine::setSendError(const IOError& io_error) noexcept
    {
        setError(detail::mapIoErrorToRedisError(
            io_error,
            RedisErrorType::REDIS_ERROR_TYPE_SEND_ERROR));
    }

    void RedisPipelineAwaitable::Machine::setRecvError(const IOError& io_error) noexcept
    {
        setError(detail::mapIoErrorToRedisError(
            io_error,
            RedisErrorType::REDIS_ERROR_TYPE_RECV_ERROR));
    }

    bool RedisPipelineAwaitable::Machine::prepareReadWindow()
    {
        m_state->read_iov_count = m_state->client->bufferProvider().getWriteIovecs(
            m_state->read_iovecs.data(),
            m_state->read_iovecs.size());
        if (m_state->read_iov_count == 0) {
            setError(RedisError(
                RedisErrorType::REDIS_ERROR_TYPE_BUFFER_OVERFLOW_ERROR,
                "Ring buffer exhausted before parsing complete pipeline responses"));
            return false;
        }
        return true;
    }

    std::expected<bool, RedisError> RedisPipelineAwaitable::Machine::tryParseReplies()
    {
        bool parse_error = false;
        const bool done = detail::parseRepliesFromRingBuffer(
            m_state->client->bufferProvider(),
            m_state->client->parser(),
            m_state->parse_buffer,
            m_state->expected_replies,
            m_state->values,
            parse_error);
        if (parse_error) {
            return std::unexpected(RedisError(
                RedisErrorType::REDIS_ERROR_TYPE_PARSE_ERROR,
                "Parse error"));
        }
        return done;
    }

    galay::kernel::MachineAction<RedisPipelineAwaitable::Result>
    RedisPipelineAwaitable::Machine::advance()
    {
        if (m_state->result.has_value()) {
            return galay::kernel::MachineAction<result_type>::complete(std::move(*m_state->result));
        }

        switch (m_state->phase) {
        case Phase::Invalid:
            setError(RedisError(
                RedisErrorType::REDIS_ERROR_TYPE_INTERNAL_ERROR,
                "RedisPipelineAwaitable in Invalid state"));
            return galay::kernel::MachineAction<result_type>::complete(std::move(*m_state->result));
        case Phase::Start:
            if (m_state->expected_replies == 0) {
                m_state->result = std::optional<std::vector<RedisValue>>(std::vector<RedisValue>{});
                m_state->phase = Phase::Done;
                return galay::kernel::MachineAction<result_type>::continue_();
            }
            m_state->phase = m_state->encoded_buffer.empty() ? Phase::Parse : Phase::Send;
            return galay::kernel::MachineAction<result_type>::continue_();
        case Phase::Send:
            if (m_state->sent >= m_state->encoded_buffer.size()) {
                m_state->phase = Phase::Parse;
                return galay::kernel::MachineAction<result_type>::continue_();
            }
            return galay::kernel::MachineAction<result_type>::waitWrite(
                m_state->encoded_buffer.data() + m_state->sent,
                m_state->encoded_buffer.size() - m_state->sent);
        case Phase::Parse: {
            auto parsed = tryParseReplies();
            if (!parsed.has_value()) {
                setError(std::move(parsed.error()));
                return galay::kernel::MachineAction<result_type>::complete(std::move(*m_state->result));
            }
            if (parsed.value()) {
                auto values = std::move(m_state->values);
                m_state->result = std::optional<std::vector<RedisValue>>(std::move(values));
                m_state->phase = Phase::Done;
                return galay::kernel::MachineAction<result_type>::continue_();
            }
            if (!prepareReadWindow()) {
                return galay::kernel::MachineAction<result_type>::complete(std::move(*m_state->result));
            }
            return galay::kernel::MachineAction<result_type>::waitReadv(
                m_state->read_iovecs.data(),
                m_state->read_iov_count);
        }
        case Phase::Done:
            return galay::kernel::MachineAction<result_type>::complete(std::move(*m_state->result));
        }

        setError(RedisError(
            RedisErrorType::REDIS_ERROR_TYPE_INTERNAL_ERROR,
            "Unknown RedisPipelineAwaitable state"));
        return galay::kernel::MachineAction<result_type>::complete(std::move(*m_state->result));
    }

    void RedisPipelineAwaitable::Machine::onRead(std::expected<size_t, IOError> result)
    {
        if (m_state->result.has_value()) {
            return;
        }
        if (!result.has_value()) {
            setRecvError(result.error());
            return;
        }
        if (result.value() == 0) {
            setError(RedisError(
                RedisErrorType::REDIS_ERROR_TYPE_CONNECTION_CLOSED,
                "Connection closed"));
            return;
        }

        m_state->client->bufferProvider().produce(result.value());
        m_state->phase = Phase::Parse;
    }

    void RedisPipelineAwaitable::Machine::onWrite(std::expected<size_t, IOError> result)
    {
        if (m_state->result.has_value()) {
            return;
        }
        if (!result.has_value()) {
            setSendError(result.error());
            return;
        }
        if (result.value() == 0) {
            setError(RedisError(
                RedisErrorType::REDIS_ERROR_TYPE_SEND_ERROR,
                "Send returned 0"));
            return;
        }

        m_state->sent += result.value();
        if (m_state->sent >= m_state->encoded_buffer.size()) {
            m_state->phase = Phase::Parse;
        }
    }

    bool RedisPipelineAwaitable::isInvalid() const noexcept
    {
        return m_state != nullptr && m_state->phase == Phase::Invalid;
    }

    void RedisPipelineAwaitable::reset() noexcept
    {
        if (!m_state) {
            return;
        }
        m_state->sent = 0;
        m_state->values.clear();
        m_state->parse_buffer.clear();
        m_state->result = std::unexpected(RedisError(
            RedisErrorType::REDIS_ERROR_TYPE_INTERNAL_ERROR,
            "RedisPipelineAwaitable reset"));
        m_state->phase = Phase::Invalid;
    }

    RedisConnectAwaitable::RedisConnectAwaitable(RedisClient& client,
                                                 std::string ip,
                                                 int32_t port,
                                                 std::string username,
                                                 std::string password,
                                                 int32_t db_index,
                                                 int version)
        : m_state(std::make_shared<SharedState>(
              client,
              std::move(ip),
              port,
              std::move(username),
              std::move(password),
              db_index,
              version))
        , m_inner(galay::kernel::AwaitableBuilder<RedisVoidResult>::fromStateMachine(
                      client.socket().controller(),
                      Machine(m_state))
                      .build())
    {
    }

    RedisConnectAwaitable::SharedState::SharedState(RedisClient& client,
                                                    std::string ip_in,
                                                    int32_t port_in,
                                                    std::string username_in,
                                                    std::string password_in,
                                                    int32_t db_index_in,
                                                    int version_in)
        : client(&client)
        , ip(std::move(ip_in))
        , port(port_in)
        , username(std::move(username_in))
        , password(std::move(password_in))
        , db_index(db_index_in)
        , version(version_in)
        , host(version == 6 ? IPType::IPV6 : IPType::IPV4, ip, port)
    {
        client.bufferProvider().clear();
        client.parser() = protocol::RespParser();
    }

    RedisConnectAwaitable::Machine::Machine(std::shared_ptr<SharedState> state)
        : m_state(std::move(state))
    {
    }

    void RedisConnectAwaitable::Machine::setError(RedisError error) noexcept
    {
        m_state->result = std::unexpected(std::move(error));
        m_state->phase = Phase::Invalid;
    }

    void RedisConnectAwaitable::Machine::setConnectError(const IOError& io_error) noexcept
    {
        setError(detail::mapIoErrorToRedisError(
            io_error,
            RedisErrorType::REDIS_ERROR_TYPE_CONNECTION_ERROR));
    }

    void RedisConnectAwaitable::Machine::setSendError(const IOError& io_error) noexcept
    {
        setError(detail::mapIoErrorToRedisError(
            io_error,
            RedisErrorType::REDIS_ERROR_TYPE_SEND_ERROR));
    }

    void RedisConnectAwaitable::Machine::setRecvError(const IOError& io_error) noexcept
    {
        setError(detail::mapIoErrorToRedisError(
            io_error,
            RedisErrorType::REDIS_ERROR_TYPE_RECV_ERROR));
    }

    bool RedisConnectAwaitable::Machine::prepareReadWindow()
    {
        m_state->read_iov_count = m_state->client->bufferProvider().getWriteIovecs(
            m_state->read_iovecs.data(),
            m_state->read_iovecs.size());
        if (m_state->read_iov_count == 0) {
            setError(RedisError(
                RedisErrorType::REDIS_ERROR_TYPE_BUFFER_OVERFLOW_ERROR,
                "No writable iovec for response"));
            return false;
        }
        return true;
    }

    bool RedisConnectAwaitable::Machine::prepareNextCommand()
    {
        RedisCommandBuilder builder;
        if (!m_state->auth_sent && (!m_state->username.empty() || !m_state->password.empty())) {
            m_state->pending_command = PendingCommand::Auth;
            m_state->auth_sent = true;
            m_state->encoded_cmd = m_state->username.empty()
                ? builder.auth(m_state->password).encoded
                : builder.auth(m_state->username, m_state->password).encoded;
            m_state->sent = 0;
            return true;
        }

        if (!m_state->select_sent && m_state->db_index != 0) {
            m_state->pending_command = PendingCommand::Select;
            m_state->select_sent = true;
            m_state->encoded_cmd = builder.select(m_state->db_index).encoded;
            m_state->sent = 0;
            return true;
        }

        m_state->pending_command = PendingCommand::None;
        m_state->encoded_cmd.clear();
        return false;
    }

    std::expected<bool, RedisError> RedisConnectAwaitable::Machine::tryParseReply()
    {
        bool parse_error = false;
        const bool done = detail::parseRepliesFromRingBuffer(
            m_state->client->bufferProvider(),
            m_state->client->parser(),
            m_state->parse_buffer,
            1,
            m_state->values,
            parse_error);

        if (parse_error) {
            return std::unexpected(RedisError(
                RedisErrorType::REDIS_ERROR_TYPE_PARSE_ERROR,
                "Parse response error"));
        }
        if (!done) {
            return false;
        }
        if (m_state->values.empty()) {
            return std::unexpected(RedisError(
                RedisErrorType::REDIS_ERROR_TYPE_PARSE_ERROR,
                "Empty response"));
        }

        RedisValue reply = std::move(m_state->values.front());
        m_state->values.clear();
        if (reply.isError()) {
            const auto error_type = m_state->pending_command == PendingCommand::Auth
                ? RedisErrorType::REDIS_ERROR_TYPE_AUTH_ERROR
                : RedisErrorType::REDIS_ERROR_TYPE_INVALID_ERROR;
            return std::unexpected(RedisError(error_type, reply.toError()));
        }

        if (prepareNextCommand()) {
            m_state->phase = Phase::Send;
        } else {
            m_state->phase = Phase::Done;
            m_state->result = RedisVoidResult{};
        }
        return true;
    }

    galay::kernel::MachineAction<RedisVoidResult>
    RedisConnectAwaitable::Machine::advance()
    {
        if (m_state->result.has_value()) {
            return galay::kernel::MachineAction<result_type>::complete(std::move(*m_state->result));
        }

        switch (m_state->phase) {
        case Phase::Invalid:
            setError(RedisError(
                RedisErrorType::REDIS_ERROR_TYPE_INTERNAL_ERROR,
                "RedisConnectAwaitable in Invalid state"));
            return galay::kernel::MachineAction<result_type>::complete(std::move(*m_state->result));
        case Phase::Connect:
            return galay::kernel::MachineAction<result_type>::waitConnect(m_state->host);
        case Phase::Send:
            if (m_state->sent >= m_state->encoded_cmd.size()) {
                m_state->phase = Phase::Parse;
                return galay::kernel::MachineAction<result_type>::continue_();
            }
            return galay::kernel::MachineAction<result_type>::waitWrite(
                m_state->encoded_cmd.data() + m_state->sent,
                m_state->encoded_cmd.size() - m_state->sent);
        case Phase::Parse: {
            auto parsed = tryParseReply();
            if (!parsed.has_value()) {
                setError(std::move(parsed.error()));
                return galay::kernel::MachineAction<result_type>::complete(std::move(*m_state->result));
            }
            if (parsed.value()) {
                return galay::kernel::MachineAction<result_type>::continue_();
            }
            if (!prepareReadWindow()) {
                return galay::kernel::MachineAction<result_type>::complete(std::move(*m_state->result));
            }
            return galay::kernel::MachineAction<result_type>::waitReadv(
                m_state->read_iovecs.data(),
                m_state->read_iov_count);
        }
        case Phase::Done:
            if (!m_state->result.has_value()) {
                m_state->result = RedisVoidResult{};
            }
            return galay::kernel::MachineAction<result_type>::complete(std::move(*m_state->result));
        }

        setError(RedisError(
            RedisErrorType::REDIS_ERROR_TYPE_INTERNAL_ERROR,
            "Unknown RedisConnectAwaitable state"));
        return galay::kernel::MachineAction<result_type>::complete(std::move(*m_state->result));
    }

    void RedisConnectAwaitable::Machine::onConnect(std::expected<void, IOError> result)
    {
        if (m_state->result.has_value()) {
            return;
        }
        if (!result.has_value()) {
            setConnectError(result.error());
            return;
        }

        m_state->client->setClosed(false);
        if (prepareNextCommand()) {
            m_state->phase = Phase::Send;
        } else {
            m_state->phase = Phase::Done;
            m_state->result = RedisVoidResult{};
        }
    }

    void RedisConnectAwaitable::Machine::onRead(std::expected<size_t, IOError> result)
    {
        if (m_state->result.has_value()) {
            return;
        }
        if (!result.has_value()) {
            setRecvError(result.error());
            return;
        }
        if (result.value() == 0) {
            setError(RedisError(
                RedisErrorType::REDIS_ERROR_TYPE_CONNECTION_CLOSED,
                "Connection closed"));
            return;
        }

        m_state->client->bufferProvider().produce(result.value());
        m_state->phase = Phase::Parse;
    }

    void RedisConnectAwaitable::Machine::onWrite(std::expected<size_t, IOError> result)
    {
        if (m_state->result.has_value()) {
            return;
        }
        if (!result.has_value()) {
            setSendError(result.error());
            return;
        }
        if (result.value() == 0) {
            setError(RedisError(
                RedisErrorType::REDIS_ERROR_TYPE_SEND_ERROR,
                "Send returned 0"));
            return;
        }

        m_state->sent += result.value();
        if (m_state->sent >= m_state->encoded_cmd.size()) {
            m_state->phase = Phase::Parse;
        }
    }

    bool RedisConnectAwaitable::isInvalid() const
    {
        return m_state != nullptr && m_state->phase == Phase::Invalid;
    }

    void RedisConnectAwaitable::reset() noexcept
    {
        if (!m_state) {
            return;
        }
        m_state->sent = 0;
        m_state->parse_buffer.clear();
        m_state->values.clear();
        m_state->result = std::unexpected(RedisError(
            RedisErrorType::REDIS_ERROR_TYPE_INTERNAL_ERROR,
            "RedisConnectAwaitable reset"));
        m_state->phase = Phase::Invalid;
    }

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

    RedisConnectAwaitable RedisClient::connect(const std::string& url)
    {
        std::regex pattern(R"(^redis://(?:([^:@]*)(?::([^@]*))?@)?([a-zA-Z0-9\-\.]+)(?::(\d+))?(?:/(\d+))?$)");
        std::smatch matches;
        std::string username;
        std::string password;
        std::string host;
        int32_t port = 6379;
        int32_t db_index = 0;

        if (std::regex_match(url, matches, pattern)) {
            if (matches.size() > 1 && !matches[1].str().empty()) {
                username = matches[1];
            }
            if (matches.size() > 2 && !matches[2].str().empty()) {
                password = matches[2];
            }
            if (matches.size() > 3 && !matches[3].str().empty()) {
                host = matches[3];
            }
            if (matches.size() > 4 && !matches[4].str().empty()) {
                try {
                    port = std::stoi(matches[4]);
                } catch (const std::exception& e) {
                    RedisLogWarn(m_logger, "Failed to parse port from URL, using default 6379: {}", e.what());
                    port = 6379;
                }
            }
            if (matches.size() > 5 && !matches[5].str().empty()) {
                try {
                    db_index = std::stoi(matches[5]);
                } catch (const std::exception& e) {
                    RedisLogWarn(m_logger, "Failed to parse db_index from URL, using default 0: {}", e.what());
                    db_index = 0;
                }
            }
        }

        using namespace galay::utils;
        std::string ip;
        int version = 2;
        switch (System::checkAddressType(host)) {
        case System::AddressType::IPv4:
            ip = host;
            version = 2;
            break;
        case System::AddressType::IPv6:
            ip = host;
            version = 6;
            break;
        case System::AddressType::Domain:
        case System::AddressType::Invalid:
            ip = System::resolveHostIPv4(host);
            version = 2;
            if (ip.empty()) {
                ip = System::resolveHostIPv6(host);
                version = ip.empty() ? 2 : 6;
            }
            break;
        default:
            ip = host;
            break;
        }

        RedisConnectOptions options;
        options.username = std::move(username);
        options.password = std::move(password);
        options.db_index = db_index;
        options.version = version;
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
} // namespace galay::redis
