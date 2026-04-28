#ifndef GALAY_REDIS_PROTOCOL_H
#define GALAY_REDIS_PROTOCOL_H

#include <string>
#include <vector>
#include <variant>
#include <memory>
#include <optional>
#include <expected>
#include <cstdint>
#include <string_view>
#include <charconv>
#include <type_traits>
#include <span>
#include <array>

namespace galay::redis::protocol
{
    // RESP协议类型
    enum class RespType
    {
        SimpleString,   // +
        Error,          // -
        Integer,        // :
        BulkString,     // $
        Array,          // *
        Null,           // $-1\r\n or *-1\r\n
        // RESP3扩展类型
        Double,         // ,
        Boolean,        // #
        BlobError,      // !
        VerbatimString, // =
        BigNumber,      // (
        Map,            // %
        Set,            // ~
        Push            // >
    };

    // Redis响应值的前向声明
    class RedisReply;

    // RESP值类型的变体
    using RespData = std::variant<
        std::string,                           // SimpleString, Error, BulkString
        int64_t,                               // Integer
        double,                                // Double
        bool,                                  // Boolean
        std::vector<RedisReply>,                // Array, Set, Push
        std::vector<std::pair<RedisReply, RedisReply>>, // Map
        std::monostate                         // Null
    >;

    // Redis响应值
    class RedisReply
    {
    public:
        RedisReply();
        RedisReply(RespType type, RespData data);
        RedisReply(const RedisReply& other);
        RedisReply(RedisReply&& other) noexcept;
        RedisReply& operator=(const RedisReply& other);
        RedisReply& operator=(RedisReply&& other) noexcept;

        // 类型判断
        bool isSimpleString() const { return m_type == RespType::SimpleString; }
        bool isError() const { return m_type == RespType::Error; }
        bool isInteger() const { return m_type == RespType::Integer; }
        bool isBulkString() const { return m_type == RespType::BulkString; }
        bool isArray() const { return m_type == RespType::Array; }
        bool isNull() const { return m_type == RespType::Null; }
        bool isDouble() const { return m_type == RespType::Double; }
        bool isBoolean() const { return m_type == RespType::Boolean; }
        bool isMap() const { return m_type == RespType::Map; }
        bool isSet() const { return m_type == RespType::Set; }
        bool isPush() const { return m_type == RespType::Push; }

        // 获取值
        std::string asString() const;
        int64_t asInteger() const;
        double asDouble() const;
        bool asBoolean() const;
        const std::vector<RedisReply>& asArray() const;
        const std::vector<std::pair<RedisReply, RedisReply>>& asMap() const;

        RespType getType() const { return m_type; }
        const RespData& getData() const { return m_data; }

    private:
        RespType m_type;
        RespData m_data;
    };

    // Redis协议解析错误
    enum class ParseError
    {
        Success,
        Incomplete,      // 数据不完整，需要更多数据
        InvalidFormat,   // 格式错误
        InvalidType,     // 无效的类型标识
        InvalidLength,   // 无效的长度
        BufferOverflow   // 缓冲区溢出
    };

    // Redis协议解析器
    class RespParser
    {
    public:
        RespParser();
        ~RespParser();

        // 解析RESP数据
        // 返回: pair<解析的字节数, 解析结果>
        std::expected<std::pair<size_t, RedisReply>, ParseError>
            parse(const char* data, size_t length);

        // 热路径解析接口：输出到 out，避免 pair 临时对象
        std::expected<size_t, ParseError> parseFast(const char* data,
                                                    size_t length,
                                                    RedisReply* out);

        // 重置解析器状态
        void reset();

    private:
        // 解析简单字符串 (+OK\r\n)
        std::expected<size_t, ParseError>
            parseSimpleStringFast(const char* data, size_t length, RedisReply* out);

        // 解析错误 (-Error message\r\n)
        std::expected<size_t, ParseError>
            parseErrorFast(const char* data, size_t length, RedisReply* out);

        // 解析整数 (:1000\r\n)
        std::expected<size_t, ParseError>
            parseIntegerFast(const char* data, size_t length, RedisReply* out);

        // 解析批量字符串 ($6\r\nfoobar\r\n)
        std::expected<size_t, ParseError>
            parseBulkStringFast(const char* data, size_t length, RedisReply* out);

        // 解析数组 (*2\r\n$3\r\nfoo\r\n$3\r\nbar\r\n)
        std::expected<size_t, ParseError>
            parseArrayFast(const char* data, size_t length, RedisReply* out);

        // 解析双精度浮点数 (,1.23\r\n) - RESP3
        std::expected<size_t, ParseError>
            parseDoubleFast(const char* data, size_t length, RedisReply* out);

        // 解析布尔值 (#t\r\n or #f\r\n) - RESP3
        std::expected<size_t, ParseError>
            parseBooleanFast(const char* data, size_t length, RedisReply* out);

