#ifndef GALAY_REDIS_LOG_H
#define GALAY_REDIS_LOG_H


#include <galay/common/Log.h>

namespace galay::redis
{
    #define RedisLogTrace(logger, ...)       SPDLOG_LOGGER_TRACE(logger, __VA_ARGS__)
    #define RedisLogDebug(logger, ...)       SPDLOG_LOGGER_DEBUG(logger, __VA_ARGS__)
    #define RedisLogInfo(logger, ...)        SPDLOG_LOGGER_INFO(logger, __VA_ARGS__)
    #define RedisLogWarn(logger, ...)        SPDLOG_LOGGER_WARN(logger, __VA_ARGS__)
    #define RedisLogError(logger, ...)       SPDLOG_LOGGER_ERROR(logger, __VA_ARGS__)
    #define RedisLogCritical(logger, ...)    SPDLOG_LOGGER_CRITICAL(logger, __VA_ARGS__)

}

#endif