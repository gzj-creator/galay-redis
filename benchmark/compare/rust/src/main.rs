use redis::{Commands, RedisResult};
use std::sync::Arc;
use std::sync::atomic::{AtomicU64, Ordering};
use std::thread;
use std::time::Instant;

#[derive(Clone, Debug)]
struct Options {
    host: String,
    port: u16,
    clients: usize,
    operations: usize,
    mode: String,
    batch_size: usize,
}

fn parse_u16(value: &str) -> Result<u16, String> {
    value.parse::<u16>().map_err(|_| format!("invalid u16: {value}"))
}

fn parse_usize(value: &str) -> Result<usize, String> {
    value
        .parse::<usize>()
        .map_err(|_| format!("invalid usize: {value}"))
}

fn parse_args() -> Result<Options, String> {
    let mut opts = Options {
        host: "127.0.0.1".to_string(),
        port: 6379,
        clients: 10,
        operations: 1000,
        mode: "normal".to_string(),
        batch_size: 100,
    };

    let mut args = std::env::args().skip(1);
    while let Some(arg) = args.next() {
        match arg.as_str() {
            "-h" | "--host" => {
                let v = args.next().ok_or("missing value for --host")?;
                opts.host = v;
            }
            "-p" | "--port" => {
                let v = args.next().ok_or("missing value for --port")?;
                opts.port = parse_u16(&v)?;
            }
            "-c" | "--clients" => {
                let v = args.next().ok_or("missing value for --clients")?;
                opts.clients = parse_usize(&v)?;
            }
            "-n" | "--operations" => {
                let v = args.next().ok_or("missing value for --operations")?;
                opts.operations = parse_usize(&v)?;
            }
            "-m" | "--mode" => {
                let v = args.next().ok_or("missing value for --mode")?;
                opts.mode = v;
            }
            "-b" | "--batch-size" => {
                let v = args.next().ok_or("missing value for --batch-size")?;
                opts.batch_size = parse_usize(&v)?;
            }
            "--help" => {
                return Err("help".to_string());
            }
            _ => {
                return Err(format!("unknown argument: {arg}"));
            }
        }
    }

    if opts.clients == 0 {
        return Err("clients must be > 0".to_string());
    }
    if opts.operations == 0 {
        return Err("operations must be > 0".to_string());
    }
    if opts.batch_size == 0 {
        return Err("batch-size must be > 0".to_string());
    }
    if opts.mode != "normal" && opts.mode != "pipeline" {
        return Err("mode must be normal or pipeline".to_string());
    }

    Ok(opts)
}

fn print_usage(program: &str) {
    println!(
        "Usage: {program} [-h host] [-p port] [-c clients] [-n operations] [-m normal|pipeline] [-b batch_size]"
    );
}

fn run_normal(
    conn: &mut redis::Connection,
    client_id: usize,
    operations: usize,
) -> RedisResult<(u64, u64)> {
    let mut success = 0_u64;
    let mut error = 0_u64;
    for i in 0..operations {
        let key = format!("rust:bench:{client_id}:{i}");
        let value = format!("value_{i}");
        let set_ok: RedisResult<()> = conn.set(&key, &value);
        match set_ok {
            Ok(_) => success += 1,
            Err(_) => {
                error += 1;
                continue;
            }
        }
        let get_ok: RedisResult<String> = conn.get(&key);
        match get_ok {
            Ok(_) => success += 1,
            Err(_) => error += 1,
        }
    }
    Ok((success, error))
}

fn run_pipeline(
    conn: &mut redis::Connection,
    client_id: usize,
    operations: usize,
    batch_size: usize,
) -> RedisResult<(u64, u64)> {
    let mut success = 0_u64;
    let mut error = 0_u64;
    let mut start = 0usize;

    while start < operations {
        let end = (start + batch_size).min(operations);
        let mut pipe = redis::pipe();
        for i in start..end {
            let key = format!("rust:bench:{client_id}:{i}");
            let value = format!("value_{i}");
            pipe.cmd("SET").arg(&key).arg(&value).ignore();
            pipe.cmd("GET").arg(&key).ignore();
        }

        let batch_len = (end - start) as u64;
        let result: RedisResult<()> = pipe.query(conn);
        if result.is_ok() {
            success += batch_len * 2;
        } else {
            error += batch_len * 2;
        }
        start = end;
    }
    Ok((success, error))
}

fn main() {
    let program = std::env::args()
        .next()
        .unwrap_or_else(|| "rust_redis_bench".to_string());
    let options = match parse_args() {
        Ok(v) => v,
        Err(e) if e == "help" => {
            print_usage(&program);
            return;
        }
        Err(e) => {
            eprintln!("{e}");
            print_usage(&program);
            std::process::exit(1);
        }
    };

    let endpoint = format!("redis://{}:{}/", options.host, options.port);
    let success = Arc::new(AtomicU64::new(0));
    let error = Arc::new(AtomicU64::new(0));
    let timeout = Arc::new(AtomicU64::new(0));

    println!("==================================================");
    println!("Rust Redis Benchmark");
    println!("==================================================");
    println!("Host: {}:{}", options.host, options.port);
    println!("Clients: {}", options.clients);
    println!("Operations per client: {}", options.operations);
    println!("Mode: {}", options.mode);
    if options.mode == "pipeline" {
        println!("Batch size: {}", options.batch_size);
    }

    let begin = Instant::now();
    let mut handles = Vec::with_capacity(options.clients);
    for client_id in 0..options.clients {
        let endpoint = endpoint.clone();
        let options = options.clone();
        let success = Arc::clone(&success);
        let error = Arc::clone(&error);
        let timeout = Arc::clone(&timeout);
        handles.push(thread::spawn(move || {
            let client = match redis::Client::open(endpoint.as_str()) {
                Ok(v) => v,
                Err(_) => {
                    error.fetch_add(1, Ordering::Relaxed);
                    return;
                }
            };

            let mut conn = match client.get_connection() {
                Ok(v) => v,
                Err(_) => {
                    error.fetch_add(1, Ordering::Relaxed);
                    return;
                }
            };

            let run_result = if options.mode == "pipeline" {
                run_pipeline(&mut conn, client_id, options.operations, options.batch_size)
            } else {
                run_normal(&mut conn, client_id, options.operations)
            };

            match run_result {
                Ok((ok, err)) => {
                    success.fetch_add(ok, Ordering::Relaxed);
                    error.fetch_add(err, Ordering::Relaxed);
                }
                Err(_) => {
                    timeout.fetch_add(1, Ordering::Relaxed);
                }
            }
        }));
    }

    let mut completed = 0usize;
    for h in handles {
        if h.join().is_ok() {
            completed += 1;
        } else {
            error.fetch_add(1, Ordering::Relaxed);
        }
    }
    let elapsed_ms = begin.elapsed().as_millis() as f64;
    let success_val = success.load(Ordering::Relaxed);
    let error_val = error.load(Ordering::Relaxed);
    let timeout_val = timeout.load(Ordering::Relaxed);

    println!("Finished");
    println!("Completed clients: {}", completed);
    println!("Duration: {:.0} ms", elapsed_ms);
    println!("Success: {}", success_val);
    println!("Error: {}", error_val);
    println!("Timeout: {}", timeout_val);

    if elapsed_ms > 0.0 {
        let ops = (success_val as f64) * 1000.0 / elapsed_ms;
        println!("Ops/sec: {}", ops);
    }
}
