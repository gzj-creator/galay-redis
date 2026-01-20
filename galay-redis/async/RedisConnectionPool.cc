#include "RedisConnectionPool.h"
#include "galay-redis/base/RedisLog.h"
#include <algorithm>

namespace galay::redis
{
    // ======================== PoolInitializeAwaitable 实现 ========================

    PoolInitializeAwaitable::PoolInitializeAwaitable(RedisConnectionPool& pool)
        : m_pool(pool)
    {
    }

    bool PoolInitializeAwaitable::await_suspend(std::coroutine_handle<> handle)
    {
        // 创建初始连接（同步方式）
        while (m_created_count < m_pool.m_config.initial_connections) {
            auto result = m_pool.getConnectionSync();
            if (result) {
                m_created_count++;
            } else {
                // 创建失败，停止
                RedisLogError(m_pool.m_logger, "Failed to create connection {}/{}: {}",
                             m_created_count + 1, m_pool.m_config.initial_connections,
                             result.error().message());
                break;
            }
        }

        // 立即恢复协程
        return false;
    }

    RedisVoidResult PoolInitializeAwaitable::await_resume()
    {
        if (m_created_count < m_pool.m_config.min_connections) {
            return std::unexpected(RedisError(
                RedisErrorType::REDIS_ERROR_TYPE_CONNECTION_ERROR,
                "Failed to create minimum connections"
            ));
        }

        m_pool.m_is_initialized = true;
        RedisLogInfo(m_pool.m_logger, "Connection pool initialized with {} connections", m_created_count);
        return {};
    }

    // ======================== PoolAcquireAwaitable 实现 ========================

    PoolAcquireAwaitable::PoolAcquireAwaitable(RedisConnectionPool& pool)
        : m_pool(pool)
        , m_start_time(std::chrono::steady_clock::now())
    {
    }

    bool PoolAcquireAwaitable::await_suspend(std::coroutine_handle<> handle)
    {
        if (!m_pool.m_is_initialized) {
            return false;  // 立即恢复，返回错误
        }

        if (m_pool.m_is_shutting_down) {
            return false;  // 立即恢复，返回错误
        }

        m_pool.m_waiting_requests++;

        std::lock_guard<std::mutex> lock(m_pool.m_mutex);

        // 尝试从可用连接中获取
        while (!m_pool.m_available_connections.empty()) {
            m_conn = m_pool.m_available_connections.front();
            m_pool.m_available_connections.pop();

            // 检查连接是否健康
            if (!m_conn->isClosed() && m_conn->isHealthy()) {
                m_conn->updateLastUsed();
                m_pool.m_total_acquired++;
                m_pool.m_waiting_requests--;
                return false;  // 立即恢复
            } else {
                // 连接不健康，销毁并继续
                auto it = std::find(m_pool.m_all_connections.begin(),
                                   m_pool.m_all_connections.end(), m_conn);
                if (it != m_pool.m_all_connections.end()) {
                    m_pool.m_all_connections.erase(it);
                }
                m_pool.m_total_destroyed++;
                m_conn = nullptr;
            }
        }

        // 如果还可以创建新连接
        if (m_pool.m_all_connections.size() < m_pool.m_config.max_connections) {
            // 释放锁后创建连接
            m_pool.m_mutex.unlock();
            auto result = m_pool.getConnectionSync();
            m_pool.m_mutex.lock();

            if (result) {
                m_conn = result.value();
                m_conn->updateLastUsed();
                m_pool.m_total_acquired++;
                m_pool.m_waiting_requests--;

                RedisLogDebug(m_pool.m_logger, "Created and acquired new connection, total: {}",
                             m_pool.m_all_connections.size());
                return false;  // 立即恢复
            } else {
                RedisLogWarn(m_pool.m_logger, "Failed to create new connection: {}",
                            result.error().message());
            }
        }

        m_pool.m_waiting_requests--;
        return false;  // 立即恢复，返回错误
    }

