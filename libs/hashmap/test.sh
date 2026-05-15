#!/usr/bin/env bash
set -euo pipefail

# Black-box test runner for the hashmap.h/hashmap.c implementation.
# It generates a temporary C test program that uses the public API only.
# Usage:
#   ./test.sh
#   CC=clang CFLAGS='-O2 -g' ./test.sh

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC="${SRC:-$ROOT_DIR/hashmap.c}"
HDR="${HDR:-$ROOT_DIR/hashmap.h}"
CC_BIN="${CC:-cc}"
TMP_DIR="$(mktemp -d "${TMPDIR:-/tmp}/hm_blackbox.XXXXXX")"
trap 'rm -rf "$TMP_DIR"' EXIT

if [[ ! -f "$SRC" ]]; then
  echo "error: source file not found: $SRC" >&2
  exit 1
fi
if [[ ! -f "$HDR" ]]; then
  echo "error: header file not found: $HDR" >&2
  exit 1
fi

read -r -a EXTRA_CFLAGS <<< "${CFLAGS:--O2}"
COMMON_FLAGS=(-std=c99 -Wall -Wextra -pedantic "${EXTRA_CFLAGS[@]}")

cp "$SRC" "$TMP_DIR/hashmap.c"
cp "$HDR" "$TMP_DIR/hashmap.h"

cat > "$TMP_DIR/hashmap_blackbox_test.c" <<'C_EOF'
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hashmap.h"

static int failures = 0;

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
        failures++; \
    } \
} while (0)

#define CHECK_RC(expr, want) do { \
    hm_result got__ = (expr); \
    if (got__ != (want)) { \
        fprintf(stderr, "FAIL %s:%d: %s got=%d want=%d\n", __FILE__, __LINE__, #expr, got__, (want)); \
        failures++; \
    } \
} while (0)

static void *as_value(uintptr_t v) { return (void *)v; }
static uintptr_t from_value(void *v) { return (uintptr_t)v; }

static uint64_t constant_hash(const char *key, size_t key_len, uint64_t seed, void *ctx) {
    (void)key; (void)key_len; (void)seed; (void)ctx;
    return 0x12345678u;
}

static hm_result upsert_add(const char *key, size_t key_len, int found,
                            void *old_value, void **new_value, void *ctx) {
    uintptr_t delta = *(uintptr_t *)ctx;
    (void)key; (void)key_len;
    *new_value = as_value((found ? from_value(old_value) : 0u) + delta);
    return HM_OK;
}

static int freed_values = 0;
static void counting_free(void *value, void *ctx) {
    (void)value; (void)ctx;
    freed_values++;
}

static uint64_t seed_sensitive_hash(const char *key, size_t key_len, uint64_t seed, void *ctx) {
    unsigned n = 0;
    size_t i;
    (void)ctx;
    if (seed == 222u) return 0u;
    for (i = 0; i < key_len; i++) if (key[i] >= '0' && key[i] <= '9') n = n * 10u + (unsigned)(key[i] - '0');
    return (uint64_t)n * 11400714819323198485ull;
}

static uint64_t capacity_sensitive_hash(const char *key, size_t key_len, uint64_t seed, void *ctx) {
    unsigned n = 0;
    size_t i;
    (void)seed; (void)ctx;
    for (i = 0; i < key_len; i++) if (key[i] >= '0' && key[i] <= '9') n = n * 10u + (unsigned)(key[i] - '0');
    return (uint64_t)n * 16u;
}

static uint64_t robinhood_regression_hash(const char *key, size_t key_len, uint64_t seed, void *ctx) {
    (void)key_len; (void)seed; (void)ctx;
    if (strcmp(key, "A") == 0) return 0u;
    if (strcmp(key, "B") == 0) return 1u;
    if (strcmp(key, "C") == 0) return 1u;
    if (strcmp(key, "Z") == 0) return 0u;
    return constant_hash(key, key_len, seed, ctx);
}

struct alloc_state { size_t malloc_calls; size_t calloc_calls; size_t free_calls; size_t fail_after; };