        // 解析映射 (%2\r\n+key1\r\n+val1\r\n+key2\r\n+val2\r\n) - RESP3
        std::expected<size_t, ParseError>
            parseMapFast(const char* data, size_t length, RedisReply* out);

        // 解析集合 (~2\r\n+item1\r\n+item2\r\n) - RESP3
        std::expected<size_t, ParseError>
            parseSetFast(const char* data, size_t length, RedisReply* out);

        // 辅助函数：查找\r\n
        std::optional<size_t> findCRLF(const char* data, size_t length, size_t offset = 0);

        // 辅助函数：解析整数
        std::expected<int64_t, ParseError> parseIntegerValue(const char* data, size_t length);
    };

    // Redis协议编码器
    class RespEncoder
    {
    public:
        RespEncoder();
        ~RespEncoder();

        // 编码简单字符串
        std::string encodeSimpleString(const std::string& str);

        // 编码错误
        std::string encodeError(const std::string& error);

        // 编码整数
        std::string encodeInteger(int64_t value);

        // 编码批量字符串
        std::string encodeBulkString(const std::string& str);

        // 编码空值
        std::string encodeNull();

        // 编码数组
        std::string encodeArray(const std::vector<std::string>& elements);

        // 编码Redis命令 (特殊的数组格式) - 模板化版本
        template<typename... Args>
        std::string encodeCommand(const std::string& cmd, Args&&... args);

        // 编码Redis命令 - 支持初始化列表参数
        std::string encodeCommand(const std::string& cmd, std::initializer_list<std::string> args);

        // 编码完整的Redis命令 - 模板化版本，支持任意容器
        template<typename Container>
        std::string encodeCommand(const Container& cmd_parts);

        // 编码完整的Redis命令 - 支持初始化列表
        std::string encodeCommand(std::initializer_list<std::string> cmd_parts);

        // 直接追加编码后的命令，避免每条命令产生临时字符串
        void append(std::string& out, const std::vector<std::string>& cmd_parts) const;
        void append(std::string& out,
                           std::string_view cmd,
                           const std::vector<std::string>& args) const;
        void append(std::string& out,
                           std::string_view cmd,
                           std::span<const std::string_view> args) const;
        void append(std::string& out,
                           std::string_view cmd,
                           std::initializer_list<std::string_view> args) const;

        [[nodiscard]] size_t estimateCommandBytes(std::string_view cmd,
                                                  std::span<const std::string_view> args) const
        {
            size_t total = 1 + decimalDigits(1 + args.size()) + 2;
            total += estimateBulkStringBytes(cmd.size());
            for (const auto& arg : args) {
                total += estimateBulkStringBytes(arg.size());
            }
            return total;
        }

        [[nodiscard]] size_t estimateCommandBytes(std::string_view cmd,
                                                  std::initializer_list<std::string_view> args) const
        {
            return estimateCommandBytes(
                cmd,
                std::span<const std::string_view>(args.begin(), args.size()));
        }

        [[nodiscard]] size_t estimateCommandBytes(std::string_view cmd,
                                                  const std::vector<std::string>& args) const
        {
            size_t total = 1 + decimalDigits(1 + args.size()) + 2;
            total += estimateBulkStringBytes(cmd.size());
            for (const auto& arg : args) {
                total += estimateBulkStringBytes(arg.size());
            }
            return total;
        }

        // Fast-path: append command without reserve (caller must ensure capacity)
        void appendCommandFast(std::string& out, const std::vector<std::string>& cmd_parts) const
        {
            if (cmd_parts.empty()) {
                out += "*0\r\n";
                return;
            }
            out.push_back('*');
            appendUnsignedDecimal(out, cmd_parts.size());
            out += "\r\n";
            for (const auto& part : cmd_parts) {
                appendBulkString(out, part);
            }
        }

        // Fast-path overloads for pre-reserved output buffers.
        void appendCommandFast(std::string& out,
                               std::string_view cmd,
                               std::span<const std::string_view> args) const
        {
            const size_t arg_count = 1 + args.size();
            out.push_back('*');
            appendUnsignedDecimal(out, arg_count);
            out += "\r\n";
            appendBulkString(out, cmd);
            for (const auto& arg : args) {
                appendBulkString(out, arg);
            }
        }

        void appendCommandFast(std::string& out,
                               std::string_view cmd,
                               const std::vector<std::string>& args) const
        {
            const size_t arg_count = 1 + args.size();
            out.push_back('*');
            appendUnsignedDecimal(out, arg_count);
            out += "\r\n";
            appendBulkString(out, cmd);
            for (const auto& arg : args) {
                appendBulkString(out, arg);
            }
        }

        void appendCommandFast(std::string& out,
                               std::string_view cmd,
                               std::initializer_list<std::string_view> args) const
        {
            const size_t arg_count = 1 + args.size();
            out.push_back('*');
            appendUnsignedDecimal(out, arg_count);
            out += "\r\n";
            appendBulkString(out, cmd);
            for (const auto& arg : args) {
                appendBulkString(out, arg);
            }
        }