    std::expected<std::shared_ptr<PooledConnection>, RedisError>
    PoolAcquireAwaitable::await_resume()
    {
        if (!m_pool.m_is_initialized) {
            return std::unexpected(RedisError(
                RedisErrorType::REDIS_ERROR_TYPE_INTERNAL_ERROR,
                "Connection pool not initialized"
            ));
        }

        if (m_pool.m_is_shutting_down) {
            return std::unexpected(RedisError(
                RedisErrorType::REDIS_ERROR_TYPE_INTERNAL_ERROR,
                "Connection pool is shutting down"
            ));
        }

        if (!m_conn) {
            return std::unexpected(RedisError(
                RedisErrorType::REDIS_ERROR_TYPE_TIMEOUT_ERROR,
                "No available connections"
            ));
        }

        // 更新性能指标
        auto elapsed = std::chrono::steady_clock::now() - m_start_time;
        auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
        m_pool.m_total_acquire_time_ms += elapsed_ms;

        double current_max = m_pool.m_max_acquire_time_ms.load();
        while (elapsed_ms > current_max) {
            if (m_pool.m_max_acquire_time_ms.compare_exchange_weak(current_max, elapsed_ms)) {
                break;
            }
        }

        // 更新峰值活跃连接数
        size_t active = m_pool.m_all_connections.size() - m_pool.m_available_connections.size();
        size_t current_peak = m_pool.m_peak_active_connections.load();
        while (active > current_peak) {
            if (m_pool.m_peak_active_connections.compare_exchange_weak(current_peak, active)) {
                break;
            }
        }

        return m_conn;
    }

    // ======================== RedisConnectionPool 实现 ========================

    RedisConnectionPool::RedisConnectionPool(IOScheduler* scheduler, ConnectionPoolConfig config)
        : m_scheduler(scheduler)
        , m_config(std::move(config))
    {
        // 验证配置
        if (!m_config.validate()) {
            throw std::invalid_argument("Invalid connection pool configuration");
        }

        // 初始化日志
        try {
            m_logger = spdlog::get("RedisConnectionPool");
            if (!m_logger) {
                m_logger = spdlog::stdout_color_mt("RedisConnectionPool");
            }
        } catch (const spdlog::spdlog_ex& ex) {
            m_logger = spdlog::get("RedisConnectionPool");
            if (!m_logger) {
                m_logger = spdlog::default_logger();
            }
        }

        RedisLogInfo(m_logger, "Connection pool created: host={}:{}, min={}, max={}, initial={}",
                     m_config.host, m_config.port,
                     m_config.min_connections, m_config.max_connections,
                     m_config.initial_connections);
    }

    RedisConnectionPool::~RedisConnectionPool()
    {
        if (m_is_initialized && !m_is_shutting_down) {
            RedisLogWarn(m_logger, "Connection pool destroyed without proper shutdown");
            shutdown();
        }
    }

    PoolInitializeAwaitable& RedisConnectionPool::initialize()
    {
        if (!m_init_awaitable.has_value()) {
            m_init_awaitable.emplace(*this);
        }
        return *m_init_awaitable;
    }

    PoolAcquireAwaitable& RedisConnectionPool::acquire()
    {
        if (!m_acquire_awaitable.has_value()) {
            m_acquire_awaitable.emplace(*this);
        }
        return *m_acquire_awaitable;
    }

    void RedisConnectionPool::release(std::shared_ptr<PooledConnection> conn)
    {
        if (!conn) {
            return;
        }

        if (m_is_shutting_down) {
            RedisLogDebug(m_logger, "Connection released during shutdown, will be destroyed");
            return;
        }

        std::lock_guard<std::mutex> lock(m_mutex);

        // 检查连接是否健康
        if (conn->isClosed() || !conn->isHealthy()) {
            RedisLogWarn(m_logger, "Unhealthy connection released, removing from pool");
            auto it = std::find(m_all_connections.begin(), m_all_connections.end(), conn);
            if (it != m_all_connections.end()) {
                m_all_connections.erase(it);
            }
            m_total_destroyed++;
            return;
        }

        // 如果连接数超过最大值，销毁连接
        if (m_all_connections.size() > m_config.max_connections) {
            RedisLogDebug(m_logger, "Pool size exceeds max, destroying connection");
            auto it = std::find(m_all_connections.begin(), m_all_connections.end(), conn);
            if (it != m_all_connections.end()) {
                m_all_connections.erase(it);
            }
            m_total_destroyed++;
            return;
        }

        // 归还到可用连接池
        m_available_connections.push(conn);
        m_total_released++;

        RedisLogDebug(m_logger, "Connection released to pool, available: {}, total: {}",
                     m_available_connections.size(), m_all_connections.size());

        m_cv.notify_one();
    }

