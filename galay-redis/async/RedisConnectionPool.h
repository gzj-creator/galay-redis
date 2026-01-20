#ifndef GALAY_REDIS_CONNECTION_POOL_H
#define GALAY_REDIS_CONNECTION_POOL_H

#include "RedisClient.h"
#include <galay-kernel/kernel/IOScheduler.hpp>
#include <memory>
#include <queue>
#include <vector>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <chrono>
#include <coroutine>

namespace galay::redis
{
    using galay::kernel::IOScheduler;

    /**
     * @brief 连接池配置
     */
    struct ConnectionPoolConfig
    {
        // 连接参数
        std::string host = "127.0.0.1";
        int32_t port = 6379;
        std::string username = "";
        std::string password = "";
        int32_t db_index = 0;

        // 连接池大小
        size_t min_connections = 2;      // 最小连接数
        size_t max_connections = 10;     // 最大连接数
        size_t initial_connections = 2;  // 初始连接数

        // 超时配置
        std::chrono::milliseconds acquire_timeout = std::chrono::seconds(5);  // 获取连接超时
        std::chrono::milliseconds idle_timeout = std::chrono::minutes(5);     // 空闲连接超时
        std::chrono::milliseconds connect_timeout = std::chrono::seconds(3);  // 连接超时

        // 健康检查
        bool enable_health_check = true;
        std::chrono::milliseconds health_check_interval = std::chrono::seconds(30);

        // 重连配置
        bool enable_auto_reconnect = true;
        int max_reconnect_attempts = 3;

        // 连接验证配置
        bool enable_connection_validation = true;  // 获取连接时是否验证
        bool validate_on_acquire = false;          // 每次获取时都验证（性能开销较大）
        bool validate_on_return = false;           // 归还时验证

        // 验证配置
        bool validate() const
        {
            return min_connections <= max_connections &&
                   initial_connections >= min_connections &&
                   initial_connections <= max_connections &&
                   max_connections > 0;
        }

        // 创建默认配置
        static ConnectionPoolConfig defaultConfig()
        {
            return ConnectionPoolConfig{};
        }

        // 创建自定义配置
        static ConnectionPoolConfig create(const std::string& host, int32_t port,
                                          size_t min_conn = 2, size_t max_conn = 10)
        {
            ConnectionPoolConfig config;
            config.host = host;
            config.port = port;
            config.min_connections = min_conn;
            config.max_connections = max_conn;
            config.initial_connections = min_conn;
            return config;
        }
    };

    /**
     * @brief 连接包装器，用于管理连接的生命周期
     */
    class PooledConnection
    {
    public:
        PooledConnection(std::shared_ptr<RedisClient> client, IOScheduler* scheduler)
            : m_client(std::move(client))
            , m_scheduler(scheduler)
            , m_last_used(std::chrono::steady_clock::now())
            , m_is_healthy(true)
        {
        }

        RedisClient* get() { return m_client.get(); }
        const RedisClient* get() const { return m_client.get(); }

        RedisClient* operator->() { return m_client.get(); }
        const RedisClient* operator->() const { return m_client.get(); }

        RedisClient& operator*() { return *m_client; }
        const RedisClient& operator*() const { return *m_client; }

        // 更新最后使用时间
        void updateLastUsed()
        {
            m_last_used = std::chrono::steady_clock::now();
        }

        // 获取空闲时间
        std::chrono::milliseconds getIdleTime() const
        {
            auto now = std::chrono::steady_clock::now();
            return std::chrono::duration_cast<std::chrono::milliseconds>(now - m_last_used);
        }

        // 健康状态
        bool isHealthy() const { return m_is_healthy; }
        void setHealthy(bool healthy) { m_is_healthy = healthy; }

        // 检查是否已关闭
        bool isClosed() const { return m_client->isClosed(); }

    private:
        std::shared_ptr<RedisClient> m_client;
        IOScheduler* m_scheduler;
        std::chrono::steady_clock::time_point m_last_used;
        bool m_is_healthy;
    };

    // 前向声明
    class RedisConnectionPool;

