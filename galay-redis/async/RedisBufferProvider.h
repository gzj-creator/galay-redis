#ifndef GALAY_REDIS_BUFFER_PROVIDER_H
#define GALAY_REDIS_BUFFER_PROVIDER_H

#include <galay-kernel/common/Buffer.h>

#include <cstddef>
#include <sys/uio.h>

namespace galay::redis
{
    class RedisBufferProvider
    {
    public:
        virtual ~RedisBufferProvider() = default;

        virtual size_t getWriteIovecs(struct iovec* out, size_t max_iovecs = 2) = 0;
        virtual size_t getReadIovecs(struct iovec* out, size_t max_iovecs = 2) const = 0;
        virtual void produce(size_t len) = 0;
        virtual void consume(size_t len) = 0;
        virtual void clear() = 0;
    };

    class RedisRingBufferProvider final : public RedisBufferProvider
    {
    public:
        explicit RedisRingBufferProvider(size_t capacity);

        size_t getWriteIovecs(struct iovec* out, size_t max_iovecs = 2) override;
        size_t getReadIovecs(struct iovec* out, size_t max_iovecs = 2) const override;
        void produce(size_t len) override;
        void consume(size_t len) override;
        void clear() override;

    private:
        galay::kernel::RingBuffer m_buffer;
    };
} // namespace galay::redis

#endif // GALAY_REDIS_BUFFER_PROVIDER_H
