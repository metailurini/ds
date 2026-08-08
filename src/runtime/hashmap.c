/*
 * hashmap.c
 *
 * Production-leaning embedded string/slice -> void* hash map implementation in C.
 * Implementation file for hashmap.h.
 *
 * Features:
 * - Open addressing with real Robin Hood insertion and linear lookup
 * - Backward-shift deletion, so normal removals do not accumulate tombstones
 * - Length-aware keys: supports C strings and explicit byte slices, including embedded NUL bytes
 * - Seeded, configurable key hashing
 * - Stored per-entry hash and key length to avoid unnecessary comparisons
 * - Configurable load/max-capacity policies
 * - Optional fixed-capacity/no-automatic-growth mode for deterministic embedded use
 * - Custom allocator support for embedded/fail-injection/memory-pool use cases
 * - Correct NULL value handling through hm_get() / hm_get_len() out-parameter APIs
 * - Optional configured value destructor
 * - Mutation-safe iterator, including length-aware iteration
 * - Upsert / compute and get-or-insert APIs to avoid redundant user-side lookups
 * - Validation API for debug/self-test builds
 * - Stats API with max/average probe length and load factors
 * - Owns copied keys; values are caller-owned unless a value destructor is configured/supplied
 *
 * Example programs and tests should include hashmap.h and link this file.
 */

#include "hashmap.h"

#include <stdlib.h>
#include <string.h>


typedef struct hm_lookup {
    size_t slot;
    int found;
    hm_result rc;
} hm_lookup;

typedef struct hm_validation_counts {
    size_t len;
    size_t tombstones;
} hm_validation_counts;

typedef struct hm_upsert_state {
    void *old_value;
    void *new_value;
    int found;
} hm_upsert_state;

static void *hm_default_malloc(size_t size, void *ctx) { (void)ctx; return malloc(size); }
static void *hm_default_calloc(size_t count, size_t size, void *ctx) { (void)ctx; return calloc(count, size); }
static void hm_default_free(void *ptr, void *ctx) { (void)ctx; free(ptr); }

static hm_allocator hm_make_default_allocator(void) {
    hm_allocator a;
    a.malloc_fn = hm_default_malloc;
    a.calloc_fn = hm_default_calloc;
    a.free_fn = hm_default_free;
    a.ctx = NULL;
    return a;
}

static hm_allocator hm_allocator_get(const hashmap *hm) {
    return hm ? hm->config.allocator : hm_make_default_allocator();
}

static uint64_t hm_default_hash(const char *key, size_t key_len, uint64_t seed, void *ctx) {
    uint64_t h = 1469598103934665603ULL ^ seed;
    size_t i;
    (void)ctx;
    for (i = 0; i < key_len; i++) {
        h ^= (unsigned char)key[i];
        h *= 1099511628211ULL;
    }
    h ^= h >> 33; h *= 0xff51afd7ed558ccdULL;
    h ^= h >> 33; h *= 0xc4ceb9fe1a85ec53ULL;
    h ^= h >> 33;
    return h;
}

static int hm_allocator_is_zero(const hm_allocator *a) {
    return a && !a->malloc_fn && !a->calloc_fn && !a->free_fn && !a->ctx;
}

static int hm_allocator_valid_or_zero(const hm_allocator *a) {
    return !a || hm_allocator_is_zero(a) || (a->malloc_fn && a->calloc_fn && a->free_fn);
}

static void hm_config_defaults(hm_config *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->allocator = hm_make_default_allocator();
    cfg->seed = HM_DEFAULT_SEED;
    cfg->hash_fn = hm_default_hash;
    cfg->max_load_percent = HM_DEFAULT_MAX_LOAD_PERCENT;
    cfg->max_tombstone_percent = HM_DEFAULT_MAX_TOMBSTONE_PERCENT;
    cfg->max_probe_length = HM_DEFAULT_MAX_PROBE_LENGTH;
}

static int hm_config_apply_user(hm_config *cfg, const hm_config *src) {
    if (!src) return 1;
    if (!hm_allocator_valid_or_zero(&src->allocator)) return 0;
    if (!hm_allocator_is_zero(&src->allocator)) cfg->allocator = src->allocator;
    if (src->seed != 0) cfg->seed = src->seed;
    if (src->hash_fn) cfg->hash_fn = src->hash_fn;
    cfg->hash_ctx = src->hash_ctx;
    cfg->value_free = src->value_free;
    cfg->value_ctx = src->value_ctx;
    if (src->max_load_percent != 0) cfg->max_load_percent = src->max_load_percent;
    if (src->max_tombstone_percent != 0) cfg->max_tombstone_percent = src->max_tombstone_percent;
    cfg->max_capacity = src->max_capacity;
    if (src->max_probe_length != 0) cfg->max_probe_length = src->max_probe_length;
    cfg->no_growth = src->no_growth ? 1 : 0;
    cfg->key_arena = src->key_arena;
    cfg->key_arena_capacity = src->key_arena_capacity;
    cfg->key_arena_used = src->key_arena_used;
    cfg->key_arena_strict = src->key_arena_strict ? 1 : 0;
    return 1;
}

static int hm_config_validate(const hm_config *cfg) {
    return cfg &&
           cfg->max_load_percent >= 10u && cfg->max_load_percent <= 95u &&
           cfg->max_tombstone_percent <= 95u &&
           (cfg->max_capacity == 0 || cfg->max_capacity >= HM_MIN_CAPACITY);
}

static int hm_config_normalize(hm_config *dst, const hm_config *src) {
    hm_config cfg;
    if (!dst) return 0;
    hm_config_defaults(&cfg);
    if (!hm_config_apply_user(&cfg, src)) return 0;
    if (!hm_config_validate(&cfg)) return 0;
    *dst = cfg;
    return 1;
}

static void *hm_alloc(const hashmap *hm, size_t size) {
    hm_allocator a = hm_allocator_get(hm);
    return a.malloc_fn(size, a.ctx);
}

static void *hm_calloc(const hashmap *hm, size_t count, size_t size) {
    if (size != 0 && count > ((size_t)-1) / size) return NULL;
    hm_allocator a = hm_allocator_get(hm);
    return a.calloc_fn(count, size, a.ctx);
}

static void hm_dealloc(const hashmap *hm, void *ptr) {
    if (!ptr) return;
    hm_allocator a = hm_allocator_get(hm);
    a.free_fn(ptr, a.ctx);
}

