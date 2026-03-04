#include "RedisBufferProvider.h"

namespace galay::redis
{
    RedisRingBufferProvider::RedisRingBufferProvider(size_t capacity)
        : m_buffer(capacity)
    {
    }

    size_t RedisRingBufferProvider::getWriteIovecs(struct iovec* out, size_t max_iovecs)
    {
        return m_buffer.getWriteIovecs(out, max_iovecs);
    }

    size_t RedisRingBufferProvider::getReadIovecs(struct iovec* out, size_t max_iovecs) const
    {
        return m_buffer.getReadIovecs(out, max_iovecs);
    }

    void RedisRingBufferProvider::produce(size_t len)
    {
        m_buffer.produce(len);
    }

    void RedisRingBufferProvider::consume(size_t len)
    {
        m_buffer.consume(len);
    }

    void RedisRingBufferProvider::clear()
    {
        m_buffer.clear();
    }
} // namespace galay::redis
