#ifndef GALAY_REDIS_ASYNC_CONFIG_H
#define GALAY_REDIS_ASYNC_CONFIG_H

#include <chrono>

namespace galay::redis
{
    /**
     * @brief 异步Redis超时配置结构体
     * 用于在每个异步接口调用时指定 send/recv 的超时参数
     */
    struct AsyncRedisConfig
    {
        /**
         * @brief 发送超时时间，-1表示禁用超时
         * 推荐范围：1000-5000ms
         */
        std::chrono::milliseconds send_timeout = std::chrono::milliseconds(-1);

        /**
         * @brief 接收超时时间，-1表示禁用超时
         * 推荐范围：3000-10000ms
         */
        std::chrono::milliseconds recv_timeout = std::chrono::milliseconds(-1);

        /**
         * @brief 读缓冲区大小
         * 推荐范围：8192-65536
         */
        size_t buffer_size = 8192;

        /**
         * @brief 检查发送超时是否启用
         * @return true表示启用超时（timeout >= 0ms）
         */
        bool isSendTimeoutEnabled() const
        {
            return send_timeout >= std::chrono::milliseconds(0);
        }

        /**
         * @brief 检查接收超时是否启用
         * @return true表示启用超时（timeout >= 0ms）
         */
        bool isRecvTimeoutEnabled() const
        {
            return recv_timeout >= std::chrono::milliseconds(0);
        }

        /**
         * @brief 创建一个启用全部超时的配置
         * @param send 发送超时时间
         * @param recv 接收超时时间
         * @return 配置对象
         */
        static AsyncRedisConfig withTimeout(std::chrono::milliseconds send,
                                            std::chrono::milliseconds recv)
        {
            return {send, recv};
        }

        /**
         * @brief 创建一个仅启用接收超时的配置
         * @param recv 接收超时时间
         * @return 配置对象
         */
        static AsyncRedisConfig withRecvTimeout(std::chrono::milliseconds recv)
        {
            return {std::chrono::milliseconds(-1), recv};
        }

        /**
         * @brief 创建一个仅启用发送超时的配置
         * @param send 发送超时时间
         * @return 配置对象
         */
        static AsyncRedisConfig withSendTimeout(std::chrono::milliseconds send)
        {
            return {send, std::chrono::milliseconds(-1)};
        }

        /**
         * @brief 创建一个禁用所有超时的配置
         * @return 配置对象
         */
        static AsyncRedisConfig noTimeout()
        {
            return {};
        }
    };
}

#endif // GALAY_REDIS_ASYNC_CONFIG_H
