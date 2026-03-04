#include <array>
#include <chrono>

#include "async/RedisClient.h"
#include "protocol/Builder.h"

using namespace galay::kernel;
using namespace galay::redis;

namespace
{
    class DummyBufferProvider final : public RedisBufferProvider
    {
    public:
        DummyBufferProvider()
            : m_impl(4096)
        {
        }

        size_t getWriteIovecs(struct iovec* out, size_t max_iovecs) override
        {
            return m_impl.getWriteIovecs(out, max_iovecs);
        }

        size_t getReadIovecs(struct iovec* out, size_t max_iovecs) const override
        {
            return m_impl.getReadIovecs(out, max_iovecs);
        }

        void produce(size_t len) override
        {
            m_impl.produce(len);
        }

        void consume(size_t len) override
        {
            m_impl.consume(len);
        }

        void clear() override
        {
            m_impl.clear();
        }

    private:
        RingBuffer m_impl;
    };

    class DummyScheduler final : public IOScheduler
    {
    public:
        void start() override {}
        void stop() override {}

        bool spawn(Coroutine) override { return true; }
        bool spawnImmidiately(Coroutine) override { return true; }

        int addAccept(IOController*) override { return -1; }
        int addConnect(IOController*) override { return -1; }
        int addRecv(IOController*) override { return -1; }
        int addSend(IOController*) override { return -1; }
        int addReadv(IOController*) override { return -1; }
        int addWritev(IOController*) override { return -1; }
        int addClose(IOController*) override { return -1; }
        int addFileRead(IOController*) override { return -1; }
        int addFileWrite(IOController*) override { return -1; }
        int addRecvFrom(IOController*) override { return -1; }
        int addSendTo(IOController*) override { return -1; }
        int addFileWatch(IOController*) override { return -1; }
        int addSendFile(IOController*) override { return -1; }
        int addCustom(IOController*) override { return -1; }
        int remove(IOController*) override { return -1; }
    };
}

int main()
{
    DummyScheduler scheduler;
    auto client = RedisClientBuilder()
                      .scheduler(&scheduler)
                      .bufferProvider(std::make_shared<DummyBufferProvider>())
                      .build();

    RedisCommandBuilder builder;
    builder.append("SET", std::array<std::string_view, 2>{"raw:key", "v1"});
    builder.append("GET", std::array<std::string_view, 1>{"raw:key"});

    auto result = client.command(builder.release()).timeout(std::chrono::milliseconds(200));
    (void)result;
    return 0;
}
