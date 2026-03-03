#include <iostream>
#include <string>
#include <vector>

#include "async/RedisTopologyClient.h"

using namespace galay::kernel;
using namespace galay::redis;

namespace
{
    class CountingHoldScheduler final : public IOScheduler
    {
    public:
        void start() override {}
        void stop() override {}

        bool spawn(Coroutine co) override
        {
            co.belongScheduler(this);
            ++m_spawn_count;
            m_pending.push_back(std::move(co));
            return true;
        }

        bool spawnImmidiately(Coroutine co) override
        {
            return spawn(std::move(co));
        }

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

        size_t spawnCount() const noexcept { return m_spawn_count; }

        void drain()
        {
            while (!m_pending.empty()) {
                auto co = std::move(m_pending.back());
                m_pending.pop_back();
                resume(co);
            }
        }

    private:
        size_t m_spawn_count = 0;
        std::vector<Coroutine> m_pending;
    };

    int g_failures = 0;

    void expectEqual(const std::string& name, size_t actual, size_t expected)
    {
        if (actual != expected) {
            ++g_failures;
            std::cerr << "[FAILED] " << name << ": expected=" << expected << ", actual=" << actual << std::endl;
        }
    }
}

int main()
{
    std::cout << "Running topology single-flight tests..." << std::endl;

    {
        CountingHoldScheduler scheduler;
        auto ms = RedisMasterSlaveClientBuilder().scheduler(&scheduler).build();

        (void)ms.refreshFromSentinel();
        (void)ms.refreshFromSentinel();
        (void)ms.refreshFromSentinel();
        expectEqual("refreshFromSentinel burst spawn count", scheduler.spawnCount(), 1);

        scheduler.drain();

        (void)ms.refreshFromSentinel();
        expectEqual("refreshFromSentinel next wave spawn count", scheduler.spawnCount(), 2);
        scheduler.drain();
    }

    {
        CountingHoldScheduler scheduler;
        auto cluster = RedisClusterClientBuilder().scheduler(&scheduler).build();

        (void)cluster.refreshSlots();
        (void)cluster.refreshSlots();
        (void)cluster.refreshSlots();
        expectEqual("refreshSlots burst spawn count", scheduler.spawnCount(), 1);

        scheduler.drain();

        (void)cluster.refreshSlots();
        expectEqual("refreshSlots next wave spawn count", scheduler.spawnCount(), 2);
        scheduler.drain();
    }

    if (g_failures != 0) {
        std::cerr << "[FAILED] topology single-flight tests failed, count=" << g_failures << std::endl;
        return 1;
    }

    std::cout << "[PASSED] topology single-flight tests passed" << std::endl;
    return 0;
}

