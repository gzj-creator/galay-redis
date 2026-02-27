#ifndef GALAY_REDIS_BASE_H
#define GALAY_REDIS_BASE_H

#include <concepts>
#include <string>
#include <cstdint>

namespace galay::redis
{
    template <typename T>
    concept KVPair = std::same_as<T, std::pair<std::string, std::string>>;

    template <typename T>
    concept KeyType = std::same_as<T, std::string>;

    template <typename T>
    concept ValType = std::same_as<T, std::string> || std::same_as<T, int64_t> || std::same_as<T, double>;

    template <typename T>
    concept ScoreValType = std::same_as<T, std::pair<double, std::string>>;

}


#endif