static int hm_mul_overflows_size(size_t a, size_t b) { return b != 0 && a > ((size_t)-1) / b; }
static int hm_add_overflows_size(size_t a, size_t b) { return a > ((size_t)-1) - b; }

static int hm_next_pow2_checked(size_t n, size_t *out) {
    size_t p = HM_MIN_CAPACITY;
    if (!out) return 0;
    if (n <= p) { *out = p; return 1; }
    while (p < n) {
        if (p > ((size_t)-1) / 2u) return 0;
        p <<= 1;
    }
    *out = p;
    return 1;
}

static int hm_capacity_allowed(const hashmap *hm, size_t capacity) {
    return hm && (hm->config.max_capacity == 0 || capacity <= hm->config.max_capacity);
}

static hm_result hm_slots_for_len(const hashmap *hm, size_t live_entries, size_t *out_capacity) {
    size_t wanted;
    unsigned max_load;
    size_t entries = live_entries ? live_entries : 1u;
    if (!hm || !out_capacity) return HM_ERR_INVALID;
    max_load = hm->config.max_load_percent ? hm->config.max_load_percent : HM_DEFAULT_MAX_LOAD_PERCENT;
    if (max_load < 10u || max_load > 95u) return HM_ERR_INVALID;
    if (hm_mul_overflows_size(entries, 100u)) return HM_ERR_OOM;
    wanted = (entries * 100u + (size_t)max_load - 1u) / (size_t)max_load;
    if (wanted < HM_MIN_CAPACITY) wanted = HM_MIN_CAPACITY;
    if (!hm_next_pow2_checked(wanted, &wanted)) return HM_ERR_OOM;
    if (!hm_capacity_allowed(hm, wanted)) return HM_ERR_FULL;
    *out_capacity = wanted;
    return HM_OK;
}

static void hm_reset_empty_with_config(hashmap *hm, const hm_config *config) {
    hm_config cfg;
    if (!hm) return;
    if (!hm_config_normalize(&cfg, config)) hm_config_normalize(&cfg, NULL);
    hm->entries = NULL;
    hm->capacity = 0;
    hm->len = 0;
    hm->tombstones = 0;
    hm->version = 0;
    hm->config = cfg;
}

static uint64_t hm_hash_key_len(const hashmap *hm, const char *key, size_t key_len) {
    return hm->config.hash_fn(key, key_len, hm->config.seed, hm->config.hash_ctx);
}

static char *hm_key_dup_owned_storage(hashmap *hm, const char *key, size_t key_len, hm_key_storage *storage) {
    char *copy;
    size_t need;
    if (storage) *storage = HM_KEY_STORAGE_HEAP;
    if (!key && key_len != 0) return NULL;
    if (key_len == (size_t)-1) return NULL;
    need = key_len + 1u;
    if (hm && hm->config.key_arena && hm->config.key_arena_used <= hm->config.key_arena_capacity &&
        need <= hm->config.key_arena_capacity - hm->config.key_arena_used) {
        copy = hm->config.key_arena + hm->config.key_arena_used;
        hm->config.key_arena_used += need;
        if (storage) *storage = HM_KEY_STORAGE_ARENA;
    } else {
        if (hm && hm->config.key_arena && hm->config.key_arena_strict) return NULL;
        copy = (char *)hm_alloc(hm, need);
        if (!copy) return NULL;
        if (storage) *storage = HM_KEY_STORAGE_HEAP;
    }
    if (key_len != 0) memcpy(copy, key, key_len);
    copy[key_len] = '\0';
    return copy;
}


static void hm_key_release(hashmap *hm, hm_entry *e) {
    if (!hm || !e || !e->key) return;
    if (e->key_storage == HM_KEY_STORAGE_HEAP) hm_dealloc(hm, e->key);
}

static size_t hm_probe_distance(size_t capacity, uint64_t hash, size_t index) {
    size_t ideal = (size_t)hash & (capacity - 1u);
    return (index >= ideal) ? (index - ideal) : (capacity - ideal + index);
}

static int hm_probe_limit_exceeded(const hashmap *hm, size_t probe_distance) {
    return hm && hm->config.max_probe_length != (size_t)-1 && probe_distance > hm->config.max_probe_length;
}

static int hm_key_matches(const hm_entry *e, const char *key, size_t key_len, uint64_t hash) {
    return e->state == HM_OCCUPIED && e->hash == hash && e->key_len == key_len &&
           (key_len == 0 || memcmp(e->key, key, key_len) == 0);
}

static hm_lookup hm_lookup_make(hm_result rc, size_t slot, int found) {
    hm_lookup out;
    out.slot = slot;
    out.found = found;
    out.rc = rc;
    return out;
}

static hm_lookup hm_lookup_hash_len(const hashmap *hm, const char *key, size_t key_len, uint64_t hash,
                                    int want_insert_slot) {
    size_t mask;
    size_t idx;
    size_t probes;
    size_t first_deleted = (size_t)-1;
    if (!hm || !hm->entries || hm->capacity == 0) return hm_lookup_make(HM_ERR_INVALID, 0, 0);
    mask = hm->capacity - 1u;
    idx = (size_t)hash & mask;
    for (probes = 0; probes < hm->capacity; probes++) {
        hm_entry *e = &hm->entries[idx];
        if (hm_probe_limit_exceeded(hm, probes)) return hm_lookup_make(HM_ERR_PROBE_LIMIT, idx, 0);
        if (e->state == HM_EMPTY) {
            return hm_lookup_make(want_insert_slot ? HM_OK : HM_ERR_NOT_FOUND,
                                  (first_deleted != (size_t)-1) ? first_deleted : idx, 0);
        }
        if (e->state == HM_DELETED) {
            if (first_deleted == (size_t)-1) first_deleted = idx;
        } else if (hm_key_matches(e, key, key_len, hash)) {
            return hm_lookup_make(HM_OK, idx, 1);
        } else if (hm_probe_distance(hm->capacity, e->hash, idx) < probes) {
            return hm_lookup_make(want_insert_slot ? HM_OK : HM_ERR_NOT_FOUND,
                                  (first_deleted != (size_t)-1) ? first_deleted : idx, 0);
        }
        idx = (idx + 1u) & mask;
    }
    if (first_deleted != (size_t)-1 && want_insert_slot) return hm_lookup_make(HM_OK, first_deleted, 0);
    return hm_lookup_make(HM_ERR_FULL, 0, 0);
}

