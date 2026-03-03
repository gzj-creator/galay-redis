#include "RedisTopologyClient.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <string_view>
#include <unordered_map>

namespace galay::redis
{
    namespace
    {
        std::string valueToString(const RedisValue& value)
        {
            if (value.isString()) {
                return value.toString();
            }
            if (value.isStatus()) {
                return value.toStatus();
            }
            if (value.isInteger()) {
                return std::to_string(value.toInteger());
            }
            return "";
        }

        bool parseInt32(std::string_view input, int32_t* out)
        {
            if (!out || input.empty()) {
                return false;
            }
            int32_t value = 0;
            const auto* begin = input.data();
            const auto* end = begin + input.size();
            const auto result = std::from_chars(begin, end, value);
            if (result.ec != std::errc() || result.ptr != end) {
                return false;
            }
            *out = value;
            return true;
        }

        RedisError ioErrorToRedisError(const galay::kernel::IOError& io_error)
        {
            if (IOError::contains(io_error.code(), galay::kernel::kTimeout)) {
                return RedisError(RedisErrorType::REDIS_ERROR_TYPE_TIMEOUT_ERROR, io_error.message());
            }
            if (IOError::contains(io_error.code(), galay::kernel::kDisconnectError)) {
                return RedisError(RedisErrorType::REDIS_ERROR_TYPE_CONNECTION_CLOSED, io_error.message());
            }
            return RedisError(RedisErrorType::REDIS_ERROR_TYPE_NETWORK_ERROR, io_error.message());
        }

        std::optional<std::pair<std::string, int32_t>> parseHostPort(const std::string& host_port)
        {
            if (host_port.empty()) {
                return std::nullopt;
            }

            const auto at_pos = host_port.find('@');
            const auto endpoint = (at_pos == std::string::npos) ? host_port : host_port.substr(0, at_pos);
            const auto colon_pos = endpoint.rfind(':');
            if (colon_pos == std::string::npos || colon_pos == 0 || colon_pos == endpoint.size() - 1) {
                return std::nullopt;
            }

            const std::string host = endpoint.substr(0, colon_pos);
            int32_t port = 0;
            if (!parseInt32(std::string_view(endpoint).substr(colon_pos + 1), &port)) {
                return std::nullopt;
            }
            return std::make_pair(host, port);
        }

        RedisCommandResult cloneRedisCommandResult(const RedisCommandResult& result)
        {
            if (!result.has_value()) {
                return std::unexpected(result.error());
            }

            std::vector<RedisValue> values;
            values.reserve(result.value().size());
            for (const auto& value : result.value()) {
                values.emplace_back(value.getReply());
            }
            return values;
        }
    }

    RedisCommandResultAwaitable::RedisCommandResultAwaitable(
        std::shared_ptr<galay::kernel::AsyncWaiter<RedisCommandResult>> waiter)
        : m_waiter(std::move(waiter))
        , m_awaitable(m_waiter.get())
        , m_result(RedisCommandResult{})
    {
    }

    bool RedisCommandResultAwaitable::await_ready() const noexcept
    {
        return m_awaitable.await_ready();
    }

    bool RedisCommandResultAwaitable::await_suspend(
        std::coroutine_handle<galay::kernel::Coroutine::promise_type> handle) noexcept
    {
        return m_awaitable.await_suspend(handle);
    }

    RedisCommandResult RedisCommandResultAwaitable::await_resume() noexcept
    {
        if (!m_result) {
            return std::unexpected(ioErrorToRedisError(m_result.error()));
        }

        auto wait_result = m_awaitable.await_resume();
        if (!wait_result) {
            return std::unexpected(ioErrorToRedisError(wait_result.error()));
        }
        return std::move(wait_result.value());
    }

    RedisMasterSlaveClient::RedisMasterSlaveClient(IOScheduler* scheduler, AsyncRedisConfig config)
        : m_scheduler(scheduler)
        , m_config(std::move(config))
    {
    }

    RedisClient* RedisMasterSlaveClient::ensureMaster()
    {
        if (!m_master) {
            m_master = std::make_unique<RedisClient>(m_scheduler, m_config);
        }
        return m_master.get();
    }

    RedisConnectAwaitable RedisMasterSlaveClient::connectMaster(const RedisNodeAddress& master)
    {
        m_master_address = master;
        m_master_connected = false;
        auto* master_client = ensureMaster();
        return master_client->connect(master.host,
                                      master.port,
                                      master.username,
                                      master.password,
                                      master.db_index,
                                      master.version);
    }

    RedisConnectAwaitable RedisMasterSlaveClient::addReplica(const RedisNodeAddress& replica)
    {
        auto client = std::make_unique<RedisClient>(m_scheduler, m_config);
        auto* raw_client = client.get();
        m_replicas.push_back(std::move(client));
        m_replica_addresses.push_back(replica);
        m_replica_connected.push_back(false);
        return raw_client->connect(replica.host,
                                   replica.port,
                                   replica.username,
                                   replica.password,
                                   replica.db_index,
                                   replica.version);
    }

