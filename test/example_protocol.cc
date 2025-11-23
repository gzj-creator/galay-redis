#include <iostream>
#include "sync/RedisSession.h"
#include "base/RedisConfig.h"

using namespace galay::redis;

int main() {
    std::cout << "Redis Protocol Client Example" << std::endl;
    std::cout << "=============================" << std::endl << std::endl;

    // 创建Redis会话
    RedisConfig config;
    RedisSession session(config);

    // 连接到Redis服务器
    std::cout << "Connecting to Redis server..." << std::endl;
    auto connect_result = session.connect("127.0.0.1", 6379, "", "");
    if (!connect_result) {
        std::cerr << "Failed to connect: " << connect_result.error().message() << std::endl;
        std::cerr << "Please make sure Redis server is running on 127.0.0.1:6379" << std::endl;
        return 1;
    }
    std::cout << "✓ Connected successfully!" << std::endl << std::endl;

    // 测试SET命令
    std::cout << "1. Testing SET command..." << std::endl;
    auto set_result = session.set("example_key", "Hello, Redis!");
    if (set_result) {
        if (set_result->isStatus()) {
            std::cout << "   SET response: " << set_result->toStatus() << std::endl;
        } else {
            std::cout << "   SET successful" << std::endl;
        }
    } else {
        std::cerr << "   SET failed: " << set_result.error().message() << std::endl;
    }
    std::cout << std::endl;

    // 测试GET命令
    std::cout << "2. Testing GET command..." << std::endl;
    auto get_result = session.get("example_key");
    if (get_result && get_result->isString()) {
        std::cout << "   GET response: " << get_result->toString() << std::endl;
    } else if (get_result && get_result->isNull()) {
        std::cout << "   Key not found (null)" << std::endl;
    } else {
        std::cerr << "   GET failed" << std::endl;
    }
    std::cout << std::endl;

    // 测试EXISTS命令
    std::cout << "3. Testing EXISTS command..." << std::endl;
    auto exists_result = session.exist("example_key");
    if (exists_result && exists_result->isInteger()) {
        std::cout << "   EXISTS response: " << exists_result->toInteger()
                  << " (1=exists, 0=not exists)" << std::endl;
    } else {
        std::cerr << "   EXISTS failed" << std::endl;
    }
    std::cout << std::endl;

    // 测试INCR命令
    std::cout << "4. Testing INCR command..." << std::endl;
    auto incr_result = session.incr("counter");
    if (incr_result && incr_result->isInteger()) {
        std::cout << "   Counter value: " << incr_result->toInteger() << std::endl;
    } else {
        std::cerr << "   INCR failed" << std::endl;
    }
    std::cout << std::endl;

    // 测试Hash操作
    std::cout << "5. Testing Hash operations..." << std::endl;
    session.hset("user:1000", "name", "Alice");
    session.hset("user:1000", "age", "25");

    auto hget_result = session.hget("user:1000", "name");
    if (hget_result && hget_result->isString()) {
        std::cout << "   HGET name: " << hget_result->toString() << std::endl;
    }

    auto hgetall_result = session.hgetAll("user:1000");
    if (hgetall_result && hgetall_result->isArray()) {
        std::cout << "   HGETALL: {";
        auto arr = hgetall_result->toArray();
        for (size_t i = 0; i < arr.size(); i += 2) {
            if (i > 0) std::cout << ", ";
            if (arr[i].isString() && i + 1 < arr.size() && arr[i + 1].isString()) {
                std::cout << arr[i].toString() << ": " << arr[i + 1].toString();
            }
        }
        std::cout << "}" << std::endl;
    }
    std::cout << std::endl;

    // 测试List操作
    std::cout << "6. Testing List operations..." << std::endl;
    session.del("mylist");  // 清除可能存在的旧数据

    auto lrange_result = session.lrange("mylist", 0, -1);
    if (lrange_result && lrange_result->isArray()) {
        std::cout << "   LRANGE: [";
        auto arr = lrange_result->toArray();
        for (size_t i = 0; i < arr.size(); ++i) {
            if (i > 0) std::cout << ", ";
            if (arr[i].isString()) {
                std::cout << arr[i].toString();
            }
        }
        std::cout << "]" << std::endl;
    }
    std::cout << std::endl;

    // 清理测试数据
    std::cout << "7. Cleaning up test data..." << std::endl;
    session.del("example_key");
    session.del("counter");
    session.del("mylist");
    session.del("user:1000");
    std::cout << "   ✓ Test data cleaned up" << std::endl;
    std::cout << std::endl;

    // 断开连接
    std::cout << "Disconnecting from Redis server..." << std::endl;
    session.disconnect();
    std::cout << "✓ Disconnected successfully!" << std::endl;
    std::cout << std::endl;

    std::cout << "Example completed successfully!" << std::endl;
    return 0;
}
