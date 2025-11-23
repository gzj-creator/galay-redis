#ifndef GALAY_REDIS_PROTOCOL_CONNECTION_H
#define GALAY_REDIS_PROTOCOL_CONNECTION_H

#include "RedisProtocol.h"
#include "../base/RedisError.h"
#include <string>
#include <expected>
#include <vector>
#include <cstdint>

namespace galay::redis::protocol
{
    // 简单的TCP连接封装，用于同步Redis操作
    class Connection
    {
    public:
        Connection();
        ~Connection();

        // 连接到服务器
        std::expected<void, RedisError> connect(const std::string& host, int port, uint32_t timeout_ms = 5000);

        // 断开连接
        void disconnect();

        // 检查连接状态
        bool isConnected() const { return m_connected; }

        // 发送数据
        std::expected<void, RedisError> send(const std::string& data);

        // 接收并解析Redis响应
        std::expected<RedisReply, RedisError> receiveReply();

        // 发送命令并接收响应（便捷方法）
        std::expected<RedisReply, RedisError> execute(const std::string& encoded_command);

    private:
        int m_socket_fd;
        bool m_connected;
        RespParser m_parser;
        std::vector<char> m_recv_buffer;
        static constexpr size_t BUFFER_SIZE = 8192;
    };
}

#endif
