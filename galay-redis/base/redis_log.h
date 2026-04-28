#ifndef GALAY_REDIS_LOG_H
#define GALAY_REDIS_LOG_H

#include <memory>
#include <mutex>
#include <string>

#include <spdlog/spdlog.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

namespace galay::redis
{
    using RedisLoggerPtr = std::shared_ptr<spdlog::logger>;

    class RedisLog
    {
    public:
        static RedisLog* getInstance()
        {
            static RedisLog instance;
            return &instance;
        }

        static void enable()
        {
            console();
        }

        static void console()
        {
            console("RedisLogger");
        }

        static void console(const std::string& logger_name)
        {
            auto instance = getInstance();
            std::lock_guard<std::mutex> lock(instance->m_mutex);
            try {
                auto logger = spdlog::get(logger_name);
                if (!logger) {
                    logger = spdlog::stdout_color_mt(logger_name);
                }
                applyDefault(logger);
                instance->m_logger = std::move(logger);
            } catch (const spdlog::spdlog_ex&) {
                instance->m_logger = spdlog::get(logger_name);
            }
        }

        static void file(const std::string& log_file_path = "galay-redis.log",
                         const std::string& logger_name = "RedisLogger",
                         bool truncate = false)
        {
            auto instance = getInstance();
            std::lock_guard<std::mutex> lock(instance->m_mutex);
            auto logger = std::make_shared<spdlog::logger>(
                logger_name,
                std::make_shared<spdlog::sinks::basic_file_sink_mt>(log_file_path, !truncate));
            applyDefault(logger);
            instance->m_logger = std::move(logger);
        }

        static void disable()
        {
            auto instance = getInstance();
            std::lock_guard<std::mutex> lock(instance->m_mutex);
            if (instance->m_logger) {
                instance->m_logger->set_level(spdlog::level::off);
            }
            instance->m_logger.reset();
        }

        static void setLogger(RedisLoggerPtr logger)
        {
            auto instance = getInstance();
            std::lock_guard<std::mutex> lock(instance->m_mutex);
            instance->m_logger = std::move(logger);
        }

        RedisLoggerPtr getLogger() const
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            return m_logger;
        }

    private:
        static void applyDefault(const RedisLoggerPtr& logger)
        {
            if (!logger) {
                return;
            }
            logger->set_pattern("[%Y-%m-%d %T.%e] [%^%L%$] [%s:%#] %v");
#ifdef ENABLE_DEBUG
            logger->set_level(spdlog::level::debug);
#else
            logger->set_level(spdlog::level::info);
#endif
        }

        mutable std::mutex m_mutex;
        RedisLoggerPtr m_logger;
    };

    namespace detail
    {
        inline RedisLoggerPtr resolveLogger(const RedisLoggerPtr& logger)
        {
            if (logger) {
                return logger;
            }
            return RedisLog::getInstance()->getLogger();
        }
    } // namespace detail

    #define RedisLogTrace(logger, ...) \
        do { auto _logger = ::galay::redis::detail::resolveLogger((logger)); if (_logger) SPDLOG_LOGGER_TRACE(_logger, __VA_ARGS__); } while (0)
    #define RedisLogDebug(logger, ...) \
        do { auto _logger = ::galay::redis::detail::resolveLogger((logger)); if (_logger) SPDLOG_LOGGER_DEBUG(_logger, __VA_ARGS__); } while (0)
    #define RedisLogInfo(logger, ...) \
        do { auto _logger = ::galay::redis::detail::resolveLogger((logger)); if (_logger) SPDLOG_LOGGER_INFO(_logger, __VA_ARGS__); } while (0)
    #define RedisLogWarn(logger, ...) \
        do { auto _logger = ::galay::redis::detail::resolveLogger((logger)); if (_logger) SPDLOG_LOGGER_WARN(_logger, __VA_ARGS__); } while (0)
    #define RedisLogError(logger, ...) \
        do { auto _logger = ::galay::redis::detail::resolveLogger((logger)); if (_logger) SPDLOG_LOGGER_ERROR(_logger, __VA_ARGS__); } while (0)
    #define RedisLogCritical(logger, ...) \
        do { auto _logger = ::galay::redis::detail::resolveLogger((logger)); if (_logger) SPDLOG_LOGGER_CRITICAL(_logger, __VA_ARGS__); } while (0)

}

#endif
