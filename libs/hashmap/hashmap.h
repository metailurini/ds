#ifndef HASHMAP_H
#define HASHMAP_H

#include <stddef.h>
#include <stdint.h>

#ifndef HM_MIN_CAPACITY
#define HM_MIN_CAPACITY 16u
#endif

#ifndef HM_DEFAULT_SEED
#define HM_DEFAULT_SEED 0xcbf29ce484222325ULL
#endif

#ifndef HM_DEFAULT_MAX_LOAD_PERCENT
#define HM_DEFAULT_MAX_LOAD_PERCENT 70u
#endif

#ifndef HM_DEFAULT_MAX_TOMBSTONE_PERCENT
#define HM_DEFAULT_MAX_TOMBSTONE_PERCENT 50u
#endif

#ifndef HM_DEFAULT_MAX_PROBE_LENGTH
#define HM_DEFAULT_MAX_PROBE_LENGTH 256u
#endif

typedef enum hm_result {
    HM_OK = 0,
    HM_ERR_INVALID = 1,
    HM_ERR_OOM = 2,
    HM_ERR_NOT_FOUND = 3,
    HM_ERR_MODIFIED = 4,
    HM_ERR_FULL = 5,
    HM_ERR_PROBE_LIMIT = 6
} hm_result;

typedef enum hm_state {
    HM_EMPTY = 0,
    HM_OCCUPIED = 1,
    HM_DELETED = 2
} hm_state;

typedef enum hm_key_storage {
    HM_KEY_STORAGE_HEAP = 0,
    HM_KEY_STORAGE_ARENA = 1,
    HM_KEY_STORAGE_BORROWED = 2,
    HM_KEY_STORAGE_COPY = 3
} hm_key_storage;

typedef struct hm_allocator {
    void *(*malloc_fn)(size_t size, void *ctx);
    void *(*calloc_fn)(size_t count, size_t size, void *ctx);
    void (*free_fn)(void *ptr, void *ctx);
    void *ctx;
} hm_allocator;

typedef uint64_t (*hm_hash_fn)(const char *key, size_t key_len, uint64_t seed, void *ctx);
typedef void (*hm_value_free_fn)(void *value, void *ctx);
typedef hm_result (*hm_upsert_fn)(const char *key, size_t key_len, int found,
                                  void *old_value, void **new_value, void *ctx);

typedef struct hm_config {
    hm_allocator allocator;
    uint64_t seed;
    hm_hash_fn hash_fn;
    void *hash_ctx;
    hm_value_free_fn value_free;
    void *value_ctx;
    unsigned max_load_percent;
    unsigned max_tombstone_percent; /* kept for source compatibility; backward-shift deletion normally keeps this 0 */
    size_t max_capacity;            /* 0 means unlimited. */
    size_t max_probe_length;         /* 0 uses the default fail-fast probe limit; (size_t)-1 disables it. */
    int no_growth;                  /* Nonzero disables automatic allocation/growth during inserts. */
    char *key_arena;                /* Optional caller-owned arena for copied keys. */
    size_t key_arena_capacity;
    size_t key_arena_used;
    int key_arena_strict;
} hm_config;

typedef struct hm_entry {
    char *key;
    size_t key_len;
    void *value;
    uint64_t hash;
    hm_state state;
    hm_key_storage key_storage;
} hm_entry;

typedef struct hashmap {
    /* Private fields: callers should use the public API instead of editing these directly. */
    hm_entry *entries;
    size_t capacity;
    size_t len;
    size_t tombstones;
    size_t version;
    hm_config config;
} hashmap;

typedef struct hm_iter {
    size_t index;
    size_t version;
    size_t last_index;
    int has_current;
} hm_iter;

typedef struct hm_stats {
    size_t capacity;
    size_t len;
    size_t tombstones;
    size_t max_probe;
    size_t p50_probe;
    size_t p95_probe;
    size_t p99_probe;
    double avg_probe;
    double live_load;
    double used_load;
} hm_stats;

typedef struct hm_put_item {
    const char *key;
    size_t key_len;
    void *value;
    void **old_value;
} hm_put_item;

typedef struct hm_remove_item {
    const char *key;
    size_t key_len;
    void **old_value;
} hm_remove_item;

