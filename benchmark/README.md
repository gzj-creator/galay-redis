# Benchmark

目录结构：

- `B1-RedisClientBench.cc`：RedisClient 普通/Pipeline/Batch 压测
- `B2-ConnectionPoolBench.cc`：连接池并发压测

构建：

```bash
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release -DBUILD_BENCHMARKS=ON
cmake --build build-release --parallel
```

## B1 参数

```bash
./build-release/benchmark/B1-RedisClientBench \
  [-h host] [-p port] [-c clients] [-n operations] \
  [-m normal|normal-batch|pipeline] [-b batch_size] \
  [--timeout-ms -1|N] [--buffer-size bytes] [--track-alloc] [-q]
```

- `--timeout-ms`: `-1` 表示不启用请求超时；`N>=0` 表示所有请求统一使用该超时。
- `--buffer-size`: 客户端 ring-buffer 大小，跨客户端对比时必须保持一致。
- `--track-alloc`: 启用分配统计（会引入额外开销，建议只在 allocation 分析时开启）。

B1 输出包含：
- `Ops/sec`（吞吐）
- `Request latency p50/p99`（单次请求/批次调用延迟）
- `Alloc calls/op`、`Alloc bytes/op`（仅在 `--track-alloc` 时输出）

## 控制变量建议（跨语言公平对比）

1. timeout 两边要么都开、要么都关（推荐统一关闭：`--timeout-ms -1`）。  
2. buffer size 一致（例如统一 `32768` 或统一 `65536`）。  
3. batch size、clients、operations 一致。  
4. 每组至少跑 3 次，取中位数。  

示例（pipeline，无 timeout）：

```bash
./build-release/benchmark/B1-RedisClientBench \
  -h 127.0.0.1 -p 6379 -c 10 -n 50000 -m pipeline -b 100 \
  --timeout-ms -1 --buffer-size 32768 -q
```

示例（normal-batch，开启分配统计）：

```bash
./build-release/benchmark/B1-RedisClientBench \
  -h 127.0.0.1 -p 6379 -c 10 -n 50000 -m normal-batch -b 100 \
  --timeout-ms -1 --buffer-size 32768 --track-alloc -q
```

B2 示例：

```bash
./build-release/benchmark/B2-ConnectionPoolBench -h 127.0.0.1 -p 6379 -c 20 -n 300 -m 4 -x 20 -q
```
