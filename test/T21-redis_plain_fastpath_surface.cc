#include <chrono>
#include <concepts>
#include <string_view>
#include <type_traits>

#include "async/RedisClient.h"

using namespace galay::redis;

namespace
{
    template <typename T>
    concept HasBorrowedPlainFastPath = requires(T& client,
                                                RedisBorrowedCommand packet,
                                                std::string_view encoded) {
        { client.commandBorrowed(packet) } -> std::same_as<RedisExchangeOperation>;
        { client.commandBorrowed(packet).timeout(std::chrono::milliseconds(1)) };
        { client.batchBorrowed(encoded, size_t{2}) } -> std::same_as<RedisExchangeOperation>;
        { client.batchBorrowed(encoded, size_t{2}).timeout(std::chrono::milliseconds(1)) };
    };

    static_assert(HasBorrowedPlainFastPath<RedisClient>);
    static_assert(std::is_same_v<
                  decltype(&RedisClient::commandBorrowed),
                  RedisExchangeOperation (RedisClient::*)(RedisBorrowedCommand)>);
    static_assert(std::is_same_v<
                  decltype(&RedisClient::batchBorrowed),
                  RedisExchangeOperation (RedisClient::*)(std::string_view, size_t)>);
}

int main()
{
    return 0;
}