    RedisConnectAwaitable RedisMasterSlaveClient::addSentinel(const RedisNodeAddress& sentinel)
    {
        NodeHandle node;
        node.address = sentinel;
        node.client = std::make_unique<RedisClient>(m_scheduler, m_config);
        node.connected = false;
        auto* raw_client = node.client.get();
        m_sentinels.push_back(std::move(node));
        return raw_client->connect(sentinel.host,
                                   sentinel.port,
                                   sentinel.username,
                                   sentinel.password,
                                   sentinel.db_index,
                                   sentinel.version);
    }

    void RedisMasterSlaveClient::setSentinelMasterName(std::string master_name)
    {
        if (!master_name.empty()) {
            m_sentinel_master_name = std::move(master_name);
        }
    }

    void RedisMasterSlaveClient::setAutoRetryAttempts(size_t attempts) noexcept
    {
        m_auto_retry_attempts = std::max<size_t>(1, attempts);
    }

    RedisCommandResultAwaitable RedisMasterSlaveClient::refreshFromSentinel()
    {
        auto waiter = std::make_shared<galay::kernel::AsyncWaiter<RedisCommandResult>>();
        m_sentinel_refresh_waiters.push_back(waiter);
        if (!m_sentinel_refresh_inflight) {
            m_sentinel_refresh_inflight = true;
            m_scheduler->spawn(refreshSentinelCoroutine());
        }
        return RedisCommandResultAwaitable(waiter);
    }

    RedisCommandResultAwaitable RedisMasterSlaveClient::executeWriteAuto(
        const std::string& cmd,
        const std::vector<std::string>& args)
    {
        auto waiter = std::make_shared<galay::kernel::AsyncWaiter<RedisCommandResult>>();
        m_scheduler->spawn(executeAutoCoroutine(false, cmd, args, waiter));
        return RedisCommandResultAwaitable(waiter);
    }

    RedisCommandResultAwaitable RedisMasterSlaveClient::executeReadAuto(
        const std::string& cmd,
        const std::vector<std::string>& args)
    {
        auto waiter = std::make_shared<galay::kernel::AsyncWaiter<RedisCommandResult>>();
        m_scheduler->spawn(executeAutoCoroutine(true, cmd, args, waiter));
        return RedisCommandResultAwaitable(waiter);
    }

    RedisClientAwaitable RedisMasterSlaveClient::executeWrite(const std::string& cmd,
                                                               const std::vector<std::string>& args)
    {
        return ensureMaster()->execute(cmd, args);
    }

    RedisPipelineAwaitable RedisMasterSlaveClient::pipelineWrite(
        const std::vector<std::vector<std::string>>& commands)
    {
        return ensureMaster()->pipeline(commands);
    }

    RedisClient* RedisMasterSlaveClient::chooseReadClient()
    {
        if (m_replicas.empty()) {
            return ensureMaster();
        }

        const size_t base_index = m_read_cursor % m_replicas.size();
        for (size_t i = 0; i < m_replicas.size(); ++i) {
            const size_t idx = (base_index + i) % m_replicas.size();
            auto* replica = m_replicas[idx].get();
            if (replica) {
                m_read_cursor = idx + 1;
                return replica;
            }
        }

        return ensureMaster();
    }

    RedisClientAwaitable RedisMasterSlaveClient::executeRead(const std::string& cmd,
                                                              const std::vector<std::string>& args)
    {
        return chooseReadClient()->execute(cmd, args);
    }

    RedisPipelineAwaitable RedisMasterSlaveClient::pipelineRead(
        const std::vector<std::vector<std::string>>& commands)
    {
        return chooseReadClient()->pipeline(commands);
    }

    RedisClient& RedisMasterSlaveClient::master()
    {
        return *ensureMaster();
    }

    std::optional<std::reference_wrapper<RedisClient>> RedisMasterSlaveClient::replica(size_t index)
    {
        if (index >= m_replicas.size() || !m_replicas[index]) {
            return std::nullopt;
        }
        return *m_replicas[index];
    }

    size_t RedisMasterSlaveClient::replicaCount() const noexcept
    {
        return m_replicas.size();
    }

    bool RedisMasterSlaveClient::isRetryableConnectionError(const RedisError& error) const noexcept
    {
        switch (error.type()) {
            case REDIS_ERROR_TYPE_CONNECTION_ERROR:
            case REDIS_ERROR_TYPE_TIMEOUT_ERROR:
            case REDIS_ERROR_TYPE_SEND_ERROR:
            case REDIS_ERROR_TYPE_RECV_ERROR:
            case REDIS_ERROR_TYPE_NETWORK_ERROR:
            case REDIS_ERROR_TYPE_CONNECTION_CLOSED:
                return true;
            default:
                return false;
        }
    }