static hm_lookup hm_find_existing_hash_len(const hashmap *hm, const char *key, size_t key_len, uint64_t hash) {
    return hm_lookup_hash_len(hm, key, key_len, hash, 0);
}

static int hm_slot_available(const hm_entry *entry) {
    return entry && (entry->state == HM_EMPTY || entry->state == HM_DELETED);
}

static int hm_should_steal_slot(size_t incoming_dist, size_t occupant_dist) {
    return occupant_dist < incoming_dist;
}

static void hm_swap_entries(hm_entry *a, hm_entry *b) {
    hm_entry tmp = *a;
    *a = *b;
    *b = tmp;
}

static void hm_place_entry(hashmap *hm, hm_entry *slot, const hm_entry *incoming) {
    if (slot->state == HM_DELETED && hm->tombstones != 0) hm->tombstones--;
    *slot = *incoming;
    hm->len++;
}

static hm_result hm_check_robin_hood_insert(const hashmap *hm, hm_entry incoming) {
    size_t mask;
    size_t idx;
    size_t dist = 0;
    size_t probes;
    if (!hm || !hm->entries || hm->capacity == 0 || incoming.state != HM_OCCUPIED) return HM_ERR_INVALID;
    mask = hm->capacity - 1u;
    idx = (size_t)incoming.hash & mask;
    for (probes = 0; probes < hm->capacity; probes++) {
        const hm_entry *cur = &hm->entries[idx];
        if (hm_probe_limit_exceeded(hm, dist)) return HM_ERR_PROBE_LIMIT;
        if (hm_slot_available(cur)) return HM_OK;
        if (hm_should_steal_slot(dist, hm_probe_distance(hm->capacity, cur->hash, idx))) {
            incoming = *cur;
            dist = hm_probe_distance(hm->capacity, incoming.hash, idx);
        }
        idx = (idx + 1u) & mask;
        dist++;
    }
    return HM_ERR_FULL;
}

static hm_result hm_insert_entry_robin_hood(hashmap *hm, hm_entry incoming) {
    size_t mask;
    size_t idx;
    size_t dist = 0;
    size_t probes;
    hm_result rc;
    if (!hm || !hm->entries || hm->capacity == 0 || incoming.state != HM_OCCUPIED) return HM_ERR_INVALID;
    rc = hm_check_robin_hood_insert(hm, incoming);
    if (rc != HM_OK) return rc;
    mask = hm->capacity - 1u;
    idx = (size_t)incoming.hash & mask;
    for (probes = 0; probes < hm->capacity; probes++) {
        hm_entry *cur = &hm->entries[idx];
        if (hm_probe_limit_exceeded(hm, dist)) return HM_ERR_PROBE_LIMIT;
        if (hm_slot_available(cur)) {
            hm_place_entry(hm, cur, &incoming);
            return HM_OK;
        }
        if (hm_should_steal_slot(dist, hm_probe_distance(hm->capacity, cur->hash, idx))) {
            hm_swap_entries(&incoming, cur);
            dist = hm_probe_distance(hm->capacity, incoming.hash, idx);
        }
        idx = (idx + 1u) & mask;
        dist++;
    }
    return HM_ERR_FULL;
}

static void hm_backward_shift_delete(hashmap *hm, size_t hole) {
    size_t mask;
    size_t next;
    if (!hm || !hm->entries || hm->capacity == 0) return;
    mask = hm->capacity - 1u;
    next = (hole + 1u) & mask;
    while (hm->entries[next].state == HM_OCCUPIED &&
           hm_probe_distance(hm->capacity, hm->entries[next].hash, next) > 0u) {
        hm->entries[hole] = hm->entries[next];
        hole = next;
        next = (next + 1u) & mask;
    }
    memset(&hm->entries[hole], 0, sizeof(hm->entries[hole]));
}

static void hm_value_replace(hashmap *hm, void *previous, void *value, void **old_value) {
    if (old_value) *old_value = previous;
    else if (hm && hm->config.value_free && previous != value) hm->config.value_free(previous, hm->config.value_ctx);
}

static hm_result hm_remove_slot(hashmap *hm, size_t slot, void **old_value) {
    hm_entry *e;
    if (old_value) *old_value = NULL;
    if (!hm || !hm->entries || slot >= hm->capacity) return HM_ERR_INVALID;
    e = &hm->entries[slot];
    if (e->state != HM_OCCUPIED) return HM_ERR_INVALID;
    if (old_value) *old_value = e->value;
    else if (hm->config.value_free) hm->config.value_free(e->value, hm->config.value_ctx);
    hm_key_release(hm, e);
    hm_backward_shift_delete(hm, slot);
    hm->len--;
    hm->version++;
    return HM_OK;
}

static hm_result hm_init_with_capacity(hashmap *hm, size_t capacity) {
    size_t rounded;
    if (!hm) return HM_ERR_INVALID;
    if (!hm_next_pow2_checked(capacity, &rounded)) return HM_ERR_OOM;
    if (!hm_capacity_allowed(hm, rounded)) return HM_ERR_FULL;
    hm->entries = (hm_entry *)hm_calloc(hm, rounded, sizeof(hm_entry));
    if (!hm->entries) {
        hm_config preserved = hm->config;
        hm_reset_empty_with_config(hm, &preserved);
        return HM_ERR_OOM;
    }
    hm->capacity = rounded;
    hm->len = 0;
    hm->tombstones = 0;
    hm->version++;
    return HM_OK;
}

hm_result hm_init_with_config(hashmap *hm, const hm_config *config) {
    hm_config cfg;
    if (!hm) return HM_ERR_INVALID;
    if (!hm_config_normalize(&cfg, config)) return HM_ERR_INVALID;
    hm_reset_empty_with_config(hm, &cfg);
    return hm_init_with_capacity(hm, HM_MIN_CAPACITY);
}

hm_result hm_init(hashmap *hm) { return hm_init_with_config(hm, NULL); }

hm_result hm_init_with_allocator(hashmap *hm, const hm_allocator *allocator) {
    hm_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    if (allocator) cfg.allocator = *allocator;
    return hm_init_with_config(hm, &cfg);
}

hm_result hm_init_with_seed(hashmap *hm, uint64_t seed) {
    hm_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.seed = seed ? seed : HM_DEFAULT_SEED;
    return hm_init_with_config(hm, &cfg);
}

