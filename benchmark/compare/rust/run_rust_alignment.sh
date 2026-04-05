#!/bin/bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
RUST_DIR="${SCRIPT_DIR}"
PROJECT_DIR="$(cd "${SCRIPT_DIR}/../../../" && pwd)"

BUILD_DIR="${PROJECT_DIR}/build"
HOST="127.0.0.1"
PORT="6379"
CLIENTS="10"
OPERATIONS="1000"
MODE="normal"
BATCH_SIZE="100"

usage() {
    cat <<EOF
Usage: $0 [--build-dir dir] [--host host] [--port port] [--clients n] [--operations n] [--mode normal|pipeline] [--batch-size n]
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --build-dir) BUILD_DIR="$2"; shift 2 ;;
        --host) HOST="$2"; shift 2 ;;
        --port) PORT="$2"; shift 2 ;;
        --clients) CLIENTS="$2"; shift 2 ;;
        --operations) OPERATIONS="$2"; shift 2 ;;
        --mode) MODE="$2"; shift 2 ;;
        --batch-size) BATCH_SIZE="$2"; shift 2 ;;
        --help) usage; exit 0 ;;
        *) echo "Unknown argument: $1"; usage; exit 1 ;;
    esac
done

if [[ "$MODE" != "normal" && "$MODE" != "pipeline" ]]; then
    echo "mode must be normal or pipeline"
    exit 1
fi

CPP_BIN="${BUILD_DIR}/benchmark/B1-redis_client_bench"
if [[ ! -x "${CPP_BIN}" ]]; then
    echo "C++ benchmark binary not found: ${CPP_BIN}"
    echo "build first: cmake --build ${BUILD_DIR} --target B1-redis_client_bench"
    exit 1
fi

RUST_TARGET_DIR="${RUST_DIR}/target"
CARGO_HOME="${CARGO_HOME:-/tmp/cargo-galay-redis}"
mkdir -p "${CARGO_HOME}"

echo "== Building Rust benchmark =="
cargo build --release --manifest-path "${RUST_DIR}/Cargo.toml" --target-dir "${RUST_TARGET_DIR}"
RUST_BIN="${RUST_TARGET_DIR}/release/galay-redis-rust-bench"
if [[ ! -x "${RUST_BIN}" ]]; then
    echo "Rust benchmark binary not found: ${RUST_BIN}"
    exit 1
fi

echo
echo "== Running C++ benchmark =="
CPP_OUT="$("${CPP_BIN}" \
    -h "${HOST}" -p "${PORT}" -c "${CLIENTS}" -n "${OPERATIONS}" \
    -m "${MODE}" -b "${BATCH_SIZE}" \
    --timeout-ms -1 --buffer-size 65536 -q)"
echo "${CPP_OUT}"
CPP_OPS="$(echo "${CPP_OUT}" | awk -F': ' '$1=="Ops/sec" {print $2}' | tail -n 1)"

echo
echo "== Running Rust benchmark =="
RUST_OUT="$("${RUST_BIN}" \
    --host "${HOST}" --port "${PORT}" --clients "${CLIENTS}" --operations "${OPERATIONS}" \
    --mode "${MODE}" --batch-size "${BATCH_SIZE}")"
echo "${RUST_OUT}"
RUST_OPS="$(echo "${RUST_OUT}" | awk -F': ' '$1=="Ops/sec" {print $2}' | tail -n 1)"

if [[ -z "${CPP_OPS}" || -z "${RUST_OPS}" ]]; then
    echo "failed to parse Ops/sec from benchmark output"
    exit 1
fi

echo
echo "== Summary =="
echo "Scenario: host=${HOST} port=${PORT} clients=${CLIENTS} operations=${OPERATIONS} mode=${MODE} batch_size=${BATCH_SIZE}"
echo "C++ Ops/sec: ${CPP_OPS}"
echo "Rust Ops/sec: ${RUST_OPS}"