    RedisClient* RedisMasterSlaveClient::chooseAvailableSentinel()
    {
        for (auto& sentinel : m_sentinels) {
            if (sentinel.client) {
                return sentinel.client.get();
            }
        }
        return nullptr;
    }

    bool RedisMasterSlaveClient::parseMasterAddressReply(const std::vector<RedisValue>& values,
                                                         RedisNodeAddress* out_addr) const
    {
        if (!out_addr || values.empty() || !values[0].isArray()) {
            return false;
        }

        const auto parts = values[0].toArray();
        if (parts.size() < 2) {
            return false;
        }

        const auto host = valueToString(parts[0]);
        const auto port_str = valueToString(parts[1]);
        int32_t port = 0;
        if (host.empty() || !parseInt32(port_str, &port)) {
            return false;
        }

        *out_addr = m_master_address;
        out_addr->host = host;
        out_addr->port = port;
        return true;
    }

    bool RedisMasterSlaveClient::parseReplicaListReply(const std::vector<RedisValue>& values,
                                                       std::vector<RedisNodeAddress>* replicas) const
    {
        if (!replicas || values.empty() || !values[0].isArray()) {
            return false;
        }

        replicas->clear();
        const auto rows = values[0].toArray();
        for (const auto& row : rows) {
            if (!row.isArray()) {
                continue;
            }

            const auto kvs = row.toArray();
            std::unordered_map<std::string, std::string> fields;
            for (size_t i = 0; i + 1 < kvs.size(); i += 2) {
                const auto key = valueToString(kvs[i]);
                const auto val = valueToString(kvs[i + 1]);
                if (!key.empty()) {
                    fields[key] = val;
                }
            }

            const auto it_ip = fields.find("ip");
            const auto it_port = fields.find("port");
            if (it_ip == fields.end() || it_port == fields.end()) {
                continue;
            }

            auto flags = fields["flags"];
            if (flags.find("s_down") != std::string::npos ||
                flags.find("o_down") != std::string::npos ||
                flags.find("disconnected") != std::string::npos) {
                continue;
            }

            int32_t port = 0;
            if (!parseInt32(it_port->second, &port)) {
                continue;
            }

            RedisNodeAddress replica = m_master_address;
            replica.host = it_ip->second;
            replica.port = port;
            replicas->push_back(std::move(replica));
        }

        return true;
    }

    Coroutine RedisMasterSlaveClient::refreshSentinelCoroutine()
    {
        auto notify_and_finish = [this](RedisCommandResult result) mutable {
            auto waiters = std::move(m_sentinel_refresh_waiters);
            m_sentinel_refresh_waiters.clear();
            m_sentinel_refresh_inflight = false;
            size_t last = waiters.size();
            while (last > 0 && !waiters[last - 1]) {
                --last;
            }
            for (size_t i = 0; i < last; ++i) {
                if (!waiters[i]) {
                    continue;
                }
                if (i + 1 == last) {
                    waiters[i]->notify(std::move(result));
                } else {
                    auto cloned_result = cloneRedisCommandResult(result);
                    waiters[i]->notify(std::move(cloned_result));
                }
            }
        };

        RedisCommandResult final_result = std::unexpected(
            RedisError(REDIS_ERROR_TYPE_CONNECTION_ERROR, "No available sentinel"));

        if (m_sentinels.empty()) {
            notify_and_finish(std::move(final_result));
            co_return;
        }

        NodeHandle* selected = nullptr;
        for (auto& sentinel : m_sentinels) {
            if (!sentinel.client) {
                continue;
            }
            if (!sentinel.connected) {
                auto connect_result = co_await sentinel.client->connect(sentinel.address.host,
                                                                        sentinel.address.port,
                                                                        sentinel.address.username,
                                                                        sentinel.address.password,
                                                                        sentinel.address.db_index,
                                                                        sentinel.address.version);
                if (!connect_result) {
                    continue;
                }
                sentinel.connected = true;
            }
            selected = &sentinel;
            break;
        }

        if (!selected) {
            notify_and_finish(std::move(final_result));
            co_return;
        }

        auto master_reply = co_await selected->client->execute("SENTINEL",
                                                               {"get-master-addr-by-name", m_sentinel_master_name});
        if (!master_reply) {
            notify_and_finish(std::unexpected(master_reply.error()));
            co_return;
        }
        if (!master_reply.value().has_value()) {
            notify_and_finish(std::unexpected(RedisError(REDIS_ERROR_TYPE_PARSE_ERROR,
                                                         "Sentinel master reply missing")));
            co_return;
        }

        RedisNodeAddress latest_master = m_master_address;
        if (!parseMasterAddressReply(master_reply.value().value(), &latest_master)) {
            notify_and_finish(std::unexpected(RedisError(REDIS_ERROR_TYPE_PARSE_ERROR,
                                                         "Failed to parse sentinel master address")));
            co_return;
        }

        const bool master_changed = latest_master.host != m_master_address.host ||
                                    latest_master.port != m_master_address.port;
        m_master_address = latest_master;
        if (master_changed) {
            m_master_connected = false;
            if (m_master) {
                co_await m_master->close();
            }
            m_master = std::make_unique<RedisClient>(m_scheduler, m_config);
        }

        auto replicas_reply = co_await selected->client->execute("SENTINEL",
                                                                 {"replicas", m_sentinel_master_name});
        if (replicas_reply && replicas_reply.value().has_value()) {
            std::vector<RedisNodeAddress> parsed_replicas;
            if (parseReplicaListReply(replicas_reply.value().value(), &parsed_replicas)) {
                m_replica_addresses = std::move(parsed_replicas);
                m_replicas.clear();
                m_replica_connected.clear();
                m_replicas.reserve(m_replica_addresses.size());
                m_replica_connected.reserve(m_replica_addresses.size());
                for (const auto& addr : m_replica_addresses) {
                    auto client = std::make_unique<RedisClient>(m_scheduler, m_config);
                    auto connect_res = co_await client->connect(addr.host,
                                                                addr.port,
                                                                addr.username,
                                                                addr.password,
                                                                addr.db_index,
                                                                addr.version);
                    m_replica_connected.push_back(connect_res.has_value());
                    m_replicas.push_back(std::move(client));
                }
            }
        }

        auto& master_values = master_reply.value().value();
        notify_and_finish(std::move(master_values));
        co_return;
    }

