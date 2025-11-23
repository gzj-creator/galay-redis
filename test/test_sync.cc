#include <iostream>
#include "sync/RedisSession.h"

using namespace galay::redis;
using namespace galay;

void testSyncRedis() {
    std::cout << "Testing synchronous Redis operations..." << std::endl;

    RedisConfig config;
    RedisSession session(config);

    // Test connection
    auto connect_result = session.connect("redis://:galay123@140.143.142.251:6379");
    if (!connect_result) {
        std::cout << "Failed to connect: " << connect_result.error().message() << std::endl;
        return;
    }
    std::cout << "Connected successfully" << std::endl;

    // Test SET
    auto set_result = session.set("test_key", "test_value");
    if (!set_result) {
        std::cout << "Failed to SET: " << set_result.error().message() << std::endl;
        return;
    }
    std::cout << "SET operation successful" << std::endl;

    // Test GET
    auto get_result = session.get("test_key");
    if (!get_result) {
        std::cout << "Failed to GET: " << get_result.error().message() << std::endl;
        return;
    }
    std::cout << "GET result: " << get_result.value().toString() << std::endl;

    // Test DEL
    auto del_result = session.del("test_key");
    if (!del_result) {
        std::cout << "Failed to DEL: " << del_result.error().message() << std::endl;
        return;
    }
    std::cout << "DEL operation successful, deleted " << del_result.value().toInteger() << " keys" << std::endl;

    // Test disconnect
    auto disconnect_result = session.disconnect();
    if (!disconnect_result) {
        std::cout << "Failed to disconnect: " << disconnect_result.error().message() << std::endl;
        return;
    }
    std::cout << "Disconnected successfully" << std::endl;
}


int main() {
    std::cout << "Starting Redis client tests..." << std::endl;

    try {
        testSyncRedis();
    } catch (const std::exception& e) {
        std::cerr << "Test failed with exception: " << e.what() << std::endl;
        return 1;
    }

    std::cout << "All tests completed." << std::endl;
    return 0;
}
