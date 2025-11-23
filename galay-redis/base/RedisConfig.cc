#include "RedisConfig.h"

namespace galay::redis
{
    void RedisConfig::connectWithTimeout(uint64_t timeout)
    {
        m_params = timeout;
        m_connection_option = RedisConnectionOption::kRedisConnectionWithTimeout;
    }

    void RedisConfig::connectWithBind(const std::string &addr)
    {
        m_params = addr;
        m_connection_option = RedisConnectionOption::kRedisConnectionWithBind;
    }

    void RedisConfig::connectWithBindAndReuse(const std::string &addr)
    {
        m_params = addr;
        m_connection_option = RedisConnectionOption::kRedisConnectionWithBindAndReuse;
    }

    void RedisConfig::connectWithUnix(const std::string &path)
    {
        m_params = path;
        m_connection_option = RedisConnectionOption::kRedisConnectionWithUnix;
    }

    void RedisConfig::connectWithUnixAndTimeout(const std::string &path, uint64_t timeout)
    {
        m_params = std::make_pair(path, timeout);
        m_connection_option = RedisConnectionOption::kRedisConnectionWithUnixAndTimeout;
    }

    RedisConnectionOption &RedisConfig::getConnectOption()
    {
        return m_connection_option;
    }

    std::any &RedisConfig::getParams()
    {
        return m_params;
    }
}