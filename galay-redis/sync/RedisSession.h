#ifndef GALAY_REDIS_SYNC_SESSION_H
#define GALAY_REDIS_SYNC_SESSION_H

#include <galay/kernel/coroutine/CoSchedulerHandle.hpp>
#include <string>
#include <memory>
#include <galay/common/Common.h>
#include <galay/kernel/runtime/Runtime.h>
#include <galay/kernel/coroutine/Result.hpp>
#include "base/RedisError.h"
#include "base/RedisConfig.h"
#include "base/RedisValue.h"
#include "base/RedisBase.h"
#include "protocol/Connection.h"
#include "protocol/RedisProtocol.h"

namespace galay::redis 
{
    class RedisSession 
    {
    public:

        RedisSession(RedisConfig config);
        RedisSession(RedisConfig config, Logger::uptr logger);

        //redis://user:password@host:port/db_index
        std::expected<void, RedisError> connect(const std::string& url);
        std::expected<void, RedisError> connect(const std::string& ip, int32_t port, const std::string& username, const std::string& password);
        std::expected<void, RedisError> connect(const std::string& ip, int32_t port, const std::string& username, const std::string& password, int32_t db_index);
        std::expected<void, RedisError> connect(const std::string& ip, int32_t port, const std::string& username, const std::string& password, int32_t db_index, int version);
        
        std::expected<void, RedisError> disconnect();
        /*
        * return : status
        */
       std::expected<RedisValue, RedisError> selectDB(int32_t db_index);
        /*
        * return : status
        */
       std::expected<RedisValue, RedisError> flushDB();
        /*
        * role: 切换版本
        * RESP2 return: array
        * RESP3 return: map
        */
       std::expected<RedisValue, RedisError> switchVersion(int version);
        /*
        * return: integer(1, exist, 0, not exist)
        */
       std::expected<RedisValue, RedisError> exist(const std::string &key);
        /*
        * return: RedisValue
        */
       std::expected<RedisValue, RedisError> get(const std::string& key);
        /*
        * return: status
        */
       std::expected<RedisValue, RedisError> set(const std::string& key, const std::string& value);
        /*
        * return: status
        */
        template <KVPair... KV>
       std::expected<RedisValue, RedisError> mset(KV... pairs);
        /*
        * return: RedisValue array
        */
        template<KeyType... Key>
       std::expected<RedisValue, RedisError> mget(Key... keys);
        /*
        * return: integer
        */
       std::expected<RedisValue, RedisError> del(const std::string &key);
        /*
        * return: status
        */
       std::expected<RedisValue, RedisError> setEx(const std::string& key, int64_t seconds, const std::string& value);
        /*
        * return: status
        */
       std::expected<RedisValue, RedisError> psetEx(const std::string& key, int64_t milliseconds, const std::string& value); 
        /*
        * return: integer
        */
       std::expected<RedisValue, RedisError> incr(const std::string& key);
        /*
        * return: integer
        */
       std::expected<RedisValue, RedisError> incrBy(std::string key, int64_t value);
        /*
        * return: integer
        */
       std::expected<RedisValue, RedisError> decr(const std::string& key);
        /*
        * return: RedisValue
        */
       std::expected<RedisValue, RedisError> hget(const std::string& key, const std::string& field);
        /*
        * return: status
        */
       std::expected<RedisValue, RedisError> hset(const std::string& key, const std::string& field, const std::string& value);
        /*
        * return: integer
        */
        template <KeyType... Key>
       std::expected<RedisValue, RedisError> hdel(const std::string& key, Key... fields);
        /*
        * return: integer
        */
        template <KeyType... Key>
       std::expected<RedisValue, RedisError> hmget(const std::string& key, Key... field);
        /*
        * return: integer
        */
        template<KVPair... KV>
       std::expected<RedisValue, RedisError> hmset(const std::string& key, KV... fields);
        /*
        * return: RedisValue map or array
        */
       std::expected<RedisValue, RedisError> hgetAll(const std::string& key);
        /*
        * return: integer
        */
       std::expected<RedisValue, RedisError> hincrBy(const std::string& key, std::string field, int64_t value);
        /*
        * return: integer
        */
        template <ValType... Val>
       std::expected<RedisValue, RedisError> lpush(const std::string& key, Val... values);
        /*
        * return: integer
        */
        template <ValType... Val>
       std::expected<RedisValue, RedisError> rpush(const std::string& key, Val... values);
        /*
        * return: integer
        */
       std::expected<RedisValue, RedisError> lLen(const std::string& key);
        /*
        * return: RedisValue array
        */
       std::expected<RedisValue, RedisError> lrange(const std::string& key, int64_t start, int64_t end);
        /*
        * return: integer
        */
       std::expected<RedisValue, RedisError> lrem(const std::string& key, const std::string& value, int64_t count);
        /*
        * return: integer
        */
        template <ValType... Val>
       std::expected<RedisValue, RedisError> sadd(const std::string& key, Val... members);
        /*
        * return: RedisValue array
        */
       std::expected<RedisValue, RedisError> smembers(const std::string& key);
        /*
        * return: integer
        */
        template <ValType... Val>
       std::expected<RedisValue, RedisError> srem(const std::string& key, Val... members);
        /*
        * return: RedisValue array or set
        */
        template<KeyType... Key>
       std::expected<RedisValue, RedisError> sinter(Key... keys);
        /*
        * return: RedisValue array or set
        */
        template<KeyType... Key>
       std::expected<RedisValue, RedisError> sunion(Key... keys);
        /*
        * return: integer
        */
       std::expected<RedisValue, RedisError> smove(const std::string& source, const std::string& destination, const std::string& member);
        /*
        * return: integer
        */
       std::expected<RedisValue, RedisError> scard(const std::string& key);

        /*
        * return: integer
        */
        template <ScoreValType... KV>
       std::expected<RedisValue, RedisError> zadd(const std::string& key, KV... values);
        /*
        * return: RedisValue array
        */
       std::expected<RedisValue, RedisError> zrange(const std::string& key, uint32_t beg, uint32_t end);
        /*
        * return: string or double
        */
       std::expected<RedisValue, RedisError> zscore(const std::string& key, const std::string& member);
        
        template <KeyType... Key>
       std::expected<RedisValue, RedisError> zrem(const std::string& key, Key... members);


       std::expected<RedisValue, RedisError> redisCommand(const std::string& cmd);

        ~RedisSession();
    private:
        Logger::uptr m_logger;
        std::ostringstream m_stream;
        std::unique_ptr<protocol::Connection> m_connection;
        protocol::RespEncoder m_encoder;
        RedisConfig m_config;
    };

}


#endif