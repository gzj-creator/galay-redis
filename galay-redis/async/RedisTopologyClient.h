#ifndef GALAY_REDIS_TOPOLOGY_CLIENT_H
#define GALAY_REDIS_TOPOLOGY_CLIENT_H

#include <cstdint>
#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <array>
#include <utility>
#include "RedisClient.h"

namespace galay::redis
{
    using RedisCommandResult = std::expected<std::vector<RedisValue>, RedisError>;

    struct RedisNodeAddress
    {
        std::string host = "127.0.0.1";
        int32_t port = 6379;
        std::string username;
        std::string password;
        int32_t db_index = 0;
        int version = 2;
    };

    class RedisMasterSlaveClient;

    class RedisMasterSlaveClientBuilder
    {
    public:
        RedisMasterSlaveClientBuilder& scheduler(IOScheduler* scheduler)
        {
            m_scheduler = scheduler;
            return *this;
        }

        RedisMasterSlaveClientBuilder& config(AsyncRedisConfig config)
        {
            m_config = std::move(config);
            return *this;
        }

        RedisMasterSlaveClientBuilder& sendTimeout(std::chrono::milliseconds timeout)
        {
            m_config.send_timeout = timeout;
            return *this;
        }

        RedisMasterSlaveClientBuilder& recvTimeout(std::chrono::milliseconds timeout)
        {
            m_config.recv_timeout = timeout;
            return *this;
        }

        RedisMasterSlaveClientBuilder& bufferSize(size_t size)
        {
            m_config.buffer_size = size;
            return *this;
        }

        RedisMasterSlaveClient build() const;

        AsyncRedisConfig buildConfig() const
        {
            return m_config;
        }

    private:
        IOScheduler* m_scheduler = nullptr;
        AsyncRedisConfig m_config = AsyncRedisConfig::noTimeout();
    };

    class RedisMasterSlaveClient
    {
    public:
        explicit RedisMasterSlaveClient(IOScheduler* scheduler,
                                        AsyncRedisConfig config = AsyncRedisConfig::noTimeout());

        RedisConnectOperation connectMaster(const RedisNodeAddress& master);
        RedisConnectOperation addReplica(const RedisNodeAddress& replica);

        Task<RedisCommandResult> execute(const std::string& cmd,
                                         const std::vector<std::string>& args,
                                         bool prefer_read = false,
                                         bool auto_retry = true);
        RedisExchangeOperation batch(std::span<const RedisCommandView> commands,
                                     bool prefer_read = false);
        RedisConnectOperation addSentinel(const RedisNodeAddress& sentinel);
        void setSentinelMasterName(std::string master_name);
        void setAutoRetryAttempts(size_t attempts) noexcept;
        Task<RedisCommandResult> refreshFromSentinel();

        RedisClient& master();
        std::optional<std::reference_wrapper<RedisClient>> replica(size_t index);
        size_t replicaCount() const noexcept;

    private:
        struct NodeHandle
        {
            RedisNodeAddress address;
            std::unique_ptr<RedisClient> client;
            bool connected = false;
        };

        Task<RedisCommandResult> executeAutoCoroutine(bool prefer_read,
                                                      std::string cmd,
                                                      std::vector<std::string> args,
                                                      size_t max_attempts);
        Task<RedisCommandResult> refreshSentinelCoroutine();

        bool isRetryableConnectionError(const RedisError& error) const noexcept;
        RedisClient* chooseReadClient();
        RedisClient* ensureMaster();
        RedisClient* chooseAvailableSentinel();
        bool parseMasterAddressReply(const std::vector<RedisValue>& values, RedisNodeAddress* out_addr) const;
        bool parseReplicaListReply(const std::vector<RedisValue>& values, std::vector<RedisNodeAddress>* replicas) const;

        IOScheduler* m_scheduler;
        AsyncRedisConfig m_config;
        std::unique_ptr<RedisClient> m_master;
        RedisNodeAddress m_master_address;
        std::vector<std::unique_ptr<RedisClient>> m_replicas;
        std::vector<RedisNodeAddress> m_replica_addresses;
        std::vector<bool> m_replica_connected;
        std::vector<NodeHandle> m_sentinels;
        std::string m_sentinel_master_name = "mymaster";
        bool m_master_connected = false;
        size_t m_read_cursor = 0;
        size_t m_auto_retry_attempts = 2;
    };

    struct RedisClusterNodeAddress : RedisNodeAddress
    {
        uint16_t slot_start = 0;
        uint16_t slot_end = 16383;
    };

