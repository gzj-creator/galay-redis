/**
 * @file buf_provider.h
 * @brief Redis 缓冲区提供者抽象接口及环形缓冲区实现
 * @author galay-redis
 * @version 1.0.0
 *
 * @details 定义了 Redis 读写缓冲区的抽象接口 RedisBufferProvider，
 *          以及基于环形缓冲区的具体实现 RedisRingBufferProvider。
 *          该抽象层用于解耦 Redis 客户端与具体的缓冲区管理策略。
 */

#ifndef GALAY_REDIS_BUFFER_PROVIDER_H
#define GALAY_REDIS_BUFFER_PROVIDER_H

#include <galay-kernel/common/buffer.h>

#include <cstddef>
#include <sys/uio.h>

namespace galay::redis
{
    /**
     * @brief Redis 缓冲区提供者抽象基类
     * @details 定义了 Redis 客户端进行异步 I/O 所需的缓冲区管理接口，
     *          支持分散读（scatter read）和聚集写（gather write）操作。
     */
    class RedisBufferProvider
    {
    public:
        virtual ~RedisBufferProvider() = default;

        /**
         * @brief 获取写入区域的 iovec 数组，用于聚集写操作
         * @param[out] out 输出的 iovec 数组
         * @param max_iovecs 最大 iovec 数量，默认为 2
         * @return 实际填充的 iovec 数量
         */
        virtual size_t getWriteIovecs(struct iovec* out, size_t max_iovecs = 2) = 0;

        /**
         * @brief 获取读取区域的 iovec 数组，用于分散读操作
         * @param[out] out 输出的 iovec 数组
         * @param max_iovecs 最大 iovec 数量，默认为 2
         * @return 实际填充的 iovec 数量
         */
        virtual size_t getReadIovecs(struct iovec* out, size_t max_iovecs = 2) const = 0;

        /**
         * @brief 生产数据，将写入指针向前移动指定长度
         * @param len 已写入的数据长度
         */
        virtual void produce(size_t len) = 0;

        /**
         * @brief 消费数据，将读取指针向前移动指定长度
         * @param len 已读取的数据长度
         */
        virtual void consume(size_t len) = 0;

        /**
         * @brief 清空缓冲区，重置读写指针
         */
        virtual void clear() = 0;
    };

    /**
     * @brief 基于环形缓冲区的 Redis 缓冲区提供者
     * @details 使用 galay::kernel::RingBuffer 实现的缓冲区提供者，
     *          支持高效的内存复用，适用于高频率的 Redis 通信场景。
     */
    class RedisRingBufferProvider final : public RedisBufferProvider
    {
    public:
        /**
         * @brief 构造指定容量的环形缓冲区提供者
         * @param capacity 环形缓冲区的容量（字节）
         */
        explicit RedisRingBufferProvider(size_t capacity);

        size_t getWriteIovecs(struct iovec* out, size_t max_iovecs = 2) override;
        size_t getReadIovecs(struct iovec* out, size_t max_iovecs = 2) const override;
        void produce(size_t len) override;
        void consume(size_t len) override;
        void clear() override;

    private:
        galay::kernel::RingBuffer m_buffer; ///< 底层环形缓冲区
    };
} // namespace galay::redis

#endif // GALAY_REDIS_BUFFER_PROVIDER_H
