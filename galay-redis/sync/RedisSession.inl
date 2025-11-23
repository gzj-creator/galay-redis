#ifndef GALAY_REDIS_SYNC_SESSION_INL
#define GALAY_REDIS_SYNC_SESSION_INL

#include "RedisSession.h"

namespace galay::redis 
{

template <KVPair... KV>
inline std::expected<RedisValue, RedisError> RedisSession::mset(KV... pairs)
{
    m_stream << "MSET";
    ((m_stream << " " << std::get<0>(pairs) << " " << std::get<1>(pairs)), ...);
    auto reply = redisCommand(m_stream.str());
    m_stream.str("");
    return reply;
}

template<KeyType... Key>
inline std::expected<RedisValue, RedisError> RedisSession::mget(Key... keys)
{
    m_stream << "MGET";
    ((m_stream << " " << keys), ...);
    auto reply = redisCommand(m_stream.str());
    m_stream.str("");
    return reply;
}

template <KeyType... Key>
inline std::expected<RedisValue, RedisError> RedisSession::hdel(const std::string& key, Key... fields)
{
    m_stream << "HDEL " << key;
    ((m_stream << " " << fields), ...);
    auto reply = redisCommand(m_stream.str());
    m_stream.str("");
    return reply;
}

template <KeyType... Key>
inline std::expected<RedisValue, RedisError> RedisSession::hmget(const std::string &key, Key... field)
{
    m_stream << "HMGET " << key;
    ((m_stream << " " << field), ...);
    auto reply = redisCommand(m_stream.str());
    m_stream.str("");
    return reply;
}

template <KVPair... KV>
inline std::expected<RedisValue, RedisError> RedisSession::hmset(const std::string& key, KV... pairs)
{
    m_stream << "HMSET " << key;
    ((m_stream << " " << std::get<0>(pairs) << " " << std::get<1>(pairs)), ...);
    auto reply = redisCommand(m_stream.str());
    m_stream.str("");
    return reply;
}

template <ValType... Val>
inline std::expected<RedisValue, RedisError> RedisSession::lpush(const std::string& key, Val... values)
{
    m_stream << "LPUSH " << key;
    ((m_stream << " " << values), ...);
    auto reply = redisCommand(m_stream.str());
    m_stream.str("");
    return reply;
}

template <ValType... Val>
inline std::expected<RedisValue, RedisError> RedisSession::rpush(const std::string& key, Val... values)
{
    m_stream << "RPUSH " << key;
    ((m_stream << " " << values), ...);
    auto reply = redisCommand(m_stream.str());
    m_stream.str("");
    return reply;
}

template <ValType... Val>
inline std::expected<RedisValue, RedisError> RedisSession::sadd(const std::string &key, Val... members)
{
    m_stream << "SADD " << key;
    ((m_stream << " " << members), ...);
    auto reply = redisCommand(m_stream.str());
    m_stream.str("");
    return reply;
}

template <ValType... Val>
inline std::expected<RedisValue, RedisError> RedisSession::srem(const std::string &key, Val... members)
{
    m_stream << "SREM " << key;
    ((m_stream << " " << members), ...);
    auto reply = redisCommand(m_stream.str());
    m_stream.str("");
    return reply;
}

template <KeyType... Key>
inline std::expected<RedisValue, RedisError> RedisSession::sinter(Key... keys)
{
    m_stream << "SINTER";
    ((m_stream << " " << keys), ...);
    auto reply = redisCommand(m_stream.str());
    m_stream.str("");
    return reply;
}

template <KeyType... Key>
inline std::expected<RedisValue, RedisError> RedisSession::sunion(Key... keys)
{
    m_stream << "SUNION";
    ((m_stream << " " << keys), ...);
    auto reply = redisCommand(m_stream.str());
    m_stream.str("");
    return reply;
}

template <ScoreValType... KV>
inline std::expected<RedisValue, RedisError> RedisSession::zadd(const std::string &key, KV... values)
{
    m_stream << "ZADD " << key;
    ((m_stream << " " << std::to_string(std::get<0>(values)) << " " << std::get<1>(values)), ...);
    auto reply = redisCommand(m_stream.str());
    m_stream.str("");
    return reply;
}

template <KeyType... Key>
inline std::expected<RedisValue, RedisError> RedisSession::zrem(const std::string &key, Key... members)
{
    m_stream << "ZREM " << key;
    ((m_stream << " " << members), ...);
    auto reply = redisCommand(m_stream.str());
    m_stream.str("");
    return reply;
}

}


#endif