static void *test_malloc(size_t size, void *ctx) {
    struct alloc_state *s = (struct alloc_state *)ctx;
    s->malloc_calls++;
    if (s->fail_after && s->malloc_calls + s->calloc_calls > s->fail_after) return NULL;
    return malloc(size);
}

static void *test_calloc(size_t count, size_t size, void *ctx) {
    struct alloc_state *s = (struct alloc_state *)ctx;
    s->calloc_calls++;
    if (s->fail_after && s->malloc_calls + s->calloc_calls > s->fail_after) return NULL;
    return calloc(count, size);
}

static void test_free(void *ptr, void *ctx) {
    struct alloc_state *s = (struct alloc_state *)ctx;
    s->free_calls++;
    free(ptr);
}

static void test_basic_put_get_replace_remove(void) {
    hashmap hm;
    void *out = NULL;
    void *old = NULL;
    int inserted = -1;

    CHECK_RC(hm_init(&hm), HM_OK);
    CHECK(hm_len(&hm) == 0);
    CHECK(hm_capacity(&hm) >= HM_MIN_CAPACITY);

    CHECK_RC(hm_put(&hm, "alpha", as_value(11), NULL), HM_OK);
    CHECK_RC(hm_get(&hm, "alpha", &out), HM_OK);
    CHECK(from_value(out) == 11);
    CHECK(hm_contains_key(&hm, "alpha") == 1);

    CHECK_RC(hm_put(&hm, "alpha", as_value(22), &old), HM_OK);
    CHECK(from_value(old) == 11);
    CHECK_RC(hm_get_or_insert(&hm, "alpha", as_value(44), &out, &inserted), HM_OK);
    CHECK(inserted == 0 && from_value(out) == 22);
    CHECK_RC(hm_get_or_insert(&hm, "beta", as_value(55), &out, &inserted), HM_OK);
    CHECK(inserted == 1 && from_value(out) == 55);

    CHECK_RC(hm_put(&hm, "null-value", NULL, NULL), HM_OK);
    out = as_value(1);
    CHECK_RC(hm_get(&hm, "null-value", &out), HM_OK);
    CHECK(out == NULL);

    CHECK_RC(hm_remove(&hm, "alpha", &old), HM_OK);
    CHECK(from_value(old) == 22);
    CHECK_RC(hm_remove(&hm, "alpha", NULL), HM_ERR_NOT_FOUND);
    CHECK_RC(hm_validate(&hm), HM_OK);
    hm_free(&hm);
}

static void test_length_aware_and_empty_keys(void) {
    hashmap hm;
    const char k1[] = {'a', '\0', 'b'};
    const char k2[] = {'a', '\0', 'c'};
    void *out = NULL;

    CHECK_RC(hm_init(&hm), HM_OK);
    CHECK_RC(hm_put_len(&hm, k1, sizeof(k1), as_value(101), NULL), HM_OK);
    CHECK_RC(hm_put_len(&hm, k2, sizeof(k2), as_value(202), NULL), HM_OK);
    CHECK_RC(hm_put_len(&hm, "", 0, as_value(303), NULL), HM_OK);
    CHECK_RC(hm_put_len(&hm, NULL, 0, as_value(404), NULL), HM_OK);
    CHECK(hm_len(&hm) == 3); /* "" and NULL/0 are the same empty byte key. */
    CHECK_RC(hm_get_len(&hm, k1, sizeof(k1), &out), HM_OK);
    CHECK(from_value(out) == 101);
    CHECK_RC(hm_get_len(&hm, k2, sizeof(k2), &out), HM_OK);
    CHECK(from_value(out) == 202);
    CHECK_RC(hm_get_len(&hm, NULL, 1, &out), HM_ERR_INVALID);
    CHECK_RC(hm_validate(&hm), HM_OK);
    hm_free(&hm);
}

