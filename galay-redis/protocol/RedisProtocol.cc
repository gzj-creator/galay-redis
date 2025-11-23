#include "RedisProtocol.h"
#include <cstring>
#include <charconv>
#include <sstream>

namespace galay::redis::protocol
{
    // RedisReply实现
    RedisReply::RedisReply()
        : m_type(RespType::Null), m_data(std::monostate{})
    {
    }

    RedisReply::RedisReply(RespType type, RespData data)
        : m_type(type), m_data(std::move(data))
    {
    }

    RedisReply::RedisReply(const RedisReply& other)
        : m_type(other.m_type), m_data(other.m_data)
    {
    }

    RedisReply::RedisReply(RedisReply&& other) noexcept
        : m_type(other.m_type), m_data(std::move(other.m_data))
    {
    }

    RedisReply& RedisReply::operator=(const RedisReply& other)
    {
        if (this != &other) {
            m_type = other.m_type;
            m_data = other.m_data;
        }
        return *this;
    }

    RedisReply& RedisReply::operator=(RedisReply&& other) noexcept
    {
        if (this != &other) {
            m_type = other.m_type;
            m_data = std::move(other.m_data);
        }
        return *this;
    }

    std::string RedisReply::asString() const
    {
        if (auto* str = std::get_if<std::string>(&m_data)) {
            return *str;
        }
        return "";
    }

    int64_t RedisReply::asInteger() const
    {
        if (auto* val = std::get_if<int64_t>(&m_data)) {
            return *val;
        }
        return 0;
    }

    double RedisReply::asDouble() const
    {
        if (auto* val = std::get_if<double>(&m_data)) {
            return *val;
        }
        return 0.0;
    }

    bool RedisReply::asBoolean() const
    {
        if (auto* val = std::get_if<bool>(&m_data)) {
            return *val;
        }
        return false;
    }

    const std::vector<RedisReply>& RedisReply::asArray() const
    {
        static std::vector<RedisReply> empty;
        if (auto* arr = std::get_if<std::vector<RedisReply>>(&m_data)) {
            return *arr;
        }
        return empty;
    }

    const std::vector<std::pair<RedisReply, RedisReply>>& RedisReply::asMap() const
    {
        static std::vector<std::pair<RedisReply, RedisReply>> empty;
        if (auto* map = std::get_if<std::vector<std::pair<RedisReply, RedisReply>>>(&m_data)) {
            return *map;
        }
        return empty;
    }

    // RespParser实现
    RespParser::RespParser()
    {
    }

    RespParser::~RespParser()
    {
    }

    void RespParser::reset()
    {
    }

    std::optional<size_t> RespParser::findCRLF(const char* data, size_t length, size_t offset)
    {
        for (size_t i = offset; i < length - 1; ++i) {
            if (data[i] == '\r' && data[i + 1] == '\n') {
                return i;
            }
        }
        return std::nullopt;
    }

    std::expected<int64_t, ParseError> RespParser::parseIntegerValue(const char* data, size_t length)
    {
        if (length == 0) {
            return std::unexpected(ParseError::InvalidFormat);
        }

        int64_t result = 0;
        bool negative = false;
        size_t i = 0;

        if (data[0] == '-') {
            negative = true;
            i = 1;
        } else if (data[0] == '+') {
            i = 1;
        }

        for (; i < length; ++i) {
            if (data[i] < '0' || data[i] > '9') {
                return std::unexpected(ParseError::InvalidFormat);
            }
            result = result * 10 + (data[i] - '0');
        }

        return negative ? -result : result;
    }

    std::expected<std::pair<size_t, RedisReply>, ParseError>
    RespParser::parse(const char* data, size_t length)
    {
        if (length < 1) {
            return std::unexpected(ParseError::Incomplete);
        }

        char type_marker = data[0];
        switch (type_marker) {
            case '+':  // Simple String
                return parseSimpleString(data, length);
            case '-':  // Error
                return parseError(data, length);
            case ':':  // Integer
                return parseInteger(data, length);
            case '$':  // Bulk String
                return parseBulkString(data, length);
            case '*':  // Array
                return parseArray(data, length);
            case ',':  // Double (RESP3)
                return parseDouble(data, length);
            case '#':  // Boolean (RESP3)
                return parseBoolean(data, length);
            case '%':  // Map (RESP3)
                return parseMap(data, length);
            case '~':  // Set (RESP3)
                return parseSet(data, length);
            default:
                return std::unexpected(ParseError::InvalidType);
        }
    }

    std::expected<std::pair<size_t, RedisReply>, ParseError>
    RespParser::parseSimpleString(const char* data, size_t length)
    {
        auto crlf_pos = findCRLF(data, length, 1);
        if (!crlf_pos) {
            return std::unexpected(ParseError::Incomplete);
        }

        std::string value(data + 1, *crlf_pos - 1);
        RedisReply result(RespType::SimpleString, value);
        return std::make_pair(*crlf_pos + 2, std::move(result));
    }

