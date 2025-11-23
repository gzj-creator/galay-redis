#ifndef GALAY_REDIS_PROTOCOL_H
#define GALAY_REDIS_PROTOCOL_H

#include <string>
#include <vector>
#include <variant>
#include <memory>
#include <optional>
#include <expected>
#include <cstdint>

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

        // 重置解析器状态
        void reset();

    private:
        // 解析简单字符串 (+OK\r\n)
        std::expected<std::pair<size_t, RedisReply>, ParseError>
            parseSimpleString(const char* data, size_t length);

        // 解析错误 (-Error message\r\n)
        std::expected<std::pair<size_t, RedisReply>, ParseError>
            parseError(const char* data, size_t length);

        // 解析整数 (:1000\r\n)
        std::expected<std::pair<size_t, RedisReply>, ParseError>
            parseInteger(const char* data, size_t length);

        // 解析批量字符串 ($6\r\nfoobar\r\n)
        std::expected<std::pair<size_t, RedisReply>, ParseError>
            parseBulkString(const char* data, size_t length);

        // 解析数组 (*2\r\n$3\r\nfoo\r\n$3\r\nbar\r\n)
        std::expected<std::pair<size_t, RedisReply>, ParseError>
            parseArray(const char* data, size_t length);

        // 解析双精度浮点数 (,1.23\r\n) - RESP3
        std::expected<std::pair<size_t, RedisReply>, ParseError>
            parseDouble(const char* data, size_t length);

        // 解析布尔值 (#t\r\n or #f\r\n) - RESP3
        std::expected<std::pair<size_t, RedisReply>, ParseError>
            parseBoolean(const char* data, size_t length);

        // 解析映射 (%2\r\n+key1\r\n+val1\r\n+key2\r\n+val2\r\n) - RESP3
        std::expected<std::pair<size_t, RedisReply>, ParseError>
            parseMap(const char* data, size_t length);

        // 解析集合 (~2\r\n+item1\r\n+item2\r\n) - RESP3
        std::expected<std::pair<size_t, RedisReply>, ParseError>
            parseSet(const char* data, size_t length);

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

        // 编码Redis命令 (特殊的数组格式)
        std::string encodeCommand(const std::string& cmd, const std::vector<std::string>& args);

        // 编码完整的Redis命令
        std::string encodeCommand(const std::vector<std::string>& cmd_parts);

        // RESP3扩展
        std::string encodeDouble(double value);
        std::string encodeBoolean(bool value);
    };

} // namespace galay::redis::protocol

#endif // GALAY_REDIS_PROTOCOL_H