static void test_growth_collisions_delete_and_stats(void) {
    hashmap hm;
    hm_config cfg;
    hm_stats st;
    char key[64];
    size_t i;
    void *out = NULL;

    memset(&cfg, 0, sizeof(cfg));
    cfg.hash_fn = constant_hash;
    cfg.max_load_percent = 80u;
    CHECK_RC(hm_init_with_config(&hm, &cfg), HM_OK);

    for (i = 0; i < 96; i++) {
        snprintf(key, sizeof(key), "collision-%03zu", i);
        CHECK_RC(hm_put(&hm, key, as_value(i + 1u), NULL), HM_OK);
    }
    CHECK(hm_len(&hm) == 96);
    CHECK_RC(hm_stats_get(&hm, &st), HM_OK);
    CHECK(st.max_probe > 10);

    for (i = 0; i < 96; i += 2) {
        snprintf(key, sizeof(key), "collision-%03zu", i);
        CHECK_RC(hm_remove(&hm, key, NULL), HM_OK);
    }
    for (i = 1; i < 96; i += 2) {
        snprintf(key, sizeof(key), "collision-%03zu", i);
        CHECK_RC(hm_get(&hm, key, &out), HM_OK);
        CHECK(from_value(out) == i + 1u);
    }
    CHECK(hm_tombstones(&hm) == 0);
    CHECK_RC(hm_validate(&hm), HM_OK);
    hm_free(&hm);
}

static void test_probe_limit_guard(void) {
    hashmap hm;
    hm_config cfg;
    char key[64];
    size_t i;
    hm_result rc = HM_OK;
    hm_stats st;

    memset(&cfg, 0, sizeof(cfg));
    cfg.hash_fn = constant_hash;
    CHECK_RC(hm_init_with_config(&hm, &cfg), HM_OK);
    CHECK_RC(hm_reserve(&hm, 512), HM_OK);
    for (i = 0; i < 512; i++) {
        snprintf(key, sizeof(key), "probe-limit-%03zu", i);
        rc = hm_put(&hm, key, as_value(i + 1u), NULL);
        if (rc == HM_ERR_PROBE_LIMIT) break;
        CHECK_RC(rc, HM_OK);
    }
    CHECK(rc == HM_ERR_PROBE_LIMIT);
    CHECK(i > 0);
    CHECK_RC(hm_stats_get(&hm, &st), HM_OK);
    CHECK(st.max_probe <= HM_DEFAULT_MAX_PROBE_LENGTH);
    CHECK(st.p50_probe <= st.p95_probe);
    CHECK(st.p95_probe <= st.p99_probe);
    CHECK(st.p99_probe <= st.max_probe);
    CHECK_RC(hm_validate(&hm), HM_OK);
    hm_free(&hm);
}

static void test_reserve_freeze_compact_and_limits(void) {
    hashmap hm;
    hm_config cfg;
    size_t cap;
    size_t i;
    char key[32];

    CHECK_RC(hm_init(&hm), HM_OK);
    CHECK_RC(hm_reserve(&hm, 128), HM_OK);
    cap = hm_capacity(&hm);
    CHECK(cap >= 128);
    CHECK_RC(hm_freeze_capacity(&hm), HM_OK);
    CHECK(hm_is_frozen(&hm));
    for (i = 0; i < cap; i++) {
        snprintf(key, sizeof(key), "k%zu", i);
        hm_result rc = hm_put(&hm, key, as_value(i + 1), NULL);
        if (rc != HM_OK) {
            CHECK(rc == HM_ERR_FULL || rc == HM_ERR_PROBE_LIMIT);
            break;
        }
    }
    CHECK(i < cap);
    CHECK(hm_len(&hm) == i);
    for (size_t j = 0; j < i; j++) {
        void *out = NULL;
        snprintf(key, sizeof(key), "k%zu", j);
        CHECK_RC(hm_get(&hm, key, &out), HM_OK);
        CHECK(from_value(out) == j + 1u);
    }
    CHECK_RC(hm_unfreeze_capacity(&hm), HM_OK);
    CHECK(!hm_is_frozen(&hm));
    CHECK_RC(hm_compact(&hm), HM_OK);
    CHECK_RC(hm_shrink_to_fit(&hm), HM_OK);
    CHECK_RC(hm_validate(&hm), HM_OK);
    hm_free(&hm);

    memset(&cfg, 0, sizeof(cfg));
    cfg.max_capacity = HM_MIN_CAPACITY;
    CHECK_RC(hm_init_with_config(&hm, &cfg), HM_OK);
    CHECK_RC(hm_reserve(&hm, HM_MIN_CAPACITY * 4u), HM_ERR_FULL);
    hm_free(&hm);
}

