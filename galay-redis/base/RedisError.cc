#include "RedisError.h"

namespace galay::redis 
{
    const char* msg[] = {
        "success",
        "url part invalid error",
        "host invalid error",
        "connection error",
        "command error",
        "timeout error",
        "auth error",
        "invalid error",
        "unknown error",
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