# Redis协议手撸实现说明

## 概述

本项目实现了一个完整的Redis RESP (REdis Serialization Protocol) 协议解析器和客户端，**不依赖hiredis库**。所有的协议解析和网络通信都是手工实现的。

## 目录结构

```
galay-redis/
├── protocol/                    # 新增的协议解析目录
│   ├── RedisProtocol.h         # RESP协议解析器和编码器头文件
│   ├── RedisProtocol.cc        # RESP协议解析器和编码器实现
│   ├── RedisClient.h           # Redis客户端头文件（基于原生socket）
│   ├── RedisClient.cc          # Redis客户端实现
│   └── README.md               # 详细使用文档
├── base/                        # 基础类
│   ├── RedisError.h            # 错误类型（已扩展支持协议解析错误）
│   └── ...
├── test/                        # 测试代码
│   ├── test_protocol.cc        # 协议解析测试
│   └── example_protocol.cc     # 使用示例
└── CMakeLists.txt              # 已更新，包含protocol目录
```

## 核心实现

### 1. RESP协议解析器 (RespParser)

**文件**: `galay-redis/protocol/RedisProtocol.h/cc`

实现了完整的RESP2和部分RESP3协议解析：

- **RESP2支持**:
  - Simple String (`+OK\r\n`)
  - Error (`-ERR message\r\n`)
  - Integer (`:1000\r\n`)
  - Bulk String (`$6\r\nfoobar\r\n`)
  - Array (`*2\r\n$3\r\nfoo\r\n$3\r\nbar\r\n`)
  - Null (`$-1\r\n`)

- **RESP3支持**:
  - Double (`,1.23\r\n`)
  - Boolean (`#t\r\n` / `#f\r\n`)
  - Map (`%2\r\n+key\r\n+val\r\n...`)
  - Set (`~2\r\n+item1\r\n+item2\r\n`)

**核心特性**:
- 零拷贝设计：直接在输入缓冲区解析，不做不必要的内存拷贝
- 增量解析：支持流式解析，可以处理不完整的数据包
- 递归解析：正确处理嵌套的Array和Map结构
- 错误处理：使用`std::expected`返回解析结果或错误

**关键方法**:
```cpp
std::expected<std::pair<size_t, RespValue>, ParseError>
    parse(const char* data, size_t length);
```
返回值：`pair<已解析字节数, 解析结果>`

### 2. RESP协议编码器 (RespEncoder)

**文件**: `galay-redis/protocol/RedisProtocol.h/cc`

实现了Redis命令和数据的编码：

- 编码各种RESP类型（字符串、整数、数组等）
- 专门的命令编码方法，将Redis命令转换为RESP格式
- 支持RESP3类型编码（Double、Boolean等）

**关键方法**:
```cpp
// 编码Redis命令
std::string encodeCommand(const std::string& cmd, const std::vector<std::string>& args);
std::string encodeCommand(const std::vector<std::string>& cmd_parts);

// 编码各种数据类型
std::string encodeBulkString(const std::string& str);
std::string encodeInteger(int64_t value);
std::string encodeArray(const std::vector<std::string>& elements);
```

### 3. Redis客户端 (RedisClient)

**文件**: `galay-redis/protocol/RedisClient.h/cc`

基于原生socket实现的Redis客户端，提供了：

**连接管理**:
- TCP连接（已实现）
- SSL连接（框架已就绪，待实现）
- 连接超时控制
- 自动认证和数据库选择

**命令执行**:
- 通用命令执行接口
- 常用命令的封装方法（GET、SET、HGET、LPUSH等）
- 完整的错误处理

**网络通信**:
- 基于原生POSIX socket API
- 非阻塞连接建立（支持超时）
- 流式接收和解析响应数据
- 缓冲区溢出保护

**关键类**:
```cpp
// Redis连接抽象基类
class RedisConnection {
    virtual std::expected<void, RedisError> connect(...) = 0;
    virtual std::expected<size_t, RedisError> send(...) = 0;
    virtual std::expected<size_t, RedisError> recv(...) = 0;
};

// TCP连接实现
class TcpConnection : public RedisConnection {
    // 使用原生socket实现
    int m_socket_fd;
};

// Redis客户端
class RedisClient {
    std::unique_ptr<RedisConnection> m_connection;
    RespParser m_parser;
    RespEncoder m_encoder;
};
```

### 4. 类型安全的值表示 (RespValue)

使用C++17的`std::variant`实现类型安全的Redis值表示：

```cpp
using RespData = std::variant<
    std::string,                                      // 字符串类型
    int64_t,                                          // 整数
    double,                                           // 双精度浮点
    bool,                                             // 布尔值
    std::vector<RespValue>,                           // 数组/集合
    std::vector<std::pair<RespValue, RespValue>>,     // 映射
    std::monostate                                    // Null
>;

class RespValue {
    RespType m_type;
    RespData m_data;

    // 类型判断
    bool isString() const;
    bool isInteger() const;
    // ...

    // 值获取
    std::string asString() const;
    int64_t asInteger() const;
    // ...
};
```

## 使用示例

### 基础使用

