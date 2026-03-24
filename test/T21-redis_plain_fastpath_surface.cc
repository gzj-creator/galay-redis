#include <chrono>
#include <concepts>
#include <string_view>

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
}

int main()
{
    return 0;
}