    /**
     * @brief 连接池初始化等待体
     */
    class PoolInitializeAwaitable
    {
    public:
        PoolInitializeAwaitable(RedisConnectionPool& pool);

        bool await_ready() const noexcept { return false; }
        bool await_suspend(std::coroutine_handle<> handle);
        RedisVoidResult await_resume();

    private:
        RedisConnectionPool& m_pool;
        size_t m_created_count = 0;
    };

    /**
     * @brief 连接池获取连接等待体
     */
    class PoolAcquireAwaitable
    {
    public:
        PoolAcquireAwaitable(RedisConnectionPool& pool);

        bool await_ready() const noexcept { return false; }
        bool await_suspend(std::coroutine_handle<> handle);
        std::expected<std::shared_ptr<PooledConnection>, RedisError> await_resume();

    private:
        RedisConnectionPool& m_pool;
        std::shared_ptr<PooledConnection> m_conn;
        std::chrono::steady_clock::time_point m_start_time;
    };

    /**
     * @brief Redis 连接池
     * @details 提供连接复用、自动扩缩容、健康检查等功能
     */
    class RedisConnectionPool
    {
    public:
        /**
         * @brief 构造函数
         * @param scheduler IO调度器
         * @param config 连接池配置
         */
        RedisConnectionPool(IOScheduler* scheduler, ConnectionPoolConfig config = ConnectionPoolConfig::defaultConfig());

        // 禁止拷贝
        RedisConnectionPool(const RedisConnectionPool&) = delete;
        RedisConnectionPool& operator=(const RedisConnectionPool&) = delete;

        // 禁止移动（因为包含 mutex）
        RedisConnectionPool(RedisConnectionPool&&) = delete;
        RedisConnectionPool& operator=(RedisConnectionPool&&) = delete;

        /**
         * @brief 初始化连接池
         * @return 初始化等待体
         */
        PoolInitializeAwaitable& initialize();

        /**
         * @brief 获取连接（协程安全）
         * @return 连接获取等待体
         */
        PoolAcquireAwaitable& acquire();

        /**
         * @brief 归还连接
         * @param conn 要归还的连接
         */
        void release(std::shared_ptr<PooledConnection> conn);

        /**
         * @brief 手动触发健康检查
         */
        void triggerHealthCheck();

        /**
         * @brief 手动触发空闲连接清理
         */
        void triggerIdleCleanup();

        /**
         * @brief 预热连接池（创建到最小连接数）
         */
        void warmup();

        /**
         * @brief 清理所有不健康的连接
         */
        size_t cleanupUnhealthyConnections();

        /**
         * @brief 扩容连接池（创建指定数量的连接）
         * @param count 要创建的连接数
         * @return 实际创建的连接数
         */
        size_t expandPool(size_t count);

        /**
         * @brief 缩容连接池（移除空闲连接到目标数量）
         * @param target_size 目标连接数
         * @return 实际移除的连接数
         */
        size_t shrinkPool(size_t target_size);

        /**
         * @brief 关闭连接池（同步方法）
         */
        void shutdown();

        /**
         * @brief 获取连接池统计信息
         */
        struct PoolStats
        {
            size_t total_connections;      // 总连接数
            size_t available_connections;  // 可用连接数
            size_t active_connections;     // 活跃连接数
            size_t waiting_requests;       // 等待中的请求数
            uint64_t total_acquired;       // 总获取次数
            uint64_t total_released;       // 总归还次数
            uint64_t total_created;        // 总创建次数
            uint64_t total_destroyed;      // 总销毁次数
            uint64_t health_check_failures;// 健康检查失败次数
            uint64_t reconnect_attempts;   // 重连尝试次数
            uint64_t reconnect_successes;  // 重连成功次数
            uint64_t validation_failures;  // 验证失败次数

            // 性能监控指标
            double avg_acquire_time_ms;    // 平均获取连接时间（毫秒）
            double max_acquire_time_ms;    // 最大获取连接时间（毫秒）
            size_t peak_active_connections;// 峰值活跃连接数
            uint64_t total_acquire_time_ms;// 总获取时间（用于计算平均值）
        };