static void test_failed_seed_rehash_is_transactional(void) {
    hashmap hm;
    hm_config cfg;
    char key[32];
    size_t i;
    uint64_t old_seed;

    memset(&cfg, 0, sizeof(cfg));
    cfg.hash_fn = seed_sensitive_hash;
    cfg.seed = 111u;
    cfg.max_probe_length = 4u;
    CHECK_RC(hm_init_with_config(&hm, &cfg), HM_OK);
    for (i = 0; i < 12; i++) {
        snprintf(key, sizeof(key), "seed-%zu", i);
        CHECK_RC(hm_put(&hm, key, as_value(i + 10u), NULL), HM_OK);
    }
    old_seed = hm_seed(&hm);
    CHECK_RC(hm_validate(&hm), HM_OK);
    CHECK_RC(hm_set_seed(&hm, 222u), HM_ERR_PROBE_LIMIT);
    CHECK(hm_seed(&hm) == old_seed);
    CHECK(hm_len(&hm) == 12);
    CHECK_RC(hm_validate(&hm), HM_OK);
    for (i = 0; i < 12; i++) {
        void *out = NULL;
        snprintf(key, sizeof(key), "seed-%zu", i);
        CHECK_RC(hm_get(&hm, key, &out), HM_OK);
        CHECK(from_value(out) == i + 10u);
    }
    hm_free(&hm);
}

static void test_failed_compact_is_transactional(void) {
    hashmap hm;
    hm_config cfg;
    char key[32];
    size_t i;
    size_t before_cap;

    memset(&cfg, 0, sizeof(cfg));
    cfg.hash_fn = capacity_sensitive_hash;
    cfg.max_probe_length = 4u;
    CHECK_RC(hm_init_with_config(&hm, &cfg), HM_OK);
    CHECK_RC(hm_reserve(&hm, 128), HM_OK);
    before_cap = hm_capacity(&hm);
    CHECK(before_cap >= 128);
    for (i = 0; i < 10; i++) {
        snprintf(key, sizeof(key), "cap-%zu", i);
        CHECK_RC(hm_put(&hm, key, as_value(i + 20u), NULL), HM_OK);
    }
    CHECK_RC(hm_validate(&hm), HM_OK);
    CHECK_RC(hm_compact(&hm), HM_ERR_PROBE_LIMIT);
    CHECK(hm_capacity(&hm) == before_cap);
    CHECK(hm_len(&hm) == 10);
    CHECK_RC(hm_validate(&hm), HM_OK);
    for (i = 0; i < 10; i++) {
        void *out = NULL;
        snprintf(key, sizeof(key), "cap-%zu", i);
        CHECK_RC(hm_get(&hm, key, &out), HM_OK);
        CHECK(from_value(out) == i + 20u);
    }
    hm_free(&hm);
}

