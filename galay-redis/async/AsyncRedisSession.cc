#include "AsyncRedisSession.h"
#include "base/RedisLog.h"
#include <galay/kernel/coroutine/AsyncWaiter.hpp>
#include <galay/kernel/async/AsyncFactory.h>
#include <galay/common/Error.h>

namespace galay::redis
{
    using galay::AsyncWaiter;

    // ======================== 构造函数 ========================

    /**
     * @brief 构造函数 - 从已连接的socket创建Redis会话
     * @param socket 已经建立连接的TCP socket
     * @param handle 协程调度器句柄
     */
    AsyncRedisSession::AsyncRedisSession(AsyncTcpSocket&& socket, CoSchedulerHandle handle)
        : m_socket(std::move(socket)), m_handle(handle)
    {
        // 预分配接收缓冲区，避免频繁内存分配
        m_recv_buffer.resize(BUFFER_SIZE);
    }

    /**
     * @brief 移动构造函数
     * @param other 要移动的源对象
     */
    AsyncRedisSession::AsyncRedisSession(AsyncRedisSession&& other)
        : m_is_closed(other.m_is_closed)
        , m_socket(std::move(other.m_socket))
        , m_handle(other.m_handle)
        , m_encoder(std::move(other.m_encoder))
        , m_parser(std::move(other.m_parser))
        , m_recv_buffer(std::move(other.m_recv_buffer))
        , m_buffer_pos(other.m_buffer_pos)
    {
        // 标记源对象为已关闭，防止其析构函数操作资源
        other.m_is_closed = true;
    }

    // ======================== 核心内部接口 ========================

    /**
     * @brief 发送已编码的Redis命令到服务器
     * @param encoded_cmd RESP协议编码后的命令字符串
     * @return 异步结果，成功时为void，失败时包含错误信息
     */
    AsyncResult<std::expected<void, RedisError>>
    AsyncRedisSession::sendCommand(const std::string& encoded_cmd)
    {
        auto waiter = std::make_shared<AsyncWaiter<void, RedisError>>();

        // 创建协程任务来执行异步发送
        waiter->appendTask([](auto waiter, auto& socket, const std::string& cmd) -> Coroutine<nil> {
            // co_await等待socket发送完成
            auto result = co_await socket.send(Bytes(cmd.data(), cmd.size()));
            if (!result) {
                // 发送失败，通知错误
                waiter->notify(std::unexpected(RedisError(RedisErrorType::REDIS_ERROR_TYPE_NETWORK_ERROR, "Failed to send command")));
            } else {
                // 发送成功
                waiter->notify({});
            }
            co_return nil();
        }(waiter, m_socket, encoded_cmd));

        return waiter->wait();
    }

    /**
     * @brief 从服务器接收并解析一个完整的Redis响应
     * @return 异步结果，成功时包含解析后的RedisReply，失败时包含错误信息
     *
     * 实现细节：
     * 1. 首先尝试解析缓冲区中已有的数据
     * 2. 如果数据不完整，继续从socket接收更多数据
     * 3. 解析成功后，将剩余数据前移到缓冲区开头
     * 4. 支持缓冲区自动扩容
     */
    AsyncResult<std::expected<protocol::RedisReply, RedisError>>
    AsyncRedisSession::receiveReply()
    {
        auto waiter = std::make_shared<AsyncWaiter<protocol::RedisReply, RedisError>>();

        waiter->appendTask([this](auto waiter, auto& socket, auto& parser, auto& recv_buffer, size_t& buffer_pos) -> Coroutine<nil> {
            while (true) {
                // 检查缓冲区中是否有完整的响应
                if (buffer_pos > 0) {
                    auto parse_result = parser.parse(recv_buffer.data(), buffer_pos);
                    if (parse_result.has_value()) {
                        // 解析成功，获取消耗的字节数和解析结果
                        auto [consumed, reply] = parse_result.value();
                        // 将未处理的剩余数据前移
                        if (consumed < buffer_pos) {
                            std::memmove(recv_buffer.data(),
                                       recv_buffer.data() + consumed,
                                       buffer_pos - consumed);
                        }
                        buffer_pos -= consumed;
                        // 通知等待者解析结果
                        waiter->notify(std::move(reply));
                        co_return nil();
                    }
                    // 数据不完整，继续接收
                }
                // 从socket接收更多数据到缓冲区当前位置之后
                auto bytes = co_await socket.recv(recv_buffer.data() + buffer_pos,
                                                 recv_buffer.size() - buffer_pos);
                if (!bytes) {
                    // 接收失败
                    waiter->notify(std::unexpected(RedisError(RedisErrorType::REDIS_ERROR_TYPE_NETWORK_ERROR, "Failed to receive data")));
                    co_return nil();
                }

                if (bytes.value().size() == 0) {
                    // 连接被服务器关闭
                    waiter->notify(std::unexpected(RedisError(RedisErrorType::REDIS_ERROR_TYPE_CONNECTION_CLOSED, "Connection closed by server")));
                    co_return nil();
                }

                // 更新缓冲区数据位置
                buffer_pos += bytes.value().size();

                // 如果缓冲区满了，自动扩容（翻倍）
                if (buffer_pos >= recv_buffer.size()) {
                    recv_buffer.resize(recv_buffer.size() * 2);
                }
            }
            co_return nil();
        }(waiter, m_socket, m_parser, m_recv_buffer, m_buffer_pos));

        return waiter->wait();
    }