hm_result hm_init_with_allocator_and_seed(hashmap *hm, const hm_allocator *allocator, uint64_t seed) {
    hm_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    if (allocator) cfg.allocator = *allocator;
    cfg.seed = seed ? seed : HM_DEFAULT_SEED;
    return hm_init_with_config(hm, &cfg);
}

hm_result hm_init_with_key_arena(hashmap *hm, void *arena, size_t arena_capacity, int strict) {
    hm_config cfg;
    if (!arena && arena_capacity != 0) return HM_ERR_INVALID;
    memset(&cfg, 0, sizeof(cfg));
    cfg.key_arena = (char *)arena;
    cfg.key_arena_capacity = arena_capacity;
    cfg.key_arena_strict = strict ? 1 : 0;
    return hm_init_with_config(hm, &cfg);
}

hm_result hm_set_key_arena(hashmap *hm, void *arena, size_t arena_capacity, int strict) {
    if (!hm || (!arena && arena_capacity != 0)) return HM_ERR_INVALID;
    if (hm->len != 0) return HM_ERR_INVALID;
    hm->config.key_arena = (char *)arena;
    hm->config.key_arena_capacity = arena_capacity;
    hm->config.key_arena_used = 0;
    hm->config.key_arena_strict = strict ? 1 : 0;
    hm->version++;
    return HM_OK;
}

size_t hm_key_arena_used(const hashmap *hm) { return hm ? hm->config.key_arena_used : 0; }
size_t hm_key_arena_capacity(const hashmap *hm) { return hm ? hm->config.key_arena_capacity : 0; }

static void hm_destroy_entries(hashmap *hm, hm_value_free_fn value_free, void *value_ctx) {
    size_t i;
    if (!hm || !hm->entries) return;
    for (i = 0; i < hm->capacity; i++) {
        hm_entry *e = &hm->entries[i];
        if (e->state == HM_OCCUPIED) {
            if (value_free) value_free(e->value, value_ctx);
            hm_key_release(hm, e);
        }
    }
}

void hm_clear_with_values(hashmap *hm, hm_value_free_fn value_free, void *value_ctx) {
    if (!hm) return;
    hm_destroy_entries(hm, value_free, value_ctx);
    if (hm->entries && hm->capacity != 0) memset(hm->entries, 0, hm->capacity * sizeof(hm_entry));
    hm->len = 0;
    hm->tombstones = 0;
    hm->config.key_arena_used = 0;
    hm->version++;
}

void hm_clear(hashmap *hm) { if (hm) hm_clear_with_values(hm, hm->config.value_free, hm->config.value_ctx); }

void hm_free_with_values(hashmap *hm, hm_value_free_fn value_free, void *value_ctx) {
    hm_config cfg;
    if (!hm) return;
    cfg = hm->config;
    cfg.key_arena_used = 0;
    hm_destroy_entries(hm, value_free, value_ctx);
    hm_dealloc(hm, hm->entries);
    hm_reset_empty_with_config(hm, &cfg);
    hm->version++;
}

void hm_free(hashmap *hm) { if (hm) hm_free_with_values(hm, hm->config.value_free, hm->config.value_ctx); }
void hm_reset(hashmap *hm) { if (hm) hm_free_with_values(hm, hm->config.value_free, hm->config.value_ctx); }

static hm_result hm_rebuild_init_target(const hashmap *hm, hashmap *next, size_t new_capacity,
                                        const hm_config *config) {
    if (!hm || !next) return HM_ERR_INVALID;
    if (!hm_capacity_allowed(hm, new_capacity)) return HM_ERR_FULL;
    hm_reset_empty_with_config(next, config ? config : &hm->config);
    return hm_init_with_capacity(next, new_capacity);
}

static hm_result hm_rebuild_move_entries(hashmap *hm, hashmap *next, int recompute_hashes) {
    size_t i;
    hm_result rc;
    if (!hm || !next) return HM_ERR_INVALID;
    for (i = 0; i < hm->capacity; i++) {
        hm_entry *old = &hm->entries[i];
        if (old->state == HM_OCCUPIED) {
            hm_entry moved = *old;
            if (recompute_hashes) moved.hash = hm_hash_key_len(next, old->key, old->key_len);
            rc = hm_insert_entry_robin_hood(next, moved);
            if (rc != HM_OK) return rc;
        }
    }
    return HM_OK;
}

static void hm_rebuild_abort(hashmap *next) {
    if (next) hm_dealloc(next, next->entries);
}

static void hm_rebuild_commit(hashmap *hm, hashmap *next) {
    hm_dealloc(hm, hm->entries);
    next->version = hm->version + 1u;
    *hm = *next;
}

static hm_result hm_rebuild(hashmap *hm, size_t new_capacity, const hm_config *config, int recompute_hashes) {
    hashmap next;
    hm_result rc;
    rc = hm_rebuild_init_target(hm, &next, new_capacity, config);
    if (rc != HM_OK) return rc;
    rc = hm_rebuild_move_entries(hm, &next, recompute_hashes);
    if (rc != HM_OK) {
        hm_rebuild_abort(&next);
        return rc;
    }
    hm_rebuild_commit(hm, &next);
    return HM_OK;
}

static hm_result hm_rehash(hashmap *hm, size_t new_capacity) {
    return hm_rebuild(hm, new_capacity, NULL, 0);
}

static int hm_insert_needs_initial_table(const hashmap *hm) {
    return hm && (!hm->entries || hm->capacity == 0);
}

static int hm_insert_would_exceed_load(const hashmap *hm) {
    size_t used;
    size_t threshold;
    if (!hm || hm_add_overflows_size(hm->len, hm->tombstones) ||
        hm_add_overflows_size(hm->len + hm->tombstones, 1u)) return 1;
    used = hm->len + hm->tombstones + 1u;
    threshold = (hm->capacity * (size_t)hm->config.max_load_percent) / 100u;
    if (threshold == 0) threshold = 1;
    return used > threshold;
}

static int hm_should_rehash_for_tombstones(const hashmap *hm) {
    return hm && hm->tombstones > 0;
}

static hm_result hm_next_growth_capacity(const hashmap *hm, size_t *out_capacity) {
    size_t new_capacity;
    if (!hm || !out_capacity) return HM_ERR_INVALID;
    if (hm->capacity > ((size_t)-1) / 2u) return HM_ERR_OOM;
    new_capacity = hm->capacity * 2u;
    if (!hm_capacity_allowed(hm, new_capacity)) return HM_ERR_FULL;
    *out_capacity = new_capacity;
    return HM_OK;
}