static void test_iterators_upsert_and_bulk(void) {
    hashmap hm;
    hm_iter it;
    const char *key = NULL;
    size_t key_len = 0;
    void *value = NULL;
    void *old = NULL;
    uintptr_t delta = 7;
    int inserted = -1;
    hm_put_item puts[3];
    hm_remove_item removes[2];
    size_t processed = 0;
    size_t removed = 0;

    CHECK_RC(hm_init(&hm), HM_OK);
    CHECK_RC(hm_put(&hm, "a", as_value(1), NULL), HM_OK);
    CHECK_RC(hm_put(&hm, "b", as_value(2), NULL), HM_OK);
    CHECK_RC(hm_put(&hm, "c", as_value(3), NULL), HM_OK);

    CHECK_RC(hm_iter_init(&hm, &it), HM_OK);
    CHECK_RC(hm_iter_next_len(&hm, &it, &key, &key_len, &value), HM_OK);
    CHECK(key && key_len == strlen(key));
    CHECK_RC(hm_put(&hm, "d", as_value(4), NULL), HM_OK);
    CHECK_RC(hm_iter_next(&hm, &it, &key, &value), HM_ERR_MODIFIED);

    CHECK_RC(hm_iter_init(&hm, &it), HM_OK);
    while (hm_iter_next(&hm, &it, &key, &value) == HM_OK) {
        if (from_value(value) % 2u == 0u) {
            CHECK_RC(hm_iter_remove_current(&hm, &it, &old), HM_OK);
            removed++;
        } else if (strcmp(key, "a") == 0) {
            CHECK_RC(hm_iter_set_value(&hm, &it, as_value(100), &old), HM_OK);
        }
    }
    CHECK(removed == 2);

    CHECK_RC(hm_upsert(&hm, "counter", upsert_add, &delta, &value), HM_OK);
    CHECK(from_value(value) == 7);
    CHECK_RC(hm_upsert(&hm, "counter", upsert_add, &delta, &value), HM_OK);
    CHECK(from_value(value) == 14);
    CHECK_RC(hm_get_or_insert(&hm, "counter", as_value(99), &value, &inserted), HM_OK);
    CHECK(inserted == 0 && from_value(value) == 14);
    CHECK_RC(hm_get_or_insert(&hm, "fresh", as_value(99), &value, &inserted), HM_OK);
    CHECK(inserted == 1 && from_value(value) == 99);

    puts[0].key = "x"; puts[0].key_len = 1; puts[0].value = as_value(1); puts[0].old_value = NULL;
    puts[1].key = "y"; puts[1].key_len = 1; puts[1].value = as_value(2); puts[1].old_value = NULL;
    puts[2].key = "z"; puts[2].key_len = 1; puts[2].value = as_value(3); puts[2].old_value = NULL;
    CHECK_RC(hm_put_many_len(&hm, puts, 3, &processed), HM_OK);
    CHECK(processed == 3);
    removes[0].key = "x"; removes[0].key_len = 1; removes[0].old_value = NULL;
    removes[1].key = "z"; removes[1].key_len = 1; removes[1].old_value = NULL;
    CHECK_RC(hm_remove_many_len(&hm, removes, 2, &processed), HM_OK);
    CHECK(processed == 2);
    CHECK_RC(hm_get(&hm, "y", &value), HM_OK);
    CHECK(from_value(value) == 2);
    CHECK_RC(hm_validate(&hm), HM_OK);
    hm_free(&hm);
}

static void test_arena_borrowed_moved_and_destructor(void) {
    hashmap hm;
    char arena[128];
    char borrowed[] = "borrowed-key";
    char *owned = NULL;
    void *out = NULL;

    CHECK_RC(hm_init_with_key_arena(&hm, arena, sizeof(arena), 1), HM_OK);
    CHECK_RC(hm_put(&hm, "arena-key-long-enough", as_value(1), NULL), HM_OK);
    CHECK(hm_key_arena_used(&hm) > 0);
    CHECK(hm_key_arena_capacity(&hm) == sizeof(arena));
    CHECK_RC(hm_validate(&hm), HM_OK);
    hm_free(&hm);

    CHECK_RC(hm_init(&hm), HM_OK);
    CHECK_RC(hm_put_borrowed(&hm, borrowed, as_value(2), NULL), HM_OK);
    CHECK_RC(hm_get(&hm, "borrowed-key", &out), HM_OK);
    CHECK(from_value(out) == 2);
    owned = (char *)malloc(10);
    CHECK(owned != NULL);
    if (owned) {
        memcpy(owned, "owned-key", 10);
        CHECK_RC(hm_put_move_key(&hm, owned, as_value(3), NULL), HM_OK);
        owned = NULL;
        CHECK_RC(hm_get(&hm, "owned-key", &out), HM_OK);
        CHECK(from_value(out) == 3);
    }
    hm_free(&hm);

    {
        hm_config cfg;
        hm_allocator alloc;
        struct alloc_state state;
        char *dup = NULL;
        memset(&state, 0, sizeof(state));
        memset(&alloc, 0, sizeof(alloc));
        alloc.malloc_fn = test_malloc;
        alloc.calloc_fn = test_calloc;
        alloc.free_fn = test_free;
        alloc.ctx = &state;
        memset(&cfg, 0, sizeof(cfg));
        cfg.allocator = alloc;
        CHECK_RC(hm_init_with_config(&hm, &cfg), HM_OK);
        dup = (char *)test_malloc(10, &state);
        CHECK(dup != NULL);
        if (dup) {
            memcpy(dup, "owned-key", 10);
            CHECK_RC(hm_put_move_key(&hm, dup, as_value(10), NULL), HM_OK);
        }
        dup = (char *)test_malloc(10, &state);
        CHECK(dup != NULL);
        if (dup) {
            size_t frees_before = state.free_calls;
            void *old = NULL;
            memcpy(dup, "owned-key", 10);
            CHECK_RC(hm_put_move_key(&hm, dup, as_value(11), &old), HM_OK);
            CHECK(from_value(old) == 10);
            CHECK(state.free_calls == frees_before + 1u);
            CHECK_RC(hm_get(&hm, "owned-key", &out), HM_OK);
            CHECK(from_value(out) == 11);
        }
        hm_free(&hm);
    }

    freed_values = 0;
    {
        hm_config cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.value_free = counting_free;
        CHECK_RC(hm_init_with_config(&hm, &cfg), HM_OK);
        CHECK_RC(hm_put(&hm, "v1", as_value(1), NULL), HM_OK);
        CHECK_RC(hm_put(&hm, "v2", as_value(2), NULL), HM_OK);
        hm_clear(&hm);
        CHECK(freed_values == 2);
        hm_free(&hm);
    }
}

