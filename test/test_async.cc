#include <iostream>
#include <galay/kernel/async/AsyncFactory.h>
#include <galay/kernel/runtime/Runtime.h>
#include <galay/kernel/coroutine/CoSchedulerHandle.hpp>
#include <galay/kernel/coroutine/AsyncWaiter.hpp>
#include "async/AsyncRedisSession.h"

using namespace galay;
using namespace galay::redis;

// 异步测试协程
Coroutine<nil> testAsyncRedis(CoSchedulerHandle handle) {
    std::cout << "Testing asynchronous Redis operations..." << std::endl;

    std::cout << "Connected successfully" << std::endl;

    // 创建异步Redis会话
    AsyncRedisSession session(handle);


    // 认证
    std::cout << "Connecting to Redis server..." << std::endl;
    auto connect_result = co_await session.connect("redis://:galay123@140.143.142.251:6379");
    if (!connect_result) {
        std::cout << "Failed to connect: " << connect_result.error().message() << std::endl;
        co_return nil();
    }
    
    std::cout << "Authenticated successfully" << std::endl;

    // Test SET
    std::cout << "Testing SET operation..." << std::endl;
    auto set_result = co_await session.set("test_key", "test_value");
    if (!set_result || set_result->empty()) {
        std::cout << "Failed to SET: " << (set_result ? "empty result" : set_result.error().message()) << std::endl;
        co_return nil();
    }
    std::cout << "SET operation successful" << std::endl;

    // Test GET
    std::cout << "Testing GET operation..." << std::endl;
    auto get_result = co_await session.get("test_key");
    if (!get_result || get_result->empty()) {
        std::cout << "Failed to GET: " << (get_result ? "empty result" : get_result.error().message()) << std::endl;
        co_return nil();
    }
    std::cout << "GET result: " << get_result->front().toString() << std::endl;

    // Test DEL
    std::cout << "Testing DEL operation..." << std::endl;
    auto del_result = co_await session.del("test_key");
    if (!del_result || del_result->empty()) {
        std::cout << "Failed to DEL: " << (del_result ? "empty result" : del_result.error().message()) << std::endl;
        co_return nil();
    }
    std::cout << "DEL operation successful, deleted " << del_result->front().toInteger() << " keys" << std::endl;

    // Test disconnect
    std::cout << "Closing connection..." << std::endl;
    auto close_result = co_await session.close();
    if (!close_result) {
        std::cout << "Failed to close: " << close_result.error().message() << std::endl;
        co_return nil();
    }
    std::cout << "Connection closed successfully" << std::endl;

    co_return nil();
}

int main() {
    std::cout << "Starting Async Redis client tests..." << std::endl;

    try {
        // 创建异步运行时和调度器
        RuntimeBuilder builder;
        Runtime runtime = builder.build(); // 使用默认构造函数
        runtime.start(); // 启动运行时

        auto handle = runtime.getCoSchedulerHandle();

       
        handle.spawn(testAsyncRedis(handle));
        getchar();
        runtime.stop(); // 停止运行时

    } catch (const std::exception& e) {
        std::cerr << "Test failed with exception: " << e.what() << std::endl;
        return 1;
    }

    std::cout << "All async tests completed." << std::endl;
    return 0;
}