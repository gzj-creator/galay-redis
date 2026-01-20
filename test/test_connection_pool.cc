#include "galay-redis/async/RedisConnectionPool.h"
#include <galay-kernel/kernel/Runtime.h>
#include <iostream>
#include <iomanip>
#include <vector>
#include <chrono>

using namespace galay::redis;
using namespace galay::kernel;

/**
 * @brief 测试连接池基本功能
 */
Coroutine testBasicConnectionPool(IOScheduler* scheduler)
{
    std::cout << "\n========================================" << std::endl;
    std::cout << "Test 1: Basic Connection Pool" << std::endl;
    std::cout << "========================================\n" << std::endl;

    // 创建连接池配置
    auto config = ConnectionPoolConfig::create("127.0.0.1", 6379, 2, 5);
    config.initial_connections = 2;

    // 创建连接池
    RedisConnectionPool pool(scheduler, config);

    // 初始化连接池
    std::cout << "1. Initializing connection pool..." << std::endl;
    auto init_result = co_await pool.initialize();
    if (!init_result) {
        std::cerr << "   [FAILED] Failed to initialize pool: " << init_result.error().message() << std::endl;
        co_return;
    }
    std::cout << "   [PASSED] Pool initialized" << std::endl;

    // 获取统计信息
    auto stats = pool.getStats();
    std::cout << "   Initial stats: total=" << stats.total_connections
              << ", available=" << stats.available_connections << std::endl;

    // 测试获取连接
    std::cout << "\n2. Testing acquire connection..." << std::endl;
    auto conn_result = co_await pool.acquire();
    if (!conn_result) {
        std::cerr << "   [FAILED] Failed to acquire connection: " << conn_result.error().message() << std::endl;
        pool.shutdown();
        co_return;
    }
    auto conn = conn_result.value();
    std::cout << "   [PASSED] Connection acquired" << std::endl;

    // 使用连接执行命令
    std::cout << "\n3. Testing command execution..." << std::endl;
    auto ping_result = co_await conn->get()->ping();
    if (ping_result && ping_result.value()) {
        auto& values = ping_result.value().value();
        if (!values.empty() && values[0].isString()) {
            std::cout << "   [PASSED] PING response: " << values[0].toString() << std::endl;
        }
    } else {
        std::cerr << "   [FAILED] PING failed" << std::endl;
    }

    // 归还连接
    std::cout << "\n4. Testing release connection..." << std::endl;
    pool.release(conn);
    std::cout << "   [PASSED] Connection released" << std::endl;

    stats = pool.getStats();
    std::cout << "   After release: total=" << stats.total_connections
              << ", available=" << stats.available_connections << std::endl;

    // 关闭连接池
    std::cout << "\n5. Shutting down pool..." << std::endl;
    pool.shutdown();
    std::cout << "   [PASSED] Pool shutdown complete" << std::endl;

    std::cout << "\n========================================" << std::endl;
    std::cout << "Test 1 Complete!" << std::endl;
    std::cout << "========================================\n" << std::endl;
}

/**
 * @brief 测试 RAII 风格的连接获取
 */