```cpp
#include "protocol/RedisClient.h"

using namespace galay::redis::protocol;

// 创建客户端
RedisClientConfig config;
config.host = "127.0.0.1";
config.port = 6379;

RedisClient client(config);

// 连接
auto result = client.connect();
if (!result) {
    std::cerr << "Error: " << result.error().message() << std::endl;
    return;
}

// 执行命令
auto set_result = client.set("key", "value");
auto get_result = client.get("key");

if (get_result && get_result->isBulkString()) {
    std::cout << "Value: " << get_result->asString() << std::endl;
}
```

### URL连接

```cpp
// 支持标准Redis URL格式
client.connect("redis://:password@localhost:6379/0");
```

### 自定义命令

```cpp
// 执行任意Redis命令
auto result = client.execute({"HGETALL", "user:1000"});
auto result2 = client.execute("INFO", {"SERVER"});
```

## 技术亮点

### 1. 现代C++特性

- **C++23标准**: 使用最新的C++23特性
- **std::expected**: 用于错误处理，替代异常
- **std::variant**: 用于类型安全的值表示
- **移动语义**: 充分利用移动语义减少拷贝
- **RAII**: 自动资源管理

### 2. 零拷贝设计

协议解析器直接在输入缓冲区上工作：
- 不创建中间字符串对象
- 使用指针和偏移量进行解析
- 只在必要时才拷贝数据

### 3. 增量解析

支持处理不完整的数据包：
```cpp
while (true) {
    auto result = parser.parse(buffer.data(), buffer.size());
    if (result) {
        return result->second;  // 解析成功
    } else if (result.error() == ParseError::Incomplete) {
        // 需要更多数据，继续接收
        recv_more_data();
    } else {
        // 解析错误
        return error;
    }
}
```

### 4. 类型安全

使用强类型系统：
- 编译时类型检查
- 不使用void*指针
- 明确的类型转换

### 5. 错误处理

使用`std::expected`进行错误处理：
```cpp
std::expected<RespValue, RedisError> get(const std::string& key);

// 使用
auto result = client.get("key");
if (result) {
    // 成功
    use_value(*result);
} else {
    // 失败
    handle_error(result.error());
}
```

## 测试

### 1. 协议解析测试 (test_protocol.cc)

测试内容：
- RESP2/RESP3各种类型的解析
- 协议编码
- 编码解码往返测试
- 客户端基本功能（可选，需要Redis服务器）

运行测试：
```bash
# 编译
cd build
cmake ..
make

# 运行离线测试
./test_protocol

# 运行在线测试（需要Redis服务器）
./test_protocol --online
```

### 2. 使用示例 (example_protocol.cc)

完整的使用示例，演示：
- 连接管理
- 基本命令（SET、GET、PING等）
- List操作
- Hash操作
- Set操作
- Sorted Set操作
- 自定义命令

运行示例：
```bash
./example_protocol
```

## 编译配置

CMakeLists.txt已更新：

```cmake
# 包含protocol目录的源文件
file(GLOB SOURCES "base/*.cc" "async/*.cc" "sync/*.cc" "protocol/*.cc")
```

## 性能特性

1. **内存效率**:
   - 零拷贝解析
   - 移动语义减少拷贝
   - 缓冲区复用

2. **解析效率**:
   - 单遍解析
   - 最小化字符串操作
   - 高效的整数解析

3. **网络效率**:
   - 批量发送命令
   - 流式接收响应
   - 非阻塞连接建立

## 与原有实现的对比

| 特性 | 原有实现(hiredis) | 新实现(protocol) |
|------|------------------|------------------|
| 依赖 | hiredis库 | 无依赖（仅标准库） |
| 协议解析 | hiredis | 手工实现 |
| 网络通信 | hiredis | 原生socket |
| RESP2支持 | ✅ | ✅ |
| RESP3支持 | ✅ | ⚠️ 部分支持 |
| SSL支持 | ✅ | ⚠️ 框架就绪 |
| 类型安全 | ❌ (void*) | ✅ (variant) |
| 错误处理 | redisReply* | std::expected |
| 异步支持 | ✅ | ❌ (待实现) |

## 待完善功能

- [ ] SSL/TLS连接实现
- [ ] 完整的RESP3协议支持（Blob Error, Verbatim String等）
- [ ] 连接池
- [ ] Pipeline支持
- [ ] Pub/Sub支持
- [ ] 事务支持（MULTI/EXEC）
- [ ] Lua脚本支持
- [ ] 异步客户端
- [ ] 集群支持
- [ ] Sentinel支持

## 总结

本实现提供了：

1. **完整的协议栈**: 从协议解析到网络通信的完整实现
2. **现代C++设计**: 使用C++23的现代特性
3. **高性能**: 零拷贝、移动语义、高效解析
4. **类型安全**: 强类型系统，编译时检查
5. **易用性**: 清晰的API设计，完善的错误处理
6. **可扩展性**: 良好的架构设计，易于扩展新功能

这个实现可以作为：
- hiredis的替代方案
- Redis协议的学习材料
- 高性能网络编程的示例
- 进一步开发的基础（如连接池、集群支持等）
