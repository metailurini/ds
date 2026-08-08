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

static inline DsStr ds_str_clone(DsStr value) {
    DsStr out = {ds_str_dup_range(value.data ? value.data : "", value.len), value.len};
    return out;
}

static inline DsSpan ds_span_zero(const DsSource *source) {
    return (DsSpan){{0, 1, 1}, {0, 1, 1}, source};
}

#define DS_VEC_PUSH(vec, value, initial_cap) do { \
    if ((vec)->len == (vec)->cap) { \
        (vec)->cap = (vec)->cap ? (vec)->cap * 2 : (initial_cap); \
        (vec)->items = ds_xrealloc((vec)->items, (vec)->cap * sizeof(*(vec)->items)); \
    } \
    (vec)->items[(vec)->len++] = (value); \
} while (0)

#endif
