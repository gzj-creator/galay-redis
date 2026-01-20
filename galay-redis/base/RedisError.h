#ifndef GALAY_REDIS_ERROR_H
#define GALAY_REDIS_ERROR_H

#include <string>

namespace galay::redis 
{
    enum RedisErrorType
    {
        REDIS_ERROR_TYPE_SUCCESS,                   //成功
        REDIS_ERROR_TYPE_URL_INVALID_ERROR,         //url无效（不符合正则）
        REDIS_ERROR_TYPE_HOST_INVALID_ERROR,        //host无效
        REDIS_ERROR_TYPE_PORT_INVALID_ERROR,        //port无效
        REDIS_ERROR_TYPE_DB_INDEX_INVALID_ERROR,    //db_index无效
        REDIS_ERROR_TYPE_ADDRESS_TYPE_INVALID_ERROR, //地址类型无效
        REDIS_ERROR_TYPE_VERSION_INVALID_ERROR,     //version无效
        REDIS_ERROR_TYPE_CONNECTION_ERROR,          //连接错误
        REDIS_ERROR_TYPE_FREE_REDISOBJ_ERROR,       //释放redisContext对象失败

        REDIS_ERROR_TYPE_COMMAND_ERROR,
        REDIS_ERROR_TYPE_TIMEOUT_ERROR,
        REDIS_ERROR_TYPE_AUTH_ERROR,
        REDIS_ERROR_TYPE_INVALID_ERROR,
        REDIS_ERROR_TYPE_UNKNOWN_ERROR,
        REDIS_ERROR_TYPE_PARSE_ERROR,               //协议解析错误
        REDIS_ERROR_TYPE_SEND_ERROR,                //发送数据错误
        REDIS_ERROR_TYPE_RECV_ERROR,                //接收数据错误
        REDIS_ERROR_TYPE_BUFFER_OVERFLOW_ERROR,     //缓冲区溢出
        REDIS_ERROR_TYPE_NETWORK_ERROR,             //网络错误
        REDIS_ERROR_TYPE_CONNECTION_CLOSED,         //连接已关闭
        REDIS_ERROR_TYPE_INTERNAL_ERROR,            //内部错误
    };

    // 为了兼容性，提供别名
    using RedisErrorCode = RedisErrorType;

    // 便捷的错误码常量
    constexpr RedisErrorType NetworkError = REDIS_ERROR_TYPE_NETWORK_ERROR;
    constexpr RedisErrorType ConnectionClosed = REDIS_ERROR_TYPE_CONNECTION_CLOSED;


    class RedisError
    {
    public:
        RedisError(RedisErrorType type);
        RedisError(RedisErrorType type, std::string extra_msg);
        RedisErrorType type() const;
        std::string message() const;
    private:
        RedisErrorType m_type;
        std::string m_extra_msg;
    };
}
#endif