    Coroutine RedisMasterSlaveClient::executeAutoCoroutine(
        bool prefer_read,
        std::string cmd,
        std::vector<std::string> args,
        std::shared_ptr<galay::kernel::AsyncWaiter<RedisCommandResult>> waiter)
    {
        RedisCommandResult final_result = std::unexpected(
            RedisError(REDIS_ERROR_TYPE_CONNECTION_ERROR, "Command execution not started"));

        const size_t max_attempts = std::max<size_t>(1, m_auto_retry_attempts);
        for (size_t attempt = 0; attempt < max_attempts; ++attempt) {
            RedisClient* target = prefer_read ? chooseReadClient() : ensureMaster();
            bool is_master_target = (target == ensureMaster());
            size_t replica_index = static_cast<size_t>(-1);
            if (!is_master_target) {
                for (size_t i = 0; i < m_replicas.size(); ++i) {
                    if (m_replicas[i].get() == target) {
                        replica_index = i;
                        break;
                    }
                }
            }

            if (is_master_target && !m_master_connected) {
                auto connect_result = co_await ensureMaster()->connect(m_master_address.host,
                                                                       m_master_address.port,
                                                                       m_master_address.username,
                                                                       m_master_address.password,
                                                                       m_master_address.db_index,
                                                                       m_master_address.version);
                if (!connect_result) {
                    final_result = std::unexpected(connect_result.error());
                } else {
                    m_master_connected = true;
                }
            } else if (!is_master_target &&
                       replica_index != static_cast<size_t>(-1) &&
                       replica_index < m_replica_addresses.size() &&
                       replica_index < m_replica_connected.size() &&
                       !m_replica_connected[replica_index]) {
                const auto& addr = m_replica_addresses[replica_index];
                auto connect_result = co_await target->connect(addr.host,
                                                               addr.port,
                                                               addr.username,
                                                               addr.password,
                                                               addr.db_index,
                                                               addr.version);
                if (!connect_result) {
                    final_result = std::unexpected(connect_result.error());
                } else {
                    m_replica_connected[replica_index] = true;
                }
            }

            auto exec_result = co_await target->execute(cmd, args);
            if (exec_result && exec_result.value().has_value()) {
                auto& exec_values = exec_result.value().value();
                final_result = std::move(exec_values);
                break;
            }

            if (!exec_result) {
                final_result = std::unexpected(exec_result.error());
            } else {
                final_result = std::unexpected(RedisError(REDIS_ERROR_TYPE_PARSE_ERROR,
                                                         "Empty response from redis command"));
            }

            if (!final_result.has_value() &&
                isRetryableConnectionError(final_result.error()) &&
                !m_sentinels.empty() &&
                (attempt + 1) < max_attempts) {
                auto refresh_result = co_await refreshFromSentinel();
                if (refresh_result) {
                    continue;
                }
            }
            break;
        }

        waiter->notify(std::move(final_result));
    }