    std::expected<std::shared_ptr<PooledConnection>, RedisError>
    RedisConnectionPool::getConnectionSync()
    {
        RedisLogDebug(m_logger, "Creating new connection to {}:{}", m_config.host, m_config.port);

        // 带重试的连接创建
        for (int attempt = 0; attempt < m_config.max_reconnect_attempts; ++attempt) {
            if (attempt > 0) {
                m_reconnect_attempts++;
                RedisLogInfo(m_logger, "Reconnect attempt {}/{} for {}:{}",
                            attempt + 1, m_config.max_reconnect_attempts,
                            m_config.host, m_config.port);
            }

            try {
                auto client = std::make_shared<RedisClient>(m_scheduler);
                auto conn = std::make_shared<PooledConnection>(client, m_scheduler);

                // 注意：这里需要在协程上下文中调用 co_await client->connect()
                // 由于当前是同步方法，暂时创建未连接的客户端
                // 实际使用时，连接会在第一次使用时建立

                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    m_all_connections.push_back(conn);
                    m_total_created++;
                }

                if (attempt > 0) {
                    m_reconnect_successes++;
                    RedisLogInfo(m_logger, "Reconnect succeeded on attempt {}", attempt + 1);
                }

                RedisLogDebug(m_logger, "Connection created successfully, total: {}",
                             m_all_connections.size());
                return conn;

            } catch (const std::exception& e) {
                RedisLogError(m_logger, "Failed to create connection (attempt {}): {}",
                             attempt + 1, e.what());

                if (attempt == m_config.max_reconnect_attempts - 1) {
                    return std::unexpected(RedisError(
                        RedisErrorType::REDIS_ERROR_TYPE_CONNECTION_ERROR,
                        std::string("Failed to create connection after ") +
                        std::to_string(m_config.max_reconnect_attempts) + " attempts: " + e.what()
                    ));
                }
            }
        }

