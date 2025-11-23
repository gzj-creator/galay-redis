#ifndef GALAY_REDIS_BASE_H
#define GALAY_REDIS_BASE_H

#include <concepts>
#include <string>

namespace galay::redis 
{
    template <typename T>
    concept KVPair = requires(T type)
    {
        std::is_same_v<T, std::pair<std::string, std::string>>;
    };

    template <typename T>
    concept KeyType = requires(T type)
    {
        std::is_same_v<T, std::string>;
    };

    template <typename T>
    concept ValType = requires(T type)
    {
        std::is_same_v<T, std::string>;
    };

    template <typename T>
    concept ScoreValType = requires(T type)
    {
        std::is_same_v<T, std::pair<double, std::string>>;
    };

}


#endif