    /**
     * @brief 执行一个完整的请求-响应周期
     * @param encoded_cmd 已编码的命令字符串
     * @return 异步结果，成功时包含RedisValue，失败时包含错误信息
     *
     * 流程：
     * 1. 发送命令到服务器
     * 2. 接收服务器响应
     * 3. 将协议层的RedisReply转换为业务层的RedisValue
     */
    AsyncResult<std::expected<RedisValue, RedisError>>
    AsyncRedisSession::executeCommand(std::string&& encoded_cmd)
    {
        auto waiter = std::make_shared<AsyncWaiter<RedisValue, RedisError>>();

        waiter->appendTask([this](auto waiter, auto* self, std::string cmd) -> Coroutine<nil> {
            // 第一步：发送命令（直接 co_await，不创建中间 waiter）
            auto send_result = co_await self->sendCommand(cmd);
            if (!send_result) {
                // 发送失败，直接返回错误
                waiter->notify(std::unexpected(send_result.error()));
                co_return nil();
            }
            // 第二步：接收响应（直接 co_await，不创建中间 waiter）
            auto recv_result = co_await self->receiveReply();
            if (!recv_result) {
                // 接收失败，返回错误
                waiter->notify(std::unexpected(recv_result.error()));
                co_return nil();
            }
            // 第三步：将协议层RedisReply转换为业务层RedisValue
            RedisValue value(recv_result.value());
            waiter->notify(std::move(value));
            co_return nil();
        }(waiter, this, std::move(encoded_cmd)));

        return waiter->wait();
    }

    // ======================== 通用命令执行接口 ========================

    /**
     * @brief 执行单个命令（无参数）
     * @param cmd 命令名称，如 "PING"
     * @return 异步结果
     */
    AsyncResult<std::expected<RedisValue, RedisError>>
    AsyncRedisSession::execute(const std::string& cmd)
    {
        std::vector<std::string> parts = {cmd};
        return execute(parts);
    }

    /**
     * @brief 执行带参数的命令
     * @param cmd 命令名称，如 "GET"
     * @param args 参数列表，如 {"mykey"}
     * @return 异步结果
     */
    AsyncResult<std::expected<RedisValue, RedisError>>
    AsyncRedisSession::execute(const std::string& cmd, const std::vector<std::string>& args)
    {
        std::vector<std::string> parts;
        parts.reserve(1 + args.size());
        parts.push_back(cmd);
        parts.insert(parts.end(), args.begin(), args.end());
        return execute(parts);
    }

    /**
     * @brief 执行命令（使用命令部分数组）
     * @param cmd_parts 命令和参数的数组，如 {"SET", "key", "value"}
     * @return 异步结果
     */
    AsyncResult<std::expected<RedisValue, RedisError>>
    AsyncRedisSession::execute(const std::vector<std::string>& cmd_parts)
    {
        // 检查会话是否已关闭
        if (m_is_closed) {
            auto waiter = std::make_shared<AsyncWaiter<RedisValue, RedisError>>();
            waiter->notify(std::unexpected(RedisError(ConnectionClosed, "Session is closed")));
            return waiter->wait();
        }

        // 使用RESP协议编码器编码命令
        auto encoded = m_encoder.encodeCommand(cmd_parts);
        return executeCommand(std::move(encoded));
    }

    // ======================== 基础Redis命令 ========================

