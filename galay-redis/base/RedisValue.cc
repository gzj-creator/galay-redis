#include "RedisValue.h"

namespace galay::redis
{
    // RedisValue实现
    RedisValue::RedisValue()
        : m_reply()
    {
    }

    RedisValue::RedisValue(protocol::RedisReply reply)
        : m_reply(std::move(reply))
    {
    }

    RedisValue::RedisValue(RedisValue&& other) noexcept
        : m_reply(std::move(other.m_reply))
        , m_cached_array(std::move(other.m_cached_array))
        , m_cached_map(std::move(other.m_cached_map))
        , m_array_cached(other.m_array_cached)
        , m_map_cached(other.m_map_cached)
    {
        other.m_array_cached = false;
        other.m_map_cached = false;
    }

    RedisValue& RedisValue::operator=(RedisValue&& other) noexcept
    {
        if (this != &other) {
            m_reply = std::move(other.m_reply);
            m_cached_array = std::move(other.m_cached_array);
            m_cached_map = std::move(other.m_cached_map);
            m_array_cached = other.m_array_cached;
            m_map_cached = other.m_map_cached;
            other.m_array_cached = false;
            other.m_map_cached = false;
        }
        return *this;
    }

    bool RedisValue::isNull()
    {
        return m_reply.isNull();
    }

    bool RedisValue::isStatus()
    {
        return m_reply.isSimpleString();
    }

    std::string RedisValue::toStatus()
    {
        return m_reply.asString();
    }

    bool RedisValue::isError()
    {
        return m_reply.isError();
    }

    std::string RedisValue::toError()
    {
        return m_reply.asString();
    }

    bool RedisValue::isInteger()
    {
        return m_reply.isInteger();
    }

    int64_t RedisValue::toInteger()
    {
        return m_reply.asInteger();
    }

    bool RedisValue::isString()
    {
        return m_reply.isBulkString();
    }

    std::string RedisValue::toString()
    {
        return m_reply.asString();
    }

    bool RedisValue::isArray()
    {
        return m_reply.isArray();
    }

    std::vector<RedisValue> RedisValue::toArray()
    {
        if (!m_array_cached) {
            m_cached_array.clear();
            if (m_reply.isArray()) {
                const auto& arr = m_reply.asArray();
                m_cached_array.reserve(arr.size());
                for (auto& elem : arr) {
                    // 使用const_cast来绕过const限制，因为我们需要移动
                    m_cached_array.push_back(RedisValue(protocol::RedisReply(const_cast<protocol::RedisReply&>(elem))));
                }
            }
            m_array_cached = true;
        }
        // 返回移动的副本，避免拷贝问题
        std::vector<RedisValue> result;
        result.reserve(m_cached_array.size());
        for (auto& elem : m_cached_array) {
            result.push_back(RedisValue(protocol::RedisReply(elem.m_reply)));
        }
        return result;
    }

    bool RedisValue::isDouble()
    {
        return m_reply.isDouble();
    }

    double RedisValue::toDouble()
    {
        return m_reply.asDouble();
    }

    bool RedisValue::isBool()
    {
        return m_reply.isBoolean();
    }

    bool RedisValue::toBool()
    {
        return m_reply.asBoolean();
    }

    bool RedisValue::isMap()
    {
        return m_reply.isMap();
    }

    std::map<std::string, RedisValue> RedisValue::toMap()
    {
        if (!m_map_cached) {
            m_cached_map.clear();
            if (m_reply.isMap()) {
                const auto& map_data = m_reply.asMap();
                for (auto& [key, value] : map_data) {
                    m_cached_map.emplace(
                        key.asString(),
                        RedisValue(protocol::RedisReply(const_cast<protocol::RedisReply&>(value)))
                    );
                }
            }
            m_map_cached = true;
        }
        // 返回移动的副本，避免拷贝问题
        std::map<std::string, RedisValue> result;
        for (auto& [key, value] : m_cached_map) {
            result.emplace(key, RedisValue(protocol::RedisReply(value.m_reply)));
        }
        return result;
    }

    bool RedisValue::isSet()
    {
        return m_reply.isSet();
    }

    std::vector<RedisValue> RedisValue::toSet()
    {
        std::vector<RedisValue> result;
        if (m_reply.isSet()) {
            const auto& set_data = m_reply.asArray();  // Set uses array internally
            result.reserve(set_data.size());
            for (auto& elem : set_data) {
                result.push_back(RedisValue(protocol::RedisReply(const_cast<protocol::RedisReply&>(elem))));
            }
        }
        return result;
    }

    bool RedisValue::isAttr()
    {
        return false;  // 暂未实现
    }

    bool RedisValue::isPush()
    {
        return m_reply.isPush();
    }

    std::vector<RedisValue> RedisValue::toPush()
    {
        std::vector<RedisValue> result;
        if (m_reply.isPush()) {
            const auto& push_data = m_reply.asArray();
            result.reserve(push_data.size());
            for (auto& elem : push_data) {
                result.push_back(RedisValue(protocol::RedisReply(const_cast<protocol::RedisReply&>(elem))));
            }
        }
        return result;
    }

    bool RedisValue::isBigNumber()
    {
        return false;  // 暂未实现
    }

    std::string RedisValue::toBigNumber()
    {
        return "";  // 暂未实现
    }

    bool RedisValue::isVerb()
    {
        return false;  // 暂未实现
    }

    std::string RedisValue::toVerb()
    {
        return "";  // 暂未实现
    }

    // RedisAsyncValue实现
    RedisAsyncValue::RedisAsyncValue()
        : RedisValue()
    {
    }

    RedisAsyncValue::RedisAsyncValue(protocol::RedisReply reply)
        : RedisValue(std::move(reply))
    {
    }

    RedisAsyncValue::RedisAsyncValue(RedisAsyncValue&& other) noexcept
        : RedisValue(std::move(other))
    {
    }

    RedisAsyncValue& RedisAsyncValue::operator=(RedisAsyncValue&& other) noexcept
    {
        RedisValue::operator=(std::move(other));
        return *this;
    }
}