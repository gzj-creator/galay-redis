#ifndef GALAY_REDIS_ASYNC_SESSION_H
#define GALAY_REDIS_ASYNC_SESSION_H

#include <galay/kernel/async/Socket.h>
#include <galay/kernel/coroutine/CoSchedulerHandle.hpp>
#include <galay/kernel/coroutine/Result.hpp>
#include <galay/kernel/coroutine/AsyncWaiter.hpp>
#include <galay/common/Base.h>
#include <string>
#include <expected>
#include <vector>
#include "../base/RedisError.h"
#include "../base/RedisValue.h"
#include "../protocol/RedisProtocol.h"
#include "galay/common/Log.h"

namespace galay::redis
{
    // 使用galay框架的类型
    using galay::AsyncTcpSocket;
    using galay::CoSchedulerHandle;
    using galay::AsyncResult;
    using galay::Coroutine;
    using galay::nil;
    using galay::Host;
    using galay::Bytes;
    using galay::error::CommonError;

    template<typename T, typename E>
    using AsyncWaiter = galay::AsyncWaiter<T, E>;

    // 异步Redis会话类 - 类似 HttpConnection 的设计模式
    class AsyncRedisSession
    {
    public:
        AsyncRedisSession(CoSchedulerHandle handle);
        // 移动构造和赋值
        AsyncRedisSession(AsyncRedisSession&& other) noexcept;
        AsyncRedisSession& operator=(AsyncRedisSession&& other) noexcept;

        // 禁止拷贝
        AsyncRedisSession(const AsyncRedisSession&) = delete;
        AsyncRedisSession& operator=(const AsyncRedisSession&) = delete;

        // ======================== 连接方法 ========================

        /**
         * @brief 使用URL连接到Redis服务器
         * @param url Redis连接URL，格式：redis://[username:password@]host:port[/db_index]
         * @return 异步结果，成功返回void，失败返回RedisError
         */
        AsyncResult<std::expected<void, RedisError>> connect(const std::string& url);

        /**
         * @brief 连接到Redis服务器（基础版本）
         * @param ip Redis服务器IP地址
         * @param port Redis服务器端口
         * @param username 用户名（Redis 6.0+ ACL，为空则不使用）
         * @param password 密码（为空则不认证）
         * @return 异步结果，成功返回void，失败返回RedisError
         */
        AsyncResult<std::expected<void, RedisError>> connect(const std::string& ip, int32_t port,
                                                              const std::string& username, const std::string& password);

        /**
         * @brief 连接到Redis服务器并选择数据库
         * @param ip Redis服务器IP地址
         * @param port Redis服务器端口
         * @param username 用户名（Redis 6.0+ ACL，为空则不使用）
         * @param password 密码（为空则不认证）
         * @param db_index 数据库索引（0-15）
         * @return 异步结果，成功返回void，失败返回RedisError
         */
        AsyncResult<std::expected<void, RedisError>> connect(const std::string& ip, int32_t port,
                                                              const std::string& username, const std::string& password,
                                                              int32_t db_index);

        /**
         * @brief 连接到Redis服务器并指定协议版本
         * @param ip Redis服务器IP地址
         * @param port Redis服务器端口
         * @param username 用户名（Redis 6.0+ ACL，为空则不使用）
         * @param password 密码（为空则不认证）
         * @param db_index 数据库索引（0-15）
         * @param version RESP协议版本（2或3）
         * @return 异步结果，成功返回void，失败返回RedisError
         */
        AsyncResult<std::expected<void, RedisError>> connect(const std::string& ip, int32_t port,
                                                              const std::string& username, const std::string& password,
                                                              int32_t db_index, int version);
        // 可变参数模板版本 - 避免创建临时 vector（高性能版本）
        template<typename... Args>
        AsyncResult<std::expected<RedisValue, RedisError>> execute(const std::string& cmd, Args&&... args)
        {
            // 检查会话是否已关闭
            if (m_is_closed) {
                auto waiter = std::make_shared<AsyncWaiter<RedisValue, RedisError>>();
                waiter->notify(std::unexpected(RedisError(ConnectionClosed, "Session is closed")));
                return waiter->wait();
            }

            // 构建命令参数列表
            std::vector<std::string> cmd_parts;
            cmd_parts.reserve(1 + sizeof...(args));
            cmd_parts.push_back(cmd);
            (cmd_parts.push_back(std::forward<Args>(args)), ...);

            // 编码并执行命令
            auto encoded = m_encoder.encodeCommand(cmd_parts);
            return executeCommand(std::move(encoded));
        }

        // ======================== 基础Redis命令 ========================

        /**
         * @brief 使用密码认证（Redis 5.0之前版本）
         * @param password 密码
         * @return 异步结果，成功返回 SimpleString "OK"
         */
        AsyncResult<std::expected<RedisValue, RedisError>> auth(const std::string& password);