    /**
     * @brief 使用密码认证（Redis 5.0之前版本）
     * @param password 密码
     * @return 异步结果
     */
    AsyncResult<std::expected<void, RedisError>>
    AsyncRedisSession::auth(const std::string& password)
    {
        auto waiter = std::make_shared<AsyncWaiter<void, RedisError>>();

        waiter->appendTask([this](auto waiter, auto* self, std::string pwd) -> Coroutine<nil> {
            auto result = co_await self->execute("AUTH", {pwd});
            if (!result) {
                waiter->notify(std::unexpected(result.error()));
            } else {
                waiter->notify({});
            }
            co_return nil();
        }(waiter, this, password));

        return waiter->wait();
    }

    /**
     * @brief 使用用户名和密码认证（Redis 6.0+ ACL）
     * @param username 用户名
     * @param password 密码
     * @return 异步结果
     */
    AsyncResult<std::expected<void, RedisError>>
    AsyncRedisSession::auth(const std::string& username, const std::string& password)
    {
        auto waiter = std::make_shared<AsyncWaiter<void, RedisError>>();

        waiter->appendTask([this](auto waiter, auto* self, const std::string& user, const std::string& pwd) -> Coroutine<nil> {
            auto result = co_await self->execute("AUTH", {user, pwd});
            if (!result) {
                waiter->notify(std::unexpected(result.error()));
            } else {
                waiter->notify({});
            }
            co_return nil();
        }(waiter, this, username, password));

        return waiter->wait();
    }

    /**
     * @brief 切换到指定的数据库
     * @param db_index 数据库索引（0-15，默认有16个数据库）
     * @return 异步结果
     */
    AsyncResult<std::expected<void, RedisError>>
    AsyncRedisSession::select(int32_t db_index)
    {
        auto waiter = std::make_shared<AsyncWaiter<void, RedisError>>();

        waiter->appendTask([](auto waiter, auto* self, int32_t db) -> Coroutine<nil> {
            auto result = co_await self->execute("SELECT", {std::to_string(db)});
            if (!result) {
                waiter->notify(std::unexpected(result.error()));
            } else {
                waiter->notify({});
            }
            co_return nil();
        }(waiter, this, db_index));

        return waiter->wait();
    }

    /**
     * @brief 测试连接是否正常
     * @return 异步结果，通常返回 "PONG"
     */
    AsyncResult<std::expected<RedisValue, RedisError>>
    AsyncRedisSession::ping()
    {
        return execute("PING");
    }

    /**
     * @brief 回显字符串
     * @param message 要回显的消息
     * @return 异步结果，返回相同的消息
     */
    AsyncResult<std::expected<RedisValue, RedisError>>
    AsyncRedisSession::echo(const std::string& message)
    {
        return execute("ECHO", {message});
    }

    // ======================== String操作 ========================

    /**
     * @brief 获取键的值
     * @param key 键名
     * @return 异步结果，键存在时返回值，不存在时返回null
     */
    AsyncResult<std::expected<RedisValue, RedisError>>
    AsyncRedisSession::get(const std::string& key)
    {
        return execute("GET", {key});
    }

    /**
     * @brief 设置键的值
     * @param key 键名
     * @param value 要设置的值
     * @return 异步结果，通常返回 "OK"
     */
    AsyncResult<std::expected<RedisValue, RedisError>>
    AsyncRedisSession::set(const std::string& key, const std::string& value)
    {
        return execute("SET", {key, value});
    }

    /**
     * @brief 设置键的值并指定过期时间（秒）
     * @param key 键名
     * @param seconds 过期时间（秒）
     * @param value 要设置的值
     * @return 异步结果，通常返回 "OK"
     */
    AsyncResult<std::expected<RedisValue, RedisError>>
    AsyncRedisSession::setex(const std::string& key, int64_t seconds, const std::string& value)
    {
        return execute("SETEX", {key, std::to_string(seconds), value});
    }

    /**
     * @brief 删除一个键
     * @param key 键名
     * @return 异步结果，返回删除的键数量（0或1）
     */
    AsyncResult<std::expected<RedisValue, RedisError>>
    AsyncRedisSession::del(const std::string& key)
    {
        return execute("DEL", {key});
    }

    /**
     * @brief 检查键是否存在
     * @param key 键名
     * @return 异步结果，存在返回1，不存在返回0
     */
    AsyncResult<std::expected<RedisValue, RedisError>>
    AsyncRedisSession::exists(const std::string& key)
    {
        return execute("EXISTS", {key});
    }