static hm_result hm_ensure_capacity_for_insert(hashmap *hm) {
    size_t new_capacity;
    hm_result rc;
    if (!hm) return HM_ERR_INVALID;
    if (hm_insert_needs_initial_table(hm)) return hm->config.no_growth ? HM_ERR_FULL : hm_init_with_capacity(hm, HM_MIN_CAPACITY);
    if (hm_add_overflows_size(hm->len, hm->tombstones) || hm_add_overflows_size(hm->len + hm->tombstones, 1u)) return HM_ERR_OOM;
    if (!hm_insert_would_exceed_load(hm)) return HM_OK;
    if (hm_should_rehash_for_tombstones(hm)) return hm->config.no_growth ? HM_ERR_FULL : hm_rehash(hm, hm->capacity);
    if (hm->config.no_growth) return HM_ERR_FULL;
    rc = hm_next_growth_capacity(hm, &new_capacity);
    if (rc != HM_OK) return rc;
    return hm_rehash(hm, new_capacity);
}

hm_result hm_reserve(hashmap *hm, size_t min_capacity) {
    size_t wanted;
    hm_result rc;
    if (!hm) return HM_ERR_INVALID;
    rc = hm_slots_for_len(hm, min_capacity, &wanted);
    if (rc != HM_OK) return rc;
    if (hm->entries && hm->capacity >= wanted) return HM_OK;
    if (!hm->entries) return hm_init_with_capacity(hm, wanted);
    return hm_rehash(hm, wanted);
}

static hm_result hm_set_frozen(hashmap *hm, int frozen) {
    if (!hm) return HM_ERR_INVALID;
    hm->config.no_growth = frozen ? 1 : 0;
    hm->version++;
    return HM_OK;
}

hm_result hm_freeze_capacity(hashmap *hm) { return hm_set_frozen(hm, 1); }
hm_result hm_unfreeze_capacity(hashmap *hm) { return hm_set_frozen(hm, 0); }
int hm_is_frozen(const hashmap *hm) { return hm ? hm->config.no_growth != 0 : 0; }

hm_result hm_compact(hashmap *hm) {
    size_t wanted;
    hm_result rc;
    if (!hm || !hm->entries) return HM_ERR_INVALID;
    rc = hm_slots_for_len(hm, hm->len ? hm->len : 1u, &wanted);
    if (rc != HM_OK) return rc;
    return hm_rehash(hm, wanted);
}

hm_result hm_shrink_to_fit(hashmap *hm) { return hm_compact(hm); }

static hm_result hm_make_insert_entry(hashmap *hm, const char *key, size_t key_len, void *value,
                                      uint64_t hash, hm_key_storage mode, hm_entry *out) {
    hm_entry incoming;
    if (!out) return HM_ERR_INVALID;
    memset(&incoming, 0, sizeof(incoming));
    if (mode == HM_KEY_STORAGE_BORROWED) {
        incoming.key = (char *)key;
        incoming.key_storage = HM_KEY_STORAGE_BORROWED;
    } else if (mode == HM_KEY_STORAGE_HEAP && key) {
        incoming.key = (char *)key;
        incoming.key_storage = HM_KEY_STORAGE_HEAP;
    } else {
        incoming.key = hm_key_dup_owned_storage(hm, key, key_len, &incoming.key_storage);
    }
    if (!incoming.key && key_len != 0) return HM_ERR_OOM;
    incoming.key_len = key_len;
    incoming.value = value;
    incoming.hash = hash;
    incoming.state = HM_OCCUPIED;
    *out = incoming;
    return HM_OK;
}

static hm_result hm_replace_existing_key_storage(hashmap *hm, hm_entry *e, const char *key,
                                                 size_t key_len, hm_key_storage mode) {
    char *copy;
    hm_key_storage storage;
    if (!hm || !e || e->state != HM_OCCUPIED) return HM_ERR_INVALID;

    if (mode == HM_KEY_STORAGE_HEAP) {
        if (key && key != e->key) {
            hm_key_release(hm, e);
            e->key = (char *)key;
            e->key_storage = HM_KEY_STORAGE_HEAP;
        }
        return HM_OK;
    }

    if (mode == HM_KEY_STORAGE_BORROWED) {
        if (e->key_storage == HM_KEY_STORAGE_BORROWED) e->key = (char *)key;
        return HM_OK;
    }

    if (e->key_storage != HM_KEY_STORAGE_BORROWED) return HM_OK;

    copy = hm_key_dup_owned_storage(hm, key, key_len, &storage);
    if (!copy && key_len != 0) return HM_ERR_OOM;
    e->key = copy;
    e->key_storage = storage;
    return HM_OK;
}

static hm_result hm_put_replace_existing(hashmap *hm, size_t slot, const char *key, void *value,
                                         size_t key_len, void **old_value, hm_key_storage mode) {
    hm_entry *e;
    hm_result rc;
    if (!hm || !hm->entries || slot >= hm->capacity) return HM_ERR_INVALID;
    e = &hm->entries[slot];
    if (e->state != HM_OCCUPIED) return HM_ERR_INVALID;
    rc = hm_replace_existing_key_storage(hm, e, key, key_len, mode);
    if (rc != HM_OK) return rc;
    hm_value_replace(hm, e->value, value, old_value);
    e->value = value;
    hm->version++;
    return HM_OK;
}

static void hm_put_cleanup_failed_insert(hashmap *hm, hm_entry *incoming, const char *key, hm_key_storage mode) {
    if (!incoming) return;
    hm_key_release(hm, incoming);
    if (mode == HM_KEY_STORAGE_HEAP && key && incoming->key != key) hm_dealloc(hm, (void *)key);
}