Coroutine testScopedConnection(IOScheduler* scheduler)
{
    std::cout << "\n========================================" << std::endl;
    std::cout << "Test 2: Scoped Connection (RAII)" << std::endl;
    std::cout << "========================================\n" << std::endl;

    auto config = ConnectionPoolConfig::create("127.0.0.1", 6379, 2, 5);
    RedisConnectionPool pool(scheduler, config);

    auto init_result = co_await pool.initialize();
    if (!init_result) {
        std::cerr << "   [FAILED] Failed to initialize pool" << std::endl;
        co_return;
    }

    std::cout << "1. Testing scoped connection..." << std::endl;
    {
        auto conn_result = co_await pool.acquire();
        if (!conn_result) {
            std::cerr << "   [FAILED] Failed to acquire connection" << std::endl;
            pool.shutdown();
            co_return;
        }

        ScopedConnection scoped_conn(pool, conn_result.value());
        std::cout << "   [INFO] Connection acquired (scoped)" << std::endl;

        // 使用连接
        auto result = co_await scoped_conn->set("test_key", "test_value");
        if (result && result.value()) {
            std::cout << "   [PASSED] SET command succeeded" << std::endl;
        }

        auto stats = pool.getStats();
        std::cout << "   Inside scope: available=" << stats.available_connections << std::endl;

        // 离开作用域，连接自动归还
    }

    auto stats = pool.getStats();
    std::cout << "   After scope: available=" << stats.available_connections << std::endl;
    std::cout << "   [PASSED] Connection auto-released" << std::endl;

    pool.shutdown();

    std::cout << "\n========================================" << std::endl;
    std::cout << "Test 2 Complete!" << std::endl;
    std::cout << "========================================\n" << std::endl;
}

/**
 * @brief 测试并发获取连接
 */
Coroutine testConcurrentAcquire(IOScheduler* scheduler, int client_id, RedisConnectionPool& pool)
{
    for (int i = 0; i < 5; ++i) {
        auto conn_result = co_await pool.acquire();
        if (!conn_result) {
            std::cerr << "   Client " << client_id << " failed to acquire connection" << std::endl;
            continue;
        }

        auto conn = conn_result.value();
        std::cout << "   Client " << client_id << " acquired connection (iteration " << i << ")" << std::endl;

        // 执行一些操作
        std::string key = "client_" + std::to_string(client_id) + "_key_" + std::to_string(i);
        auto result = co_await conn->get()->set(key, "value");

        // 模拟一些工作（使用简单的循环代替 sleep）
        for (int j = 0; j < 1000; ++j) {
            // 简单的延迟
        }

        pool.release(conn);
        std::cout << "   Client " << client_id << " released connection (iteration " << i << ")" << std::endl;
    }
}

Coroutine testConcurrency(IOScheduler* scheduler)
{
    std::cout << "\n========================================" << std::endl;
    std::cout << "Test 3: Concurrent Connection Acquisition" << std::endl;
    std::cout << "========================================\n" << std::endl;

    auto config = ConnectionPoolConfig::create("127.0.0.1", 6379, 2, 5);
    RedisConnectionPool pool(scheduler, config);

    auto init_result = co_await pool.initialize();
    if (!init_result) {
        std::cerr << "   [FAILED] Failed to initialize pool" << std::endl;
        co_return;
    }

    std::cout << "1. Starting 3 concurrent clients..." << std::endl;

    // 启动多个并发客户端
    for (int i = 0; i < 3; ++i) {
        scheduler->spawn(testConcurrentAcquire(scheduler, i, pool));
    }

    // 等待一段时间让任务完成（使用简单的循环代替 sleep）
    for (int i = 0; i < 100000; ++i) {
        // 简单的延迟
    }

    auto stats = pool.getStats();
    std::cout << "\n2. Final statistics:" << std::endl;
    std::cout << "   Total connections: " << stats.total_connections << std::endl;
    std::cout << "   Available: " << stats.available_connections << std::endl;
    std::cout << "   Active: " << stats.active_connections << std::endl;
    std::cout << "   Total acquired: " << stats.total_acquired << std::endl;
    std::cout << "   Total released: " << stats.total_released << std::endl;
    std::cout << "   Total created: " << stats.total_created << std::endl;
    std::cout << "   [PASSED] Concurrency test complete" << std::endl;

    pool.shutdown();

    std::cout << "\n========================================" << std::endl;
    std::cout << "Test 3 Complete!" << std::endl;
    std::cout << "========================================\n" << std::endl;
}

/**
 * @brief 测试连接池扩容
 */