    class RedisClusterClient;

    class RedisClusterClientBuilder
    {
    public:
        RedisClusterClientBuilder& scheduler(IOScheduler* scheduler)
        {
            m_scheduler = scheduler;
            return *this;
        }

        RedisClusterClientBuilder& config(AsyncRedisConfig config)
        {
            m_config = std::move(config);
            return *this;
        }

        RedisClusterClientBuilder& sendTimeout(std::chrono::milliseconds timeout)
        {
            m_config.send_timeout = timeout;
            return *this;
        }

        RedisClusterClientBuilder& recvTimeout(std::chrono::milliseconds timeout)
        {
            m_config.recv_timeout = timeout;
            return *this;
        }

        RedisClusterClientBuilder& bufferSize(size_t size)
        {
            m_config.buffer_size = size;
            return *this;
        }

        RedisClusterClient build() const;

        AsyncRedisConfig buildConfig() const
        {
            return m_config;
        }

    private:
        IOScheduler* m_scheduler = nullptr;
        AsyncRedisConfig m_config = AsyncRedisConfig::noTimeout();
    };

    class RedisClusterClient
    {
    public:
        explicit RedisClusterClient(IOScheduler* scheduler,
                                    AsyncRedisConfig config = AsyncRedisConfig::noTimeout());

        RedisConnectOperation addNode(const RedisClusterNodeAddress& node);
        void setSlotRange(size_t node_index, uint16_t slot_start, uint16_t slot_end);
        void setAutoRefreshInterval(std::chrono::milliseconds interval);

        Task<RedisCommandResult> execute(const std::string& cmd,
                                         const std::vector<std::string>& args,
                                         std::string routing_key = std::string(),
                                         bool auto_retry = true);
        RedisExchangeOperation batch(std::span<const RedisCommandView> commands,
                                     std::string routing_key = std::string());
        Task<RedisCommandResult> refreshSlots();

        uint16_t keySlot(const std::string& key) const;
        size_t nodeCount() const noexcept;
        std::optional<std::reference_wrapper<RedisClient>> node(size_t index);

    private:
        struct ClusterNode
        {
            RedisClusterNodeAddress address;
            std::unique_ptr<RedisClient> client;
            bool connected = false;
        };

        struct RedirectInfo
        {
            enum class Type
            {
                None,
                Moved,
                Ask,
            };
            Type type = Type::None;
            uint16_t slot = 0;
            std::string host;
            int32_t port = 0;
        };

        Task<RedisCommandResult> refreshSlotsCoroutine();
        Task<RedisCommandResult> executeAutoCoroutine(std::string routing_key,
                                                      std::string cmd,
                                                      std::vector<std::string> args,
                                                      bool force_key_routing,
                                                      bool allow_auto_refresh,
                                                      size_t max_attempts);

        static uint16_t crc16(const uint8_t* data, size_t len);
        static std::string extractHashTag(const std::string& key);
        static std::optional<RedirectInfo> parseRedirect(const RedisValue& value);

        RedisClient* chooseNodeBySlot(uint16_t slot) noexcept;
        RedisClient* chooseNodeByKey(const std::string& key) noexcept;
        ClusterNode* chooseNodeHandleBySlot(uint16_t slot) noexcept;
        ClusterNode* chooseNodeHandleByKey(const std::string& key) noexcept;
        ClusterNode* findOrCreateNode(const std::string& host, int32_t port);
        bool applyClusterSlots(const std::vector<RedisValue>& values, std::string* error_message);
        bool shouldAutoRefresh() const noexcept;

        IOScheduler* m_scheduler;
        AsyncRedisConfig m_config;
        std::vector<ClusterNode> m_nodes;
        std::unique_ptr<RedisClient> m_fallback_client;
        std::array<int, 16384> m_slot_owner{};
        std::chrono::milliseconds m_auto_refresh_interval{5000};
        std::chrono::steady_clock::time_point m_last_refresh_time{};
        bool m_slot_cache_ready = false;
    };

    class RedissMasterSlaveClient;

    class RedissMasterSlaveClientBuilder
    {
    public:
        RedissMasterSlaveClientBuilder& scheduler(IOScheduler* scheduler)
        {
            m_scheduler = scheduler;
            return *this;
        }

        RedissMasterSlaveClientBuilder& config(AsyncRedisConfig config)
        {
            m_config = std::move(config);
            return *this;
        }

