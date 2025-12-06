#include "AsyncRedisSession.h"
#include "base/RedisError.h"
#include "base/RedisLog.h"
#include "base/RedisValue.h"
#include "galay/common/Buffer.h"
#include "galay/kernel/async/Bytes.h"
#include "galay/kernel/coroutine/CoSchedulerHandle.hpp"
#include "galay/kernel/coroutine/Waker.h"
#include <cstddef>
#include <galay/kernel/async/AsyncFactory.h>
#include <galay/common/Error.h>
#include <galay/utils/System.h>
#include <regex>

namespace galay::redis
{

    /**
     * @brief 带配置的构造函数
     * @param handle 协程调度器句柄
     * @param config 异步Redis配置对象
     */
    AsyncRedisSession::AsyncRedisSession(CoSchedulerHandle handle, AsyncRedisConfig config)
        : m_socket(), m_handle(handle), m_config(config)
    {
    }

    /*
     * @brief 移动构造函数
     * @param other 要移动的源对象
     */
    AsyncRedisSession::AsyncRedisSession(AsyncRedisSession&& other) noexcept
        : m_is_closed(other.m_is_closed)
        , m_socket(std::move(other.m_socket))
        , m_handle(other.m_handle)
        , m_encoder(std::move(other.m_encoder))
        , m_parser(std::move(other.m_parser))
    {
        // 标记源对象为已关闭，防止其析构函数操作资源
        other.m_is_closed = true;
    }

    /**
     * @brief 移动赋值运算符
     * @param other 要移动的源对象
     * @return 当前对象的引用
     */
    AsyncRedisSession& AsyncRedisSession::operator=(AsyncRedisSession&& other) noexcept
    {
        if (this != &other) {
            m_is_closed = other.m_is_closed;
            m_socket = std::move(other.m_socket);
            m_handle = other.m_handle;
            m_encoder = std::move(other.m_encoder);
            m_parser = std::move(other.m_parser);
            other.m_is_closed = true;
        }
        return *this;
    }

    // ======================== 连接方法 ========================

    /**
     * @brief 使用URL连接到Redis服务器
     * @param url Redis连接URL，格式：redis://[username:password@]host:port[/db_index]
     * @return 异步结果，成功返回void，失败返回RedisError
     */
    RedisVoidResult
    AsyncRedisSession::connect(const std::string& url)
    {
        auto waiter = std::make_shared<AsyncWaiter<void, RedisError>>();

        // 解析URL
        std::regex pattern(R"(^redis://(?:([^:@]*)(?::([^@]*))?@)?([a-zA-Z0-9\-\.]+)(?::(\d+))?(?:/(\d+))?$)");
        std::smatch matches;
        std::string username, password, host;
        int32_t port = 6379, db_index = 0;

        if (std::regex_match(url, matches, pattern)) {
            if (matches.size() > 1 && !matches[1].str().empty()) {
                username = matches[1];
            }
            if (matches.size() > 2 && !matches[2].str().empty()) {
                password = matches[2];
            }
            if (matches.size() > 3 && !matches[3].str().empty()) {
                host = matches[3];
            } else {
                RedisLogError(m_logger->getSpdlogger(), "[Redis host is invalid]");
                waiter->notify(std::unexpected(RedisError(RedisErrorType::REDIS_ERROR_TYPE_HOST_INVALID_ERROR)));
                return waiter->wait();
            }
            if (matches.size() > 4 && !matches[4].str().empty()) {
                try {
                    port = std::stoi(matches[4]);
                } catch(const std::exception& e) {
                    RedisLogError(m_logger->getSpdlogger(), "[Redis port is invalid]");
                    waiter->notify(std::unexpected(RedisError(RedisErrorType::REDIS_ERROR_TYPE_PORT_INVALID_ERROR)));
                    return waiter->wait();
                }
            }
            if (matches.size() > 5 && !matches[5].str().empty()) {
                try {
                    db_index = std::stoi(matches[5]);
                } catch(const std::exception& e) {
                    RedisLogError(m_logger->getSpdlogger(), "[Redis url is invalid]");
                    waiter->notify(std::unexpected(RedisError(RedisErrorType::REDIS_ERROR_TYPE_DB_INDEX_INVALID_ERROR)));
                    return waiter->wait();
                }
            }
        } else {
            RedisLogError(m_logger->getSpdlogger(), "[Redis url is invalid]");
            waiter->notify(std::unexpected(RedisError(RedisErrorType::REDIS_ERROR_TYPE_URL_INVALID_ERROR)));
            return waiter->wait();
        }

        // 解析主机地址
        using namespace galay::utils;
        std::string ip;
        switch (checkAddressType(host))
        {
        case AddressType::IPv4:
            ip = host;
            break;
        case AddressType::IPv6:
            waiter->notify(std::unexpected(RedisError(RedisErrorType::REDIS_ERROR_TYPE_ADDRESS_TYPE_INVALID_ERROR, "IPv6 is not supported")));
            return waiter->wait();
        case AddressType::Domain:
        {
            ip = getHostIPV4(host);
            if (ip.empty()) {
                RedisLogError(m_logger->getSpdlogger(), "[Get domain's IPV4 failed]");
                waiter->notify(std::unexpected(RedisError(RedisErrorType::REDIS_ERROR_TYPE_ADDRESS_TYPE_INVALID_ERROR)));
                return waiter->wait();
            }
            break;
        }
        default:
            RedisLogError(m_logger->getSpdlogger(), "[Unsupported address type]");
            waiter->notify(std::unexpected(RedisError(RedisErrorType::REDIS_ERROR_TYPE_ADDRESS_TYPE_INVALID_ERROR)));
            return waiter->wait();
        }

        return connect(ip, port, username, password, db_index);
    }