static void test_allocator_failure_path(void) {
    hashmap hm;
    hm_config cfg;
    hm_allocator alloc;
    struct alloc_state state;

    memset(&state, 0, sizeof(state));
    memset(&alloc, 0, sizeof(alloc));
    alloc.malloc_fn = test_malloc;
    alloc.calloc_fn = test_calloc;
    alloc.free_fn = test_free;
    alloc.ctx = &state;
    memset(&cfg, 0, sizeof(cfg));
    cfg.allocator = alloc;
    CHECK_RC(hm_init_with_config(&hm, &cfg), HM_OK);
    CHECK_RC(hm_put(&hm, "ok", as_value(1), NULL), HM_OK);
    hm_free(&hm);
    CHECK(state.free_calls > 0);

    memset(&state, 0, sizeof(state));
    state.fail_after = 1;
    alloc.ctx = &state;
    memset(&cfg, 0, sizeof(cfg));
    cfg.allocator = alloc;
    CHECK_RC(hm_init_with_config(&hm, &cfg), HM_OK);
    CHECK_RC(hm_put(&hm, "allocation-should-fail", as_value(1), NULL), HM_ERR_OOM);
    hm_free(&hm);
}

static void test_regression_failed_robinhood_insert_is_transactional(void) {
    hashmap hm;
    hm_config cfg;
    void *out = NULL;

    memset(&cfg, 0, sizeof(cfg));
    cfg.hash_fn = robinhood_regression_hash;
    cfg.max_probe_length = (size_t)-1;
    CHECK_RC(hm_init_with_config(&hm, &cfg), HM_OK);
    CHECK_RC(hm_reserve(&hm, 16), HM_OK);
    CHECK_RC(hm_put_borrowed(&hm, "A", as_value(1), NULL), HM_OK);
    CHECK_RC(hm_put_borrowed(&hm, "B", as_value(2), NULL), HM_OK);
    CHECK_RC(hm_put_borrowed(&hm, "C", as_value(3), NULL), HM_OK);
    CHECK(hm_len(&hm) == 3);
    CHECK_RC(hm_validate(&hm), HM_OK);

    hm.config.max_probe_length = 1u;
    CHECK_RC(hm_put_borrowed(&hm, "Z", as_value(26), NULL), HM_ERR_PROBE_LIMIT);

    CHECK(hm_len(&hm) == 3);
    CHECK_RC(hm_get(&hm, "A", &out), HM_OK);
    CHECK(from_value(out) == 1u);
    CHECK_RC(hm_get(&hm, "B", &out), HM_OK);
    CHECK(from_value(out) == 2u);
    CHECK_RC(hm_get(&hm, "C", &out), HM_OK);
    CHECK(from_value(out) == 3u);
    CHECK_RC(hm_get(&hm, "Z", &out), HM_ERR_NOT_FOUND);
    CHECK_RC(hm_validate(&hm), HM_OK);
    hm_free(&hm);
}

