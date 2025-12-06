#include "RedisError.h"

namespace galay::redis 
{
    const char* msg[] = {
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
    };


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
        if(! m_extra_msg.empty()) {
            return std::string(msg[static_cast<int>(m_type)]) + " extra:" + m_extra_msg;
        } 
        return msg[static_cast<int>(m_type)];
    }
}