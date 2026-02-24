# Benchmark

目录结构：

- `B1-RedisClientBench.cc`：RedisClient 普通/Pipeline 压测
- `B2-ConnectionPoolBench.cc`：连接池并发压测

构建：

```bash
cmake -S . -B build -DBUILD_BENCHMARKS=ON
cmake --build build --parallel
```

运行：

```bash
./build/benchmark/B1-RedisClientBench -h 127.0.0.1 -p 6379 -c 10 -n 100 -m normal
./build/benchmark/B1-RedisClientBench -h 127.0.0.1 -p 6379 -c 20 -n 5000 -m pipeline -b 100 -q
./build/benchmark/B2-ConnectionPoolBench -h 127.0.0.1 -p 6379 -c 20 -n 300 -m 4 -x 20 -q
```