static hm_result hm_put_len_storage(hashmap *hm, const char *key, size_t key_len, void *value, void **old_value, hm_key_storage mode) {
    uint64_t hash;
    hm_lookup existing;
    hm_result rc;
    hm_entry incoming;
    if (old_value) *old_value = NULL;
    if (!hm || (!key && key_len != 0)) return HM_ERR_INVALID;
    hash = hm_hash_key_len(hm, key, key_len);

    if (hm->entries && hm->capacity != 0) {
        existing = hm_find_existing_hash_len(hm, key, key_len, hash);
        if (existing.rc != HM_OK && existing.rc != HM_ERR_NOT_FOUND) return existing.rc;
        if (existing.found) return hm_put_replace_existing(hm, existing.slot, key, value, key_len, old_value, mode);
    }

    rc = hm_ensure_capacity_for_insert(hm);
    if (rc != HM_OK) return rc;
    rc = hm_make_insert_entry(hm, key, key_len, value, hash, mode, &incoming);
    if (rc != HM_OK) return rc;
    rc = hm_insert_entry_robin_hood(hm, incoming);
    if (rc != HM_OK) {
        hm_put_cleanup_failed_insert(hm, &incoming, key, mode);
        return rc;
    }
    hm->version++;
    return HM_OK;
}

hm_result hm_put_len(hashmap *hm, const char *key, size_t key_len, void *value, void **old_value) {
    return hm_put_len_storage(hm, key, key_len, value, old_value, (hm_key_storage)-1);
}

hm_result hm_put_borrowed_len(hashmap *hm, const char *key, size_t key_len, void *value, void **old_value) {
    if (!key && key_len != 0) return HM_ERR_INVALID;
    return hm_put_len_storage(hm, key, key_len, value, old_value, HM_KEY_STORAGE_BORROWED);
}

hm_result hm_put_borrowed(hashmap *hm, const char *key, void *value, void **old_value) {
    if (!key) return HM_ERR_INVALID;
    return hm_put_borrowed_len(hm, key, strlen(key), value, old_value);
}

hm_result hm_put_move_key_len(hashmap *hm, char *owned_key, size_t key_len, void *value, void **old_value) {
    if (!owned_key && key_len != 0) return HM_ERR_INVALID;
    return hm_put_len_storage(hm, owned_key, key_len, value, old_value, HM_KEY_STORAGE_HEAP);
}

hm_result hm_put_move_key(hashmap *hm, char *owned_key, void *value, void **old_value) {
    if (!owned_key) return HM_ERR_INVALID;
    return hm_put_move_key_len(hm, owned_key, strlen(owned_key), value, old_value);
}

hm_result hm_put(hashmap *hm, const char *key, void *value, void **old_value) {
    if (!key) return HM_ERR_INVALID;
    return hm_put_len(hm, key, strlen(key), value, old_value);
}

hm_result hm_get_len(const hashmap *hm, const char *key, size_t key_len, void **out_value) {
    uint64_t hash;
    hm_lookup found;
    if (out_value) *out_value = NULL;
    if (!hm || !hm->entries || (!key && key_len != 0) || hm->capacity == 0) return HM_ERR_INVALID;
    hash = hm_hash_key_len(hm, key, key_len);
    found = hm_find_existing_hash_len(hm, key, key_len, hash);
    if (found.rc != HM_OK) return found.rc;
    if (out_value) *out_value = hm->entries[found.slot].value;
    return HM_OK;
}

hm_result hm_get(const hashmap *hm, const char *key, void **out_value) {
    if (!key) { if (out_value) *out_value = NULL; return HM_ERR_INVALID; }
    return hm_get_len(hm, key, strlen(key), out_value);
}

int hm_contains_key_len(const hashmap *hm, const char *key, size_t key_len) { return hm_get_len(hm, key, key_len, NULL) == HM_OK; }
int hm_contains_key(const hashmap *hm, const char *key) { return key && hm_contains_key_len(hm, key, strlen(key)); }

static hm_result hm_upsert_find_current(hashmap *hm, const char *key, size_t key_len,
                                        hm_upsert_state *state) {
    uint64_t hash;
    hm_lookup existing;
    if (!state) return HM_ERR_INVALID;
    memset(state, 0, sizeof(*state));
    if (!hm || (!key && key_len != 0)) return HM_ERR_INVALID;
    if (!hm->entries || hm->capacity == 0) return HM_OK;
    hash = hm_hash_key_len(hm, key, key_len);
    existing = hm_find_existing_hash_len(hm, key, key_len, hash);
    if (existing.rc != HM_OK && existing.rc != HM_ERR_NOT_FOUND) return existing.rc;
    state->found = existing.found;
    if (state->found) state->old_value = hm->entries[existing.slot].value;
    state->new_value = state->old_value;
    return HM_OK;
}

static hm_result hm_upsert_call_update(const char *key, size_t key_len, hm_upsert_fn fn,
                                       void *ctx, hm_upsert_state *state) {
    if (!fn || !state) return HM_ERR_INVALID;
    return fn(key, key_len, state->found, state->old_value, &state->new_value, ctx);
}

static hm_result hm_upsert_commit(hashmap *hm, const char *key, size_t key_len,
                                  hm_upsert_state *state) {
    hm_result rc;
    void *replaced = NULL;
    if (!state) return HM_ERR_INVALID;
    rc = hm_put_len(hm, key, key_len, state->new_value, state->found ? &replaced : NULL);
    if (rc != HM_OK) return rc;
    if (state->found && hm->config.value_free && replaced != state->new_value) {
        hm->config.value_free(replaced, hm->config.value_ctx);
    }
    return HM_OK;
}

hm_result hm_upsert_len(hashmap *hm, const char *key, size_t key_len, hm_upsert_fn fn, void *ctx, void **new_value) {
    hm_upsert_state state;
    hm_result rc;
    if (new_value) *new_value = NULL;
    if (!hm || !fn || (!key && key_len != 0)) return HM_ERR_INVALID;
    rc = hm_upsert_find_current(hm, key, key_len, &state);
    if (rc != HM_OK) return rc;
    rc = hm_upsert_call_update(key, key_len, fn, ctx, &state);
    if (rc != HM_OK) return rc;
    rc = hm_upsert_commit(hm, key, key_len, &state);
    if (rc != HM_OK) return rc;
    if (new_value) *new_value = state.new_value;
    return HM_OK;
}

hm_result hm_upsert(hashmap *hm, const char *key, hm_upsert_fn fn, void *ctx, void **new_value) {
    if (!key) { if (new_value) *new_value = NULL; return HM_ERR_INVALID; }
    return hm_upsert_len(hm, key, strlen(key), fn, ctx, new_value);
}

static int hm_lookup_result_allows_insert(const hashmap *hm, hm_result rc) {
    return rc == HM_ERR_NOT_FOUND ||
           (rc == HM_ERR_INVALID && hm && (!hm->entries || hm->capacity == 0));
}

