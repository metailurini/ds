#include "ds_common.h"

const char *ds_str_data(DsStr value) {
    return value.data ? value.data : "";
}

char *ds_str_dup_cstr(const char *value) {
    const char *text = value ? value : "";
    return ds_str_dup_range(text, strlen(text));
}

void ds_free_cstr_array(char **items, size_t len) {
    for (size_t i = 0; i < len; i++) free(items[i]);
    free(items);
}

bool ds_str_eq_cstr(DsStr value, const char *text) {
    size_t len = strlen(text);
    return value.len == len && memcmp(ds_str_data(value), text, len) == 0;
}

bool ds_str_eq(DsStr a, DsStr b) {
    return a.len == b.len && memcmp(ds_str_data(a), ds_str_data(b), a.len) == 0;
}

bool ds_str_has_prefix_cstr(DsStr value, const char *prefix) {
    size_t len = strlen(prefix);
    return value.len >= len && memcmp(ds_str_data(value), prefix, len) == 0;
}

char *ds_str_dup_len(DsStr value) {
    return ds_str_dup_range(value.data, value.len);
}

bool ds_is_ident_start(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}

bool ds_is_ident_continue(char c) {
    return ds_is_ident_start(c) || (c >= '0' && c <= '9');
}

void ds_skip_ascii_ws(const char *data, size_t len, size_t *index) {
    while (*index < len) {
        char c = data[*index];
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r') break;
        (*index)++;
    }
}

bool ds_size_add_overflows(size_t a, size_t b) {
    return a > SIZE_MAX - b;
}

bool ds_size_mul_overflows(size_t a, size_t b) {
    return b != 0 && a > SIZE_MAX / b;
}

size_t ds_size_add_or_oom(size_t a, size_t b) {
    if (ds_size_add_overflows(a, b)) ds_fatal_oom();
    return a + b;
}

size_t ds_size_add3_or_oom(size_t a, size_t b, size_t c) {
    return ds_size_add_or_oom(ds_size_add_or_oom(a, b), c);
}

size_t ds_growth_capacity(size_t current, size_t need, size_t initial_cap) {
    size_t next = current ? current : (initial_cap ? initial_cap : 1);
    while (next < need) {
        if (next > SIZE_MAX / 2) return need;
        next *= 2;
    }
    return next;
}

void ds_reserve_char_buffer(char **data, size_t *cap, size_t need, size_t initial_cap) {
    if (need <= *cap) return;
    size_t next = ds_growth_capacity(*cap, need, initial_cap);
    *data = (char *)ds_xrealloc(*data, next);
    *cap = next;
}

bool ds_parse_int_range(DsStr text, int min, int max, int *out) {
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

DsStr ds_str_clone(DsStr value) {
    return (DsStr){ds_str_dup_len(value), value.len};
}

void ds_free_str_array(DsStr *items, size_t len) {
    for (size_t i = 0; i < len; i++) free(items[i].data);
    free(items);
}

DsStr ds_str_join_char(DsStr left, char separator, DsStr right) {
    DsStr out = {0};
    out.len = ds_size_add3_or_oom(left.len, 1, right.len);
    out.data = (char *)ds_xcalloc(ds_size_add_or_oom(out.len, 1), 1);
    if (left.len) memcpy(out.data, left.data, left.len);
    out.data[left.len] = separator;
    if (right.len) memcpy(out.data + left.len + 1, right.data, right.len);
    return out;
}

bool ds_decode_string_literal(DsStr literal, DsStr *out) {
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

bool ds_decode_string_text(DsStr text, DsStr *out) {
    if (text.len >= 6 && memcmp(text.data, "\"\"\"", 3) == 0 &&
        memcmp(text.data + text.len - 3, "\"\"\"", 3) == 0) {
        *out = (DsStr){ds_str_dup_range(text.data + 3, text.len - 6), text.len - 6};
        return true;
    }
    return ds_decode_string_literal(text, out);
}

const char *ds_path_basename(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

const char *ds_source_basename(const DsSource *source) {
    return ds_path_basename(source && source->path ? source->path : "<script>");
}

char *ds_path_dirname_dup(const char *path) {
    const char *slash = strrchr(path, '/');
    if (!slash) return ds_str_dup_range(".", 1);
    if (slash == path) return ds_str_dup_range("/", 1);
    return ds_str_dup_range(path, (size_t)(slash - path));
}

char *ds_path_join(const char *dir, const char *name) {
    if (!dir || !*dir) return ds_str_dup_cstr(name);
    if (!name || !*name) return ds_str_dup_cstr(dir);
    size_t dir_len = strlen(dir);
    size_t name_len = strlen(name);
    bool need_slash = dir[dir_len - 1] != '/';
    size_t len = ds_size_add3_or_oom(dir_len, need_slash ? 1 : 0, name_len);
    char *out = (char *)ds_xcalloc(ds_size_add_or_oom(len, 1), 1);
    memcpy(out, dir, dir_len);
    size_t pos = dir_len;
    if (need_slash) out[pos++] = '/';
    memcpy(out + pos, name, name_len);
    return out;
}

bool ds_path_looks_like_script(const char *path) {
    size_t len = strlen(path);
    return strchr(path, '/') != NULL || (len >= 3 && strcmp(path + len - 3, ".ds") == 0);
}

DsSpan ds_span_zero(const DsSource *source) {
    return (DsSpan){{0, 1, 1}, {0, 1, 1}, source};
}

void ds_fprint_str(FILE *out, DsStr value) {
    fprintf(out, "%.*s", (int)value.len, ds_str_data(value));
}

void ds_fprint_escaped(FILE *out, const char *data, size_t len, DsEscapeMode mode) {
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)data[i];
        if (c == '\\') fputs("\\\\", out);
        else if (c == '"') fputs("\\\"", out);
        else if (c == '\n') fputs("\\n", out);
        else if (c == '\t') fputs("\\t", out);
        else if (c == '\r' && mode == DS_ESCAPE_NAMED_CONTROLS) fputs("\\r", out);
        else if (mode == DS_ESCAPE_HEX_CONTROLS && (c < 32 || c == 127)) fprintf(out, "\\x%02x", c);
        else fputc((int)c, out);
    }
}

void ds_fprint_indent(FILE *out, int level) {
    for (int i = 0; i < level; i++) fputs("  ", out);
}