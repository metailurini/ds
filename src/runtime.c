#include "ds_runtime.h"
#include "runtime/hashmap.h"

#include <stdarg.h>

const char *ds_string_data(const DsString *string) {
    return string && string->data ? string->data : "";
}

static hashmap *ds_map_impl(DsMap *map) {
    return (hashmap *)map->impl;
}

static const hashmap *ds_map_impl_const(const DsMap *map) {
    return (const hashmap *)map->impl;
}

#define DS_MAP_FOREACH(map, iter, key, key_len, value) \
    for (hm_iter iter, *iter##_active = \
             hm_iter_init(ds_map_impl_const(map), &iter) == HM_OK ? &iter : NULL; \
         iter##_active; iter##_active = NULL) \
        while (hm_iter_next_len(ds_map_impl_const(map), &iter, &(key), &(key_len), &(value)) == HM_OK)

void ds_string_init(DsString *s) {
    *s = (DsString){0};
}

bool ds_string_append_range(DsString *s, const char *data, size_t len) {
    ds_reserve_char_buffer(&s->data, &s->cap, ds_size_add3_or_oom(s->len, len, 1), 16);
    if (len > 0) memcpy(s->data + s->len, data, len);
    s->len += len;
    s->data[s->len] = '\0';
    return true;
}

bool ds_string_append_cstr(DsString *s, const char *text) {
    return ds_string_append_range(s, text, strlen(text));
}

bool ds_string_append_char(DsString *s, char c) {
    return ds_string_append_range(s, &c, 1);
}

bool ds_string_appendf(DsString *s, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    va_list copy;
    va_copy(copy, args);
    int n = vsnprintf(NULL, 0, fmt, copy);
    va_end(copy);
    if (n < 0) { va_end(args); return false; }
    ds_reserve_char_buffer(&s->data, &s->cap, ds_size_add3_or_oom(s->len, (size_t)n, 1), 16);
    vsnprintf(s->data + s->len, (size_t)n + 1, fmt, args);
    va_end(args);
    s->len += (size_t)n;
    return true;
}

bool ds_string_append_escaped(DsString *s, const char *data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        char c = data[i];
        if (c == '\n') {
            if (!ds_string_append_range(s, "\\n", 2)) return false;
        } else if (c == '\t') {
            if (!ds_string_append_range(s, "\\t", 2)) return false;
        } else if (c == '"' || c == '\\') {
            if (!ds_string_append_char(s, '\\') || !ds_string_append_char(s, c)) return false;
        } else if (!ds_string_append_char(s, c)) {
            return false;
        }
    }
    return true;
}

bool ds_string_from_range(DsString *s, const char *data, size_t len) {
    ds_string_init(s);
    return ds_string_append_range(s, data, len);
}

bool ds_string_from_cstr(DsString *s, const char *text) {
    return ds_string_from_range(s, text, strlen(text));
}

void ds_string_free(DsString *s) {
    free(s->data);
    *s = (DsString){0};
}

DsValue ds_value_null(void) {
    return (DsValue){.kind = DS_VALUE_NULL};
}

DsValue ds_value_bool(bool value) {
    return (DsValue){.kind = DS_VALUE_BOOL, .as.boolean = value};
}

DsValue ds_value_int(int64_t value) {
    return (DsValue){.kind = DS_VALUE_INT, .as.integer = value};
}

DsValue ds_value_array(void) {
    return (DsValue){.kind = DS_VALUE_ARRAY};
}

bool ds_value_map_init(DsValue *out) {
    *out = (DsValue){.kind = DS_VALUE_MAP};
    return ds_map_init(&out->as.map);
}

DsValue ds_value_map(void) {
    DsValue v;
    ds_value_map_init(&v);
    return v;
}

DsValue ds_value_string_take(DsString *string) {
    DsValue v = {.kind = DS_VALUE_STRING, .as.string = *string};
    *string = (DsString){0};
    return v;
}

DsValue ds_value_command_result_take(DsString *stdout_text, DsString *stderr_text, int64_t code) {
    DsValue v = {.kind = DS_VALUE_COMMAND_RESULT,
                 .as.command_result = {*stdout_text, *stderr_text, code}};
    *stdout_text = (DsString){0};
    *stderr_text = (DsString){0};
    return v;
}

