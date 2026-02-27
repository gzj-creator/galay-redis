#include "RedisError.h"
#include <array>

namespace galay::redis
{
    static constexpr std::array<const char*, 21> msg = {{
        "success",
        "url invalid error",
        "host invalid error",
        "port invalid error",
        "db index invalid error",
        "address type invalid error",
        "version invalid error",
        "connection error",
        "free redis object error",
        "command error",
        "timeout error",
        "auth error",
        "invalid error",
        "unknown error",
        "parse error",
        "send error",
        "recv error",
        "buffer overflow error",
        "network error",
        "connection closed",
        "internal error",
    }};


    RedisError::RedisError(RedisErrorType type)
        : m_type(type)
    {
    }

    RedisError::RedisError(RedisErrorType type, std::string extra_msg)
        : m_type(type), m_extra_msg(extra_msg)
    {
    }

    RedisErrorType RedisError::type() const
    {
        return m_type;
    }

    std::string RedisError::message() const
    {
        auto idx = static_cast<size_t>(m_type);
        const char* base_msg = (idx < msg.size()) ? msg[idx] : "unknown error";
        if(! m_extra_msg.empty()) {
            return std::string(base_msg) + " extra:" + m_extra_msg;
        }
        return base_msg;
    }
}