    private:
        static void appendUnsignedDecimal(std::string& out, size_t value)
        {
            char buf[32];
            auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), value);
            if (ec == std::errc()) {
                out.append(buf, static_cast<size_t>(ptr - buf));
            } else {
                out += std::to_string(value);
            }
        }

        static size_t decimalDigits(size_t value)
        {
            size_t digits = 1;
            while (value >= 10) {
                value /= 10;
                ++digits;
            }
            return digits;
        }

        static size_t estimateBulkStringBytes(size_t value_len)
        {
            return 1 + decimalDigits(value_len) + 2 + value_len + 2;
        }

        static void appendBulkString(std::string& out, std::string_view value)
        {
            out.push_back('$');
            appendUnsignedDecimal(out, value.size());
            out += "\r\n";
            out.append(value.data(), value.size());
            out += "\r\n";
        }

        template<typename T>
        static void appendCommandPart(std::string& out, T&& value)
        {
            using Decayed = std::decay_t<T>;
            if constexpr (std::is_same_v<Decayed, std::string>) {
                appendBulkString(out, value);
            } else if constexpr (std::is_same_v<Decayed, std::string_view>) {
                appendBulkString(out, value);
            } else if constexpr (std::is_same_v<Decayed, const char*> ||
                                 std::is_same_v<Decayed, char*>) {
                appendBulkString(out, value ? std::string_view(value) : std::string_view{});
            } else if constexpr (std::is_integral_v<Decayed>) {
                char buf[32];
                auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), value);
                if (ec == std::errc()) {
                    appendBulkString(out, std::string_view(buf, static_cast<size_t>(ptr - buf)));
                } else {
                    auto str = std::to_string(value);
                    appendBulkString(out, str);
                }
            } else if constexpr (std::is_floating_point_v<Decayed>) {
                auto str = std::to_string(value);
                appendBulkString(out, str);
            } else if constexpr (std::is_convertible_v<T, std::string_view>) {
                appendBulkString(out, std::string_view(value));
            } else {
                auto str = std::string(value);
                appendBulkString(out, str);
            }
        }

        // 辅助函数：将参数转换为字符串
        template<typename T>
        std::string toString(T&& value);

        // 辅助函数：递归构建命令参数
        template<typename T, typename... Rest>
        void buildCommandArgs(std::string& result, T&& first, Rest&&... rest);

        // RESP3扩展
        std::string encodeDouble(double value);
        std::string encodeBoolean(bool value);
    };

    // 模板实现必须在头文件中
    template<typename T>
    std::string RespEncoder::toString(T&& value)
    {
        if constexpr (std::is_same_v<std::decay_t<T>, std::string> ||
                      std::is_same_v<std::decay_t<T>, const char*> ||
                      std::is_convertible_v<T, std::string_view>) {
            return std::string(std::forward<T>(value));
        } else if constexpr (std::is_integral_v<std::decay_t<T>>) {
            return std::to_string(value);
        } else if constexpr (std::is_floating_point_v<std::decay_t<T>>) {
            return std::to_string(value);
        } else {
            // 对于其他类型，尝试转换为字符串
            return std::string(value);
        }
    }

    template<typename T, typename... Rest>
    void RespEncoder::buildCommandArgs(std::string& result, T&& first, Rest&&... rest)
    {
        appendCommandPart(result, std::forward<T>(first));
        if constexpr (sizeof...(rest) > 0) {
            buildCommandArgs(result, std::forward<Rest>(rest)...);
        }
    }

    template<typename... Args>
    std::string RespEncoder::encodeCommand(const std::string& cmd, Args&&... args)
    {
        const size_t arg_count = 1 + sizeof...(args);
        std::string result;
        result.reserve(1 + decimalDigits(arg_count) + 2 + estimateBulkStringBytes(cmd.size()));
        result.push_back('*');
        result += std::to_string(arg_count);
        result += "\r\n";
        appendBulkString(result, cmd);
        if constexpr (sizeof...(args) > 0) {
            buildCommandArgs(result, std::forward<Args>(args)...);
        }
        return result;
    }

    template<typename Container>
    std::string RespEncoder::encodeCommand(const Container& cmd_parts)
    {
        size_t size = 0;
        if constexpr (requires { cmd_parts.size(); }) {
            size = cmd_parts.size();
        } else {
            size = std::distance(std::begin(cmd_parts), std::end(cmd_parts));
        }

        if (size == 0) {
            return "*0\r\n";
        }

        std::string result;
        result.reserve(1 + decimalDigits(size) + 2 + size * 8);
        result.push_back('*');
        result += std::to_string(size);
        result += "\r\n";
        for (const auto& part : cmd_parts) {
            appendCommandPart(result, part);
        }
        return result;
    }

} // namespace galay::redis::protocol

#endif // GALAY_REDIS_PROTOCOL_H