DsValue ds_value_copy(const DsValue *value) {
    DsValue out = {.kind = value->kind};
    switch (value->kind) {
        case DS_VALUE_STRING:
            ds_string_from_range(&out.as.string, ds_string_data(&value->as.string), value->as.string.len);
            break;
        case DS_VALUE_COMMAND_RESULT:
            ds_string_from_range(&out.as.command_result.stdout_text,
                                 ds_string_data(&value->as.command_result.stdout_text),
                                 value->as.command_result.stdout_text.len);
            ds_string_from_range(&out.as.command_result.stderr_text,
                                 ds_string_data(&value->as.command_result.stderr_text),
                                 value->as.command_result.stderr_text.len);
            out.as.command_result.code = value->as.command_result.code;
            break;
        case DS_VALUE_ARRAY:
            out = ds_value_array();
            for (size_t i = 0; i < value->as.array.len; i++) {
                DsValue *item = (DsValue *)value->as.array.items[i];
                DsValue *copy = (DsValue *)ds_xcalloc(1, sizeof(DsValue));
                *copy = ds_value_copy(item);
                DS_VEC_PUSH(&out.as.array, copy, 8);
            }
            break;
        case DS_VALUE_MAP:
            if (!ds_value_map_init(&out)) return ds_value_null();
            const char *key = NULL;
            size_t key_len = 0;
            void *raw = NULL;
            DS_MAP_FOREACH(&value->as.map, it, key, key_len, raw) {
                DsStr key_view = {(char *)key, key_len};
                if (!ds_map_set(&out.as.map, key_view, ds_value_copy((const DsValue *)raw))) {
                    ds_value_free(&out);
                    return ds_value_null();
                }
            }
            break;
        case DS_VALUE_BOOL:
            out.as.boolean = value->as.boolean;
            break;
        case DS_VALUE_INT:
            out.as.integer = value->as.integer;
            break;
        case DS_VALUE_NULL:
            break;
    }
    return out;
}

static void ds_value_free_boxed(void *boxed) {
    if (!boxed) return;
    ds_value_free((DsValue *)boxed);
    free(boxed);
}

static void ds_value_free_boxed_callback(void *boxed, void *ctx) {
    (void)ctx;
    ds_value_free_boxed(boxed);
}

void ds_value_free(DsValue *value) {
    switch (value->kind) {
        case DS_VALUE_STRING:
            ds_string_free(&value->as.string);
            break;
        case DS_VALUE_COMMAND_RESULT:
            ds_string_free(&value->as.command_result.stdout_text);
            ds_string_free(&value->as.command_result.stderr_text);
            break;
        case DS_VALUE_ARRAY:
            for (size_t i = 0; i < value->as.array.len; i++) ds_value_free_boxed(value->as.array.items[i]);
            ds_array_free(&value->as.array);
            break;
        case DS_VALUE_MAP:
            ds_map_free(&value->as.map);
            break;
        case DS_VALUE_NULL:
        case DS_VALUE_BOOL:
        case DS_VALUE_INT:
            break;
    }
    *value = ds_value_null();
}

bool ds_value_truthy(const DsValue *value, bool *out) {
    switch (value->kind) {
        case DS_VALUE_BOOL:
            *out = value->as.boolean;
            return true;
        case DS_VALUE_INT:
            *out = value->as.integer != 0;
            return true;
        case DS_VALUE_STRING:
            *out = value->as.string.len != 0;
            return true;
        case DS_VALUE_NULL:
            *out = false;
            return true;
        case DS_VALUE_COMMAND_RESULT:
            *out = value->as.command_result.code == 0;
            return true;
        case DS_VALUE_ARRAY:
            *out = value->as.array.len != 0;
            return true;
        case DS_VALUE_MAP:
            *out = ds_map_len(&value->as.map) != 0;
            return true;
    }
    return false;
}

bool ds_value_to_string(const DsValue *value, DsString *out) {
    ds_string_init(out);
    switch (value->kind) {
        case DS_VALUE_NULL:
            return ds_string_append_cstr(out, "null");
        case DS_VALUE_BOOL:
            return ds_string_append_cstr(out, value->as.boolean ? "true" : "false");
        case DS_VALUE_INT:
            return ds_string_appendf(out, "%lld", (long long)value->as.integer);
        case DS_VALUE_STRING:
            return ds_string_append_range(out, ds_string_data(&value->as.string), value->as.string.len);
        case DS_VALUE_COMMAND_RESULT:
            return ds_string_append_cstr(out, "[command result]");
        case DS_VALUE_ARRAY:
            return ds_string_append_cstr(out, "[array]");
        case DS_VALUE_MAP:
            return ds_string_append_cstr(out, "[map]");
    }
    return false;
}