        RedissMasterSlaveClientBuilder& tlsConfig(RedissClientConfig config)
        {
            m_tls_config = std::move(config);
            return *this;
        }

        RedissMasterSlaveClientBuilder& sendTimeout(std::chrono::milliseconds timeout)
        {
            m_config.send_timeout = timeout;
            return *this;
        }

        RedissMasterSlaveClientBuilder& recvTimeout(std::chrono::milliseconds timeout)
        {
            m_config.recv_timeout = timeout;
            return *this;
        }

        RedissMasterSlaveClientBuilder& bufferSize(size_t size)
        {
            m_config.buffer_size = size;
            return *this;
        }

        RedissMasterSlaveClient build() const;

        AsyncRedisConfig buildConfig() const
        {
            return m_config;
        }

        RedissClientConfig buildTlsConfig() const
        {
            return m_tls_config;
        }

    private:
        IOScheduler* m_scheduler = nullptr;
        AsyncRedisConfig m_config = AsyncRedisConfig::noTimeout();
        RedissClientConfig m_tls_config;
    };

    class RedissMasterSlaveClient
    {
    public:
        explicit RedissMasterSlaveClient(IOScheduler* scheduler,
                                         AsyncRedisConfig config = AsyncRedisConfig::noTimeout(),
                                         RedissClientConfig tls_config = {});

        detail::RedissConnectOperation connectMaster(const RedisNodeAddress& master);
        detail::RedissConnectOperation addReplica(const RedisNodeAddress& replica);

        Task<RedisCommandResult> execute(const std::string& cmd,
                                         const std::vector<std::string>& args,
                                         bool prefer_read = false,
                                         bool auto_retry = true);
        detail::RedissExchangeOperation batch(std::span<const RedisCommandView> commands,
                                              bool prefer_read = false);
        detail::RedissConnectOperation addSentinel(const RedisNodeAddress& sentinel);
        void setSentinelMasterName(std::string master_name);
        void setAutoRetryAttempts(size_t attempts) noexcept;
        Task<RedisCommandResult> refreshFromSentinel();

        RedissClient& master();
        std::optional<std::reference_wrapper<RedissClient>> replica(size_t index);
        size_t replicaCount() const noexcept;

    private:
        struct NodeHandle
        {
            RedisNodeAddress address;
            std::unique_ptr<RedissClient> client;
            bool connected = false;
        };

        Task<RedisCommandResult> executeAutoCoroutine(bool prefer_read,
                                                      std::string cmd,
                                                      std::vector<std::string> args,
                                                      size_t max_attempts);
        Task<RedisCommandResult> refreshSentinelCoroutine();

        bool isRetryableConnectionError(const RedisError& error) const noexcept;
        RedissClient* chooseReadClient();
        RedissClient* ensureMaster();
        bool parseMasterAddressReply(const std::vector<RedisValue>& values, RedisNodeAddress* out_addr) const;
        bool parseReplicaListReply(const std::vector<RedisValue>& values, std::vector<RedisNodeAddress>* replicas) const;

        IOScheduler* m_scheduler;
        AsyncRedisConfig m_config;
        RedissClientConfig m_tls_config;
        std::unique_ptr<RedissClient> m_master;
        RedisNodeAddress m_master_address;
        std::vector<std::unique_ptr<RedissClient>> m_replicas;
        std::vector<RedisNodeAddress> m_replica_addresses;
        std::vector<bool> m_replica_connected;
        std::vector<NodeHandle> m_sentinels;
        std::string m_sentinel_master_name = "mymaster";
        bool m_master_connected = false;
        size_t m_read_cursor = 0;
        size_t m_auto_retry_attempts = 2;
    };

    class RedissClusterClient;

    class RedissClusterClientBuilder
    {
    public:
        RedissClusterClientBuilder& scheduler(IOScheduler* scheduler)
        {
            m_scheduler = scheduler;
            return *this;
        }

        RedissClusterClientBuilder& config(AsyncRedisConfig config)
        {
            m_config = std::move(config);
            return *this;
        }

        RedissClusterClientBuilder& tlsConfig(RedissClientConfig config)
        {
            m_tls_config = std::move(config);
            return *this;
        }

        RedissClusterClientBuilder& sendTimeout(std::chrono::milliseconds timeout)
        {
            m_config.send_timeout = timeout;
            return *this;
        }

        RedissClusterClientBuilder& recvTimeout(std::chrono::milliseconds timeout)
        {
            m_config.recv_timeout = timeout;
            return *this;
        }