    /**
     * @brief 将键的整数值增1
     * @param key 键名
     * @return 异步结果，返回增加后的值
     */
    AsyncResult<std::expected<RedisValue, RedisError>>
    AsyncRedisSession::incr(const std::string& key)
    {
        return execute("INCR", {key});
    }

    /**
     * @brief 将键的整数值减1
     * @param key 键名
     * @return 异步结果，返回减少后的值
     */
    AsyncResult<std::expected<RedisValue, RedisError>>
    AsyncRedisSession::decr(const std::string& key)
    {
        return execute("DECR", {key});
    }

    // ======================== Hash操作 ========================

    /**
     * @brief 获取哈希表中指定字段的值
     * @param key 哈希表的键名
     * @param field 字段名
     * @return 异步结果，返回字段的值，不存在返回null
     */
    AsyncResult<std::expected<RedisValue, RedisError>>
    AsyncRedisSession::hget(const std::string& key, const std::string& field)
    {
        return execute("HGET", {key, field});
    }

    /**
     * @brief 设置哈希表中字段的值
     * @param key 哈希表的键名
     * @param field 字段名
     * @param value 字段值
     * @return 异步结果，新字段返回1，更新返回0
     */
    AsyncResult<std::expected<RedisValue, RedisError>>
    AsyncRedisSession::hset(const std::string& key, const std::string& field, const std::string& value)
    {
        return execute("HSET", {key, field, value});
    }

    /**
     * @brief 删除哈希表中的字段
     * @param key 哈希表的键名
     * @param field 要删除的字段名
     * @return 异步结果，返回删除的字段数量
     */
    AsyncResult<std::expected<RedisValue, RedisError>>
    AsyncRedisSession::hdel(const std::string& key, const std::string& field)
    {
        return execute("HDEL", {key, field});
    }

    /**
     * @brief 获取哈希表中的所有字段和值
     * @param key 哈希表的键名
     * @return 异步结果，返回字段名和值的数组
     */
    AsyncResult<std::expected<RedisValue, RedisError>>
    AsyncRedisSession::hgetAll(const std::string& key)
    {
        return execute("HGETALL", {key});
    }

    // ======================== List操作 ========================

    /**
     * @brief 将值插入列表头部
     * @param key 列表的键名
     * @param value 要插入的值
     * @return 异步结果，返回列表的长度
     */
    AsyncResult<std::expected<RedisValue, RedisError>>
    AsyncRedisSession::lpush(const std::string& key, const std::string& value)
    {
        return execute("LPUSH", {key, value});
    }

    /**
     * @brief 将值插入列表尾部
     * @param key 列表的键名
     * @param value 要插入的值
     * @return 异步结果，返回列表的长度
     */
    AsyncResult<std::expected<RedisValue, RedisError>>
    AsyncRedisSession::rpush(const std::string& key, const std::string& value)
    {
        return execute("RPUSH", {key, value});
    }

    /**
     * @brief 移除并返回列表头部的元素
     * @param key 列表的键名
     * @return 异步结果，返回弹出的元素，列表空时返回null
     */
    AsyncResult<std::expected<RedisValue, RedisError>>
    AsyncRedisSession::lpop(const std::string& key)
    {
        return execute("LPOP", {key});
    }

    /**
     * @brief 移除并返回列表尾部的元素
     * @param key 列表的键名
     * @return 异步结果，返回弹出的元素，列表空时返回null
     */
    AsyncResult<std::expected<RedisValue, RedisError>>
    AsyncRedisSession::rpop(const std::string& key)
    {
        return execute("RPOP", {key});
    }

    /**
     * @brief 获取列表的长度
     * @param key 列表的键名
     * @return 异步结果，返回列表长度
     */
    AsyncResult<std::expected<RedisValue, RedisError>>
    AsyncRedisSession::llen(const std::string& key)
    {
        return execute("LLEN", {key});
    }

    /**
     * @brief 获取列表指定范围的元素
     * @param key 列表的键名
     * @param start 开始索引（0表示第一个元素，-1表示最后一个元素）
     * @param stop 结束索引
     * @return 异步结果，返回指定范围的元素数组
     */
    AsyncResult<std::expected<RedisValue, RedisError>>
    AsyncRedisSession::lrange(const std::string& key, int64_t start, int64_t stop)
    {
        return execute("LRANGE", {key, std::to_string(start), std::to_string(stop)});
    }

    // ======================== Set操作 ========================

