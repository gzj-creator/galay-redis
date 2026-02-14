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
#include "galay-kernel/concurrency/AsyncWaiter.h"
#include "RedisClient.h"

namespace galay::redis
{
    using RedisCommandResult = std::expected<std::vector<RedisValue>, RedisError>;

    class RedisCommandResultAwaitable
    {
    public:
        explicit RedisCommandResultAwaitable(
            std::shared_ptr<galay::kernel::AsyncWaiter<RedisCommandResult>> waiter);

        bool await_ready() const noexcept;
        bool await_suspend(std::coroutine_handle<galay::kernel::Coroutine::promise_type> handle) noexcept;
        RedisCommandResult await_resume() noexcept;

    private:
        std::shared_ptr<galay::kernel::AsyncWaiter<RedisCommandResult>> m_waiter;
        galay::kernel::AsyncWaiterAwaitable<RedisCommandResult> m_awaitable;
    };

    struct RedisNodeAddress
    {
        std::string host = "127.0.0.1";
        int32_t port = 6379;
        std::string username;
        std::string password;
        int32_t db_index = 0;
        int version = 2;
    };

    class RedisMasterSlaveClient
    {
    public:
        explicit RedisMasterSlaveClient(IOScheduler* scheduler,
                                        AsyncRedisConfig config = AsyncRedisConfig::noTimeout());

        RedisConnectAwaitable& connectMaster(const RedisNodeAddress& master);
        RedisConnectAwaitable& addReplica(const RedisNodeAddress& replica);

        RedisClientAwaitable& executeWrite(const std::string& cmd, const std::vector<std::string>& args);
        RedisPipelineAwaitable& pipelineWrite(const std::vector<std::vector<std::string>>& commands);
        RedisClientAwaitable& executeRead(const std::string& cmd, const std::vector<std::string>& args);
        RedisPipelineAwaitable& pipelineRead(const std::vector<std::vector<std::string>>& commands);
        RedisConnectAwaitable& addSentinel(const RedisNodeAddress& sentinel);
        void setSentinelMasterName(std::string master_name);
        void setAutoRetryAttempts(size_t attempts) noexcept;
        RedisCommandResultAwaitable refreshFromSentinel();
        RedisCommandResultAwaitable executeWriteAuto(const std::string& cmd, const std::vector<std::string>& args);
        RedisCommandResultAwaitable executeReadAuto(const std::string& cmd, const std::vector<std::string>& args);

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

        Coroutine executeAutoCoroutine(bool prefer_read,
                                       std::string cmd,
                                       std::vector<std::string> args,
                                       std::shared_ptr<galay::kernel::AsyncWaiter<RedisCommandResult>> waiter);
        Coroutine refreshSentinelCoroutine(std::shared_ptr<galay::kernel::AsyncWaiter<RedisCommandResult>> waiter);

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

    class RedisClusterClient
    {
    public:
        explicit RedisClusterClient(IOScheduler* scheduler,
                                    AsyncRedisConfig config = AsyncRedisConfig::noTimeout());

        RedisConnectAwaitable& addNode(const RedisClusterNodeAddress& node);
        void setSlotRange(size_t node_index, uint16_t slot_start, uint16_t slot_end);
        void setAutoRefreshInterval(std::chrono::milliseconds interval);

        RedisClientAwaitable& execute(const std::string& cmd, const std::vector<std::string>& args);
        RedisClientAwaitable& executeByKey(const std::string& routing_key,
                                           const std::string& cmd,
                                           const std::vector<std::string>& args);
        RedisPipelineAwaitable& pipelineByKey(const std::string& routing_key,
                                              const std::vector<std::vector<std::string>>& commands);
        RedisCommandResultAwaitable refreshSlots();
        RedisCommandResultAwaitable executeAuto(const std::string& cmd, const std::vector<std::string>& args);
        RedisCommandResultAwaitable executeByKeyAuto(const std::string& routing_key,
                                                     const std::string& cmd,
                                                     const std::vector<std::string>& args);

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

        Coroutine refreshSlotsCoroutine(std::shared_ptr<galay::kernel::AsyncWaiter<RedisCommandResult>> waiter);
        Coroutine executeAutoCoroutine(std::string routing_key,
                                       std::string cmd,
                                       std::vector<std::string> args,
                                       bool force_key_routing,
                                       std::shared_ptr<galay::kernel::AsyncWaiter<RedisCommandResult>> waiter);

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
}

#endif // GALAY_REDIS_TOPOLOGY_CLIENT_H