    std::expected<std::pair<size_t, RedisReply>, ParseError>
    RespParser::parseError(const char* data, size_t length)
    {
        auto crlf_pos = findCRLF(data, length, 1);
        if (!crlf_pos) {
            return std::unexpected(ParseError::Incomplete);
        }

        std::string value(data + 1, *crlf_pos - 1);
        RedisReply result(RespType::Error, value);
        return std::make_pair(*crlf_pos + 2, std::move(result));
    }

    std::expected<std::pair<size_t, RedisReply>, ParseError>
    RespParser::parseInteger(const char* data, size_t length)
    {
        auto crlf_pos = findCRLF(data, length, 1);
        if (!crlf_pos) {
            return std::unexpected(ParseError::Incomplete);
        }

        auto int_result = parseIntegerValue(data + 1, *crlf_pos - 1);
        if (!int_result) {
            return std::unexpected(int_result.error());
        }

        RedisReply result(RespType::Integer, *int_result);
        return std::make_pair(*crlf_pos + 2, std::move(result));
    }

    std::expected<std::pair<size_t, RedisReply>, ParseError>
    RespParser::parseBulkString(const char* data, size_t length)
    {
        // 解析长度
        auto crlf_pos = findCRLF(data, length, 1);
        if (!crlf_pos) {
            return std::unexpected(ParseError::Incomplete);
        }

        auto len_result = parseIntegerValue(data + 1, *crlf_pos - 1);
        if (!len_result) {
            return std::unexpected(len_result.error());
        }

        int64_t str_len = *len_result;

        // 处理空值
        if (str_len == -1) {
            RedisReply result(RespType::Null, std::monostate{});
            return std::make_pair(*crlf_pos + 2, std::move(result));
        }

        if (str_len < 0) {
            return std::unexpected(ParseError::InvalidLength);
        }

        size_t content_start = *crlf_pos + 2;
        size_t content_end = content_start + str_len;

        // 检查是否有足够的数据
        if (content_end + 2 > length) {
            return std::unexpected(ParseError::Incomplete);
        }

        // 验证结尾的\r\n
        if (data[content_end] != '\r' || data[content_end + 1] != '\n') {
            return std::unexpected(ParseError::InvalidFormat);
        }

        std::string value(data + content_start, str_len);
        RedisReply result(RespType::BulkString, value);
        return std::make_pair(content_end + 2, std::move(result));
    }

    std::expected<std::pair<size_t, RedisReply>, ParseError>
    RespParser::parseArray(const char* data, size_t length)
    {
        // 解析数组长度
        auto crlf_pos = findCRLF(data, length, 1);
        if (!crlf_pos) {
            return std::unexpected(ParseError::Incomplete);
        }

        auto len_result = parseIntegerValue(data + 1, *crlf_pos - 1);
        if (!len_result) {
            return std::unexpected(len_result.error());
        }

        int64_t array_len = *len_result;

        // 处理空数组
        if (array_len == -1) {
            RedisReply result(RespType::Null, std::monostate{});
            return std::make_pair(*crlf_pos + 2, std::move(result));
        }

        if (array_len < 0) {
            return std::unexpected(ParseError::InvalidLength);
        }

        std::vector<RedisReply> elements;
        elements.reserve(array_len);
        size_t offset = *crlf_pos + 2;

        for (int64_t i = 0; i < array_len; ++i) {
            if (offset >= length) {
                return std::unexpected(ParseError::Incomplete);
            }

            auto elem_result = parse(data + offset, length - offset);
            if (!elem_result) {
                return std::unexpected(elem_result.error());
            }

            offset += elem_result->first;
            elements.push_back(std::move(elem_result->second));
        }

        RedisReply result(RespType::Array, std::move(elements));
        return std::make_pair(offset, std::move(result));
    }

    std::expected<std::pair<size_t, RedisReply>, ParseError>
    RespParser::parseDouble(const char* data, size_t length)
    {
        auto crlf_pos = findCRLF(data, length, 1);
        if (!crlf_pos) {
            return std::unexpected(ParseError::Incomplete);
        }

        std::string str(data + 1, *crlf_pos - 1);
        double value;

        try {
            value = std::stod(str);
        } catch (...) {
            return std::unexpected(ParseError::InvalidFormat);
        }

        RedisReply result(RespType::Double, value);
        return std::make_pair(*crlf_pos + 2, std::move(result));
    }

    std::expected<std::pair<size_t, RedisReply>, ParseError>
    RespParser::parseBoolean(const char* data, size_t length)
    {
        if (length < 4) {  // #t\r\n or #f\r\n
            return std::unexpected(ParseError::Incomplete);
        }

        bool value;
        if (data[1] == 't') {
            value = true;
        } else if (data[1] == 'f') {
            value = false;
        } else {
            return std::unexpected(ParseError::InvalidFormat);
        }

        if (data[2] != '\r' || data[3] != '\n') {
            return std::unexpected(ParseError::InvalidFormat);
        }

        RedisReply result(RespType::Boolean, value);
        return std::make_pair(4, std::move(result));
    }

