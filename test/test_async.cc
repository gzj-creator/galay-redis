#include <iostream>
#include <galay-kernel/kernel/Runtime.h>
#include "async/AsyncRedisSession.h"

using namespace galay::kernel;
using namespace galay::redis;

// 异步测试协程
Coroutine testAsyncRedis(IOScheduler* scheduler) {
    std::cout << "Testing asynchronous Redis operations..." << std::endl;

    // 创建异步Redis会话
    AsyncRedisSession session(scheduler);

    // 连接到Redis服务器 - 新API：直接co_await
    std::cout << "Connecting to Redis server..." << std::endl;
    auto connect_result = co_await session.connect("redis://:galay123@140.143.142.251:6379");

    if (!connect_result) {
        std::cerr << "Connect failed: " << connect_result.error().message() << std::endl;
        co_return;
    }
    std::cout << "Connected successfully!" << std::endl;

    // Test SET - 新API：直接co_await
    std::cout << "Testing SET operation..." << std::endl;
    auto set_result = co_await session.set("test_key", "test_value");

    if (!set_result) {
        std::cerr << "SET failed: " << set_result.error().message() << std::endl;
        co_return;
    }
    std::cout << "SET operation successful" << std::endl;

    // Test GET - 新API：直接co_await
    std::cout << "Testing GET operation..." << std::endl;
    auto get_result = co_await session.get("test_key");

    if (!get_result) {
        std::cerr << "GET failed: " << get_result.error().message() << std::endl;
        co_return;
    }

    if (get_result.value().has_value() && !get_result.value().value().empty()) {
        std::cout << "GET result: " << get_result.value().value().front().toString() << std::endl;
    }

    // Test DEL - 新API：直接co_await
    std::cout << "Testing DEL operation..." << std::endl;
    auto del_result = co_await session.del("test_key");

    if (!del_result) {
        std::cerr << "DEL failed: " << del_result.error().message() << std::endl;
        co_return;
    }

    if (del_result.value().has_value() && !del_result.value().value().empty()) {
        std::cout << "DEL operation successful, deleted " << del_result.value().value().front().toInteger() << " keys" << std::endl;
    }

    // Test disconnect - 新API：直接co_await
    std::cout << "Closing connection..." << std::endl;
    auto close_result = co_await session.close();

    if (!close_result) {
        std::cerr << "Close failed: " << close_result.error().message() << std::endl;
        co_return;
    }
    std::cout << "Connection closed successfully" << std::endl;

    co_return;
}

int main() {
    std::cout << "Starting Async Redis client tests..." << std::endl;

    try {
        // 创建运行时
        Runtime runtime;
        runtime.start();

        auto* scheduler = runtime.getNextIOScheduler();
        if (!scheduler) {
            std::cerr << "Failed to get IO scheduler" << std::endl;
            return 1;
        }

        // 启动测试协程
        scheduler->spawn(testAsyncRedis(scheduler));

        // 等待一段时间让测试完成
        std::this_thread::sleep_for(std::chrono::seconds(10));

        runtime.stop();

    } catch (const std::exception& e) {
        std::cerr << "Test failed with exception: " << e.what() << std::endl;
        return 1;
    }

    std::cout << "All async tests completed." << std::endl;
    return 0;
}