    /**
     * @brief 向集合添加成员
     * @param key 集合的键名
     * @param member 要添加的成员
     * @return 异步结果，返回添加的成员数量（0表示已存在）
     */
    AsyncResult<std::expected<RedisValue, RedisError>>
    AsyncRedisSession::sadd(const std::string& key, const std::string& member)
    {
        return execute("SADD", {key, member});
    }

    /**
     * @brief 从集合移除成员
     * @param key 集合的键名
     * @param member 要移除的成员
     * @return 异步结果，返回移除的成员数量
     */
    AsyncResult<std::expected<RedisValue, RedisError>>
    AsyncRedisSession::srem(const std::string& key, const std::string& member)
    {
        return execute("SREM", {key, member});
    }

    /**
     * @brief 获取集合的所有成员
     * @param key 集合的键名
     * @return 异步结果，返回所有成员的数组
     */
    AsyncResult<std::expected<RedisValue, RedisError>>
    AsyncRedisSession::smembers(const std::string& key)
    {
        return execute("SMEMBERS", {key});
    }

    /**
     * @brief 获取集合的成员数量
     * @param key 集合的键名
     * @return 异步结果，返回集合大小
     */
    AsyncResult<std::expected<RedisValue, RedisError>>
    AsyncRedisSession::scard(const std::string& key)
    {
        return execute("SCARD", {key});
    }

    // ======================== Sorted Set操作 ========================

    /**
     * @brief 向有序集合添加成员
     * @param key 有序集合的键名
     * @param score 成员的分数
     * @param member 要添加的成员
     * @return 异步结果，返回添加的成员数量（0表示已存在且更新了分数）
     */
    AsyncResult<std::expected<RedisValue, RedisError>>
    AsyncRedisSession::zadd(const std::string& key, double score, const std::string& member)
    {
        return execute("ZADD", {key, std::to_string(score), member});
    }

    /**
     * @brief 从有序集合移除成员
     * @param key 有序集合的键名
     * @param member 要移除的成员
     * @return 异步结果，返回移除的成员数量
     */
    AsyncResult<std::expected<RedisValue, RedisError>>
    AsyncRedisSession::zrem(const std::string& key, const std::string& member)
    {
        return execute("ZREM", {key, member});
    }

    /**
     * @brief 获取有序集合指定范围的成员（按分数排序）
     * @param key 有序集合的键名
     * @param start 开始索引
     * @param stop 结束索引
     * @return 异步结果，返回指定范围的成员数组
     */
    AsyncResult<std::expected<RedisValue, RedisError>>
    AsyncRedisSession::zrange(const std::string& key, int64_t start, int64_t stop)
    {
        return execute("ZRANGE", {key, std::to_string(start), std::to_string(stop)});
    }

    /**
     * @brief 获取有序集合中成员的分数
     * @param key 有序集合的键名
     * @param member 成员名
     * @return 异步结果，返回成员的分数，不存在返回null
     */
    AsyncResult<std::expected<RedisValue, RedisError>>
    AsyncRedisSession::zscore(const std::string& key, const std::string& member)
    {
        return execute("ZSCORE", {key, member});
    }

    // ======================== 连接管理 ========================

    /**
     * @brief 关闭连接
     * @return 异步结果
     */
    AsyncResult<std::expected<void, CommonError>>
    AsyncRedisSession::close()
    {
        auto waiter = std::make_shared<AsyncWaiter<void, CommonError>>();

        // 如果已经关闭，直接返回成功
        if (m_is_closed) {
            waiter->notify({});
            return waiter->wait();
        }

        // 创建协程任务关闭socket
        waiter->appendTask([](auto waiter, auto& socket, bool& is_closed) -> Coroutine<nil> {
            auto result = co_await socket.close();
            is_closed = true;
            if (!result) {
                waiter->notify(std::unexpected(result.error()));
            } else {
                waiter->notify({});
            }
            co_return nil();
        }(waiter, m_socket, m_is_closed));

        return waiter->wait();
    }

    /**
     * @brief 检查连接是否已关闭
     * @return true表示已关闭，false表示未关闭
     */
    bool AsyncRedisSession::isClosed() const
    {
        return m_is_closed;
    }

    /**
     * @brief 标记连接为已关闭状态（不执行实际关闭操作）
     * 用于异常情况下标记连接不可用
     */
    void AsyncRedisSession::markClosed()
    {
        m_is_closed = true;
    }

}