        return std::unexpected(RedisError(
            RedisErrorType::REDIS_ERROR_TYPE_CONNECTION_ERROR,
            "Failed to create connection"
        ));
    }

    bool RedisConnectionPool::checkConnectionHealthSync(std::shared_ptr<PooledConnection> conn)
    {
        if (!conn || conn->isClosed()) {
            return false;
        }

        // TODO: 实现同步健康检查
        // 在实际使用中，需要在协程上下文中调用 co_await conn->get()->ping()

        return conn->isHealthy();
    }

    void RedisConnectionPool::triggerHealthCheck()
    {
        if (!m_config.enable_health_check) {
            return;
        }

        RedisLogInfo(m_logger, "Running health check on {} connections", m_all_connections.size());

        std::vector<std::shared_ptr<PooledConnection>> unhealthy_connections;

        {
            std::lock_guard<std::mutex> lock(m_mutex);

            for (auto& conn : m_all_connections) {
                if (!checkConnectionHealthSync(conn)) {
                    unhealthy_connections.push_back(conn);
                }
            }

            if (!unhealthy_connections.empty()) {
                // 从可用连接中移除
                std::queue<std::shared_ptr<PooledConnection>> temp_queue;
                while (!m_available_connections.empty()) {
                    auto c = m_available_connections.front();
                    m_available_connections.pop();
                    if (c->isHealthy()) {
                        temp_queue.push(c);
                    }
                }
                m_available_connections = std::move(temp_queue);

                // 从所有连接中移除
                m_all_connections.erase(
                    std::remove_if(m_all_connections.begin(), m_all_connections.end(),
                        [](const auto& conn) { return !conn->isHealthy(); }),
                    m_all_connections.end()
                );

                m_total_destroyed += unhealthy_connections.size();

                RedisLogWarn(m_logger, "Removed {} unhealthy connections, remaining: {}",
                            unhealthy_connections.size(), m_all_connections.size());
            }
        }

        // 如果连接数低于最小值，创建新连接
        size_t current_size = m_all_connections.size();
        while (current_size < m_config.min_connections) {
            auto result = getConnectionSync();
            if (!result) {
                RedisLogError(m_logger, "Failed to create replacement connection: {}",
                             result.error().message());
                break;
            }

            // 将新连接加入可用队列
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_available_connections.push(result.value());
                current_size = m_all_connections.size();
            }

            RedisLogInfo(m_logger, "Created replacement connection, total: {}", current_size);
        }
    }

    void RedisConnectionPool::triggerIdleCleanup()
    {
        RedisLogInfo(m_logger, "Running idle connection cleanup");

        std::vector<std::shared_ptr<PooledConnection>> idle_connections;

        {
            std::lock_guard<std::mutex> lock(m_mutex);

            // 检查可用连接中的空闲连接
            std::queue<std::shared_ptr<PooledConnection>> temp_queue;
            while (!m_available_connections.empty()) {
                auto conn = m_available_connections.front();
                m_available_connections.pop();

                if (conn->getIdleTime() > m_config.idle_timeout &&
                    m_all_connections.size() > m_config.min_connections) {
                    idle_connections.push_back(conn);
                } else {
                    temp_queue.push(conn);
                }
            }
            m_available_connections = std::move(temp_queue);
        }

        // 移除空闲连接
        if (!idle_connections.empty()) {
            std::lock_guard<std::mutex> lock(m_mutex);
            for (auto& conn : idle_connections) {
                auto it = std::find(m_all_connections.begin(), m_all_connections.end(), conn);
                if (it != m_all_connections.end()) {
                    m_all_connections.erase(it);
                    m_total_destroyed++;
                }
            }

            RedisLogInfo(m_logger, "Cleaned up {} idle connections, remaining: {}",
                        idle_connections.size(), m_all_connections.size());
        }
    }

    void RedisConnectionPool::warmup()
    {
        RedisLogInfo(m_logger, "Warming up connection pool to {} connections", m_config.min_connections);

        size_t current_size;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            current_size = m_all_connections.size();
        }

        size_t created = 0;
        while (current_size < m_config.min_connections) {
            auto result = getConnectionSync();
            if (!result) {
                RedisLogError(m_logger, "Failed to create warmup connection: {}", result.error().message());
                break;
            }

            // 将新连接加入可用队列
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_available_connections.push(result.value());
                current_size = m_all_connections.size();
            }
            created++;
        }

        RedisLogInfo(m_logger, "Warmup complete, created {} connections, total: {}", created, current_size);
    }

    size_t RedisConnectionPool::cleanupUnhealthyConnections()
    {
        RedisLogInfo(m_logger, "Cleaning up unhealthy connections");

        std::vector<std::shared_ptr<PooledConnection>> unhealthy_connections;

        {
            std::lock_guard<std::mutex> lock(m_mutex);

            // 找出所有不健康的连接
            for (auto& conn : m_all_connections) {
                if (conn->isClosed() || !conn->isHealthy()) {
                    unhealthy_connections.push_back(conn);
                }
            }

            if (!unhealthy_connections.empty()) {
                // 从可用连接中移除
                std::queue<std::shared_ptr<PooledConnection>> temp_queue;
                while (!m_available_connections.empty()) {
                    auto c = m_available_connections.front();
                    m_available_connections.pop();
                    if (!c->isClosed() && c->isHealthy()) {
                        temp_queue.push(c);
                    }
                }
                m_available_connections = std::move(temp_queue);

                // 从所有连接中移除
                m_all_connections.erase(
                    std::remove_if(m_all_connections.begin(), m_all_connections.end(),
                        [](const auto& conn) { return conn->isClosed() || !conn->isHealthy(); }),
                    m_all_connections.end()
                );

                m_total_destroyed += unhealthy_connections.size();
            }
        }

        size_t removed = unhealthy_connections.size();
        if (removed > 0) {
            RedisLogInfo(m_logger, "Cleaned up {} unhealthy connections, remaining: {}",
                        removed, m_all_connections.size());
        }

        return removed;
    }

    size_t RedisConnectionPool::expandPool(size_t count)
    {
        if (count == 0) {
            return 0;
        }

        RedisLogInfo(m_logger, "Expanding pool by {} connections", count);

        size_t created = 0;
        for (size_t i = 0; i < count; ++i) {
            size_t current_size;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                current_size = m_all_connections.size();
            }

            // 检查是否超过最大连接数
            if (current_size >= m_config.max_connections) {
                RedisLogWarn(m_logger, "Cannot expand pool: reached max connections ({})", m_config.max_connections);
                break;
            }

            auto result = getConnectionSync();
            if (!result) {
                RedisLogError(m_logger, "Failed to create connection during expansion: {}",
                             result.error().message());
                break;
            }

            // 将新连接加入可用队列
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_available_connections.push(result.value());
            }
            created++;
        }

        RedisLogInfo(m_logger, "Pool expansion complete, created {} connections, total: {}",
                     created, m_all_connections.size());
        return created;
    }

    size_t RedisConnectionPool::shrinkPool(size_t target_size)
    {
        RedisLogInfo(m_logger, "Shrinking pool to {} connections", target_size);

        std::vector<std::shared_ptr<PooledConnection>> connections_to_remove;

        {
            std::lock_guard<std::mutex> lock(m_mutex);

            // 确保不低于最小连接数
            if (target_size < m_config.min_connections) {
                target_size = m_config.min_connections;
                RedisLogWarn(m_logger, "Target size adjusted to min_connections: {}", target_size);
            }

            // 如果当前连接数已经小于等于目标，不需要缩容
            if (m_all_connections.size() <= target_size) {
                RedisLogInfo(m_logger, "Current size ({}) <= target size ({}), no shrink needed",
                            m_all_connections.size(), target_size);
                return 0;
            }

            // 从可用连接中移除多余的连接
            size_t to_remove = m_all_connections.size() - target_size;
            std::queue<std::shared_ptr<PooledConnection>> temp_queue;

            while (!m_available_connections.empty() && connections_to_remove.size() < to_remove) {
                auto conn = m_available_connections.front();
                m_available_connections.pop();
                connections_to_remove.push_back(conn);
            }

            // 将剩余的连接放回队列
            while (!m_available_connections.empty()) {
                temp_queue.push(m_available_connections.front());
                m_available_connections.pop();
            }
            m_available_connections = std::move(temp_queue);

            // 从所有连接中移除
            for (auto& conn : connections_to_remove) {
                auto it = std::find(m_all_connections.begin(), m_all_connections.end(), conn);
                if (it != m_all_connections.end()) {
                    m_all_connections.erase(it);
                }
            }

            m_total_destroyed += connections_to_remove.size();
        }

        size_t removed = connections_to_remove.size();
        RedisLogInfo(m_logger, "Pool shrink complete, removed {} connections, remaining: {}",
                     removed, m_all_connections.size());
        return removed;
    }

    void RedisConnectionPool::shutdown()
    {
        if (m_is_shutting_down) {
            return;
        }

        m_is_shutting_down = true;
        RedisLogInfo(m_logger, "Shutting down connection pool");

        std::vector<std::shared_ptr<PooledConnection>> all_connections;

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            all_connections = m_all_connections;
            m_all_connections.clear();

            // 清空可用连接队列
            while (!m_available_connections.empty()) {
                m_available_connections.pop();
            }
        }

        m_is_initialized = false;
        RedisLogInfo(m_logger, "Connection pool shutdown complete, closed {} connections",
                     all_connections.size());
    }

    RedisConnectionPool::PoolStats RedisConnectionPool::getStats() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        PoolStats stats;
        stats.total_connections = m_all_connections.size();
        stats.available_connections = m_available_connections.size();
        stats.active_connections = stats.total_connections - stats.available_connections;
        stats.waiting_requests = m_waiting_requests.load();
        stats.total_acquired = m_total_acquired.load();
        stats.total_released = m_total_released.load();
        stats.total_created = m_total_created.load();
        stats.total_destroyed = m_total_destroyed.load();
        stats.health_check_failures = m_health_check_failures.load();
        stats.reconnect_attempts = m_reconnect_attempts.load();
        stats.reconnect_successes = m_reconnect_successes.load();
        stats.validation_failures = m_validation_failures.load();

        // 性能监控指标
        stats.total_acquire_time_ms = m_total_acquire_time_ms.load();
        stats.max_acquire_time_ms = m_max_acquire_time_ms.load();
        stats.peak_active_connections = m_peak_active_connections.load();

        // 计算平均获取时间
        if (stats.total_acquired > 0) {
            stats.avg_acquire_time_ms = static_cast<double>(stats.total_acquire_time_ms) / stats.total_acquired;
        } else {
            stats.avg_acquire_time_ms = 0.0;
        }

        return stats;
    }

} // namespace galay::redis