int ds_value_compare(const DsValue *left, const DsValue *right) {
    if (left && right && left->kind == DS_VALUE_INT && right->kind == DS_VALUE_INT) {
        if (left->as.integer < right->as.integer) return -1;
        if (left->as.integer > right->as.integer) return 1;
        return 0;
    }
    DsString a, b;
    ds_value_to_string(left, &a);
    ds_value_to_string(right, &b);
    size_t min = a.len < b.len ? a.len : b.len;
    int cmp = memcmp(ds_string_data(&a), ds_string_data(&b), min);
    if (cmp == 0) {
        if (a.len < b.len) cmp = -1;
        else if (a.len > b.len) cmp = 1;
    }
    ds_string_free(&a);
    ds_string_free(&b);
    return cmp;
}

void ds_array_init(DsArray *array) {
    *array = (DsArray){0};
}

void ds_array_clear(DsArray *array) {
    array->len = 0;
}

void ds_array_free(DsArray *array) {
    free(array->items);
    ds_array_init(array);
}

bool ds_map_init(DsMap *map) {
    if (!map) return false;
    map->impl = calloc(1, sizeof(hashmap));
    if (!map->impl) return false;
    if (hm_init(ds_map_impl(map)) != HM_OK) {
        free(map->impl);
        map->impl = NULL;
        return false;
    }
    return true;
}

DsValue *ds_map_get(DsMap *map, DsStr key) {
    void *value = NULL;
    if (!map->impl || hm_get_len(ds_map_impl(map), key.data, key.len, &value) != HM_OK) return NULL;
    return (DsValue *)value;
}

bool ds_map_set(DsMap *map, DsStr key, DsValue value) {
    if (!map || !map->impl) {
        ds_value_free(&value);
        return false;
    }
    DsValue *boxed = (DsValue *)ds_xcalloc(1, sizeof(DsValue));
    void *old = NULL;
    hm_result rc;
    *boxed = value;
    rc = hm_put_len(ds_map_impl(map), key.data, key.len, boxed, &old);
    if (rc != HM_OK) {
        ds_value_free_boxed(boxed);
        return false;
    }
    ds_value_free_boxed(old);
    return true;
}

size_t ds_map_len(const DsMap *map) {
    return map->impl ? hm_len(ds_map_impl_const(map)) : 0;
}

static int ds_str_key_cmp(const void *a, const void *b) {
    const DsStr *left = (const DsStr *)a;
    const DsStr *right = (const DsStr *)b;
    size_t min = left->len < right->len ? left->len : right->len;
    int cmp = memcmp(left->data ? left->data : "", right->data ? right->data : "", min);
    if (cmp != 0) return cmp;
    if (left->len < right->len) return -1;
    if (left->len > right->len) return 1;
    return 0;
}

bool ds_map_sorted_keys(const DsMap *map, DsStr **out_keys, size_t *out_len) {
    *out_keys = NULL;
    *out_len = 0;
    if (!map->impl) return true;
    size_t len = ds_map_len(map);
    if (len == 0) return true;
    DsStr *keys = (DsStr *)ds_xcalloc(len, sizeof(DsStr));
    const char *key = NULL;
    size_t key_len = 0;
    void *raw = NULL;
    size_t i = 0;
    DS_MAP_FOREACH(map, it, key, key_len, raw) {
        if (i >= len) break;
        (void)raw;
        keys[i].data = ds_str_dup_range(key, key_len);
        keys[i].len = key_len;
        i++;
    }
    *out_len = i;
    qsort(keys, i, sizeof(DsStr), ds_str_key_cmp);
    *out_keys = keys;
    return true;
}

void ds_map_sorted_keys_free(DsStr *keys, size_t len) {
    ds_free_str_array(keys, len);
}

void ds_map_clear(DsMap *map) {
    if (!map->impl) return;
    hm_clear_with_values(ds_map_impl(map), ds_value_free_boxed_callback, NULL);
}

void ds_map_free(DsMap *map) {
    if (!map->impl) return;
    ds_map_clear(map);
    hm_free(ds_map_impl(map));
    free(map->impl);
    map->impl = NULL;
}
