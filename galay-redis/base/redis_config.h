/**
 * @file redis_config.h
 * @brief Redis 同步连接配置
 * @author galay-redis
 * @version 1.0.0
 *
 * @details 定义同步 Redis 连接（RedisSession）的连接选项和配置类，
 *          支持超时、地址绑定、Unix 域套接字等多种连接方式。
 */

#ifndef GALAY_REDIS_CONFIG_H
#define GALAY_REDIS_CONFIG_H

#include <any>
#include <string>
#include <cstdint>

namespace galay::redis
{
    /**
     * @brief Redis 连接选项枚举
     */
    enum class RedisConnectionOption {
        kRedisConnectionWithNull,               ///< 默认连接方式
        kRedisConnectionWithTimeout,            ///< 设置超时时间，只适用于 RedisSession，异步设置无效
        kRedisConnectionWithBind,               ///< 绑定本地地址
        kRedisConnectionWithBindAndReuse,       ///< 绑定本地地址并设置 SO_REUSEADDR
        kRedisConnectionWithUnix,               ///< 使用 Unix 域套接字
        kRedisConnectionWithUnixAndTimeout,     ///< 使用 Unix 域套接字并设置超时时间
    };


    /**
     * @brief Redis 同步连接配置类
     * @details 管理同步 Redis 连接的参数和连接选项
     */
    class RedisConfig
    {
    public:
        /**
         * @brief 设置超时连接方式
         * @param timeout 超时时间（毫秒）
         */
        void connectWithTimeout(uint64_t timeout);

        /**
         * @brief 设置绑定本地地址连接方式
         * @param addr 本地地址
         */
        void connectWithBind(const std::string& addr);

        /**
         * @brief 设置绑定本地地址并复用端口的连接方式
         * @param addr 本地地址
         */
        void connectWithBindAndReuse(const std::string& addr);

        /**
         * @brief 设置 Unix 域套接字连接方式
         * @param path Unix 域套接字路径
         */
        void connectWithUnix(const std::string& path);

        /**
         * @brief 设置 Unix 域套接字并带超时的连接方式
         * @param path Unix 域套接字路径
         * @param timeout 超时时间（毫秒）
         */
        void connectWithUnixAndTimeout(const std::string& path, uint64_t timeout);

        /**
         * @brief 获取连接选项引用
         * @return 连接选项的引用
         */
        RedisConnectionOption& getConnectOption();

        /**
         * @brief 获取连接参数
         * @return 参数的 std::any 引用
         */
        std::any& getParams();

    private:
        std::any m_params;                                                          ///< 连接参数
        RedisConnectionOption m_connection_option = RedisConnectionOption::kRedisConnectionWithNull; ///< 连接选项
    };
}

#endif