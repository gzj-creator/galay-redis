# Example

目录结构（参考 galay-rpc / galay-http）：

- `common/`：示例公共配置
- `include/`：头文件消费示例（E1~E3）
- `import/`：模块消费示例（E1~E3，只有模块工具链可用时才生成 target）

构建：

```bash
cmake -S . -B build -DBUILD_EXAMPLES=ON
cmake --build build --parallel
```

运行：

```bash
./build/examples/E1-async_basic_demo 127.0.0.1 6379
./build/examples/E2-pipeline_demo 127.0.0.1 6379 demo:pipeline: 20
./build/examples/E3-topology_pubsub_demo 127.0.0.1 6379
./build/examples/E1-async_basic_demo-import 127.0.0.1 6379
./build/examples/E2-pipeline_demo-import 127.0.0.1 6379 demo:pipeline: 20
./build/examples/E3-topology_pubsub_demo-import 127.0.0.1 6379
```

如果当前工具链不支持仓库的 C++23 模块路径，`*-import` 目标会在配置阶段被自动跳过。

## 示例与模块对齐

- `E1-async_basic_demo` ↔ `E1-async_basic_demo-import`
- `E2-pipeline_demo` ↔ `E2-pipeline_demo-import`
- `E3-topology_pubsub_demo` ↔ `E3-topology_pubsub_demo-import`
