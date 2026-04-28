#ifndef GALAY_REDIS_CONFIG_H
#define GALAY_REDIS_CONFIG_H

#include <any>
#include <string>
#include <cstdint>

namespace galay::redis
{
    enum class RedisConnectionOption {
        kRedisConnectionWithNull,
        kRedisConnectionWithTimeout,				//设置超时时间,只适用于RedisSession，异步设置无效
        kRedisConnectionWithBind,                   //绑定本地地址
        kRedisConnectionWithBindAndReuse,           //绑定本地地址并设置SO_REUSEADDR
        kRedisConnectionWithUnix,                   //使用unix域套接字
        kRedisConnectionWithUnixAndTimeout,			//设置超时时间,只适用于RedisSession，异步设置无效
    };


    class RedisConfig 
    {
    public: 
        void connectWithTimeout(uint64_t timeout);
        void connectWithBind(const std::string& addr);
        void connectWithBindAndReuse(const std::string& addr);
        void connectWithUnix(const std::string& path);
        void connectWithUnixAndTimeout(const std::string& path, uint64_t timeout);

        RedisConnectionOption& getConnectOption();
        std::any& getParams();

    private:
        std::any m_params;
        RedisConnectionOption m_connection_option = RedisConnectionOption::kRedisConnectionWithNull;
    };
}

#endif