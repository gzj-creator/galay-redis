# Example

目录结构（参考 galay-rpc / galay-http）：

- `common/`：示例公共配置
- `include/`：示例实现（E1~E3）

构建：

```bash
cmake -S . -B build -DBUILD_EXAMPLES=ON
cmake --build build --parallel
```

运行：

```bash
./build/example/E1-AsyncBasicDemo 127.0.0.1 6379
./build/example/E2-PipelineDemo 127.0.0.1 6379 demo:pipeline: 20
./build/example/E3-TopologyPubSubDemo 127.0.0.1 6379
```