hm_result hm_get_or_insert_len(hashmap *hm, const char *key, size_t key_len, void *default_value, void **out_value, int *inserted) {
    void *existing = NULL;
    hm_result rc;
    if (out_value) *out_value = NULL;
    if (inserted) *inserted = 0;
    if (!hm || (!key && key_len != 0)) return HM_ERR_INVALID;
    rc = hm_get_len(hm, key, key_len, &existing);
    if (rc == HM_OK) {
        if (out_value) *out_value = existing;
        return HM_OK;
    }
    if (!hm_lookup_result_allows_insert(hm, rc)) return rc;
    rc = hm_put_len(hm, key, key_len, default_value, NULL);
    if (rc != HM_OK) return rc;
    if (out_value) *out_value = default_value;
    if (inserted) *inserted = 1;
    return HM_OK;
}

hm_result hm_get_or_insert(hashmap *hm, const char *key, void *default_value, void **out_value, int *inserted) {
    if (!key) { if (out_value) *out_value = NULL; if (inserted) *inserted = 0; return HM_ERR_INVALID; }
    return hm_get_or_insert_len(hm, key, strlen(key), default_value, out_value, inserted);
}

hm_result hm_put_many_len(hashmap *hm, const hm_put_item *items, size_t count, size_t *processed) {
    size_t i;
    hm_result rc;
    if (processed) *processed = 0;
    if (!hm || (!items && count != 0)) return HM_ERR_INVALID;
    if (hm_add_overflows_size(hm->len, count)) return HM_ERR_OOM;
    if (count != 0) {
        rc = hm_reserve(hm, hm->len + count);
        if (rc != HM_OK && rc != HM_ERR_FULL) return rc;
    }
    for (i = 0; i < count; i++) {
        rc = hm_put_len(hm, items[i].key, items[i].key_len, items[i].value, items[i].old_value);
        if (rc != HM_OK) { if (processed) *processed = i; return rc; }
    }
    if (processed) *processed = count;
    return HM_OK;
}

hm_result hm_remove_many_len(hashmap *hm, const hm_remove_item *items, size_t count, size_t *processed) {
    size_t i;
    hm_result rc;
    if (processed) *processed = 0;
    if (!hm || (!items && count != 0)) return HM_ERR_INVALID;
    for (i = 0; i < count; i++) {
        rc = hm_remove_len(hm, items[i].key, items[i].key_len, items[i].old_value);
        if (rc != HM_OK) { if (processed) *processed = i; return rc; }
    }
    if (processed) *processed = count;
    return HM_OK;
}

hm_result hm_remove_len(hashmap *hm, const char *key, size_t key_len, void **old_value) {
    uint64_t hash;
    hm_lookup found;
    if (old_value) *old_value = NULL;
    if (!hm || !hm->entries || (!key && key_len != 0)) return HM_ERR_INVALID;
    hash = hm_hash_key_len(hm, key, key_len);
    found = hm_find_existing_hash_len(hm, key, key_len, hash);
    if (found.rc != HM_OK) return found.rc;
    return hm_remove_slot(hm, found.slot, old_value);
}

hm_result hm_remove(hashmap *hm, const char *key, void **old_value) {
    if (!key) { if (old_value) *old_value = NULL; return HM_ERR_INVALID; }
    return hm_remove_len(hm, key, strlen(key), old_value);
}

size_t hm_len(const hashmap *hm) { return hm ? hm->len : 0; }
size_t hm_capacity(const hashmap *hm) { return hm ? hm->capacity : 0; }
size_t hm_tombstones(const hashmap *hm) { return hm ? hm->tombstones : 0; }
uint64_t hm_seed(const hashmap *hm) { return hm ? hm->config.seed : 0; }

hm_result hm_set_seed(hashmap *hm, uint64_t seed) {
    hm_config cfg;
    uint64_t new_seed;
    if (!hm) return HM_ERR_INVALID;
    if (hm->config.no_growth && hm->entries && hm->len != 0) return HM_ERR_FULL;
    new_seed = seed ? seed : HM_DEFAULT_SEED;
    if (!hm->entries || hm->len == 0) { hm->config.seed = new_seed; hm->version++; return HM_OK; }
    cfg = hm->config;
    cfg.seed = new_seed;
    return hm_rebuild(hm, hm->capacity, &cfg, 1);
}

static size_t hm_percentile_rank(size_t len, unsigned percentile) {
    size_t rank = (len / 100u) * (size_t)percentile + ((len % 100u) * (size_t)percentile + 99u) / 100u;
    return rank ? rank : 1u;
}

static void hm_stats_init_basic(const hashmap *hm, hm_stats *out) {
    memset(out, 0, sizeof(*out));
    out->capacity = hm->capacity;
    out->len = hm->len;
    out->tombstones = hm->tombstones;
}

static void hm_stats_collect_probe_totals(const hashmap *hm, size_t *total_probe, size_t *max_probe) {
    size_t i;
    *total_probe = 0;
    *max_probe = 0;
    for (i = 0; i < hm->capacity; i++) {
        const hm_entry *e = &hm->entries[i];
        if (e->state == HM_OCCUPIED) {
            size_t probe = hm_probe_distance(hm->capacity, e->hash, i);
            if (probe > *max_probe) *max_probe = probe;
            *total_probe += probe;
        }
    }
}

static void hm_stats_find_probe_percentiles(const hashmap *hm, size_t max_probe, hm_stats *out) {
    size_t threshold;
    size_t p50_rank = hm_percentile_rank(hm->len, 50u);
    size_t p95_rank = hm_percentile_rank(hm->len, 95u);
    size_t p99_rank = hm_percentile_rank(hm->len, 99u);
    int p50_found = 0;
    int p95_found = 0;
    int p99_found = 0;
    for (threshold = 0; threshold <= max_probe; threshold++) {
        size_t i;
        size_t count = 0;
        for (i = 0; i < hm->capacity; i++) {
            const hm_entry *e = &hm->entries[i];
            if (e->state == HM_OCCUPIED && hm_probe_distance(hm->capacity, e->hash, i) <= threshold) count++;
        }
        if (!p50_found && count >= p50_rank) { out->p50_probe = threshold; p50_found = 1; }
        if (!p95_found && count >= p95_rank) { out->p95_probe = threshold; p95_found = 1; }
        if (!p99_found && count >= p99_rank) { out->p99_probe = threshold; p99_found = 1; }
        if (count >= p99_rank) break;
    }
}