static void test_regression_set_key_arena_cannot_corrupt_live_keys(void) {
    hashmap hm;
    char arena[64];
    hm_result rc;
    void *out = NULL;

    CHECK_RC(hm_init_with_key_arena(&hm, arena, sizeof(arena), 1), HM_OK);
    CHECK_RC(hm_put(&hm, "abc", as_value(1), NULL), HM_OK);
    CHECK_RC(hm_get(&hm, "abc", &out), HM_OK);
    CHECK(from_value(out) == 1u);

    rc = hm_set_key_arena(&hm, arena, sizeof(arena), 1);
    if (rc == HM_OK) {
        CHECK_RC(hm_put(&hm, "xyz", as_value(2), NULL), HM_OK);
        CHECK_RC(hm_get(&hm, "abc", &out), HM_OK);
        CHECK(from_value(out) == 1u);
        CHECK_RC(hm_get(&hm, "xyz", &out), HM_OK);
        CHECK(from_value(out) == 2u);
        CHECK_RC(hm_validate(&hm), HM_OK);
    } else {
        CHECK(rc == HM_ERR_INVALID);
        CHECK_RC(hm_get(&hm, "abc", &out), HM_OK);
        CHECK(from_value(out) == 1u);
        CHECK_RC(hm_validate(&hm), HM_OK);
    }
    hm_free(&hm);
}

static void test_regression_clear_free_reset_reclaim_arena_usage(void) {
    hashmap hm;
    char arena[128];

    CHECK_RC(hm_init_with_key_arena(&hm, arena, sizeof(arena), 1), HM_OK);
    CHECK_RC(hm_put(&hm, "first-key", as_value(1), NULL), HM_OK);
    CHECK(hm_key_arena_used(&hm) > 0u);
    hm_clear(&hm);
    CHECK(hm_len(&hm) == 0u);
    CHECK(hm_key_arena_used(&hm) == 0u);
    CHECK_RC(hm_put(&hm, "second-key", as_value(2), NULL), HM_OK);
    CHECK_RC(hm_validate(&hm), HM_OK);
    hm_free(&hm);
    CHECK(hm_len(&hm) == 0u);
    CHECK(hm_key_arena_used(&hm) == 0u);

    CHECK_RC(hm_init_with_key_arena(&hm, arena, sizeof(arena), 1), HM_OK);
    CHECK_RC(hm_put(&hm, "reset-key", as_value(3), NULL), HM_OK);
    CHECK(hm_key_arena_used(&hm) > 0u);
    hm_reset(&hm);
    CHECK(hm_len(&hm) == 0u);
    CHECK(hm_key_arena_used(&hm) == 0u);
    hm_free(&hm);
}

static void test_regression_replacing_borrowed_key_with_copy_owns_key(void) {
    hashmap hm;
    char borrowed[] = "abc";
    void *out = NULL;

    CHECK_RC(hm_init(&hm), HM_OK);
    CHECK_RC(hm_put_borrowed(&hm, borrowed, as_value(1), NULL), HM_OK);
    CHECK_RC(hm_put(&hm, "abc", as_value(2), NULL), HM_OK);
    strcpy(borrowed, "zzz");

    CHECK_RC(hm_get(&hm, "abc", &out), HM_OK);
    CHECK(from_value(out) == 2u);
    CHECK_RC(hm_get(&hm, "zzz", &out), HM_ERR_NOT_FOUND);
    CHECK_RC(hm_validate(&hm), HM_OK);
    hm_free(&hm);
}