    /**
     * @brief 连接到Redis服务器（基础版本）
     */
    RedisVoidResult
    AsyncRedisSession::connect(const std::string& ip, int32_t port,
                                const std::string& username, const std::string& password)
    {
        return connect(ip, port, username, password, 0);
    }

    /**
     * @brief 连接到Redis服务器并选择数据库
     */
    RedisVoidResult
    AsyncRedisSession::connect(const std::string& ip, int32_t port,
                                const std::string& username, const std::string& password,
                                int32_t db_index)
    {
        return connect(ip, port, username, password, db_index, 2);
    }

    /**
     * @brief 连接到Redis服务器并指定协议版本
     */
    RedisVoidResult
    AsyncRedisSession::connect(const std::string& ip, int32_t port,
                                const std::string& username, const std::string& password,
                                int32_t db_index, int version)
    {
        auto waiter = std::make_shared<AsyncWaiter<void, RedisError>>();

        waiter->appendTask([this](auto waiter, std::string host, int32_t port,
                                  std::string username, std::string password,
                                  int32_t db_index, int version) -> Coroutine<nil> {
            // 处理localhost
            if (host == "localhost") {
                host = "127.0.0.1";
            }

            // 创建socket
            m_socket = m_handle.getAsyncFactory().getTcpSocket();

            // 初始化socket
            if (auto result = m_socket.socket(); !result) {
                auto error_msg = result.error().message();
                RedisLogError(m_logger->getSpdlogger(), "[Failed to create socket: {}]", error_msg);
                waiter->notify(std::unexpected(RedisError(RedisErrorType::REDIS_ERROR_TYPE_NETWORK_ERROR, "Failed to create socket")));
                co_return nil();
            }

            // 连接到Redis服务器
            Host redis_host(host, port);
            auto connect_result = co_await m_socket.connect(redis_host);
            if (!connect_result) {
                RedisLogError(m_logger->getSpdlogger(), "[Redis connect to {}:{} failed: {}]", host, port, connect_result.error().message());
                waiter->notify(std::unexpected(RedisError(RedisErrorType::REDIS_ERROR_TYPE_NETWORK_ERROR, "Connection failed")));
                co_return nil();
            }

            RedisLogInfo(m_logger->getSpdlogger(), "[Redis connect to {}:{}]", host, port);
            
            m_handle.spawn(receiveReply());
            m_handle.spawn(sendCommand());

            // 认证
            if (!password.empty()) {
                std::expected<std::vector<RedisValue>, RedisError> auth_result;

                if (version == 3) {
                    // RESP3: HELLO 3 AUTH username password
                    auth_result = co_await execute("HELLO", "3", "AUTH",
                                                   username.empty() ? "default" : username,
                                                   password);
                } else {
                    // RESP2: AUTH [username] password
                    if (username.empty()) {
                        auth_result = co_await execute("AUTH", password);
                    } else {
                        auth_result = co_await execute("AUTH", username, password);
                    }
                }

                if (!auth_result || auth_result->empty() || auth_result->front().isError()) {
                    std::string error_msg = auth_result ? auth_result->front().toError() : "Authentication failure";
                    RedisLogError(m_logger->getSpdlogger(), "[Authentication failure, error is {}]", error_msg);
                    co_await close();
                    waiter->notify(std::unexpected(RedisError(RedisErrorType::REDIS_ERROR_TYPE_AUTH_ERROR, error_msg)));
                    co_return nil();
                }
                RedisLogInfo(m_logger->getSpdlogger(), "[Authentication success]");
            }

            // 选择数据库
            if (db_index != 0) {
                auto select_result = co_await select(db_index);
                if (!select_result || select_result->front().isError()) {
                    std::string error_msg = select_result ? select_result->front().toError() : "Select database failed";
                    RedisLogError(m_logger->getSpdlogger(), "[Select database {} failed, error is {}]", db_index, error_msg);
                    co_await close();
                    waiter->notify(std::unexpected(RedisError(RedisErrorType::REDIS_ERROR_TYPE_DB_INDEX_INVALID_ERROR, error_msg)));
                    co_return nil();
                }
                RedisLogInfo(m_logger->getSpdlogger(), "[Select database {}]", db_index);
            }

            // 连接成功
            waiter->notify({});
            co_return nil();
        }(waiter, ip, port, username, password, db_index, version));

        return waiter->wait();
    }