static void hm_stats_finish_loads(const hashmap *hm, hm_stats *out, size_t total_probe, size_t max_probe) {
    out->max_probe = max_probe;
    out->avg_probe = hm->len ? ((double)total_probe / (double)hm->len) : 0.0;
    out->live_load = hm->capacity ? ((double)hm->len / (double)hm->capacity) : 0.0;
    out->used_load = hm->capacity ? ((double)(hm->len + hm->tombstones) / (double)hm->capacity) : 0.0;
}

hm_result hm_stats_get(const hashmap *hm, hm_stats *out) {
    size_t total_probe;
    size_t max_probe;
    if (!hm || !out) return HM_ERR_INVALID;
    hm_stats_init_basic(hm, out);
    if (!hm->entries || hm->capacity == 0) return HM_OK;
    hm_stats_collect_probe_totals(hm, &total_probe, &max_probe);
    if (hm->len != 0) hm_stats_find_probe_percentiles(hm, max_probe, out);
    hm_stats_finish_loads(hm, out, total_probe, max_probe);
    return HM_OK;
}

static hm_result hm_validate_empty_state(const hashmap *hm) {
    if (!hm->entries) return (hm->capacity == 0 && hm->len == 0 && hm->tombstones == 0) ? HM_OK : HM_ERR_INVALID;
    return HM_OK;
}

static hm_result hm_validate_capacity_shape(const hashmap *hm) {
    if (!hm->entries) return HM_OK;
    if (hm->capacity < HM_MIN_CAPACITY || (hm->capacity & (hm->capacity - 1u)) != 0u) return HM_ERR_INVALID;
    return HM_OK;
}

static hm_result hm_validate_occupied_entry(const hashmap *hm, const hm_entry *e, size_t slot) {
    hm_lookup found;
    uint64_t recomputed;
    if (!e->key && e->key_len != 0) return HM_ERR_INVALID;
    recomputed = hm_hash_key_len(hm, e->key, e->key_len);
    if (recomputed != e->hash) return HM_ERR_INVALID;
    found = hm_find_existing_hash_len(hm, e->key, e->key_len, e->hash);
    return (found.rc == HM_OK && found.found && found.slot == slot) ? HM_OK : HM_ERR_INVALID;
}

static hm_result hm_validate_entries(const hashmap *hm, hm_validation_counts *counts) {
    size_t i;
    memset(counts, 0, sizeof(*counts));
    if (!hm->entries) return HM_OK;
    for (i = 0; i < hm->capacity; i++) {
        const hm_entry *e = &hm->entries[i];
        if (e->state == HM_OCCUPIED) {
            hm_result rc = hm_validate_occupied_entry(hm, e, i);
            if (rc != HM_OK) return rc;
            counts->len++;
        } else if (e->state == HM_DELETED) {
            counts->tombstones++;
        } else if (e->state != HM_EMPTY) {
            return HM_ERR_INVALID;
        }
    }
    return HM_OK;
}

static hm_result hm_validate_counts(const hashmap *hm, const hm_validation_counts *counts) {
    if (counts->len != hm->len) return HM_ERR_INVALID;
    if (counts->tombstones != hm->tombstones) return HM_ERR_INVALID;
    return HM_OK;
}

hm_result hm_validate(const hashmap *hm) {
    hm_validation_counts counts;
    hm_result rc;
    if (!hm) return HM_ERR_INVALID;
    rc = hm_validate_empty_state(hm);
    if (rc != HM_OK) return rc;
    rc = hm_validate_capacity_shape(hm);
    if (rc != HM_OK) return rc;
    rc = hm_validate_entries(hm, &counts);
    if (rc != HM_OK) return rc;
    rc = hm_validate_counts(hm, &counts);
    if (rc != HM_OK) return rc;
    return HM_OK;
}

hm_result hm_iter_init(const hashmap *hm, hm_iter *it) {
    if (!hm || !it) return HM_ERR_INVALID;
    it->index = 0;
    it->version = hm->version;
    it->last_index = (size_t)-1;
    it->has_current = 0;
    return HM_OK;
}

hm_result hm_iter_next_len(const hashmap *hm, hm_iter *it, const char **key, size_t *key_len, void **value) {
    if (key) *key = NULL;
    if (key_len) *key_len = 0;
    if (value) *value = NULL;
    if (!hm || !hm->entries || !it) return HM_ERR_INVALID;
    if (it->version != hm->version) return HM_ERR_MODIFIED;
    while (it->index < hm->capacity) {
        const hm_entry *e = &hm->entries[it->index++];
        if (e->state == HM_OCCUPIED) {
            it->last_index = it->index - 1u;
            it->has_current = 1;
            if (key) *key = e->key;
            if (key_len) *key_len = e->key_len;
            if (value) *value = e->value;
            return HM_OK;
        }
    }
    return HM_ERR_NOT_FOUND;
}

hm_result hm_iter_next(const hashmap *hm, hm_iter *it, const char **key, void **value) {
    return hm_iter_next_len(hm, it, key, NULL, value);
}

hm_result hm_iter_remove_current(hashmap *hm, hm_iter *it, void **old_value) {
    hm_result rc;
    if (old_value) *old_value = NULL;
    if (!hm || !hm->entries || !it || !it->has_current || it->last_index >= hm->capacity) return HM_ERR_INVALID;
    if (it->version != hm->version) return HM_ERR_MODIFIED;
    rc = hm_remove_slot(hm, it->last_index, old_value);
    if (rc != HM_OK) return rc;
    it->version = hm->version;
    it->index = it->last_index;
    it->last_index = (size_t)-1;
    it->has_current = 0;
    return HM_OK;
}

hm_result hm_iter_set_value(hashmap *hm, hm_iter *it, void *value, void **old_value) {
    hm_entry *e;
    if (old_value) *old_value = NULL;
    if (!hm || !hm->entries || !it || !it->has_current || it->last_index >= hm->capacity) return HM_ERR_INVALID;
    if (it->version != hm->version) return HM_ERR_MODIFIED;
    e = &hm->entries[it->last_index];
    if (e->state != HM_OCCUPIED) return HM_ERR_INVALID;
    hm_value_replace(hm, e->value, value, old_value);
    e->value = value;
    return HM_OK;
}