hm_result hm_init(hashmap *hm);
hm_result hm_init_with_config(hashmap *hm, const hm_config *config);
hm_result hm_init_with_allocator(hashmap *hm, const hm_allocator *allocator);
hm_result hm_init_with_seed(hashmap *hm, uint64_t seed);
hm_result hm_init_with_allocator_and_seed(hashmap *hm, const hm_allocator *allocator, uint64_t seed);
hm_result hm_init_with_key_arena(hashmap *hm, void *arena, size_t arena_capacity, int strict);
hm_result hm_set_key_arena(hashmap *hm, void *arena, size_t arena_capacity, int strict);
size_t hm_key_arena_used(const hashmap *hm);
size_t hm_key_arena_capacity(const hashmap *hm);

void hm_clear(hashmap *hm);
void hm_clear_with_values(hashmap *hm, hm_value_free_fn value_free, void *value_ctx);
void hm_free(hashmap *hm);
void hm_free_with_values(hashmap *hm, hm_value_free_fn value_free, void *value_ctx);
void hm_reset(hashmap *hm);

hm_result hm_reserve(hashmap *hm, size_t min_capacity);
hm_result hm_compact(hashmap *hm);
hm_result hm_shrink_to_fit(hashmap *hm);
hm_result hm_freeze_capacity(hashmap *hm);
hm_result hm_unfreeze_capacity(hashmap *hm);
int hm_is_frozen(const hashmap *hm);

hm_result hm_put(hashmap *hm, const char *key, void *value, void **old_value);
hm_result hm_put_len(hashmap *hm, const char *key, size_t key_len, void *value, void **old_value);
hm_result hm_put_borrowed(hashmap *hm, const char *key, void *value, void **old_value);
hm_result hm_put_borrowed_len(hashmap *hm, const char *key, size_t key_len, void *value, void **old_value);
hm_result hm_put_move_key(hashmap *hm, char *owned_key, void *value, void **old_value);
hm_result hm_put_move_key_len(hashmap *hm, char *owned_key, size_t key_len, void *value, void **old_value);
hm_result hm_upsert(hashmap *hm, const char *key, hm_upsert_fn fn, void *ctx, void **new_value);
hm_result hm_upsert_len(hashmap *hm, const char *key, size_t key_len, hm_upsert_fn fn, void *ctx, void **new_value);
hm_result hm_get_or_insert(hashmap *hm, const char *key, void *default_value, void **out_value, int *inserted);
hm_result hm_get_or_insert_len(hashmap *hm, const char *key, size_t key_len, void *default_value, void **out_value, int *inserted);
hm_result hm_put_many_len(hashmap *hm, const hm_put_item *items, size_t count, size_t *processed);
hm_result hm_remove_many_len(hashmap *hm, const hm_remove_item *items, size_t count, size_t *processed);
hm_result hm_get(const hashmap *hm, const char *key, void **out_value);
hm_result hm_get_len(const hashmap *hm, const char *key, size_t key_len, void **out_value);
hm_result hm_remove(hashmap *hm, const char *key, void **old_value);
hm_result hm_remove_len(hashmap *hm, const char *key, size_t key_len, void **old_value);
int hm_contains_key(const hashmap *hm, const char *key);
int hm_contains_key_len(const hashmap *hm, const char *key, size_t key_len);

size_t hm_len(const hashmap *hm);
size_t hm_capacity(const hashmap *hm);
size_t hm_tombstones(const hashmap *hm);
uint64_t hm_seed(const hashmap *hm);
hm_result hm_set_seed(hashmap *hm, uint64_t seed);
hm_result hm_stats_get(const hashmap *hm, hm_stats *out);
hm_result hm_validate(const hashmap *hm);

hm_result hm_iter_init(const hashmap *hm, hm_iter *it);
hm_result hm_iter_next(const hashmap *hm, hm_iter *it, const char **key, void **value);
hm_result hm_iter_next_len(const hashmap *hm, hm_iter *it, const char **key, size_t *key_len, void **value);
hm_result hm_iter_remove_current(hashmap *hm, hm_iter *it, void **old_value);
hm_result hm_iter_set_value(hashmap *hm, hm_iter *it, void *value, void **old_value);

#endif /* HASHMAP_H */
