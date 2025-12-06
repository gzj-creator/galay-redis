#ifndef GALAY_REDIS_VALUE_H
#define GALAY_REDIS_VALUE_H

#include "../protocol/RedisProtocol.h"
#include <string>
#include <vector>
#include <map>

namespace galay::redis
{
    // RedisValue类，基于protocol::RedisReply实现
    class RedisValue
    {
    public:
        RedisValue();
        explicit RedisValue(protocol::RedisReply reply);
        RedisValue(RedisValue&& other) noexcept;
        RedisValue& operator=(RedisValue&& other) noexcept;
        RedisValue(const RedisValue&) = delete;
        RedisValue& operator=(const RedisValue&) = delete;

        // 静态工厂方法
        static RedisValue fromError(const std::string& error_msg);

        // RESP2
        bool isNull();
        bool isStatus();
        std::string toStatus();
        bool isError();
        std::string toError();
        bool isInteger();
        int64_t toInteger();
        bool isString();
        std::string toString();
        bool isArray();

        //vector的生命周期需要小于等于RedisValue的生命周期
        std::vector<RedisValue> toArray();

        //Resp3
        bool isDouble();
        double toDouble();
        bool isBool();
        bool toBool();
        bool isMap();
        //map的生命周期需要小于等于RedisValue的生命周期
        std::map<std::string, RedisValue> toMap();
        bool isSet();
        //set的生命周期需要小于等于RedisValue的生命周期
        std::vector<RedisValue> toSet();
        bool isAttr();
        bool isPush();
        //push的生命周期需要小于等于RedisValue的生命周期
        std::vector<RedisValue> toPush();
        bool isBigNumber();
        std::string toBigNumber();
        //不转义字符串
        bool isVerb();
        std::string toVerb();

        // 获取底层RedisReply
        const protocol::RedisReply& getReply() const { return m_reply; }
        protocol::RedisReply& getReply() { return m_reply; }

        virtual ~RedisValue() = default;

    protected:
        protocol::RedisReply m_reply;

    private:
        // 缓存转换后的数组和map
        mutable std::vector<RedisValue> m_cached_array;
        mutable std::map<std::string, RedisValue> m_cached_map;
        mutable bool m_array_cached = false;
        mutable bool m_map_cached = false;
    };

    class RedisAsyncValue: public RedisValue
    {
    public:
        RedisAsyncValue();
        explicit RedisAsyncValue(protocol::RedisReply reply);
        RedisAsyncValue(RedisAsyncValue&& other) noexcept;
        RedisAsyncValue& operator=(RedisAsyncValue&& other) noexcept;
        RedisAsyncValue(const RedisAsyncValue&) = delete;
        RedisAsyncValue& operator=(const RedisAsyncValue&) = delete;
        ~RedisAsyncValue() = default;
    };
}

#endif