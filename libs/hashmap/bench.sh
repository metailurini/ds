#!/usr/bin/env bash
set -euo pipefail

# Benchmark runner for the hashmap.h/hashmap.c implementation.
# Usage:
#   ./bench.sh [compile flags...] [N] [COLLISION_N] [REPEAT]
#   ./bench.sh -O3 -march=native 500000 2000 3
#   CC=clang CFLAGS='-O3 -march=native' ./bench.sh 500000 2000 3
#   ./bench.sh --cflags='-O3 -march=native -flto' 500000

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC="${SRC:-$ROOT_DIR/hashmap.c}"
HDR="${HDR:-$ROOT_DIR/hashmap.h}"
CC_BIN="${CC:-cc}"
TMP_DIR="$(mktemp -d "${TMPDIR:-/tmp}/hm_bench.XXXXXX")"
trap 'rm -rf "$TMP_DIR"' EXIT

usage() {
  cat <<'USAGE'
Usage:
  ./bench.sh [compile flags...] [N] [COLLISION_N] [REPEAT]

Examples:
  ./bench.sh
  ./bench.sh 200000 2000 5
  ./bench.sh -O3 200000
  ./bench.sh -O3 -march=native 200000 2000 5
  CC=clang CFLAGS='-O3 -march=native' ./bench.sh 200000
  ./bench.sh --cflags='-O3 -march=native -flto' 200000

Environment:
  CC       compiler command, default: cc
  CFLAGS   default compile flags, default: -O2
  SRC      hashmap source file, default: ./hashmap.c
  HDR      hashmap header file, default: ./hashmap.h
USAGE
}

is_uint() {
  [[ "$1" =~ ^[0-9]+$ ]]
}

N="${N:-100000}"
COLLISION_N="${COLLISION_N:-1000}"
REPEAT="${REPEAT:-3}"
POSITIONAL=()
CLI_CFLAGS=()

while (($#)); do
  case "$1" in
    -h|--help)
      usage
      exit 0
      ;;
    --cflags=*)
      read -r -a _more_flags <<< "${1#--cflags=}"
      CLI_CFLAGS+=("${_more_flags[@]}")
      shift
      ;;
    --cflags)
      if (($# < 2)); then
        echo "error: --cflags requires an argument" >&2
        exit 2
      fi
      read -r -a _more_flags <<< "$2"
      CLI_CFLAGS+=("${_more_flags[@]}")
      shift 2
      ;;
    -O|-O0|-O1|-O2|-O3|-Os|-Oz|-Og|-Ofast|-g|-g0|-g1|-g2|-g3|-DNDEBUG|-flto|-fno-lto|-pipe)
      CLI_CFLAGS+=("$1")
      shift
      ;;
    -march=*|-mtune=*|-mcpu=*|-fsanitize=*|-fno-omit-frame-pointer|-fomit-frame-pointer)
      CLI_CFLAGS+=("$1")
      shift
      ;;
    -D*|-U*|-I*)
      CLI_CFLAGS+=("$1")
      shift
      ;;
    --)
      shift
      while (($#)); do
        POSITIONAL+=("$1")
        shift
      done
      ;;
    -*)
      # Treat unknown dash-prefixed arguments as compiler flags, not as N.
      CLI_CFLAGS+=("$1")
      shift
      ;;
    *)
      POSITIONAL+=("$1")
      shift
      ;;
  esac
done