        PoolStats getStats() const;

        /**
         * @brief 获取配置
         */
        const ConnectionPoolConfig& getConfig() const { return m_config; }

        ~RedisConnectionPool();

    private:
        friend class PoolInitializeAwaitable;
        friend class PoolAcquireAwaitable;

        /**
         * @brief 获取或创建连接（内部方法，同步）
         */
        std::expected<std::shared_ptr<PooledConnection>, RedisError> getConnectionSync();

        /**
         * @brief 检查连接健康状态（同步）
         */
        bool checkConnectionHealthSync(std::shared_ptr<PooledConnection> conn);

    private:
        IOScheduler* m_scheduler;
        ConnectionPoolConfig m_config;

        // 连接管理
        std::queue<std::shared_ptr<PooledConnection>> m_available_connections;
        std::vector<std::shared_ptr<PooledConnection>> m_all_connections;
        mutable std::mutex m_mutex;
        std::condition_variable m_cv;

        // 状态标志
        std::atomic<bool> m_is_initialized{false};
        std::atomic<bool> m_is_shutting_down{false};

        // 统计信息
        std::atomic<uint64_t> m_total_acquired{0};
        std::atomic<uint64_t> m_total_released{0};
        std::atomic<uint64_t> m_total_created{0};
        std::atomic<uint64_t> m_total_destroyed{0};
        std::atomic<uint64_t> m_health_check_failures{0};
        std::atomic<size_t> m_waiting_requests{0};
        std::atomic<uint64_t> m_reconnect_attempts{0};
        std::atomic<uint64_t> m_reconnect_successes{0};
        std::atomic<uint64_t> m_validation_failures{0};

        // 性能监控
        std::atomic<uint64_t> m_total_acquire_time_ms{0};
        std::atomic<double> m_max_acquire_time_ms{0.0};
        std::atomic<size_t> m_peak_active_connections{0};

        // awaitable 对象
        std::optional<PoolInitializeAwaitable> m_init_awaitable;
        std::optional<PoolAcquireAwaitable> m_acquire_awaitable;

        // 日志
        std::shared_ptr<spdlog::logger> m_logger;
    };

    /**
     * @brief RAII 风格的连接获取器
     * @details 自动归还连接到连接池
     */
    class ScopedConnection
    {
    public:
        ScopedConnection(RedisConnectionPool& pool, std::shared_ptr<PooledConnection> conn)
            : m_pool(&pool)
            , m_conn(std::move(conn))
        {
        }

        // 禁止拷贝
        ScopedConnection(const ScopedConnection&) = delete;
        ScopedConnection& operator=(const ScopedConnection&) = delete;

        // 允许移动
        ScopedConnection(ScopedConnection&& other) noexcept
            : m_pool(other.m_pool)
            , m_conn(std::move(other.m_conn))
        {
            other.m_pool = nullptr;
        }

        ScopedConnection& operator=(ScopedConnection&& other) noexcept
        {
            if (this != &other) {
                release();
                m_pool = other.m_pool;
                m_conn = std::move(other.m_conn);
                other.m_pool = nullptr;
            }
            return *this;
        }

        RedisClient* get() { return m_conn ? m_conn->get() : nullptr; }
        const RedisClient* get() const { return m_conn ? m_conn->get() : nullptr; }

        RedisClient* operator->() { return get(); }
        const RedisClient* operator->() const { return get(); }

        RedisClient& operator*() { return *get(); }
        const RedisClient& operator*() const { return *get(); }

        explicit operator bool() const { return m_conn != nullptr; }

        void release()
        {
            if (m_pool && m_conn) {
                m_pool->release(std::move(m_conn));
                m_conn = nullptr;
            }
        }

        ~ScopedConnection()
        {
            release();
        }

    private:
        RedisConnectionPool* m_pool;
        std::shared_ptr<PooledConnection> m_conn;
    };

} // namespace galay::redis

#endif // GALAY_REDIS_CONNECTION_POOL_H