    RedisClusterClient::RedisClusterClient(IOScheduler* scheduler, AsyncRedisConfig config)
        : m_scheduler(scheduler)
        , m_config(std::move(config))
    {
        m_slot_owner.fill(-1);
    }

    RedisConnectAwaitable RedisClusterClient::addNode(const RedisClusterNodeAddress& node)
    {
        ClusterNode cluster_node;
        cluster_node.address = node;
        cluster_node.client = std::make_unique<RedisClient>(m_scheduler, m_config);
        cluster_node.connected = false;

        auto* raw_client = cluster_node.client.get();
        m_nodes.push_back(std::move(cluster_node));
        const int idx = static_cast<int>(m_nodes.size() - 1);
        for (uint16_t slot = node.slot_start; slot <= node.slot_end; ++slot) {
            m_slot_owner[slot] = idx;
            if (slot == 16383) break;
        }
        m_slot_cache_ready = true;

        return raw_client->connect(node.host,
                                   node.port,
                                   node.username,
                                   node.password,
                                   node.db_index,
                                   node.version);
    }

    void RedisClusterClient::setSlotRange(size_t node_index, uint16_t slot_start, uint16_t slot_end)
    {
        if (node_index >= m_nodes.size()) {
            return;
        }
        m_nodes[node_index].address.slot_start = slot_start;
        m_nodes[node_index].address.slot_end = slot_end;

        const int idx = static_cast<int>(node_index);
        for (uint16_t slot = slot_start; slot <= slot_end; ++slot) {
            m_slot_owner[slot] = idx;
            if (slot == 16383) break;
        }
        m_slot_cache_ready = true;
    }

    void RedisClusterClient::setAutoRefreshInterval(std::chrono::milliseconds interval)
    {
        if (interval.count() > 0) {
            m_auto_refresh_interval = interval;
        }
    }

    RedisCommandResultAwaitable RedisClusterClient::refreshSlots()
    {
        auto waiter = std::make_shared<galay::kernel::AsyncWaiter<RedisCommandResult>>();
        m_slots_refresh_waiters.push_back(waiter);
        if (!m_slots_refresh_inflight) {
            m_slots_refresh_inflight = true;
            m_scheduler->spawn(refreshSlotsCoroutine());
        }
        return RedisCommandResultAwaitable(waiter);
    }

    RedisCommandResultAwaitable RedisClusterClient::executeAuto(const std::string& cmd,
                                                                const std::vector<std::string>& args)
    {
        auto waiter = std::make_shared<galay::kernel::AsyncWaiter<RedisCommandResult>>();
        const std::string routing_key = args.empty() ? std::string() : args.front();
        m_scheduler->spawn(executeAutoCoroutine(routing_key, cmd, args, !routing_key.empty(), waiter));
        return RedisCommandResultAwaitable(waiter);
    }

    RedisCommandResultAwaitable RedisClusterClient::executeByKeyAuto(const std::string& routing_key,
                                                                     const std::string& cmd,
                                                                     const std::vector<std::string>& args)
    {
        auto waiter = std::make_shared<galay::kernel::AsyncWaiter<RedisCommandResult>>();
        m_scheduler->spawn(executeAutoCoroutine(routing_key, cmd, args, true, waiter));
        return RedisCommandResultAwaitable(waiter);
    }

    RedisClientAwaitable RedisClusterClient::execute(const std::string& cmd,
                                                      const std::vector<std::string>& args)
    {
        RedisClient* node = nullptr;
        if (!args.empty()) {
            node = chooseNodeByKey(args.front());
        } else {
            node = chooseNodeBySlot(0);
        }

        if (!node) {
            if (!m_fallback_client) {
                m_fallback_client = std::make_unique<RedisClient>(m_scheduler, m_config);
            }
            return m_fallback_client->execute(cmd, args);
        }

        return node->execute(cmd, args);
    }

    RedisClientAwaitable RedisClusterClient::executeByKey(const std::string& routing_key,
                                                           const std::string& cmd,
                                                           const std::vector<std::string>& args)
    {
        auto* node = chooseNodeByKey(routing_key);
        if (!node) {
            if (!m_fallback_client) {
                m_fallback_client = std::make_unique<RedisClient>(m_scheduler, m_config);
            }
            return m_fallback_client->execute(cmd, args);
        }

        return node->execute(cmd, args);
    }

    RedisPipelineAwaitable RedisClusterClient::pipelineByKey(
        const std::string& routing_key,
        const std::vector<std::vector<std::string>>& commands)
    {
        auto* node = chooseNodeByKey(routing_key);
        if (!node) {
            if (!m_fallback_client) {
                m_fallback_client = std::make_unique<RedisClient>(m_scheduler, m_config);
            }
            return m_fallback_client->pipeline(commands);
        }

        return node->pipeline(commands);
    }

