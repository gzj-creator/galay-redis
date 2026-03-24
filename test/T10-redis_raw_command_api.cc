#include <chrono>
#include <concepts>
#include <memory>
#include <utility>

#include "async/RedisClient.h"

using namespace galay::redis;

namespace
{
    template <typename T>
    concept HasRawCommandApi = requires(T& client, RedisEncodedCommand command) {
        { client.command(std::move(command)) } -> std::same_as<RedisExchangeOperation>;
        { client.command(std::move(command)).timeout(std::chrono::milliseconds(200)) };
    };

    template <typename T>
    concept BuilderAcceptsBufferProvider =
        requires(T& builder, std::shared_ptr<RedisBufferProvider> provider) {
            { builder.bufferProvider(std::move(provider)) } -> std::same_as<RedisClientBuilder&>;
        };

    static_assert(HasRawCommandApi<RedisClient>);
    static_assert(BuilderAcceptsBufferProvider<RedisClientBuilder>);
}

int main()
{
    return 0;
}
