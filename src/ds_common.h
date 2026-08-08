#ifndef DS_COMMON_H
#define DS_COMMON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    char *data;
    size_t len;
} DsStr;

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

void ds_diag_init(DsDiag *diag, const DsSource *source);
void ds_diag_format_location(const DsSource *source, DsSpan span, char *buf, size_t buf_len);
void ds_diag_report(FILE *out, const DsSource *source, DsSpan span, const char *severity, const char *fmt, ...);
void ds_diag_error(DsDiag *diag, DsSpan span, const char *fmt, ...);

char *ds_str_dup_range(const char *data, size_t len);
void ds_fatal_oom(void);
void *ds_xmalloc(size_t size);
void *ds_xcalloc(size_t count, size_t size);
void *ds_xrealloc(void *ptr, size_t size);

static inline char *ds_str_dup_cstr(const char *value) {
    return ds_str_dup_range(value, strlen(value));
}

static inline bool ds_str_eq_cstr(DsStr value, const char *text) {
    size_t len = strlen(text);
    return value.len == len && memcmp(value.data ? value.data : "", text, len) == 0;
}

static inline bool ds_str_eq(DsStr a, DsStr b) {
    return a.len == b.len && memcmp(a.data ? a.data : "", b.data ? b.data : "", a.len) == 0;
}

static inline char *ds_str_dup_len(DsStr value) {
    return ds_str_dup_range(value.data ? value.data : "", value.len);
}

static inline bool ds_is_ident_start(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}

static inline bool ds_is_ident_continue(char c) {
    return ds_is_ident_start(c) || (c >= '0' && c <= '9');
}

static inline bool ds_parse_int_range(DsStr text, int min, int max, int *out) {
    if (!out || text.len == 0 || min > max) return false;
    size_t i = 0;
    bool negative = false;
    if (text.data[0] == '+' || text.data[0] == '-') {
        negative = text.data[0] == '-';
        if (++i == text.len) return false;
    }
    int64_t value = 0;
    for (; i < text.len; i++) {
        char c = text.data[i];
        if (c < '0' || c > '9') return false;
        int digit = c - '0';
        if (value > (INT64_MAX - digit) / 10) return false;
        value = value * 10 + digit;
    }
    if (negative) value = -value;
    if (value < min || value > max) return false;
    *out = (int)value;
    return true;
}

static inline bool ds_size_add_overflows(size_t a, size_t b) {
    return a > (size_t)-1 - b;
}

static inline bool ds_size_mul_overflows(size_t a, size_t b) {
    return b != 0 && a > (size_t)-1 / b;
}

static inline DsStr ds_str_clone(DsStr value) {
    DsStr out = {ds_str_dup_len(value), value.len};
    return out;
}

static inline bool ds_decode_string_literal(DsStr literal, DsStr *out) {
    out->data = NULL;
    out->len = 0;
    if (literal.len < 2 || literal.data[0] != '"' || literal.data[literal.len - 1] != '"') return false;
    char *buf = (char *)ds_xcalloc(literal.len, 1);
    size_t len = 0;
    for (size_t i = 1; i + 1 < literal.len; i++) {
        char c = literal.data[i];
        if (c == '\\' && i + 1 < literal.len - 1) {
            char escaped = literal.data[++i];
            if (escaped == 'n') c = '\n';
            else if (escaped == 't') c = '\t';
            else c = escaped;
        }
        buf[len++] = c;
    }
    buf[len] = '\0';
    *out = (DsStr){buf, len};
    return true;
}

static inline const char *ds_path_basename(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static inline char *ds_path_dirname_dup(const char *path) {
    const char *slash = strrchr(path, '/');
    if (!slash) return ds_str_dup_range(".", 1);
    if (slash == path) return ds_str_dup_range("/", 1);
    return ds_str_dup_range(path, (size_t)(slash - path));
}

static inline bool ds_path_looks_like_script(const char *path) {
    size_t len = strlen(path);
    return strchr(path, '/') != NULL || (len >= 3 && strcmp(path + len - 3, ".ds") == 0);
}

static inline DsSpan ds_span_zero(const DsSource *source) {
    return (DsSpan){{0, 1, 1}, {0, 1, 1}, source};
}

static inline void ds_fprint_str(FILE *out, DsStr value) {
    fprintf(out, "%.*s", (int)value.len, value.data ? value.data : "");
}

static inline void ds_fprint_indent(FILE *out, int level) {
    for (int i = 0; i < level; i++) fputs("  ", out);
}

#define DS_GROW_ARRAY(items, len, cap, initial_cap) do { \
    if ((len) == (cap)) { \
        (cap) = (cap) ? (cap) * 2 : (initial_cap); \
        (items) = ds_xrealloc((items), (cap) * sizeof(*(items))); \
    } \
} while (0)

#define DS_VEC_PUSH(vec, value, initial_cap) do { \
    DS_GROW_ARRAY((vec)->items, (vec)->len, (vec)->cap, initial_cap); \
    (vec)->items[(vec)->len++] = (value); \
} while (0)

#endif