    uint16_t RedisClusterClient::keySlot(const std::string& key) const
    {
        auto hash_key = extractHashTag(key);
        return crc16(reinterpret_cast<const uint8_t*>(hash_key.data()), hash_key.size()) % 16384;
    }

    size_t RedisClusterClient::nodeCount() const noexcept
    {
        return m_nodes.size();
    }

    std::optional<std::reference_wrapper<RedisClient>> RedisClusterClient::node(size_t index)
    {
        if (index >= m_nodes.size() || !m_nodes[index].client) {
            return std::nullopt;
        }
        return *m_nodes[index].client;
    }

    std::optional<RedisClusterClient::RedirectInfo> RedisClusterClient::parseRedirect(const RedisValue& value)
    {
        if (!value.isError()) {
            return std::nullopt;
        }

        const auto msg = value.toError();
        if (msg.empty()) {
            return std::nullopt;
        }

        std::vector<std::string> parts;
        size_t start = 0;
        while (start < msg.size()) {
            while (start < msg.size() && std::isspace(static_cast<unsigned char>(msg[start])) != 0) {
                ++start;
            }
            if (start >= msg.size()) {
                break;
            }
            size_t end = start;
            while (end < msg.size() && std::isspace(static_cast<unsigned char>(msg[end])) == 0) {
                ++end;
            }
            parts.push_back(msg.substr(start, end - start));
            start = end;
        }

        if (parts.size() < 3) {
            return std::nullopt;
        }

        RedirectInfo info;
        if (parts[0] == "MOVED") {
            info.type = RedirectInfo::Type::Moved;
        } else if (parts[0] == "ASK") {
            info.type = RedirectInfo::Type::Ask;
        } else {
            return std::nullopt;
        }

        int32_t slot = 0;
        if (!parseInt32(parts[1], &slot) || slot < 0 || slot > 16383) {
            return std::nullopt;
        }
        info.slot = static_cast<uint16_t>(slot);

        auto endpoint = parseHostPort(parts[2]);
        if (!endpoint.has_value()) {
            return std::nullopt;
        }
        info.host = endpoint->first;
        info.port = endpoint->second;
        return info;
    }

    RedisClusterClient::ClusterNode* RedisClusterClient::chooseNodeHandleBySlot(uint16_t slot) noexcept
    {
        if (m_nodes.empty()) {
            return nullptr;
        }

        const int owner = m_slot_owner[slot];
        if (owner >= 0 && static_cast<size_t>(owner) < m_nodes.size()) {
            return &m_nodes[owner];
        }

        for (auto& node : m_nodes) {
            if (slot >= node.address.slot_start && slot <= node.address.slot_end) {
                return &node;
            }
        }

        return &m_nodes.front();
    }

    RedisClusterClient::ClusterNode* RedisClusterClient::chooseNodeHandleByKey(const std::string& key) noexcept
    {
        return chooseNodeHandleBySlot(keySlot(key));
    }

    RedisClient* RedisClusterClient::chooseNodeBySlot(uint16_t slot) noexcept
    {
        auto* node = chooseNodeHandleBySlot(slot);
        return node ? node->client.get() : nullptr;
    }

    RedisClient* RedisClusterClient::chooseNodeByKey(const std::string& key) noexcept
    {
        auto* node = chooseNodeHandleByKey(key);
        return node ? node->client.get() : nullptr;
    }

    RedisClusterClient::ClusterNode* RedisClusterClient::findOrCreateNode(const std::string& host, int32_t port)
    {
        for (auto& node : m_nodes) {
            if (node.address.host == host && node.address.port == port) {
                return &node;
            }
        }

        ClusterNode node;
        node.address.host = host;
        node.address.port = port;
        node.address.slot_start = 0;
        node.address.slot_end = 16383;
        node.client = std::make_unique<RedisClient>(m_scheduler, m_config);
        node.connected = false;
        m_nodes.push_back(std::move(node));
        return &m_nodes.back();
    }

    bool RedisClusterClient::applyClusterSlots(const std::vector<RedisValue>& values, std::string* error_message)
    {
        if (values.empty() || !values[0].isArray()) {
            if (error_message) {
                *error_message = "CLUSTER SLOTS response is not array";
            }
            return false;
        }

        auto new_owner = m_slot_owner;
        new_owner.fill(-1);

        const auto slots_rows = values[0].toArray();
        for (const auto& row : slots_rows) {
            if (!row.isArray()) {
                continue;
            }
            const auto row_values = row.toArray();
            if (row_values.size() < 3 || !row_values[0].isInteger() || !row_values[1].isInteger() ||
                !row_values[2].isArray()) {
                continue;
            }

            const int64_t start = row_values[0].toInteger();
            const int64_t end = row_values[1].toInteger();
            if (start < 0 || end < 0 || start > end || end > 16383) {
                continue;
            }

            const auto master_node = row_values[2].toArray();
            if (master_node.size() < 2) {
                continue;
            }

            const auto host = valueToString(master_node[0]);
            const auto port_string = valueToString(master_node[1]);
            int32_t port = 0;
            if (host.empty() || !parseInt32(port_string, &port)) {
                continue;
            }

            auto* node = findOrCreateNode(host, port);
            if (!node) {
                continue;
            }

            node->address.slot_start = static_cast<uint16_t>(std::min<int64_t>(node->address.slot_start, start));
            node->address.slot_end = static_cast<uint16_t>(std::max<int64_t>(node->address.slot_end, end));

            const int owner = static_cast<int>(node - m_nodes.data());
            for (int64_t slot = start; slot <= end; ++slot) {
                new_owner[slot] = owner;
            }
        }

        m_slot_owner = new_owner;
        m_slot_cache_ready = true;
        m_last_refresh_time = std::chrono::steady_clock::now();
        return true;
    }

