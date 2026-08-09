#ifndef DS_RUNTIME_H
#define DS_RUNTIME_H

#include "ds_common.h"

typedef enum {
    DS_VALUE_NULL,
    DS_VALUE_BOOL,
    DS_VALUE_INT,
    DS_VALUE_STRING,
    DS_VALUE_COMMAND_RESULT,
    DS_VALUE_ARRAY,
    DS_VALUE_MAP
} DsValueKind;

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} DsString;

const char *ds_string_data(const DsString *string);

typedef struct {
    DsString stdout_text;
    DsString stderr_text;
    int64_t code;
} DsCommandResult;

typedef struct {
    void **items;
    size_t len;
    size_t cap;
} DsArray;

typedef struct DsValue DsValue;

typedef struct {
    void *impl;
} DsMap;

struct DsValue {
    DsValueKind kind;
    union {
        bool boolean;
        int64_t integer;
        DsString string;
        DsCommandResult command_result;
        DsArray array;
        DsMap map;
    } as;
};

void ds_string_init(DsString *s);
bool ds_string_from_cstr(DsString *s, const char *text);
bool ds_string_from_range(DsString *s, const char *data, size_t len);
bool ds_string_append_range(DsString *s, const char *data, size_t len);
bool ds_string_append_cstr(DsString *s, const char *text);
bool ds_string_append_char(DsString *s, char c);
bool ds_string_appendf(DsString *s, const char *fmt, ...);
bool ds_string_append_escaped(DsString *s, const char *data, size_t len);
void ds_string_free(DsString *s);

DsValue ds_value_null(void);
DsValue ds_value_bool(bool value);
DsValue ds_value_int(int64_t value);
DsValue ds_value_array(void);
bool ds_value_map_init(DsValue *out);
DsValue ds_value_string_take(DsString *string);
DsValue ds_value_command_result_take(DsString *stdout_text, DsString *stderr_text, int64_t code);
DsValue ds_value_copy(const DsValue *value);
void ds_value_free(DsValue *value);
bool ds_value_truthy(const DsValue *value, bool *out);
bool ds_value_to_string(const DsValue *value, DsString *out);
int ds_value_compare(const DsValue *left, const DsValue *right);

void ds_array_init(DsArray *array);
void ds_array_clear(DsArray *array);
void ds_array_free(DsArray *array);

bool ds_map_init(DsMap *map);
bool ds_map_set(DsMap *map, DsStr key, DsValue value);
DsValue *ds_map_get(DsMap *map, DsStr key);
size_t ds_map_len(const DsMap *map);
bool ds_map_sorted_keys(const DsMap *map, DsStr **out_keys, size_t *out_len);
void ds_map_sorted_keys_free(DsStr *keys, size_t len);
void ds_map_clear(DsMap *map);
void ds_map_free(DsMap *map);

#endif