        /**
         * @brief 使用用户名和密码认证（Redis 6.0+ ACL）
         * @param username 用户名
         * @param password 密码
         * @return 异步结果，成功返回 SimpleString "OK"
         */
        AsyncResult<std::expected<RedisValue, RedisError>> auth(const std::string& username, const std::string& password);

        /**
         * @brief 切换到指定的数据库
         * @param db_index 数据库索引（0-15，默认有16个数据库）
         * @return 异步结果，成功返回 SimpleString "OK"
         */
        AsyncResult<std::expected<RedisValue, RedisError>> select(int32_t db_index);

        /**
         * @brief 测试连接是否正常
         * @return 异步结果，返回 SimpleString "PONG"
         */
        AsyncResult<std::expected<RedisValue, RedisError>> ping();

        /**
         * @brief 回显字符串
         * @param message 要回显的消息
         * @return 异步结果，返回 BulkString 相同的消息
         */
        AsyncResult<std::expected<RedisValue, RedisError>> echo(const std::string& message);

        // ======================== String操作 ========================

        /**
         * @brief 获取键的值
         * @param key 键名
         * @return 异步结果，键存在返回 BulkString 值，不存在返回 Null
         */
        AsyncResult<std::expected<RedisValue, RedisError>> get(const std::string& key);

        /**
         * @brief 设置键的值
         * @param key 键名
         * @param value 要设置的值
         * @return 异步结果，成功返回 SimpleString "OK"
         */
        AsyncResult<std::expected<RedisValue, RedisError>> set(const std::string& key, const std::string& value);

        /**
         * @brief 设置键的值并指定过期时间（秒）
         * @param key 键名
         * @param seconds 过期时间（秒）
         * @param value 要设置的值
         * @return 异步结果，成功返回 SimpleString "OK"
         */
        AsyncResult<std::expected<RedisValue, RedisError>> setex(const std::string& key, int64_t seconds, const std::string& value);

        /**
         * @brief 删除一个键
         * @param key 键名
         * @return 异步结果，返回 Integer 删除的键数量（0或1）
         */
        AsyncResult<std::expected<RedisValue, RedisError>> del(const std::string& key);

        /**
         * @brief 检查键是否存在
         * @param key 键名
         * @return 异步结果，返回 Integer 1（存在）或 0（不存在）
         */
        AsyncResult<std::expected<RedisValue, RedisError>> exists(const std::string& key);

        /**
         * @brief 将键的整数值增1
         * @param key 键名
         * @return 异步结果，返回 Integer 增加后的值
         */
        AsyncResult<std::expected<RedisValue, RedisError>> incr(const std::string& key);

        /**
         * @brief 将键的整数值减1
         * @param key 键名
         * @return 异步结果，返回 Integer 减少后的值
         */
        AsyncResult<std::expected<RedisValue, RedisError>> decr(const std::string& key);

        // ======================== Hash操作 ========================

        /**
         * @brief 获取哈希表中指定字段的值
         * @param key 哈希表的键名
         * @param field 字段名
         * @return 异步结果，返回 BulkString 字段的值，不存在返回 Null
         */
        AsyncResult<std::expected<RedisValue, RedisError>> hget(const std::string& key, const std::string& field);

        /**
         * @brief 设置哈希表中字段的值
         * @param key 哈希表的键名
         * @param field 字段名
         * @param value 字段值
         * @return 异步结果，返回 Integer 1（新字段）或 0（更新已有字段）
         */
        AsyncResult<std::expected<RedisValue, RedisError>> hset(const std::string& key, const std::string& field, const std::string& value);

        /**
         * @brief 删除哈希表中的字段
         * @param key 哈希表的键名
         * @param field 要删除的字段名
         * @return 异步结果，返回 Integer 删除的字段数量（0或1）
         */
        AsyncResult<std::expected<RedisValue, RedisError>> hdel(const std::string& key, const std::string& field);

        /**
         * @brief 获取哈希表中的所有字段和值
         * @param key 哈希表的键名
         * @return 异步结果，返回 Array 包含字段名和值的交替序列
         */
        AsyncResult<std::expected<RedisValue, RedisError>> hgetAll(const std::string& key);

        // ======================== List操作 ========================

        /**
         * @brief 将值插入列表头部
         * @param key 列表的键名
         * @param value 要插入的值
         * @return 异步结果，返回 Integer 列表的长度
         */
        AsyncResult<std::expected<RedisValue, RedisError>> lpush(const std::string& key, const std::string& value);

        /**
         * @brief 将值插入列表尾部
         * @param key 列表的键名
         * @param value 要插入的值
         * @return 异步结果，返回 Integer 列表的长度
         */
        AsyncResult<std::expected<RedisValue, RedisError>> rpush(const std::string& key, const std::string& value);

        /**
         * @brief 移除并返回列表头部的元素
         * @param key 列表的键名
         * @return 异步结果，返回 BulkString 弹出的元素，列表空时返回 Null
         */
        AsyncResult<std::expected<RedisValue, RedisError>> lpop(const std::string& key);