    bool RedisClusterClient::shouldAutoRefresh() const noexcept
    {
        if (!m_slot_cache_ready) {
            return true;
        }
        const auto now = std::chrono::steady_clock::now();
        return (now - m_last_refresh_time) >= m_auto_refresh_interval;
    }

    Coroutine RedisClusterClient::refreshSlotsCoroutine()
    {
        auto notify_and_finish = [this](RedisCommandResult result) mutable {
            auto waiters = std::move(m_slots_refresh_waiters);
            m_slots_refresh_waiters.clear();
            m_slots_refresh_inflight = false;
            size_t last = waiters.size();
            while (last > 0 && !waiters[last - 1]) {
                --last;
            }
            for (size_t i = 0; i < last; ++i) {
                if (!waiters[i]) {
                    continue;
                }
                if (i + 1 == last) {
                    waiters[i]->notify(std::move(result));
                } else {
                    auto cloned_result = cloneRedisCommandResult(result);
                    waiters[i]->notify(std::move(cloned_result));
                }
            }
        };

        if (m_nodes.empty()) {
            notify_and_finish(std::unexpected(RedisError(REDIS_ERROR_TYPE_CONNECTION_ERROR, "No cluster node configured")));
            co_return;
        }

        ClusterNode* seed = nullptr;
        for (auto& node : m_nodes) {
            if (node.client) {
                seed = &node;
                break;
            }
        }
        if (!seed) {
            notify_and_finish(std::unexpected(RedisError(REDIS_ERROR_TYPE_CONNECTION_ERROR, "No valid cluster client")));
            co_return;
        }

        if (!seed->connected) {
            auto connect_result = co_await seed->client->connect(seed->address.host,
                                                                 seed->address.port,
                                                                 seed->address.username,
                                                                 seed->address.password,
                                                                 seed->address.db_index,
                                                                 seed->address.version);
            if (!connect_result) {
                notify_and_finish(std::unexpected(connect_result.error()));
                co_return;
            }
            seed->connected = true;
        }

        auto slots_result = co_await seed->client->clusterSlots();
        if (!slots_result) {
            notify_and_finish(std::unexpected(slots_result.error()));
            co_return;
        }
        if (!slots_result.value().has_value()) {
            notify_and_finish(std::unexpected(RedisError(REDIS_ERROR_TYPE_PARSE_ERROR,
                                                         "CLUSTER SLOTS returned empty payload")));
            co_return;
        }

        std::string parse_error;
        auto& slots_values = slots_result.value().value();
        auto values = std::move(slots_values);
        if (!applyClusterSlots(values, &parse_error)) {
            notify_and_finish(std::unexpected(RedisError(REDIS_ERROR_TYPE_PARSE_ERROR, parse_error)));
            co_return;
        }

        notify_and_finish(std::move(values));
        co_return;
    }