if ((${#POSITIONAL[@]} > 3)); then
  echo "error: too many numeric arguments: ${POSITIONAL[*]}" >&2
  usage >&2
  exit 2
fi

if ((${#POSITIONAL[@]} >= 1)); then N="${POSITIONAL[0]}"; fi
if ((${#POSITIONAL[@]} >= 2)); then COLLISION_N="${POSITIONAL[1]}"; fi
if ((${#POSITIONAL[@]} >= 3)); then REPEAT="${POSITIONAL[2]}"; fi

for _arg_name in N COLLISION_N REPEAT; do
  _arg_value="${!_arg_name}"
  if ! is_uint "$_arg_value"; then
    echo "error: $_arg_name must be a non-negative integer, got: $_arg_value" >&2
    exit 2
  fi
done

if [[ ! -f "$SRC" ]]; then
  echo "error: source file not found: $SRC" >&2
  exit 1
fi
if [[ ! -f "$HDR" ]]; then
  echo "error: header file not found: $HDR" >&2
  exit 1
fi

DEFAULT_CFLAGS="${CFLAGS:--O2}"
if [[ -z "${CFLAGS+x}" ]]; then
  for _flag in "${CLI_CFLAGS[@]}"; do
    case "$_flag" in
      -O|-O0|-O1|-O2|-O3|-Os|-Oz|-Og|-Ofast)
        DEFAULT_CFLAGS=""
        break
        ;;
    esac
  done
fi
read -r -a EXTRA_CFLAGS <<< "$DEFAULT_CFLAGS"
COMMON_FLAGS=(-std=c99 -Wall -Wextra -pedantic "${EXTRA_CFLAGS[@]}" "${CLI_CFLAGS[@]}")

cp "$SRC" "$TMP_DIR/hashmap.c"
cp "$HDR" "$TMP_DIR/hashmap.h"

cat > "$TMP_DIR/hashmap_bench.c" <<'C_EOF'
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "hashmap.h"

static void *as_value(uintptr_t v) { return (void *)v; }
static uintptr_t from_value(void *v) { return (uintptr_t)v; }

static double now_seconds(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec + (double)tv.tv_usec / 1000000.0;
}

static uint64_t constant_hash(const char *key, size_t key_len, uint64_t seed, void *ctx) {
    (void)key; (void)key_len; (void)seed; (void)ctx;
    return 0xfaceb00cu;
}

static uint64_t lcg_next(uint64_t *state) {
    *state = (*state * 6364136223846793005ULL) + 1442695040888963407ULL;
    return *state;
}

typedef struct bench_keyset {
    char (*keys)[64];
    size_t *lens;
    size_t count;
} bench_keyset;

static int keyset_init(bench_keyset *ks, size_t count) {
    if (!ks) return 0;
    ks->keys = NULL;
    ks->lens = NULL;
    ks->count = count;
    if (count == 0u) return 1;
    ks->keys = (char (*)[64])calloc(count, sizeof(*ks->keys));
    ks->lens = (size_t *)calloc(count, sizeof(*ks->lens));
    if (!ks->keys || !ks->lens) {
        free(ks->keys);
        free(ks->lens);
        ks->keys = NULL;
        ks->lens = NULL;
        ks->count = 0;
        return 0;
    }
    return 1;
}

static void keyset_free(bench_keyset *ks) {
    if (!ks) return;
    free(ks->keys);
    free(ks->lens);
    ks->keys = NULL;
    ks->lens = NULL;
    ks->count = 0;
}

static void keyset_set(bench_keyset *ks, size_t i, const char *fmt, unsigned long long v) {
    int n;
    if (!ks || i >= ks->count) return;
    n = snprintf(ks->keys[i], sizeof(ks->keys[i]), fmt, v);
    ks->lens[i] = (n > 0 && (size_t)n < sizeof(ks->keys[i])) ? (size_t)n : strlen(ks->keys[i]);
}

static void print_result(const char *name, size_t ops, double seconds, const hashmap *hm) {
    hm_stats st;
    double rate = seconds > 0.0 ? (double)ops / seconds : 0.0;
    memset(&st, 0, sizeof(st));
    (void)hm_stats_get(hm, &st);
    printf("%-30s ops=%10zu time=%8.4f sec rate=%12.0f ops/s len=%8zu cap=%8zu p50=%3zu p95=%3zu p99=%3zu max=%5zu avg=%7.2f\n",
           name, ops, seconds, rate, st.len, st.capacity,
           st.p50_probe, st.p95_probe, st.p99_probe, st.max_probe, st.avg_probe);
}

static const char *result_name(hm_result rc) {
    switch (rc) {
        case HM_OK: return "HM_OK";
        case HM_ERR_INVALID: return "HM_ERR_INVALID";
        case HM_ERR_OOM: return "HM_ERR_OOM";
        case HM_ERR_NOT_FOUND: return "HM_ERR_NOT_FOUND";
        case HM_ERR_MODIFIED: return "HM_ERR_MODIFIED";
        case HM_ERR_FULL: return "HM_ERR_FULL";
        case HM_ERR_PROBE_LIMIT: return "HM_ERR_PROBE_LIMIT";
        default: return "HM_ERR_UNKNOWN";
    }
}

static int bench_sequential(size_t n) {
    hashmap hm;
    bench_keyset keys;
    bench_keyset misses;
    size_t i;
    double t0, t1;
    void *out = NULL;

    if (!keyset_init(&keys, n) || !keyset_init(&misses, n)) return 1;
    for (i = 0; i < n; i++) {
        keyset_set(&keys, i, "seq-%llu", (unsigned long long)i);
        keyset_set(&misses, i, "miss-%llu", (unsigned long long)i);
    }

    if (hm_init(&hm) != HM_OK) { keyset_free(&keys); keyset_free(&misses); return 1; }
    if (hm_reserve(&hm, n) != HM_OK) { keyset_free(&keys); keyset_free(&misses); return 2; }

    t0 = now_seconds();
    for (i = 0; i < n; i++) {
        if (hm_put_len(&hm, keys.keys[i], keys.lens[i], as_value(i + 1u), NULL) != HM_OK) return 3;
    }
    t1 = now_seconds();
    print_result("sequential insert", n, t1 - t0, &hm);

    t0 = now_seconds();
    for (i = 0; i < n; i++) {
        if (hm_get_len(&hm, keys.keys[i], keys.lens[i], &out) != HM_OK || from_value(out) != i + 1u) return 4;
    }
    t1 = now_seconds();
    print_result("sequential lookup hit", n, t1 - t0, &hm);

    t0 = now_seconds();
    for (i = 0; i < n; i++) {
        if (hm_get_len(&hm, misses.keys[i], misses.lens[i], &out) != HM_ERR_NOT_FOUND) return 5;
    }
    t1 = now_seconds();
    print_result("sequential lookup miss", n, t1 - t0, &hm);

    t0 = now_seconds();
    for (i = 0; i < n; i++) {
        if (hm_remove_len(&hm, keys.keys[i], keys.lens[i], NULL) != HM_OK) return 6;
    }
    t1 = now_seconds();
    print_result("sequential remove", n, t1 - t0, &hm);
    hm_free(&hm);
    keyset_free(&keys);
    keyset_free(&misses);
    return 0;
}

static int bench_randomish(size_t n) {
    hashmap hm;
    bench_keyset keys;
    size_t i;
    uint64_t rng = 0x12345678abcdef00ULL;
    double t0, t1;
    void *out = NULL;

    if (!keyset_init(&keys, n)) return 10;
    for (i = 0; i < n; i++) {
        uint64_t x = lcg_next(&rng);
        keyset_set(&keys, i, "rnd-%016llx", (unsigned long long)x);
    }

    if (hm_init(&hm) != HM_OK) { keyset_free(&keys); return 10; }
    if (hm_reserve(&hm, n) != HM_OK) { keyset_free(&keys); return 11; }

    t0 = now_seconds();
    for (i = 0; i < n; i++) {
        if (hm_put_len(&hm, keys.keys[i], keys.lens[i], as_value((uintptr_t)(i + 1u)), NULL) != HM_OK) return 12;
    }
    t1 = now_seconds();
    print_result("randomish insert", n, t1 - t0, &hm);

    t0 = now_seconds();
    for (i = 0; i < n; i++) {
        if (hm_get_len(&hm, keys.keys[i], keys.lens[i], &out) != HM_OK) return 13;
    }
    t1 = now_seconds();
    print_result("randomish lookup hit", n, t1 - t0, &hm);
    hm_free(&hm);
    keyset_free(&keys);
    return 0;
}

static int bench_churn(size_t n) {
    hashmap hm;
    bench_keyset old_keys;
    bench_keyset new_keys;
    size_t i;
    double t0, t1;

    if (!keyset_init(&old_keys, n) || !keyset_init(&new_keys, n)) return 20;
    for (i = 0; i < n; i++) {
        keyset_set(&old_keys, i, "churn-%llu", (unsigned long long)i);
        keyset_set(&new_keys, i, "churn-new-%llu", (unsigned long long)i);
    }

    if (hm_init(&hm) != HM_OK) { keyset_free(&old_keys); keyset_free(&new_keys); return 20; }
    if (hm_reserve(&hm, n) != HM_OK) { keyset_free(&old_keys); keyset_free(&new_keys); return 21; }
    for (i = 0; i < n; i++) {
        if (hm_put_len(&hm, old_keys.keys[i], old_keys.lens[i], as_value(i + 1u), NULL) != HM_OK) return 22;
    }
    t0 = now_seconds();
    for (i = 0; i < n; i++) {
        if (hm_remove_len(&hm, old_keys.keys[i], old_keys.lens[i], NULL) != HM_OK) return 23;
        if (hm_put_len(&hm, new_keys.keys[i], new_keys.lens[i], as_value(i + 1u), NULL) != HM_OK) return 24;
    }
    t1 = now_seconds();
    print_result("remove+insert churn", n * 2u, t1 - t0, &hm);
    hm_free(&hm);
    keyset_free(&old_keys);
    keyset_free(&new_keys);
    return 0;
}

static int bench_adversarial_collisions(size_t n) {
    hashmap hm;
    hm_config cfg;
    char key[64];
    size_t i;
    size_t inserted = 0;
    double t0, t1;
    void *out = NULL;
    hm_result rc = HM_OK;

    memset(&cfg, 0, sizeof(cfg));
    cfg.hash_fn = constant_hash;
    if (hm_init_with_config(&hm, &cfg) != HM_OK) return 40;
    if (hm_reserve(&hm, n) != HM_OK) return 41;
    printf("constant-hash adversarial test: default probe guard is expected to stop pathological chains\n");
    t0 = now_seconds();
    for (i = 0; i < n; i++) {
        snprintf(key, sizeof(key), "bad-%zu", i);
        rc = hm_put(&hm, key, as_value(i + 1u), NULL);
        if (rc == HM_ERR_PROBE_LIMIT) break;
        if (rc != HM_OK) return 42;
        inserted++;
    }
    t1 = now_seconds();
    print_result("constant-hash guarded insert", inserted, t1 - t0, &hm);
    if (rc == HM_ERR_PROBE_LIMIT) {
        printf("  stopped early with %s after %zu/%zu successful inserts\n", result_name(rc), inserted, n);
    } else {
        printf("  completed without hitting probe guard (%s)\n", result_name(rc));
    }
    t0 = now_seconds();
    for (i = 0; i < inserted; i++) {
        snprintf(key, sizeof(key), "bad-%zu", i);
        if (hm_get(&hm, key, &out) != HM_OK) return 43;
    }
    t1 = now_seconds();
    print_result("constant-hash guarded lookup", inserted, t1 - t0, &hm);
    hm_free(&hm);
    return 0;
}

int main(int argc, char **argv) {
    size_t n = argc > 1 ? (size_t)strtoull(argv[1], NULL, 10) : 100000u;
    size_t collision_n = argc > 2 ? (size_t)strtoull(argv[2], NULL, 10) : 1000u;
    int repeat = argc > 3 ? atoi(argv[3]) : 3;
    int r;
    int rc;

    if (n == 0u) n = 1u;
    if (collision_n == 0u) collision_n = 1u;
    if (repeat < 1) repeat = 1;

    printf("hashmap benchmark: N=%zu collision_N=%zu repeat=%d\n", n, collision_n, repeat);
    printf("note: normal-case keys are pre-generated; timed sections measure hashmap work, not snprintf/key formatting\n");
    for (r = 0; r < repeat; r++) {
        printf("\n-- run %d --\n", r + 1);
        if ((rc = bench_sequential(n)) != 0) return rc;
        if ((rc = bench_randomish(n)) != 0) return rc;
        if ((rc = bench_churn(n / 2u + 1u)) != 0) return rc;
        if ((rc = bench_adversarial_collisions(collision_n)) != 0) return rc;
    }
    return 0;
}
C_EOF

echo "[bench] compile"
echo "[bench] cc=$CC_BIN flags=${COMMON_FLAGS[*]}"
"$CC_BIN" "${COMMON_FLAGS[@]}" -I"$TMP_DIR" -c "$TMP_DIR/hashmap.c" -o "$TMP_DIR/hashmap.o"
"$CC_BIN" "${COMMON_FLAGS[@]}" -I"$TMP_DIR" "$TMP_DIR/hashmap_bench.c" "$TMP_DIR/hashmap.o" -o "$TMP_DIR/hashmap_bench"

echo "[bench] run N=$N COLLISION_N=$COLLISION_N REPEAT=$REPEAT"
"$TMP_DIR/hashmap_bench" "$N" "$COLLISION_N" "$REPEAT"