        /**
         * @brief 移除并返回列表尾部的元素
         * @param key 列表的键名
         * @return 异步结果，返回 BulkString 弹出的元素，列表空时返回 Null
         */
        AsyncResult<std::expected<RedisValue, RedisError>> rpop(const std::string& key);

        /**
         * @brief 获取列表的长度
         * @param key 列表的键名
         * @return 异步结果，返回 Integer 列表长度
         */
        AsyncResult<std::expected<RedisValue, RedisError>> llen(const std::string& key);

        /**
         * @brief 获取列表指定范围的元素
         * @param key 列表的键名
         * @param start 开始索引（0表示第一个元素，-1表示最后一个元素）
         * @param stop 结束索引
         * @return 异步结果，返回 Array 包含指定范围的元素
         */
        AsyncResult<std::expected<RedisValue, RedisError>> lrange(const std::string& key, int64_t start, int64_t stop);

        // ======================== Set操作 ========================

        /**
         * @brief 向集合添加成员
         * @param key 集合的键名
         * @param member 要添加的成员
         * @return 异步结果，返回 Integer 添加的成员数量（0表示已存在，1表示新增）
         */
        AsyncResult<std::expected<RedisValue, RedisError>> sadd(const std::string& key, const std::string& member);

        /**
         * @brief 从集合移除成员
         * @param key 集合的键名
         * @param member 要移除的成员
         * @return 异步结果，返回 Integer 移除的成员数量（0或1）
         */
        AsyncResult<std::expected<RedisValue, RedisError>> srem(const std::string& key, const std::string& member);

        /**
         * @brief 获取集合的所有成员
         * @param key 集合的键名
         * @return 异步结果，返回 Array 包含所有成员的数组
         */
        AsyncResult<std::expected<RedisValue, RedisError>> smembers(const std::string& key);

        /**
         * @brief 获取集合的成员数量
         * @param key 集合的键名
         * @return 异步结果，返回 Integer 集合大小
         */
        AsyncResult<std::expected<RedisValue, RedisError>> scard(const std::string& key);

        // ======================== Sorted Set操作 ========================

        /**
         * @brief 向有序集合添加成员
         * @param key 有序集合的键名
         * @param score 成员的分数
         * @param member 要添加的成员
         * @return 异步结果，返回 Integer 添加的成员数量（0表示已存在且更新了分数，1表示新增）
         */
        AsyncResult<std::expected<RedisValue, RedisError>> zadd(const std::string& key, double score, const std::string& member);

        /**
         * @brief 从有序集合移除成员
         * @param key 有序集合的键名
         * @param member 要移除的成员
         * @return 异步结果，返回 Integer 移除的成员数量（0或1）
         */
        AsyncResult<std::expected<RedisValue, RedisError>> zrem(const std::string& key, const std::string& member);

        /**
         * @brief 获取有序集合指定范围的成员（按分数排序）
         * @param key 有序集合的键名
         * @param start 开始索引
         * @param stop 结束索引
         * @return 异步结果，返回 Array 包含指定范围的成员数组
         */
        AsyncResult<std::expected<RedisValue, RedisError>> zrange(const std::string& key, int64_t start, int64_t stop);

        /**
         * @brief 获取有序集合中成员的分数
         * @param key 有序集合的键名
         * @param member 成员名
         * @return 异步结果，返回 BulkString 成员的分数，不存在返回 Null
         */
        AsyncResult<std::expected<RedisValue, RedisError>> zscore(const std::string& key, const std::string& member);

        // 关闭连接
        AsyncResult<std::expected<void, CommonError>> close();
        bool isClosed() const;
        void markClosed();

        ~AsyncRedisSession() = default;

    private:
        // 私有构造函数 - 仅供工厂方法使用
        AsyncRedisSession(AsyncTcpSocket&& socket, CoSchedulerHandle handle);

        // 发送编码的命令
        AsyncResult<std::expected<void, RedisError>> sendCommand(const std::string& encoded_cmd);

        // 接收并解析Redis响应
        AsyncResult<std::expected<protocol::RedisReply, RedisError>> receiveReply();

        // 执行命令的内部实现
        AsyncResult<std::expected<RedisValue, RedisError>> executeCommand(std::string&& encoded_cmd);

        // 成员变量
        bool m_is_closed = false;
        AsyncTcpSocket m_socket;
        CoSchedulerHandle m_handle;
        protocol::RespEncoder m_encoder;
        protocol::RespParser m_parser;
        std::vector<char> m_recv_buffer;
        size_t m_buffer_pos = 0;  // 缓冲区当前位置
        static constexpr size_t BUFFER_SIZE = 8192;
        Logger::uptr m_logger = Logger::createStdoutLoggerMT("AsyncRedisLogger");
    };

}

#endif // GALAY_REDIS_ASYNC_SESSION_H
