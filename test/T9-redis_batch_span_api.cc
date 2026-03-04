#include <array>
#include <chrono>
#include <span>

#include "async/RedisClient.h"

using namespace galay::kernel;
using namespace galay::redis;

namespace
{
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
    auto client = RedisClientBuilder().scheduler(&scheduler).build();

    RedisCommandBuilder builder;
    builder.reserve(2, 3, 64);
    builder.append("SET", std::array<std::string_view, 2>{"batch:span:key", "v1"});
    builder.append("GET", std::array<std::string_view, 1>{"batch:span:key"});
    const auto first_commands = builder.commands();
    if (first_commands.size() != 2) {
        return 2;
    }
    if (first_commands[0].encoded.empty() || first_commands[1].encoded.empty()) {
        return 3;
    }

    auto pipeline_awaitable = client.batch(first_commands);
    auto batch_awaitable =
        client.batch(builder.commands()).timeout(std::chrono::milliseconds(200));

    builder.clear();
    builder.append("SET", std::array<std::string_view, 2>{"batch:span:key:2", "v2"});
    builder.append("GET", std::array<std::string_view, 1>{"batch:span:key:2"});
    auto reuse_awaitable = client.batch(builder.commands());

    (void)pipeline_awaitable;
    (void)batch_awaitable;
    (void)reuse_awaitable;
    return 0;
}