    Coroutine RedisClusterClient::executeAutoCoroutine(
        std::string routing_key,
        std::string cmd,
        std::vector<std::string> args,
        bool force_key_routing,
        std::shared_ptr<galay::kernel::AsyncWaiter<RedisCommandResult>> waiter)
    {
        if (m_nodes.empty()) {
            waiter->notify(std::unexpected(RedisError(REDIS_ERROR_TYPE_CONNECTION_ERROR, "No cluster node configured")));
            co_return;
        }

        if (shouldAutoRefresh()) {
            auto refresh_result = co_await refreshSlots();
            if (!refresh_result) {
                // 刷新失败时，继续使用本地缓存做一次最佳努力路由
            }
        }

        std::vector<std::string> cmd_parts;
        cmd_parts.reserve(1 + args.size());
        cmd_parts.push_back(cmd);
        cmd_parts.insert(cmd_parts.end(), args.begin(), args.end());

        for (int attempt = 0; attempt < 5; ++attempt) {
            ClusterNode* target = nullptr;
            if (force_key_routing && !routing_key.empty()) {
                target = chooseNodeHandleByKey(routing_key);
            } else if (!args.empty()) {
                target = chooseNodeHandleByKey(args.front());
            } else {
                target = chooseNodeHandleBySlot(0);
            }

            if (!target || !target->client) {
                waiter->notify(std::unexpected(RedisError(REDIS_ERROR_TYPE_CONNECTION_ERROR, "No target cluster node")));
                co_return;
            }

            if (!target->connected) {
                auto connect_result = co_await target->client->connect(target->address.host,
                                                                       target->address.port,
                                                                       target->address.username,
                                                                       target->address.password,
                                                                       target->address.db_index,
                                                                       target->address.version);
                if (!connect_result) {
                    waiter->notify(std::unexpected(connect_result.error()));
                    co_return;
                }
                target->connected = true;
            }

            auto exec_result = co_await target->client->execute(cmd, args);
            if (!exec_result) {
                waiter->notify(std::unexpected(exec_result.error()));
                co_return;
            }
            if (!exec_result.value().has_value() || exec_result.value()->empty()) {
                waiter->notify(std::unexpected(RedisError(REDIS_ERROR_TYPE_PARSE_ERROR,
                                                          "Cluster command returned empty payload")));
                co_return;
            }

            auto& exec_values = exec_result.value().value();
            auto values = std::move(exec_values);
            const auto redirect = parseRedirect(values.front());
            if (!redirect.has_value()) {
                waiter->notify(std::move(values));
                co_return;
            }

            auto* redirect_node = findOrCreateNode(redirect->host, redirect->port);
            if (!redirect_node || !redirect_node->client) {
                waiter->notify(std::unexpected(RedisError(REDIS_ERROR_TYPE_CONNECTION_ERROR,
                                                          "Redirect target node unavailable")));
                co_return;
            }

            if (!redirect_node->connected) {
                auto connect_result = co_await redirect_node->client->connect(redirect_node->address.host,
                                                                              redirect_node->address.port,
                                                                              redirect_node->address.username,
                                                                              redirect_node->address.password,
                                                                              redirect_node->address.db_index,
                                                                              redirect_node->address.version);
                if (!connect_result) {
                    waiter->notify(std::unexpected(connect_result.error()));
                    co_return;
                }
                redirect_node->connected = true;
            }

            if (redirect->type == RedirectInfo::Type::Moved) {
                m_slot_owner[redirect->slot] = static_cast<int>(redirect_node - m_nodes.data());
                auto refresh_result = co_await refreshSlots();
                (void)refresh_result;
                continue;
            }

            if (redirect->type == RedirectInfo::Type::Ask) {
                auto asking_result = co_await redirect_node->client->pipeline({{"ASKING"}, cmd_parts});
                if (!asking_result) {
                    waiter->notify(std::unexpected(asking_result.error()));
                    co_return;
                }
                if (!asking_result.value().has_value() || asking_result.value()->size() < 2) {
                    waiter->notify(std::unexpected(RedisError(REDIS_ERROR_TYPE_PARSE_ERROR,
                                                              "ASK redirect response invalid")));
                    co_return;
                }

                auto& ask_values_ref = asking_result.value().value();
                auto ask_values = std::move(ask_values_ref);
                std::vector<RedisValue> final_values;
                final_values.push_back(std::move(ask_values[1]));
                const auto chained_redirect = parseRedirect(final_values.front());
                if (chained_redirect.has_value()) {
                    if (chained_redirect->type == RedirectInfo::Type::Moved) {
                        m_slot_owner[chained_redirect->slot] = static_cast<int>(redirect_node - m_nodes.data());
                    }
                    continue;
                }
                waiter->notify(std::move(final_values));
                co_return;
            }
        }

        waiter->notify(std::unexpected(RedisError(REDIS_ERROR_TYPE_COMMAND_ERROR,
                                                  "Exceeded redirect retry limit")));
    }

    uint16_t RedisClusterClient::crc16(const uint8_t* data, size_t len)
    {
        uint16_t crc = 0;
        for (size_t i = 0; i < len; ++i) {
            crc ^= static_cast<uint16_t>(data[i]) << 8;
            for (int j = 0; j < 8; ++j) {
                if ((crc & 0x8000) != 0) {
                    crc = static_cast<uint16_t>((crc << 1) ^ 0x1021);
                } else {
                    crc = static_cast<uint16_t>(crc << 1);
                }
            }
        }
        return crc;
    }

    std::string RedisClusterClient::extractHashTag(const std::string& key)
    {
        const auto left = key.find('{');
        if (left == std::string::npos) {
            return key;
        }

        const auto right = key.find('}', left + 1);
        if (right == std::string::npos || right == left + 1) {
            return key;
        }

        return key.substr(left + 1, right - left - 1);
    }
}