    // ======================== 核心内部接口 ========================

    /**
     * @brief 发送已编码的Redis命令到服务器
     * @param encoded_cmd RESP协议编码后的命令字符串
     * @return 异步结果，成功时为void，失败时包含错误信息
     */
    Coroutine<nil>
    AsyncRedisSession::sendCommand()
    {
        while (true) {
            auto res_op = co_await m_channel.recv();
            if(res_op.has_value()) {
                auto [encoded_cmd, batch_size, waiter] = res_op.value();
                Bytes bytes = Bytes::fromString(encoded_cmd);
                do {
                    auto result = co_await m_socket.send(std::move(bytes));
                    if (!result) {
                        waiter->notify(std::unexpected(RedisError(RedisErrorType::REDIS_ERROR_TYPE_NETWORK_ERROR, result.error().message())));
                        break;
                    }
                    if(result.value().empty()) {
                        m_waiters.push_back({batch_size, waiter});
                        break;
                    }
                    bytes = std::move(result.value());
                } while(true);
            }
        }
        co_return nil();
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
     * 5. 如果配置了recv超时，会在接收时使用 TimerGenerator 包装
     */
    Coroutine<nil>
    AsyncRedisSession::receiveReply()
    {
        RingBuffer buffer(m_config.buffer_size);
        std::vector<RedisValue> results;
        
        while (true) {
            // 先接收数据
            auto [data, size] = buffer.getWriteBuffer();
            auto res = co_await m_socket.recv(data, size);
            if(!res) {
                if(m_waiters.empty()) {
                    co_return nil();
                }
                // 接收失败，通知当前批次的waiter
                // 添加错误到results，并更新batch_size
                auto [batch_size, waiter] = m_waiters.front();  // 拷贝一份
                results.push_back(RedisValue::fromError(res.error().message()));
                
                // 更新deque中的batch_size
                m_waiters.front().first--;
                if(m_waiters.front().first == 0) {
                    m_waiters.pop_front();
                    waiter->notify(std::unexpected<RedisError>(RedisError(RedisErrorType::REDIS_ERROR_TYPE_NETWORK_ERROR, res.error().message())));
                    results.clear();  // 清空results准备下一个批次
                }
                continue;
            }
            
            auto& bytes = res.value();
            size_t length = bytes.size();
            buffer.produce(length);
            
            // 处理缓冲区中的所有完整响应（处理粘包）
            while (true) {
                if (m_waiters.empty()) {
                    // 没有等待的请求，退出解析循环
                    break;
                }
                
                auto [reply_data, reply_length] = buffer.getReadBuffer();
                auto parse_result = m_parser.parse(reply_data, reply_length);
                
                if(parse_result) {
                    // 解析成功
                    auto [batch_size, waiter] = m_waiters.front();  // 拷贝一份
                    results.push_back(RedisValue(parse_result.value().second));
                    buffer.consume(parse_result.value().first);
                    
                    // 更新deque中的batch_size
                    m_waiters.front().first--;
                    if(m_waiters.front().first == 0) {
                        m_waiters.pop_front();
                        waiter->notify(std::expected<std::vector<RedisValue>, RedisError>(std::move(results)));
                        results.clear();  // 清空results准备下一个批次
                    }
                    // 继续尝试解析缓冲区中的下一个响应
                } else if(parse_result.error() == protocol::ParseError::Incomplete) {
                    // 数据不完整，需要接收更多数据
                    break;
                } else {
                    // 解析错误（格式错误等）
                    auto [batch_size, waiter] = m_waiters.front();  // 拷贝一份
                    results.push_back(RedisValue::fromError("Failed to parse response"));
                    
                    // 更新deque中的batch_size
                    m_waiters.front().first--;
                    if(m_waiters.front().first == 0) {
                        m_waiters.pop_front();
                        waiter->notify(std::expected<std::vector<RedisValue>, RedisError>(std::move(results)));
                        results.clear();  // 清空results准备下一个批次
                    }
                    break;
                }
            }
        }   
        co_return nil();
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
    RedisResult
    AsyncRedisSession::executeCommand(std::string&& encoded_cmd)
    {
        auto waiter = std::make_shared<AsyncWaiter<std::vector<RedisValue>, RedisError>>();
        m_channel.send({encoded_cmd, 1, waiter});
        return waiter->wait();
    }

    // ======================== 基础Redis命令 ========================

    /**
     * @brief 使用密码认证（Redis 5.0之前版本）
     * @param password 密码
     * @return 异步结果，成功返回 SimpleString "OK"
     */
    RedisResult
    AsyncRedisSession::auth(const std::string& password)
    {
        return execute("AUTH", password);
    }

    /**
     * @brief 使用用户名和密码认证（Redis 6.0+ ACL）
     * @param username 用户名
     * @param password 密码
     * @return 异步结果，成功返回 SimpleString "OK"
     */
    RedisResult
    AsyncRedisSession::auth(const std::string& username, const std::string& password)
    {
        return execute("AUTH", username, password);
    }

    /**
     * @brief 切换到指定的数据库
     * @param db_index 数据库索引（0-15，默认有16个数据库）
     * @return 异步结果，成功返回 SimpleString "OK"
     */
    RedisResult
    AsyncRedisSession::select(int32_t db_index)
    {
        return execute("SELECT", std::to_string(db_index));
    }

    /**
     * @brief 测试连接是否正常
     * @return 异步结果，通常返回 "PONG"
     */
    RedisResult
    AsyncRedisSession::ping(AsyncRedisConfig config)
    {
        return execute("PING");
    }

    /**
     * @brief 回显字符串
     * @param message 要回显的消息
     * @return 异步结果，返回相同的消息
     */
    RedisResult
    AsyncRedisSession::echo(const std::string& message)
    {
        return execute("ECHO", message);
    }

    // ======================== String操作 ========================

    /**
     * @brief 获取键的值
     * @param key 键名
     * @return 异步结果，键存在时返回值，不存在时返回null
     */
    RedisResult
    AsyncRedisSession::get(const std::string& key)
    {
        return execute("GET", key);
    }

    /**
     * @brief 设置键的值
     * @param key 键名
     * @param value 要设置的值
     * @return 异步结果，通常返回 "OK"
     */
    RedisResult
    AsyncRedisSession::set(const std::string& key, const std::string& value)
    {
        return execute("SET", key, value);
    }

    /**
     * @brief 设置键的值并指定过期时间（秒）
     * @param key 键名
     * @param seconds 过期时间（秒）
     * @param value 要设置的值
     * @return 异步结果，通常返回 "OK"
     */
    RedisResult
    AsyncRedisSession::setex(const std::string& key, int64_t seconds, const std::string& value)
    {
        return execute("SETEX", key, std::to_string(seconds), value);
    }

    /**
     * @brief 删除一个键
     * @param key 键名
     * @return 异步结果，返回删除的键数量（0或1）
     */
    RedisResult
    AsyncRedisSession::del(const std::string& key)
    {
        return execute("DEL", key);
    }

    /**
     * @brief 检查键是否存在
     * @param key 键名
     * @return 异步结果，存在返回1，不存在返回0
     */
    RedisResult
    AsyncRedisSession::exists(const std::string& key)
    {
        return execute("EXISTS", key);
    }

    /**
     * @brief 将键的整数值增1
     * @param key 键名
     * @return 异步结果，返回增加后的值
     */
    RedisResult
    AsyncRedisSession::incr(const std::string& key)
    {
        return execute("INCR", key);
    }

    /**
     * @brief 将键的整数值减1
     * @param key 键名
     * @return 异步结果，返回减少后的值
     */
    RedisResult
    AsyncRedisSession::decr(const std::string& key)
    {
        return execute("DECR", key);
    }

    // ======================== Hash操作 ========================

    /**
     * @brief 获取哈希表中指定字段的值
     * @param key 哈希表的键名
     * @param field 字段名
     * @return 异步结果，返回字段的值，不存在返回null
     */
    RedisResult
    AsyncRedisSession::hget(const std::string& key, const std::string& field)
    {
        return execute("HGET", key, field);
    }

    /**
     * @brief 设置哈希表中字段的值
     * @param key 哈希表的键名
     * @param field 字段名
     * @param value 字段值
     * @return 异步结果，新字段返回1，更新返回0
     */
    RedisResult
    AsyncRedisSession::hset(const std::string& key, const std::string& field, const std::string& value)
    {
        return execute("HSET", key, field, value);
    }

    /**
     * @brief 删除哈希表中的字段
     * @param key 哈希表的键名
     * @param field 要删除的字段名
     * @return 异步结果，返回删除的字段数量
     */
    RedisResult
    AsyncRedisSession::hdel(const std::string& key, const std::string& field)
    {
        return execute("HDEL", key, field);
    }

    /**
     * @brief 获取哈希表中的所有字段和值
     * @param key 哈希表的键名
     * @return 异步结果，返回字段名和值的数组
     */
    RedisResult
    AsyncRedisSession::hgetAll(const std::string& key)
    {
        return execute("HGETALL", key);
    }

    // ======================== List操作 ========================

    /**
     * @brief 将值插入列表头部
     * @param key 列表的键名
     * @param value 要插入的值
     * @return 异步结果，返回列表的长度
     */
    RedisResult
    AsyncRedisSession::lpush(const std::string& key, const std::string& value)
    {
        return execute("LPUSH", key, value);
    }

    /**
     * @brief 将值插入列表尾部
     * @param key 列表的键名
     * @param value 要插入的值
     * @return 异步结果，返回列表的长度
     */
    RedisResult
    AsyncRedisSession::rpush(const std::string& key, const std::string& value)
    {
        return execute("RPUSH", key, value);
    }

    /**
     * @brief 移除并返回列表头部的元素
     * @param key 列表的键名
     * @return 异步结果，返回弹出的元素，列表空时返回null
     */
    RedisResult
    AsyncRedisSession::lpop(const std::string& key)
    {
        return execute("LPOP", key);
    }

    /**
     * @brief 移除并返回列表尾部的元素
     * @param key 列表的键名
     * @return 异步结果，返回弹出的元素，列表空时返回null
     */
    RedisResult
    AsyncRedisSession::rpop(const std::string& key)
    {
        return execute("RPOP", key);
    }

    /**
     * @brief 获取列表的长度
     * @param key 列表的键名
     * @return 异步结果，返回列表长度
     */
    RedisResult
    AsyncRedisSession::llen(const std::string& key)
    {
        return execute("LLEN", key);
    }

    /**
     * @brief 获取列表指定范围的元素
     * @param key 列表的键名
     * @param start 开始索引（0表示第一个元素，-1表示最后一个元素）
     * @param stop 结束索引
     * @return 异步结果，返回指定范围的元素数组
     */
    RedisResult
    AsyncRedisSession::lrange(const std::string& key, int64_t start, int64_t stop)
    {
        return execute("LRANGE", key, std::to_string(start), std::to_string(stop));
    }

    // ======================== Set操作 ========================

    /**
     * @brief 向集合添加成员
     * @param key 集合的键名
     * @param member 要添加的成员
     * @return 异步结果，返回添加的成员数量（0表示已存在）
     */
    RedisResult
    AsyncRedisSession::sadd(const std::string& key, const std::string& member)
    {
        return execute("SADD", key, member);
    }

    /**
     * @brief 从集合移除成员
     * @param key 集合的键名
     * @param member 要移除的成员
     * @return 异步结果，返回移除的成员数量
     */
    RedisResult
    AsyncRedisSession::srem(const std::string& key, const std::string& member)
    {
        return execute("SREM", key, member);
    }

    /**
     * @brief 获取集合的所有成员
     * @param key 集合的键名
     * @return 异步结果，返回所有成员的数组
     */
    RedisResult
    AsyncRedisSession::smembers(const std::string& key)
    {
        return execute("SMEMBERS", key);
    }

    /**
     * @brief 获取集合的成员数量
     * @param key 集合的键名
     * @return 异步结果，返回集合大小
     */
    RedisResult
    AsyncRedisSession::scard(const std::string& key)
    {
        return execute("SCARD", key);
    }

    // ======================== Sorted Set操作 ========================

    /**
     * @brief 向有序集合添加成员
     * @param key 有序集合的键名
     * @param score 成员的分数
     * @param member 要添加的成员
     * @return 异步结果，返回添加的成员数量（0表示已存在且更新了分数）
     */
    RedisResult
    AsyncRedisSession::zadd(const std::string& key, double score, const std::string& member)
    {
        return execute("ZADD", key, std::to_string(score), member);
    }

    /**
     * @brief 从有序集合移除成员
     * @param key 有序集合的键名
     * @param member 要移除的成员
     * @return 异步结果，返回移除的成员数量
     */
    RedisResult
    AsyncRedisSession::zrem(const std::string& key, const std::string& member)
    {
        return execute("ZREM", key, member);
    }

    /**
     * @brief 获取有序集合指定范围的成员（按分数排序）
     * @param key 有序集合的键名
     * @param start 开始索引
     * @param stop 结束索引
     * @return 异步结果，返回指定范围的成员数组
     */
    RedisResult
    AsyncRedisSession::zrange(const std::string& key, int64_t start, int64_t stop)
    {
        return execute("ZRANGE", key, std::to_string(start), std::to_string(stop));
    }

    /**
     * @brief 获取有序集合中成员的分数
     * @param key 有序集合的键名
     * @param member 成员名
     * @return 异步结果，返回成员的分数，不存在返回null
     */
    RedisResult
    AsyncRedisSession::zscore(const std::string& key, const std::string& member)
    {
        return execute("ZSCORE", key, member);
    }


    // ======================== Pipeline批量操作 ========================

    RedisResult
    AsyncRedisSession::pipeline(const std::vector<std::vector<std::string>>& commands)
    {
        // 检查会话是否已关闭
        if (m_is_closed) {
            return {std::unexpected<RedisError>(RedisError(RedisErrorType::REDIS_ERROR_TYPE_CONNECTION_CLOSED, "Session is closed"))};
        }

        // 检查命令列表是否为空
        if (commands.empty()) {
            return {std::expected<std::vector<RedisValue>, RedisError>(std::vector<RedisValue>())};
        }

        // 编码所有命令为一个大的字符串
        std::string batch_encoded;
        for (const auto& cmd_parts : commands) {
            if (cmd_parts.empty()) {
                auto waiter = std::make_shared<AsyncWaiter<std::vector<RedisValue>, RedisError>>();
                waiter->notify(std::unexpected(RedisError(RedisErrorType::REDIS_ERROR_TYPE_COMMAND_ERROR, "Empty command in pipeline")));
                return waiter->wait();
            }
            batch_encoded += m_encoder.encodeCommand(cmd_parts);
        }

        // 发送批量命令
        auto waiter = std::make_shared<AsyncWaiter<std::vector<RedisValue>, RedisError>>();
        m_channel.send({std::move(batch_encoded), static_cast<int32_t>(commands.size()), waiter});
        return waiter->wait();
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