        RedissClusterClientBuilder& bufferSize(size_t size)
        {
            m_config.buffer_size = size;
            return *this;
        }

        RedissClusterClient build() const;

        AsyncRedisConfig buildConfig() const
        {
            return m_config;
        }

        RedissClientConfig buildTlsConfig() const
        {
            return m_tls_config;
        }

    private:
        IOScheduler* m_scheduler = nullptr;
        AsyncRedisConfig m_config = AsyncRedisConfig::noTimeout();
        RedissClientConfig m_tls_config;
    };

    class RedissClusterClient
    {
    public:
        explicit RedissClusterClient(IOScheduler* scheduler,
                                     AsyncRedisConfig config = AsyncRedisConfig::noTimeout(),
                                     RedissClientConfig tls_config = {});

        detail::RedissConnectOperation addNode(const RedisClusterNodeAddress& node);
        void setSlotRange(size_t node_index, uint16_t slot_start, uint16_t slot_end);
        void setAutoRefreshInterval(std::chrono::milliseconds interval);

        Task<RedisCommandResult> execute(const std::string& cmd,
                                         const std::vector<std::string>& args,
                                         std::string routing_key = std::string(),
                                         bool auto_retry = true);
        detail::RedissExchangeOperation batch(std::span<const RedisCommandView> commands,
                                              std::string routing_key = std::string());
        Task<RedisCommandResult> refreshSlots();

        uint16_t keySlot(const std::string& key) const;
        size_t nodeCount() const noexcept;
        std::optional<std::reference_wrapper<RedissClient>> node(size_t index);

    private:
        struct ClusterNode
        {
            RedisClusterNodeAddress address;
            std::unique_ptr<RedissClient> client;
            bool connected = false;
        };

        struct RedirectInfo
        {
            enum class Type
            {
                None,
                Moved,
                Ask,
            };
            Type type = Type::None;
            uint16_t slot = 0;
            std::string host;
            int32_t port = 0;
        };

        Task<RedisCommandResult> refreshSlotsCoroutine();
        Task<RedisCommandResult> executeAutoCoroutine(std::string routing_key,
                                                      std::string cmd,
                                                      std::vector<std::string> args,
                                                      bool force_key_routing,
                                                      bool allow_auto_refresh,
                                                      size_t max_attempts);

        static uint16_t crc16(const uint8_t* data, size_t len);
        static std::string extractHashTag(const std::string& key);
        static std::optional<RedirectInfo> parseRedirect(const RedisValue& value);

        RedissClient* chooseNodeBySlot(uint16_t slot) noexcept;
        RedissClient* chooseNodeByKey(const std::string& key) noexcept;
        ClusterNode* chooseNodeHandleBySlot(uint16_t slot) noexcept;
        ClusterNode* chooseNodeHandleByKey(const std::string& key) noexcept;
        ClusterNode* findOrCreateNode(const std::string& host, int32_t port);
        bool applyClusterSlots(const std::vector<RedisValue>& values, std::string* error_message);
        bool shouldAutoRefresh() const noexcept;

        IOScheduler* m_scheduler;
        AsyncRedisConfig m_config;
        RedissClientConfig m_tls_config;
        std::vector<ClusterNode> m_nodes;
        std::unique_ptr<RedissClient> m_fallback_client;
        std::array<int, 16384> m_slot_owner{};
        std::chrono::milliseconds m_auto_refresh_interval{5000};
        std::chrono::steady_clock::time_point m_last_refresh_time{};
        bool m_slot_cache_ready = false;
    };

    inline galay::redis::RedisMasterSlaveClient galay::redis::RedisMasterSlaveClientBuilder::build() const
    {
        return RedisMasterSlaveClient(m_scheduler, m_config);
    }

    inline galay::redis::RedisClusterClient galay::redis::RedisClusterClientBuilder::build() const
    {
        return RedisClusterClient(m_scheduler, m_config);
    }

    inline galay::redis::RedissMasterSlaveClient galay::redis::RedissMasterSlaveClientBuilder::build() const
    {
        return RedissMasterSlaveClient(m_scheduler, m_config, m_tls_config);
    }

    inline galay::redis::RedissClusterClient galay::redis::RedissClusterClientBuilder::build() const
    {
        return RedissClusterClient(m_scheduler, m_config, m_tls_config);
    }
}

#endif // GALAY_REDIS_TOPOLOGY_CLIENT_H