static void test_regression_put_many_len_overflow_is_rejected_before_mutation(void) {
    hashmap hm;
    hm_put_item items[2];
    size_t processed = 123u;
    size_t impossible_count;
    hm_result rc;
    void *out = NULL;

    CHECK_RC(hm_init(&hm), HM_OK);
    CHECK_RC(hm_put(&hm, "existing", as_value(1), NULL), HM_OK);
    impossible_count = SIZE_MAX - hm_len(&hm) + 1u;
    items[0].key = "new-key";
    items[0].key_len = 7u;
    items[0].value = as_value(2);
    items[0].old_value = NULL;
    items[1].key = NULL;
    items[1].key_len = 1u;
    items[1].value = as_value(3);
    items[1].old_value = NULL;

    rc = hm_put_many_len(&hm, items, impossible_count, &processed);
    CHECK(rc != HM_OK);
    CHECK(processed == 0u);
    CHECK(hm_len(&hm) == 1u);
    CHECK_RC(hm_get(&hm, "existing", &out), HM_OK);
    CHECK(from_value(out) == 1u);
    CHECK_RC(hm_get(&hm, "new-key", &out), HM_ERR_NOT_FOUND);
    CHECK_RC(hm_validate(&hm), HM_OK);
    hm_free(&hm);
}

static void test_remove_many_len_partial_progress_contract(void) {
    hashmap hm;
    hm_remove_item items[3];
    size_t processed = 999u;
    void *old = NULL;
    void *out = NULL;

    CHECK_RC(hm_init(&hm), HM_OK);
    CHECK_RC(hm_put(&hm, "a", as_value(1), NULL), HM_OK);
    CHECK_RC(hm_put(&hm, "b", as_value(2), NULL), HM_OK);
    items[0].key = "a";
    items[0].key_len = 1u;
    items[0].old_value = &old;
    items[1].key = "missing";
    items[1].key_len = 7u;
    items[1].old_value = NULL;
    items[2].key = "b";
    items[2].key_len = 1u;
    items[2].old_value = NULL;

    CHECK_RC(hm_remove_many_len(&hm, items, 3, &processed), HM_ERR_NOT_FOUND);
    CHECK(processed == 1u);
    CHECK(from_value(old) == 1u);
    CHECK_RC(hm_get(&hm, "a", &out), HM_ERR_NOT_FOUND);
    CHECK_RC(hm_get(&hm, "b", &out), HM_OK);
    CHECK(from_value(out) == 2u);
    CHECK_RC(hm_validate(&hm), HM_OK);
    hm_free(&hm);
}

int main(void) {
    test_basic_put_get_replace_remove();
    test_length_aware_and_empty_keys();
    test_growth_collisions_delete_and_stats();
    test_probe_limit_guard();
    test_reserve_freeze_compact_and_limits();
    test_failed_seed_rehash_is_transactional();
    test_failed_compact_is_transactional();
    test_iterators_upsert_and_bulk();
    test_arena_borrowed_moved_and_destructor();
    test_allocator_failure_path();
    test_regression_failed_robinhood_insert_is_transactional();
    test_regression_set_key_arena_cannot_corrupt_live_keys();
    test_regression_clear_free_reset_reclaim_arena_usage();
    test_regression_replacing_borrowed_key_with_copy_owns_key();
    test_regression_put_many_len_overflow_is_rejected_before_mutation();
    test_remove_many_len_partial_progress_contract();

    if (failures != 0) {
        fprintf(stderr, "blackbox tests failed: %d\n", failures);
        return 1;
    }
    printf("blackbox tests passed\n");
    return 0;
}
C_EOF

echo "[blackbox] compile library object"
"$CC_BIN" "${COMMON_FLAGS[@]}" -I"$TMP_DIR" -c "$TMP_DIR/hashmap.c" -o "$TMP_DIR/hashmap.o"

echo "[blackbox] compile generated test"
"$CC_BIN" "${COMMON_FLAGS[@]}" -I"$TMP_DIR" "$TMP_DIR/hashmap_blackbox_test.c" "$TMP_DIR/hashmap.o" -o "$TMP_DIR/hashmap_blackbox_test"

echo "[blackbox] run"
"$TMP_DIR/hashmap_blackbox_test"
