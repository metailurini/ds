#ifndef DS_COMMON_H
#define DS_COMMON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char *data;
    size_t len;
} DsStr;

const char *ds_str_data(DsStr value);

typedef struct {
    const char *path;
    char *data;
    size_t len;
} DsSource;

typedef struct {
    size_t offset;
    int line;
    int column;
} DsLoc;

typedef struct {
    DsLoc start;
    DsLoc end;
    const DsSource *source;
} DsSpan;

typedef struct {
    const DsSource *source;
    bool has_error;
} DsDiag;

bool ds_source_read(const char *path, DsSource *out, DsDiag *diag);
void ds_source_free(DsSource *source);
bool ds_file_write_atomic(const char *path, const char *data, size_t len);
bool ds_parse_i64_range(const char *data, size_t len, int64_t *out);

void ds_diag_init(DsDiag *diag, const DsSource *source);
void ds_diag_format_location(const DsSource *source, DsSpan span, char *buf, size_t buf_len);
void ds_diag_report(FILE *out, const DsSource *source, DsSpan span, const char *severity, const char *fmt, ...);
void ds_diag_error(DsDiag *diag, DsSpan span, const char *fmt, ...);

char *ds_str_dup_range(const char *data, size_t len);
void ds_fatal_oom(void);
void *ds_xmalloc(size_t size);
void *ds_xcalloc(size_t count, size_t size);
void *ds_xrealloc(void *ptr, size_t size);

char *ds_str_dup_cstr(const char *value);
void ds_free_cstr_array(char **items, size_t len);
bool ds_str_eq_cstr(DsStr value, const char *text);
bool ds_str_eq(DsStr a, DsStr b);
bool ds_str_has_prefix_cstr(DsStr value, const char *prefix);
char *ds_str_dup_len(DsStr value);
bool ds_is_ident_start(char c);
bool ds_is_ident_continue(char c);
void ds_skip_ascii_ws(const char *data, size_t len, size_t *index);
bool ds_size_add_overflows(size_t a, size_t b);
bool ds_size_mul_overflows(size_t a, size_t b);
size_t ds_size_add_or_oom(size_t a, size_t b);
size_t ds_size_add3_or_oom(size_t a, size_t b, size_t c);
size_t ds_growth_capacity(size_t current, size_t need, size_t initial_cap);
void *ds_grow_array(void *items, size_t len, size_t *cap, size_t item_size, size_t initial_cap);
void ds_reserve_char_buffer(char **data, size_t *cap, size_t need, size_t initial_cap);
bool ds_parse_int_range(DsStr text, int min, int max, int *out);
DsStr ds_str_clone(DsStr value);
void ds_free_str_array(DsStr *items, size_t len);
DsStr ds_str_join_char(DsStr left, char separator, DsStr right);
bool ds_decode_string_literal(DsStr literal, DsStr *out);
bool ds_decode_string_text(DsStr text, DsStr *out);
const char *ds_path_basename(const char *path);
const char *ds_source_basename(const DsSource *source);
char *ds_path_dirname_dup(const char *path);
char *ds_path_join(const char *dir, const char *name);
bool ds_path_looks_like_script(const char *path);
DsSpan ds_span_zero(const DsSource *source);
void ds_fprint_str(FILE *out, DsStr value);

typedef enum {
    DS_ESCAPE_BASIC,
    DS_ESCAPE_HEX_CONTROLS,
    DS_ESCAPE_NAMED_CONTROLS
} DsEscapeMode;

void ds_fprint_escaped(FILE *out, const char *data, size_t len, DsEscapeMode mode);
void ds_fprint_indent(FILE *out, int level);

#define DS_ARRAY_LEN(items) (sizeof(items) / sizeof((items)[0]))

#define DS_VEC_PUSH(vec, value, initial_cap) do { \
    (vec)->items = ds_grow_array((vec)->items, (vec)->len, &(vec)->cap, sizeof(*(vec)->items), (initial_cap)); \
    (vec)->items[(vec)->len++] = (value); \
} while (0)

#endif
