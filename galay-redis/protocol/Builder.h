#ifndef GALAY_REDIS_BUILDER_H
#define GALAY_REDIS_BUILDER_H

#include "RedisProtocol.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace galay::redis
{
    struct RedisCommandView
    {
        std::string_view command;
        std::span<const std::string_view> args;
        // Optional pre-encoded RESP command bytes for fast pipeline/batch send path.
        std::string_view encoded;
    };

    struct RedisEncodedCommand
    {
        std::string encoded;
        size_t expected_replies = 1;
    };

    class RedisCommandBuilder
    {
    public:
        RedisCommandBuilder() = default;

        void clear() noexcept;

        // Batch reserve: command count, argument count, storage bytes.
        void reserve(size_t command_count, size_t arg_count, size_t storage_bytes);

        // Batch command view API.
        RedisCommandBuilder& append(std::string_view cmd);
        RedisCommandBuilder& append(std::string_view cmd,
                                    std::span<const std::string_view> args);
        RedisCommandBuilder& append(std::string_view cmd,
                                    std::initializer_list<std::string_view> args);
        template <size_t N>
        RedisCommandBuilder& append(std::string_view cmd,
                                    const std::array<std::string_view, N>& args);

        [[nodiscard]] std::span<const RedisCommandView> commands() const;
        [[nodiscard]] size_t size() const noexcept;

        [[nodiscard]] const std::string& encoded() const noexcept;
        [[nodiscard]] bool empty() const noexcept;

        [[nodiscard]] RedisEncodedCommand build() const;
        [[nodiscard]] RedisEncodedCommand release();

        [[nodiscard]] RedisEncodedCommand command(std::string_view cmd,
                                                  size_t expected_replies = 1) const;
        [[nodiscard]] RedisEncodedCommand command(std::string_view cmd,
                                                  std::span<const std::string_view> args,
                                                  size_t expected_replies = 1) const;
        [[nodiscard]] RedisEncodedCommand command(std::string_view cmd,
                                                  std::initializer_list<std::string_view> args,
                                                  size_t expected_replies = 1) const;
        template <size_t N>
        [[nodiscard]] RedisEncodedCommand command(
            std::string_view cmd,
            const std::array<std::string_view, N>& args,
            size_t expected_replies = 1) const;
        [[nodiscard]] RedisEncodedCommand command(std::string_view cmd,
                                                  const std::vector<std::string>& args,
                                                  size_t expected_replies = 1) const;

        // ======================== 连接命令 ========================
        [[nodiscard]] RedisEncodedCommand auth(const std::string& password) const;
        [[nodiscard]] RedisEncodedCommand auth(const std::string& username,
                                               const std::string& password) const;
        [[nodiscard]] RedisEncodedCommand select(int32_t db_index) const;
        [[nodiscard]] RedisEncodedCommand ping() const;
        [[nodiscard]] RedisEncodedCommand echo(const std::string& message) const;

        // ======================== 发布订阅 ========================
        [[nodiscard]] RedisEncodedCommand publish(const std::string& channel,
                                                  const std::string& message) const;
        [[nodiscard]] RedisEncodedCommand subscribe(const std::string& channel) const;
        [[nodiscard]] RedisEncodedCommand subscribe(const std::vector<std::string>& channels) const;
        [[nodiscard]] RedisEncodedCommand unsubscribe(const std::string& channel) const;
        [[nodiscard]] RedisEncodedCommand unsubscribe(const std::vector<std::string>& channels) const;
        [[nodiscard]] RedisEncodedCommand psubscribe(const std::string& pattern) const;
        [[nodiscard]] RedisEncodedCommand psubscribe(const std::vector<std::string>& patterns) const;
        [[nodiscard]] RedisEncodedCommand punsubscribe(const std::string& pattern) const;
        [[nodiscard]] RedisEncodedCommand punsubscribe(const std::vector<std::string>& patterns) const;

        // ======================== 集群/主从命令 ========================
        [[nodiscard]] RedisEncodedCommand role() const;
        [[nodiscard]] RedisEncodedCommand replicaof(const std::string& host, int32_t port) const;
        [[nodiscard]] RedisEncodedCommand readonly() const;
        [[nodiscard]] RedisEncodedCommand readwrite() const;
        [[nodiscard]] RedisEncodedCommand clusterInfo() const;
        [[nodiscard]] RedisEncodedCommand clusterNodes() const;
        [[nodiscard]] RedisEncodedCommand clusterSlots() const;

        // ======================== String操作 ========================
        [[nodiscard]] RedisEncodedCommand get(const std::string& key) const;
        [[nodiscard]] RedisEncodedCommand set(const std::string& key,
                                              const std::string& value) const;
        [[nodiscard]] RedisEncodedCommand setex(const std::string& key,
                                                int64_t seconds,
                                                const std::string& value) const;
        [[nodiscard]] RedisEncodedCommand del(const std::string& key) const;
        [[nodiscard]] RedisEncodedCommand exists(const std::string& key) const;
        [[nodiscard]] RedisEncodedCommand incr(const std::string& key) const;
        [[nodiscard]] RedisEncodedCommand decr(const std::string& key) const;

        // ======================== Hash操作 ========================
        [[nodiscard]] RedisEncodedCommand hget(const std::string& key,
                                               const std::string& field) const;
        [[nodiscard]] RedisEncodedCommand hset(const std::string& key,
                                               const std::string& field,
                                               const std::string& value) const;
        [[nodiscard]] RedisEncodedCommand hdel(const std::string& key,
                                               const std::string& field) const;
        [[nodiscard]] RedisEncodedCommand hgetAll(const std::string& key) const;

        // ======================== List操作 ========================
        [[nodiscard]] RedisEncodedCommand lpush(const std::string& key,
                                                const std::string& value) const;
        [[nodiscard]] RedisEncodedCommand rpush(const std::string& key,
                                                const std::string& value) const;
        [[nodiscard]] RedisEncodedCommand lpop(const std::string& key) const;
        [[nodiscard]] RedisEncodedCommand rpop(const std::string& key) const;
        [[nodiscard]] RedisEncodedCommand llen(const std::string& key) const;
        [[nodiscard]] RedisEncodedCommand lrange(const std::string& key,
                                                 int64_t start,
                                                 int64_t stop) const;

        // ======================== Set操作 ========================
        [[nodiscard]] RedisEncodedCommand sadd(const std::string& key,
                                               const std::string& member) const;
        [[nodiscard]] RedisEncodedCommand srem(const std::string& key,
                                               const std::string& member) const;
        [[nodiscard]] RedisEncodedCommand smembers(const std::string& key) const;
        [[nodiscard]] RedisEncodedCommand scard(const std::string& key) const;

        // ======================== Sorted Set操作 ========================
        [[nodiscard]] RedisEncodedCommand zadd(const std::string& key,
                                               double score,
                                               const std::string& member) const;
        [[nodiscard]] RedisEncodedCommand zrem(const std::string& key,
                                               const std::string& member) const;
        [[nodiscard]] RedisEncodedCommand zrange(const std::string& key,
                                                 int64_t start,
                                                 int64_t stop) const;
        [[nodiscard]] RedisEncodedCommand zscore(const std::string& key,
                                                 const std::string& member) const;

    private:
        struct Slice
        {
            size_t offset = 0;
            size_t length = 0;
        };

        struct CommandMeta
        {
            Slice command;
            size_t arg_offset = 0;
            size_t arg_count = 0;
            Slice encoded;
        };

        static size_t normalizeExpectedReplies(size_t expected_replies) noexcept;

        Slice appendToStorage(std::string_view value);
        [[nodiscard]] std::string_view toView(Slice slice) const;
        [[nodiscard]] std::string_view toEncodedView(Slice slice) const;
        void rebuildViewsIfNeeded() const;

        protocol::RespEncoder m_encoder;
        std::string m_encoded;

        std::string m_storage;
        std::vector<Slice> m_arg_slices;
        std::vector<CommandMeta> m_commands;
        mutable std::vector<std::string_view> m_arg_views;
        mutable std::vector<RedisCommandView> m_command_views;
        mutable bool m_views_dirty = true;
    };

    template <size_t N>
    RedisCommandBuilder& RedisCommandBuilder::append(
        std::string_view cmd,
        const std::array<std::string_view, N>& args)
    {
        return append(cmd, std::span<const std::string_view>(args));
    }

    template <size_t N>
    RedisEncodedCommand RedisCommandBuilder::command(
        std::string_view cmd,
        const std::array<std::string_view, N>& args,
        size_t expected_replies) const
    {
        return command(cmd, std::span<const std::string_view>(args), expected_replies);
    }
} // namespace galay::redis

#endif // GALAY_REDIS_BUILDER_H