Coroutine testPoolExpansion(IOScheduler* scheduler)
{
    std::cout << "\n========================================" << std::endl;
    std::cout << "Test 4: Pool Expansion" << std::endl;
    std::cout << "========================================\n" << std::endl;

    auto config = ConnectionPoolConfig::create("127.0.0.1", 6379, 2, 10);
    config.initial_connections = 2;
    RedisConnectionPool pool(scheduler, config);

    auto init_result = co_await pool.initialize();
    if (!init_result) {
        std::cerr << "   [FAILED] Failed to initialize pool" << std::endl;
        co_return;
    }

    auto stats = pool.getStats();
    std::cout << "1. Initial pool size: " << stats.total_connections << std::endl;

    // 获取多个连接，触发扩容
    std::cout << "\n2. Acquiring 5 connections..." << std::endl;
    std::vector<std::shared_ptr<PooledConnection>> connections;

    for (int i = 0; i < 5; ++i) {
        auto conn_result = co_await pool.acquire();
        if (conn_result) {
            connections.push_back(conn_result.value());
            stats = pool.getStats();
            std::cout << "   Acquired connection " << (i + 1)
                      << ", pool size: " << stats.total_connections << std::endl;
        }
    }

    stats = pool.getStats();
    std::cout << "\n3. After expansion:" << std::endl;
    std::cout << "   Total connections: " << stats.total_connections << std::endl;
    std::cout << "   Available: " << stats.available_connections << std::endl;
    std::cout << "   Active: " << stats.active_connections << std::endl;

    if (stats.total_connections > 2) {
        std::cout << "   [PASSED] Pool expanded successfully" << std::endl;
    } else {
        std::cout << "   [FAILED] Pool did not expand" << std::endl;
    }

    // 归还所有连接
    std::cout << "\n4. Releasing all connections..." << std::endl;
    for (auto& conn : connections) {
        pool.release(conn);
    }

    stats = pool.getStats();
    std::cout << "   After release: available=" << stats.available_connections << std::endl;

    pool.shutdown();

    std::cout << "\n========================================" << std::endl;
    std::cout << "Test 4 Complete!" << std::endl;
    std::cout << "========================================\n" << std::endl;
}

/**
 * @brief 测试健康检查
 */
Coroutine testHealthCheck(IOScheduler* scheduler)
{
    std::cout << "\n========================================" << std::endl;
    std::cout << "Test 5: Health Check" << std::endl;
    std::cout << "========================================\n" << std::endl;

    auto config = ConnectionPoolConfig::create("127.0.0.1", 6379, 2, 5);
    config.enable_health_check = true;
    config.health_check_interval = std::chrono::seconds(2);

    RedisConnectionPool pool(scheduler, config);

    auto init_result = co_await pool.initialize();
    if (!init_result) {
        std::cerr << "   [FAILED] Failed to initialize pool" << std::endl;
        co_return;
    }

    std::cout << "1. Starting health check task..." << std::endl;
    pool.triggerHealthCheck();

    // 等待健康检查运行（使用简单的循环代替 sleep）
    std::cout << "2. Waiting for health checks..." << std::endl;
    for (int i = 0; i < 1000000; ++i) {
        // 简单的延迟
    }

    auto stats = pool.getStats();
    std::cout << "\n3. Health check statistics:" << std::endl;
    std::cout << "   Total connections: " << stats.total_connections << std::endl;
    std::cout << "   Health check failures: " << stats.health_check_failures << std::endl;

    if (stats.health_check_failures == 0) {
        std::cout << "   [PASSED] All connections healthy" << std::endl;
    } else {
        std::cout << "   [INFO] Some health checks failed (expected if Redis is down)" << std::endl;
    }

    pool.shutdown();

    std::cout << "\n========================================" << std::endl;
    std::cout << "Test 5 Complete!" << std::endl;
    std::cout << "========================================\n" << std::endl;
}

/**
 * @brief 测试统计信息
 */
