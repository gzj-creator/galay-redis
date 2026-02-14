#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
REPO_ROOT="$(cd -- "${SCRIPT_DIR}/../.." >/dev/null 2>&1 && pwd)"
COMPOSE_FILE="${SCRIPT_DIR}/docker-compose.cluster-sentinel.yml"

: "${KEEP_IT_ENV:=0}"
: "${IT_ENV_STARTED:=0}"

require_cmd() {
  local cmd="$1"
  if ! command -v "${cmd}" >/dev/null 2>&1; then
    echo "[ERROR] Required command not found: ${cmd}"
    exit 1
  fi
}

require_docker_compose() {
  if ! docker compose version >/dev/null 2>&1; then
    echo "[ERROR] 'docker compose' is unavailable. Please install Docker Compose v2."
    exit 1
  fi
}

wait_redis() {
  local host="$1"
  local port="$2"
  local retries="${3:-60}"
  local i
  for ((i = 1; i <= retries; ++i)); do
    if redis-cli -h "${host}" -p "${port}" ping >/dev/null 2>&1; then
      return 0
    fi
    sleep 1
  done
  return 1
}

find_key_for_slot() {
  local slot="$1"
  local prefix="$2"
  local i
  for ((i = 0; i < 2000000; ++i)); do
    local key="${prefix}:${i}"
    local actual
    actual="$(redis-cli -h 127.0.0.1 -p 7000 cluster keyslot "${key}")"
    if [[ "${actual}" == "${slot}" ]]; then
      echo "${key}"
      return 0
    fi
  done
  return 1
}

cleanup() {
  if [[ "${KEEP_IT_ENV}" == "1" ]]; then
    echo "[INFO] KEEP_IT_ENV=1, preserving docker environment"
    return
  fi
  if [[ "${IT_ENV_STARTED}" != "1" ]]; then
    return
  fi
  echo "[INFO] Shutting down integration containers..."
  docker compose -f "${COMPOSE_FILE}" down -v >/dev/null 2>&1 || true
}
trap cleanup EXIT

require_cmd docker
require_cmd redis-cli
require_cmd cmake
require_docker_compose

echo "[INFO] Starting Redis Cluster + Sentinel containers..."
docker compose -f "${COMPOSE_FILE}" up -d
IT_ENV_STARTED=1

for p in 7000 7001 7002 7003 7004 7005 6380 6381 6382 26379 26380 26381; do
  if ! wait_redis 127.0.0.1 "${p}" 90; then
    echo "[ERROR] Redis on port ${p} not ready"
    exit 1
  fi
done

echo "[INFO] Bootstrapping Redis Cluster (if needed)..."
if ! redis-cli -h 127.0.0.1 -p 7000 cluster info | grep -q "cluster_state:ok"; then
  redis-cli --cluster create \
    127.0.0.1:7000 \
    127.0.0.1:7001 \
    127.0.0.1:7002 \
    127.0.0.1:7003 \
    127.0.0.1:7004 \
    127.0.0.1:7005 \
    --cluster-replicas 1 \
    --cluster-yes >/dev/null
fi

for _ in $(seq 1 60); do
  if redis-cli -h 127.0.0.1 -p 7000 cluster info | grep -q "cluster_state:ok"; then
    break
  fi
  sleep 1
done
if ! redis-cli -h 127.0.0.1 -p 7000 cluster info | grep -q "cluster_state:ok"; then
  echo "[ERROR] Cluster did not reach OK state"
  exit 1
fi

echo "[INFO] Preparing MOVED/ASK test keys..."
cluster_nodes="$(redis-cli -h 127.0.0.1 -p 7000 cluster nodes)"

source_line="$(echo "${cluster_nodes}" | awk '$2 ~ /:7000@/ && $3 ~ /master/ {print; exit}')"
if [[ -z "${source_line}" ]]; then
  echo "[ERROR] Failed to locate source master line for port 7000"
  exit 1
fi
source_range="$(echo "${source_line}" | grep -Eo '[0-9]+-[0-9]+' | head -n1)"
if [[ -z "${source_range}" ]]; then
  echo "[ERROR] Failed to parse source slot range"
  exit 1
fi
source_slot_start="${source_range%-*}"

target_line="$(echo "${cluster_nodes}" | awk '$3 ~ /master/ && $2 !~ /:7000@/ {print; exit}')"
if [[ -z "${target_line}" ]]; then
  echo "[ERROR] Failed to locate target master line"
  exit 1
fi
target_range="$(echo "${target_line}" | grep -Eo '[0-9]+-[0-9]+' | head -n1)"
if [[ -z "${target_range}" ]]; then
  echo "[ERROR] Failed to parse target slot range"
  exit 1
fi
target_slot_start="${target_range%-*}"

target_endpoint="$(echo "${target_line}" | awk '{print $2}' | cut -d'@' -f1)"
target_port="${target_endpoint##*:}"

source_id="$(redis-cli -h 127.0.0.1 -p 7000 cluster myid)"
dest_id="$(redis-cli -h 127.0.0.1 -p "${target_port}" cluster myid)"

MOVED_KEY="$(find_key_for_slot "${target_slot_start}" "galay:it:moved")"
ASK_KEY="$(find_key_for_slot "${source_slot_start}" "galay:it:ask")"
if [[ -z "${MOVED_KEY}" || -z "${ASK_KEY}" ]]; then
  echo "[ERROR] Failed to generate MOVED/ASK keys"
  exit 1
fi

# 构造 ASK 状态：source(7000) 对 slot 迁出，target 对 slot 导入。
redis-cli -h 127.0.0.1 -p "${target_port}" cluster setslot "${source_slot_start}" importing "${source_id}" >/dev/null
redis-cli -h 127.0.0.1 -p 7000 cluster setslot "${source_slot_start}" migrating "${dest_id}" >/dev/null
redis-cli -h 127.0.0.1 -p 7000 del "${ASK_KEY}" >/dev/null
redis-cli -h 127.0.0.1 -p "${target_port}" del "${ASK_KEY}" >/dev/null

echo "[INFO] Building integration test target..."
cmake --build "${REPO_ROOT}/build" -j4 --target test_integration_cluster_sentinel >/dev/null

echo "[INFO] Running integration test..."
(
  cd "${REPO_ROOT}"
  GALAY_IT_ENABLE=1 \
  GALAY_IT_CLUSTER_HOST=127.0.0.1 \
  GALAY_IT_CLUSTER_PORT=7000 \
  GALAY_IT_MOVED_KEY="${MOVED_KEY}" \
  GALAY_IT_ASK_KEY="${ASK_KEY}" \
  GALAY_IT_SENTINEL_HOST=127.0.0.1 \
  GALAY_IT_SENTINEL_PORT=26379 \
  GALAY_IT_SENTINEL_MASTER_NAME=mymaster \
  GALAY_IT_TRIGGER_SENTINEL_FAILOVER=1 \
  ./build/test/test_integration_cluster_sentinel
)

echo "[INFO] Integration test completed successfully."