    std::expected<std::pair<size_t, RedisReply>, ParseError>
    RespParser::parseMap(const char* data, size_t length)
    {
        // 解析映射大小
        auto crlf_pos = findCRLF(data, length, 1);
        if (!crlf_pos) {
            return std::unexpected(ParseError::Incomplete);
        }

        auto len_result = parseIntegerValue(data + 1, *crlf_pos - 1);
        if (!len_result) {
            return std::unexpected(len_result.error());
        }

        int64_t map_size = *len_result;

        if (map_size < 0) {
            return std::unexpected(ParseError::InvalidLength);
        }

        std::vector<std::pair<RedisReply, RedisReply>> map_data;
        map_data.reserve(map_size);
        size_t offset = *crlf_pos + 2;

        for (int64_t i = 0; i < map_size; ++i) {
            // 解析key
            if (offset >= length) {
                return std::unexpected(ParseError::Incomplete);
            }

            auto key_result = parse(data + offset, length - offset);
            if (!key_result) {
                return std::unexpected(key_result.error());
            }
            offset += key_result->first;

            // 解析value
            if (offset >= length) {
                return std::unexpected(ParseError::Incomplete);
            }

            auto val_result = parse(data + offset, length - offset);
            if (!val_result) {
                return std::unexpected(val_result.error());
            }
            offset += val_result->first;

            map_data.emplace_back(std::move(key_result->second), std::move(val_result->second));
        }

        RedisReply result(RespType::Map, std::move(map_data));
        return std::make_pair(offset, std::move(result));
    }

    std::expected<std::pair<size_t, RedisReply>, ParseError>
    RespParser::parseSet(const char* data, size_t length)
    {
        // 解析集合大小
        auto crlf_pos = findCRLF(data, length, 1);
        if (!crlf_pos) {
            return std::unexpected(ParseError::Incomplete);
        }

        auto len_result = parseIntegerValue(data + 1, *crlf_pos - 1);
        if (!len_result) {
            return std::unexpected(len_result.error());
        }

        int64_t set_size = *len_result;

        if (set_size < 0) {
            return std::unexpected(ParseError::InvalidLength);
        }

        std::vector<RedisReply> set_data;
        set_data.reserve(set_size);
        size_t offset = *crlf_pos + 2;

        for (int64_t i = 0; i < set_size; ++i) {
            if (offset >= length) {
                return std::unexpected(ParseError::Incomplete);
            }

            auto elem_result = parse(data + offset, length - offset);
            if (!elem_result) {
                return std::unexpected(elem_result.error());
            }

            offset += elem_result->first;
            set_data.push_back(std::move(elem_result->second));
        }

        RedisReply result(RespType::Set, std::move(set_data));
        return std::make_pair(offset, std::move(result));
    }

    // RespEncoder实现
    RespEncoder::RespEncoder()
    {
    }

    RespEncoder::~RespEncoder()
    {
    }

    std::string RespEncoder::encodeSimpleString(const std::string& str)
    {
        return "+" + str + "\r\n";
    }

    std::string RespEncoder::encodeError(const std::string& error)
    {
        return "-" + error + "\r\n";
    }

    std::string RespEncoder::encodeInteger(int64_t value)
    {
        return ":" + std::to_string(value) + "\r\n";
    }

    std::string RespEncoder::encodeBulkString(const std::string& str)
    {
        return "$" + std::to_string(str.length()) + "\r\n" + str + "\r\n";
    }

    std::string RespEncoder::encodeNull()
    {
        return "$-1\r\n";
    }

    std::string RespEncoder::encodeArray(const std::vector<std::string>& elements)
    {
        std::string result = "*" + std::to_string(elements.size()) + "\r\n";
        for (const auto& elem : elements) {
            result += encodeBulkString(elem);
        }
        return result;
    }

    std::string RespEncoder::encodeCommand(const std::string& cmd, const std::vector<std::string>& args)
    {
        std::string result = "*" + std::to_string(1 + args.size()) + "\r\n";
        result += encodeBulkString(cmd);
        for (const auto& arg : args) {
            result += encodeBulkString(arg);
        }
        return result;
    }

    std::string RespEncoder::encodeCommand(const std::vector<std::string>& cmd_parts)
    {
        if (cmd_parts.empty()) {
            return "*0\r\n";
        }
        return encodeArray(cmd_parts);
    }

    std::string RespEncoder::encodeDouble(double value)
    {
        return "," + std::to_string(value) + "\r\n";
    }

    std::string RespEncoder::encodeBoolean(bool value)
    {
        return value ? "#t\r\n" : "#f\r\n";
    }

} // namespace galay::redis::protocol