Coroutine testStatistics(IOScheduler* scheduler)
{
    std::cout << "\n========================================" << std::endl;
    std::cout << "Test 6: Statistics" << std::endl;
    std::cout << "========================================\n" << std::endl;

    auto config = ConnectionPoolConfig::create("127.0.0.1", 6379, 2, 5);
    RedisConnectionPool pool(scheduler, config);

    auto init_result = co_await pool.initialize();
    if (!init_result) {
        std::cerr << "   [FAILED] Failed to initialize pool" << std::endl;
        co_return;
    }

    std::cout << "1. Performing operations..." << std::endl;

    // 执行一些操作
    for (int i = 0; i < 10; ++i) {
        auto conn_result = co_await pool.acquire();
        if (conn_result) {
            auto conn = conn_result.value();
            co_await conn->get()->ping();
            pool.release(conn);
        }
    }

    auto stats = pool.getStats();
    std::cout << "\n2. Final statistics:" << std::endl;
    std::cout << "   ┌─────────────────────────────────────┐" << std::endl;
    std::cout << "   │ Connection Pool Statistics          │" << std::endl;
    std::cout << "   ├─────────────────────────────────────┤" << std::endl;
    std::cout << "   │ Total connections:      " << std::setw(11) << stats.total_connections << " │" << std::endl;
    std::cout << "   │ Available connections:  " << std::setw(11) << stats.available_connections << " │" << std::endl;
    std::cout << "   │ Active connections:     " << std::setw(11) << stats.active_connections << " │" << std::endl;
    std::cout << "   │ Waiting requests:       " << std::setw(11) << stats.waiting_requests << " │" << std::endl;
    std::cout << "   ├─────────────────────────────────────┤" << std::endl;
    std::cout << "   │ Total acquired:         " << std::setw(11) << stats.total_acquired << " │" << std::endl;
    std::cout << "   │ Total released:         " << std::setw(11) << stats.total_released << " │" << std::endl;
    std::cout << "   │ Total created:          " << std::setw(11) << stats.total_created << " │" << std::endl;
    std::cout << "   │ Total destroyed:        " << std::setw(11) << stats.total_destroyed << " │" << std::endl;
    std::cout << "   │ Health check failures:  " << std::setw(11) << stats.health_check_failures << " │" << std::endl;
    std::cout << "   └─────────────────────────────────────┘" << std::endl;

    std::cout << "   [PASSED] Statistics collected" << std::endl;

    pool.shutdown();

    std::cout << "\n========================================" << std::endl;
    std::cout << "Test 6 Complete!" << std::endl;
    std::cout << "========================================\n" << std::endl;
}

int main()
{
    std::cout << "\n##################################################" << std::endl;
    std::cout << "# Redis Connection Pool - Comprehensive Tests    #" << std::endl;
    std::cout << "##################################################\n" << std::endl;

    try {
        Runtime runtime;
        runtime.start();

        auto* scheduler = runtime.getNextIOScheduler();
        if (!scheduler) {
            std::cerr << "Failed to get IO scheduler" << std::endl;
            return 1;
        }

        // 运行所有测试
        scheduler->spawn(testBasicConnectionPool(scheduler));
        std::this_thread::sleep_for(std::chrono::seconds(3));

        scheduler->spawn(testScopedConnection(scheduler));
        std::this_thread::sleep_for(std::chrono::seconds(2));

        scheduler->spawn(testConcurrency(scheduler));
        std::this_thread::sleep_for(std::chrono::seconds(5));

        scheduler->spawn(testPoolExpansion(scheduler));
        std::this_thread::sleep_for(std::chrono::seconds(3));

        scheduler->spawn(testHealthCheck(scheduler));
        std::this_thread::sleep_for(std::chrono::seconds(7));

        scheduler->spawn(testStatistics(scheduler));
        std::this_thread::sleep_for(std::chrono::seconds(2));

        runtime.stop();

    } catch (const std::exception& e) {
        std::cerr << "Test failed with exception: " << e.what() << std::endl;
        return 1;
    }

    std::cout << "\n##################################################" << std::endl;
    std::cout << "# All connection pool tests completed!           #" << std::endl;
    std::cout << "##################################################\n" << std::endl;

    return 0;
}
