#define _POSIX_C_SOURCE 200809L

#include "ds_regex.h"
#include "vm_internal.h"

#include <dirent.h>
#include <errno.h>
#include <glob.h>
#include <regex.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef enum {
    VM_PATH_KIND_EXISTS,
    VM_PATH_KIND_FILE,
    VM_PATH_KIND_DIR
} VmPathKind;

static bool helper_is(const Instr *ins, const char *name) {
    return ins->name && strcmp(ins->name, name) == 0;
}

static bool vm_string_arg(Vm *vm, Instr *ins, size_t index, const char **out, size_t *len) {
    if (index >= ins->arg_count) return false;

    /*
     * Static helper arity and literal argument-kind checks are lowerer-owned.
     * These VM diagnostics cover dynamic values produced by accepted HIR.
     */
    DsValue *v = &vm->regs[ins->args[index]];
    if (v->kind != DS_VALUE_STRING) {
        ds_diag_error(vm->diag, ins->span, "runtime standard-library helper `%s` expects string arguments", ins->name ? ins->name : "<helper>");
        return false;
    }

    *out = v->as.string.data ? v->as.string.data : "";
    *len = v->as.string.len;
    if (memchr(*out, '\0', *len)) {
        ds_diag_error(vm->diag, ins->span, "standard-library helper `%s` does not support embedded NUL bytes", ins->name ? ins->name : "<helper>");
        return false;
    }
    return true;
}

static char *vm_string_arg_dup(Vm *vm, Instr *ins, size_t index) {
    const char *s = NULL;
    size_t len = 0;
    if (!vm_string_arg(vm, ins, index, &s, &len)) return NULL;
    return ds_str_dup_range(s, len);
}

static bool vm_valid_env_name(const char *name) {
    if (!name || !name[0]) return false;

    unsigned char c = (unsigned char)name[0];
    if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_')) return false;

    for (size_t i = 1; name[i]; i++) {
        c = (unsigned char)name[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return false;
    }
    return true;
}

static bool vm_require_env_name(Vm *vm, Instr *ins, const char *name) {
    if (vm_valid_env_name(name)) return true;
    ds_diag_error(vm->diag, ins->span, "invalid environment variable name `%s` at runtime in v0.11.0", name ? name : "");
    return false;
}

static bool value_string_from_owned_cstr(DsValue *out, char *text) {
    DsString s;
    ds_string_init(&s);
    if (!ds_string_from_cstr(&s, text ? text : "")) {
        free(text);
        return false;
    }
    free(text);
    *out = ds_value_string_take(&s);
    return true;
}

static bool array_push_string(DsValue *array, const char *data, size_t len) {
    DsString s;
    ds_string_from_range(&s, data ? data : "", len);
    DsValue *item = (DsValue *)ds_xcalloc(1, sizeof(DsValue));
    *item = ds_value_string_take(&s);
    return ds_array_push(&array->as.array, item);
}

static int cmp_cstr_ptr(const void *a, const void *b) {
    const char *const *sa = (const char *const *)a;
    const char *const *sb = (const char *const *)b;
    return strcmp(*sa, *sb);
}

typedef struct {
    char **items;
    size_t len;
    size_t cap;
} VmStringVec;

static char *vm_strdup_range(const char *data, size_t len) {
    char *out = (char *)ds_xcalloc(len + 1, 1);
    if (len) memcpy(out, data, len);
    out[len] = '\0';
    return out;
}

static char *vm_strdup_cstr(const char *data) {
    return vm_strdup_range(data ? data : "", data ? strlen(data) : 0);
}

static void vm_string_vec_free(VmStringVec *vec) {
    for (size_t i = 0; i < vec->len; i++) free(vec->items[i]);
    free(vec->items);
    vec->items = NULL;
    vec->len = 0;
    vec->cap = 0;
}

static bool vm_string_vec_push_owned(VmStringVec *vec, char *text) {
    if (vec->len == vec->cap) {
        vec->cap = vec->cap ? vec->cap * 2 : 16;
        vec->items = (char **)ds_xrealloc(vec->items, vec->cap * sizeof(char *));
    }
    vec->items[vec->len++] = text;
    return true;
}

static bool vm_string_vec_push_copy(VmStringVec *vec, const char *text) {
    return vm_string_vec_push_owned(vec, vm_strdup_cstr(text));
}

static char *path_join2(const char *a, const char *b) {
    if (!a || !*a) return vm_strdup_cstr(b ? b : "");
    if (!b || !*b) return vm_strdup_cstr(a);
    size_t alen = strlen(a);
    size_t blen = strlen(b);
    bool slash = a[alen - 1] == '/';
    char *out = (char *)ds_xcalloc(alen + blen + (slash ? 1 : 2), 1);
    memcpy(out, a, alen);
    size_t pos = alen;
    if (!slash) out[pos++] = '/';
    memcpy(out + pos, b, blen);
    out[pos + blen] = '\0';
    return out;
}

static char *glob_escape_literal_path(const char *path) {
    size_t len = path ? strlen(path) : 0;
    size_t extra = 0;
    for (size_t i = 0; i < len; i++) {
        if (path[i] == '\\' || path[i] == '*' || path[i] == '?' || path[i] == '[') extra++;
    }
    char *out = (char *)ds_xcalloc(len + extra + 1, 1);
    size_t pos = 0;
    for (size_t i = 0; i < len; i++) {
        if (path[i] == '\\' || path[i] == '*' || path[i] == '?' || path[i] == '[') out[pos++] = '\\';
        out[pos++] = path[i];
    }
    out[pos] = '\0';
    return out;
}

static bool has_glob_meta(const char *s) {
    for (; s && *s; s++) {
        if (*s == '*' || *s == '?' || *s == '[') return true;
    }
    return false;
}

static void normalize_recursive_prefix(const char *prefix, char **base_pattern, bool *strip_root_dot) {
    *strip_root_dot = false;
    if (!prefix || !*prefix) {
        *base_pattern = vm_strdup_cstr(".");
        *strip_root_dot = true;
        return;
    }
    size_t len = strlen(prefix);
    while (len > 1 && prefix[len - 1] == '/') len--;
    if (len == 1 && prefix[0] == '.') {
        *base_pattern = vm_strdup_cstr(".");
        return;
    }
    *base_pattern = vm_strdup_range(prefix, len);
}

static bool collect_base_dirs(Vm *vm, Instr *ins, const char *base_pattern, VmStringVec *bases) {
    if (!has_glob_meta(base_pattern)) {
        struct stat st;
        if (lstat(base_pattern, &st) == 0 && S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode)) {
            return vm_string_vec_push_copy(bases, base_pattern);
        }
        return true;
    }

    glob_t g;
    memset(&g, 0, sizeof(g));
    int grc = glob(base_pattern, 0, NULL, &g);
    if (grc == GLOB_NOMATCH) {
        globfree(&g);
        return true;
    }
    if (grc != 0) {
        ds_diag_error(vm->diag, ins->span, "failed to evaluate recursive glob base `%s`", base_pattern);
        globfree(&g);
        return false;
    }
    for (size_t i = 0; i < g.gl_pathc; i++) {
        struct stat st;
        if (lstat(g.gl_pathv[i], &st) == 0 && S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode)) {
            vm_string_vec_push_copy(bases, g.gl_pathv[i]);
        }
    }
    globfree(&g);
    return true;
}

static bool should_skip_hidden_child(const char *name) {
    return name && name[0] == '.' && strcmp(name, ".") != 0 && strcmp(name, "..") != 0;
}

static bool collect_dirs_recursive(Vm *vm, Instr *ins, const char *dir, VmStringVec *dirs) {
    if (!vm_string_vec_push_copy(dirs, dir)) return false;
    DIR *dp = opendir(dir);
    if (!dp) {
        ds_diag_error(vm->diag, ins->span, "failed to traverse recursive glob directory `%s`: %s", dir, strerror(errno));
        return false;
    }

    struct dirent *ent = NULL;
    while ((ent = readdir(dp)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        if (should_skip_hidden_child(ent->d_name)) continue;
        char *child = path_join2(dir, ent->d_name);
        struct stat st;
        bool descend = lstat(child, &st) == 0 && S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode);
        if (descend && !collect_dirs_recursive(vm, ins, child, dirs)) {
            free(child);
            closedir(dp);
            return false;
        }
        free(child);
    }
    closedir(dp);
    return true;
}

static char *strip_leading_dot_slash_copy(const char *path, bool strip_root_dot) {
    if (strip_root_dot && path && path[0] == '.' && path[1] == '/') return vm_strdup_cstr(path + 2);
    return vm_strdup_cstr(path);
}

static bool collect_recursive_matches_for_dir(Vm *vm, Instr *ins, const char *dir, const char *suffix, bool strip_root_dot, VmStringVec *matches) {
    char *pattern = NULL;
    if (!suffix || !*suffix) {
        pattern = glob_escape_literal_path(dir);
    } else if (strcmp(dir, ".") == 0 && strip_root_dot) {
        pattern = vm_strdup_cstr(suffix);
    } else {
        char *escaped_dir = glob_escape_literal_path(dir);
        pattern = path_join2(escaped_dir, suffix);
        free(escaped_dir);
    }

    glob_t g;
    memset(&g, 0, sizeof(g));
    int grc = glob(pattern, 0, NULL, &g);
    if (grc == GLOB_NOMATCH) {
        free(pattern);
        globfree(&g);
        return true;
    }
    if (grc != 0) {
        ds_diag_error(vm->diag, ins->span, "failed to evaluate recursive glob `%s`", pattern);
        free(pattern);
        globfree(&g);
        return false;
    }
    for (size_t i = 0; i < g.gl_pathc; i++) {
        vm_string_vec_push_owned(matches, strip_leading_dot_slash_copy(g.gl_pathv[i], strip_root_dot));
    }
    free(pattern);
    globfree(&g);
    return true;
}

static bool stdlib_recursive_glob(Vm *vm, Instr *ins, const char *pattern, DsValue *out) {
    DsStr pattern_str = { (char *)pattern, strlen(pattern) };
    size_t recursive_count = 0;
    DsGlobPatternStatus status = ds_glob_pattern_validate(pattern_str, &recursive_count);
    if (status != DS_GLOB_PATTERN_OK) {
        ds_diag_error(vm->diag, ins->span, "%s", ds_glob_pattern_status_message(status));
        return false;
    }

    const char *marker = strstr(pattern, "**");
    if (!marker) return false;
    size_t prefix_len = (size_t)(marker - pattern);
    size_t suffix_start = prefix_len + 2;
    char *prefix = vm_strdup_range(pattern, prefix_len);
    char *suffix = vm_strdup_cstr(pattern + suffix_start);
    if (suffix[0] == '/') memmove(suffix, suffix + 1, strlen(suffix));

    char *base_pattern = NULL;
    bool strip_root_dot = false;
    normalize_recursive_prefix(prefix, &base_pattern, &strip_root_dot);

    VmStringVec bases = {0};
    VmStringVec dirs = {0};
    VmStringVec matches = {0};
    bool ok = collect_base_dirs(vm, ins, base_pattern, &bases);
    for (size_t i = 0; ok && i < bases.len; i++) ok = collect_dirs_recursive(vm, ins, bases.items[i], &dirs);
    for (size_t i = 0; ok && i < dirs.len; i++) ok = collect_recursive_matches_for_dir(vm, ins, dirs.items[i], suffix, strip_root_dot, &matches);

    free(prefix);
    free(suffix);
    free(base_pattern);
    vm_string_vec_free(&bases);
    vm_string_vec_free(&dirs);

    if (!ok) {
        vm_string_vec_free(&matches);
        return false;
    }

    if (matches.len > 0) qsort(matches.items, matches.len, sizeof(char *), cmp_cstr_ptr);

    DsValue array = ds_value_null();
    array.kind = DS_VALUE_ARRAY;
    ds_array_init(&array.as.array);
    const char *prev = NULL;
    for (size_t i = 0; i < matches.len; i++) {
        if (prev && strcmp(prev, matches.items[i]) == 0) continue;
        array_push_string(&array, matches.items[i], strlen(matches.items[i]));
        prev = matches.items[i];
    }
    vm_string_vec_free(&matches);
    *out = array;
    return true;
}

static bool is_executable_file(const char *path) {
    struct stat st;
    return path && stat(path, &st) == 0 && !S_ISDIR(st.st_mode) && access(path, X_OK) == 0;
}

static bool read_path_to_string_msg(Vm *vm, Instr *ins, const char *path, const char *what, DsValue *out) {
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        ds_diag_error(vm->diag, ins->span, "failed to read %s `%s`: %s", what, path, strerror(errno));
        return false;
    }

    DsString s;
    ds_string_init(&s);
    char buf[4096];
    size_t n = 0;
    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) {
        if (memchr(buf, '\0', n)) {
            ds_diag_error(vm->diag, ins->span, "%s `%s` contains embedded NUL bytes", what, path);
            fclose(fp);
            ds_string_free(&s);
            return false;
        }
        ds_string_append_range(&s, buf, n);
    }

    if (ferror(fp)) {
        ds_diag_error(vm->diag, ins->span, "failed to read %s `%s`: %s", what, path, strerror(errno));
        fclose(fp);
        ds_string_free(&s);
        return false;
    }

    fclose(fp);
    *out = ds_value_string_take(&s);
    return true;
}

static bool write_string_to_path(Vm *vm, Instr *ins, const char *path, const char *text, size_t len, bool append) {
    FILE *fp = fopen(path, append ? "ab" : "wb");
    const char *op = append ? "append" : "write";
    if (!fp) {
        ds_diag_error(vm->diag, ins->span, "failed to %s file `%s`: %s", op, path, strerror(errno));
        return false;
    }

    if (len && fwrite(text, 1, len, fp) != len) {
        ds_diag_error(vm->diag, ins->span, "failed to %s file `%s`: %s", op, path, strerror(errno));
        fclose(fp);
        return false;
    }

    if (fclose(fp) != 0) {
        ds_diag_error(vm->diag, ins->span, "failed to %s file `%s`: %s", op, path, strerror(errno));
        return false;
    }
    return true;
}

static char *path_join_parts(Vm *vm, Instr *ins) {
    DsString s;
    ds_string_init(&s);

    for (size_t i = 0; i < ins->arg_count; i++) {
        const char *part = NULL;
        size_t len = 0;
        if (!vm_string_arg(vm, ins, i, &part, &len)) {
            ds_string_free(&s);
            return NULL;
        }

        if (i == 0) {
            ds_string_append_range(&s, part, len);
            continue;
        }

        while (s.len > 0 && s.data[s.len - 1] == '/') s.len--;
        while (len > 0 && *part == '/') {
            part++;
            len--;
        }
        ds_string_append_char(&s, '/');
        ds_string_append_range(&s, part, len);
    }

    char *out = ds_str_dup_range(s.data ? s.data : "", s.len);
    ds_string_free(&s);
    return out;
}

static bool stdlib_path_status(Vm *vm, Instr *ins, DsValue *out, VmPathKind kind) {
    char *path = vm_string_arg_dup(vm, ins, 0);
    if (!path) return false;

    struct stat st;
    bool ok = stat(path, &st) == 0;
    if (!ok && errno != ENOENT && errno != ENOTDIR) {
        ds_diag_error(vm->diag, ins->span, "failed to stat `%s`: %s", path, strerror(errno));
        free(path);
        return false;
    }

    bool result = ok;
    if (kind == VM_PATH_KIND_FILE) result = ok && S_ISREG(st.st_mode);
    if (kind == VM_PATH_KIND_DIR) result = ok && S_ISDIR(st.st_mode);

    free(path);
    *out = ds_value_bool(result);
    return true;
}

static bool stdlib_file_read(Vm *vm, Instr *ins, DsValue *out) {
    char *path = vm_string_arg_dup(vm, ins, 0);
    if (!path) return false;

    bool ok = read_path_to_string_msg(vm, ins, path, "file", out);
    free(path);
    return ok;
}

static bool stdlib_file_write_or_append(Vm *vm, Instr *ins, bool append) {
    char *path = vm_string_arg_dup(vm, ins, 0);
    const char *text = NULL;
    size_t len = 0;
    if (!path || !vm_string_arg(vm, ins, 1, &text, &len)) {
        free(path);
        return false;
    }

    bool ok = write_string_to_path(vm, ins, path, text, len, append);
    free(path);
    return ok;
}

static bool stdlib_path_part(Vm *vm, Instr *ins, DsValue *out) {
    char *path = vm_string_arg_dup(vm, ins, 0);
    if (!path) return false;

    char *result = NULL;
    if (helper_is(ins, "path.basename")) {
        char *slash = strrchr(path, '/');
        const char *base = slash ? slash + 1 : path;
        result = ds_str_dup_range(base, strlen(base));
    } else if (helper_is(ins, "path.dirname")) {
        char *slash = strrchr(path, '/');
        if (!slash) result = ds_str_dup_range(".", 1);
        else if (slash == path) result = ds_str_dup_range("/", 1);
        else result = ds_str_dup_range(path, (size_t)(slash - path));
    } else {
        char *base = strrchr(path, '/');
        base = base ? base + 1 : path;
        char *dot = strrchr(base, '.');
        if (!dot || dot == base) result = ds_str_dup_range("", 0);
        else result = ds_str_dup_range(dot, strlen(dot));
    }

    free(path);
    return value_string_from_owned_cstr(out, result);
}

static bool command_exists_on_path(const char *cmd) {
    if (strchr(cmd, '/')) return is_executable_file(cmd);

    char *path = getenv("PATH");
    if (!path) return false;

    char *copy = strdup(path);
    bool found = false;
    for (char *save = NULL, *dir = strtok_r(copy, ":", &save); dir; dir = strtok_r(NULL, ":", &save)) {
        DsString full;
        ds_string_init(&full);
        ds_string_append_cstr(&full, *dir ? dir : ".");
        ds_string_append_char(&full, '/');
        ds_string_append_cstr(&full, cmd);
        found = is_executable_file(full.data);
        ds_string_free(&full);
        if (found) break;
    }
    free(copy);
    return found;
}

static bool stdlib_cmd(Vm *vm, Instr *ins, DsValue *out) {
    char *cmd = vm_string_arg_dup(vm, ins, 0);
    if (!cmd) return false;

    bool found = command_exists_on_path(cmd);
    if (!found && helper_is(ins, "cmd.require")) {
        ds_diag_error(vm->diag, ins->span, "required command `%s` was not found on PATH", cmd);
        free(cmd);
        return false;
    }

    free(cmd);
    *out = helper_is(ins, "cmd.exists") ? ds_value_bool(found) : ds_value_null();
    return true;
}

static bool stdlib_env_get(Vm *vm, Instr *ins, DsValue *out) {
    char *key = vm_string_arg_dup(vm, ins, 0);
    if (!key) return false;
    if (!vm_require_env_name(vm, ins, key)) {
        free(key);
        return false;
    }

    char *value = getenv(key);
    if (!value && ins->arg_count == 2) {
        const char *fallback = NULL;
        size_t len = 0;
        if (!vm_string_arg(vm, ins, 1, &fallback, &len)) {
            free(key);
            return false;
        }
        DsString s;
        ds_string_from_range(&s, fallback, len);
        *out = ds_value_string_take(&s);
        free(key);
        return true;
    }

    DsString s;
    ds_string_from_cstr(&s, value ? value : "");
    *out = ds_value_string_take(&s);
    free(key);
    return true;
}

static bool stdlib_env_set(Vm *vm, Instr *ins) {
    char *key = vm_string_arg_dup(vm, ins, 0);
    char *value = vm_string_arg_dup(vm, ins, 1);
    if (!key || !value) {
        free(key);
        free(value);
        return false;
    }
    if (!vm_require_env_name(vm, ins, key)) {
        free(key);
        free(value);
        return false;
    }

    if (setenv(key, value, 1) != 0) {
        ds_diag_error(vm->diag, ins->span, "failed to set environment `%s`: %s", key, strerror(errno));
        free(key);
        free(value);
        return false;
    }
    free(key);
    free(value);
    return true;
}

static bool stdlib_env_unset(Vm *vm, Instr *ins) {
    char *key = vm_string_arg_dup(vm, ins, 0);
    if (!key) return false;
    if (!vm_require_env_name(vm, ins, key)) {
        free(key);
        return false;
    }

    if (unsetenv(key) != 0) {
        ds_diag_error(vm->diag, ins->span, "failed to unset environment `%s`: %s", key, strerror(errno));
        free(key);
        return false;
    }
    free(key);
    return true;
}

static bool stdlib_glob(Vm *vm, Instr *ins, DsValue *out) {
    char *pattern = vm_string_arg_dup(vm, ins, 0);
    if (!pattern) return false;

    DsStr pattern_str = {pattern, strlen(pattern)};
    if (ds_glob_pattern_contains_recursive(pattern_str)) {
        bool ok = stdlib_recursive_glob(vm, ins, pattern, out);
        if (ok && helper_is(ins, "glob!") && out->kind == DS_VALUE_ARRAY && out->as.array.len == 0) {
            ds_diag_error(vm->diag, ins->span, "required glob `%s` had no matches", pattern);
            ds_value_free(out);
            ok = false;
        }
        free(pattern);
        return ok;
    }

    glob_t g;
    memset(&g, 0, sizeof(g));
    int grc = glob(pattern, 0, NULL, &g);

    DsValue array = ds_value_null();
    array.kind = DS_VALUE_ARRAY;
    ds_array_init(&array.as.array);

    if (grc == GLOB_NOMATCH) {
        if (helper_is(ins, "glob!")) {
            ds_diag_error(vm->diag, ins->span, "required glob `%s` had no matches", pattern);
            free(pattern);
            globfree(&g);
            ds_value_free(&array);
            return false;
        }
    } else if (grc != 0) {
        ds_diag_error(vm->diag, ins->span, "failed to evaluate glob `%s`", pattern);
        free(pattern);
        globfree(&g);
        ds_value_free(&array);
        return false;
    } else {
        qsort(g.gl_pathv, g.gl_pathc, sizeof(char *), cmp_cstr_ptr);
        for (size_t i = 0; i < g.gl_pathc; i++) array_push_string(&array, g.gl_pathv[i], strlen(g.gl_pathv[i]));
    }

    free(pattern);
    globfree(&g);
    *out = array;
    return true;
}

static bool stdlib_lines(Vm *vm, Instr *ins, DsValue *out) {
    char *path = vm_string_arg_dup(vm, ins, 0);
    if (!path) return false;

    DsValue text = ds_value_null();
    if (!read_path_to_string_msg(vm, ins, path, "lines from", &text)) {
        free(path);
        return false;
    }
    free(path);

    DsValue array = ds_value_null();
    array.kind = DS_VALUE_ARRAY;
    ds_array_init(&array.as.array);

    size_t start = 0;
    for (size_t i = 0; i < text.as.string.len; i++) {
        if (text.as.string.data[i] != '\n') continue;
        size_t end = i > start && text.as.string.data[i - 1] == '\r' ? i - 1 : i;
        array_push_string(&array, text.as.string.data + start, end - start);
        start = i + 1;
    }
    if (start < text.as.string.len) {
        array_push_string(&array, text.as.string.data + start, text.as.string.len - start);
    }

    ds_value_free(&text);
    *out = array;
    return true;
}



static bool contains_bytes(const char *s, size_t len, const char *sub, size_t sub_len) {
    if (sub_len == 0) return true;
    if (sub_len > len) return false;
    for (size_t i = 0; i + sub_len <= len; i++) if (memcmp(s + i, sub, sub_len) == 0) return true;
    return false;
}

static bool ascii_trim_bounds(const char *s, size_t len, size_t *start, size_t *end) {
    size_t a = 0, b = len;
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\n' || s[a] == '\r' || s[a] == '\v' || s[a] == '\f')) a++;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\n' || s[b - 1] == '\r' || s[b - 1] == '\v' || s[b - 1] == '\f')) b--;
    *start = a; *end = b; return true;
}

static bool string_method(Vm *vm, Instr *ins, DsValue *out) {
    const char *s = NULL; size_t len = 0;
    if (!vm_string_arg(vm, ins, 0, &s, &len)) return false;
    if (helper_is(ins, "string.trim")) {
        size_t a = 0, b = 0; ascii_trim_bounds(s, len, &a, &b);
        DsString r; ds_string_from_range(&r, s + a, b - a); *out = ds_value_string_take(&r); return true;
    }
    if (helper_is(ins, "string.upper") || helper_is(ins, "string.lower")) {
        DsString r; ds_string_from_range(&r, s, len);
        for (size_t i = 0; i < r.len; i++) {
            if (helper_is(ins, "string.upper") && r.data[i] >= 'a' && r.data[i] <= 'z') r.data[i] = (char)(r.data[i] - 'a' + 'A');
            if (helper_is(ins, "string.lower") && r.data[i] >= 'A' && r.data[i] <= 'Z') r.data[i] = (char)(r.data[i] - 'A' + 'a');
        }
        *out = ds_value_string_take(&r); return true;
    }
    if (helper_is(ins, "string.contains") || helper_is(ins, "string.starts_with") || helper_is(ins, "string.ends_with")) {
        const char *sub = NULL; size_t sub_len = 0;
        if (!vm_string_arg(vm, ins, 1, &sub, &sub_len)) return false;
        bool ok = false;
        if (helper_is(ins, "string.contains")) ok = contains_bytes(s, len, sub, sub_len);
        else if (helper_is(ins, "string.starts_with")) ok = sub_len <= len && memcmp(s, sub, sub_len) == 0;
        else ok = sub_len <= len && memcmp(s + len - sub_len, sub, sub_len) == 0;
        *out = ds_value_bool(ok); return true;
    }
    if (helper_is(ins, "string.replace")) {
        const char *from = NULL, *to = NULL; size_t from_len = 0, to_len = 0;
        if (!vm_string_arg(vm, ins, 1, &from, &from_len) || !vm_string_arg(vm, ins, 2, &to, &to_len)) return false;
        if (from_len == 0) { ds_diag_error(vm->diag, ins->span, "replace with an empty runtime source is rejected in v0.19.0"); return false; }
        DsString r; ds_string_init(&r);
        size_t i = 0;
        while (i < len) {
            if (i + from_len <= len && memcmp(s + i, from, from_len) == 0) { ds_string_append_range(&r, to, to_len); i += from_len; }
            else { ds_string_append_char(&r, s[i]); i++; }
        }
        *out = ds_value_string_take(&r); return true;
    }
    if (helper_is(ins, "string.split")) {
        const char *sep = NULL; size_t sep_len = 0;
        if (!vm_string_arg(vm, ins, 1, &sep, &sep_len)) return false;
        if (sep_len == 0) { ds_diag_error(vm->diag, ins->span, "split with an empty runtime separator is rejected in v0.19.0"); return false; }
        DsValue array = ds_value_null(); array.kind = DS_VALUE_ARRAY; ds_array_init(&array.as.array);
        size_t part = 0, i = 0;
        while (i + sep_len <= len) {
            if (memcmp(s + i, sep, sep_len) == 0) { array_push_string(&array, s + part, i - part); i += sep_len; part = i; }
            else i++;
        }
        array_push_string(&array, s + part, len - part);
        *out = array; return true;
    }
    ds_diag_error(vm->diag, ins->span, "internal VM stdlib invariant failed: unknown string method `%s` after lowering", ins->name ? ins->name : "");
    return false;
}

static bool regex_map_set_bool(DsValue *map, const char *key, bool value) {
    DsStr k = {(char *)key, strlen(key)};
    return ds_map_set(&map->as.map, k, ds_value_bool(value));
}

static bool regex_map_set_string(DsValue *map, const char *key, const char *data, size_t len) {
    DsString s;
    ds_string_from_range(&s, data ? data : "", len);
    DsStr k = {(char *)key, strlen(key)};
    return ds_map_set(&map->as.map, k, ds_value_string_take(&s));
}

static bool regex_compile_runtime(Vm *vm, Instr *ins, DsStr pattern, DsStr flags_text, regex_t *re, size_t *capture_count) {
    size_t captures = 0;
    DsRegexStatus status = ds_regex_validate_pattern(pattern, &captures);
    if (status != DS_REGEX_OK) {
        ds_diag_error(vm->diag, ins->span, "%s", ds_regex_status_message(status));
        return false;
    }
    int cflags = 0;
    status = ds_regex_validate_flags(flags_text, &cflags);
    if (status != DS_REGEX_OK) {
        ds_diag_error(vm->diag, ins->span, "%s", ds_regex_status_message(status));
        return false;
    }
    char *tmp = ds_str_dup_range(pattern.data ? pattern.data : "", pattern.len);
    int err = regcomp(re, tmp, cflags);
    free(tmp);
    if (err != 0) {
        ds_diag_error(vm->diag, ins->span, "invalid regex pattern in v0.32.0");
        return false;
    }
    if (capture_count) *capture_count = captures;
    return true;
}

static bool stdlib_regex_match(Vm *vm, Instr *ins, DsValue *out) {
    const char *text = NULL, *pattern_data = NULL, *flags_data = "";
    size_t text_len = 0, pattern_len = 0, flags_len = 0;
    if (!vm_string_arg(vm, ins, 0, &text, &text_len) || !vm_string_arg(vm, ins, 1, &pattern_data, &pattern_len)) return false;
    if (ins->arg_count > 2 && !vm_string_arg(vm, ins, 2, &flags_data, &flags_len)) return false;

    regex_t re;
    size_t capture_count = 0;
    if (!regex_compile_runtime(vm, ins, (DsStr){(char *)pattern_data, pattern_len}, (DsStr){(char *)flags_data, flags_len}, &re, &capture_count)) return false;

    regmatch_t matches[10];
    size_t nmatch = capture_count + 1;
    if (nmatch > 10) nmatch = 10;
    int rc = regexec(&re, text ? text : "", nmatch, matches, 0);
    regfree(&re);
    if (rc != 0 && rc != REG_NOMATCH) {
        ds_diag_error(vm->diag, ins->span, "failed to evaluate regex in v0.32.0");
        return false;
    }

    DsValue map = ds_value_null();
    map.kind = DS_VALUE_MAP;
    ds_map_init(&map.as.map);
    bool matched = rc == 0;
    if (!regex_map_set_bool(&map, "matched", matched) ||
        !regex_map_set_string(&map, "full", matched ? text + matches[0].rm_so : "", matched ? (size_t)(matches[0].rm_eo - matches[0].rm_so) : 0) ||
        !regex_map_set_string(&map, "0", matched ? text + matches[0].rm_so : "", matched ? (size_t)(matches[0].rm_eo - matches[0].rm_so) : 0)) {
        ds_value_free(&map);
        return false;
    }
    if (matched) {
        for (size_t i = 1; i <= capture_count && i < 10; i++) {
            char key[2] = {(char)('0' + i), '\0'};
            if (matches[i].rm_so >= 0 && matches[i].rm_eo >= matches[i].rm_so) {
                if (!regex_map_set_string(&map, key, text + matches[i].rm_so, (size_t)(matches[i].rm_eo - matches[i].rm_so))) {
                    ds_value_free(&map);
                    return false;
                }
            } else if (!regex_map_set_string(&map, key, "", 0)) {
                ds_value_free(&map);
                return false;
            }
        }
    }
    *out = map;
    return true;
}

static bool regex_expand_replacement(Vm *vm, Instr *ins, DsString *out, const char *replacement, size_t replacement_len, const char *base, const regmatch_t *matches, size_t capture_count) {
    for (size_t i = 0; i < replacement_len; i++) {
        if (replacement[i] != '$') {
            if (!ds_string_append_char(out, replacement[i])) return false;
            continue;
        }
        if (i + 1 >= replacement_len) {
            ds_diag_error(vm->diag, ins->span, "%s", ds_regex_status_message(DS_REGEX_ERR_INVALID_REPLACEMENT));
            return false;
        }
        char n = replacement[++i];
        if (n == '$') {
            if (!ds_string_append_char(out, '$')) return false;
            continue;
        }
        if (n < '0' || n > '9') {
            ds_diag_error(vm->diag, ins->span, "%s", ds_regex_status_message(DS_REGEX_ERR_INVALID_REPLACEMENT));
            return false;
        }
        size_t ref = (size_t)(n - '0');
        if (ref > capture_count || ref >= 10) {
            ds_diag_error(vm->diag, ins->span, "%s", ds_regex_status_message(DS_REGEX_ERR_UNKNOWN_CAPTURE));
            return false;
        }
        if (matches[ref].rm_so >= 0 && matches[ref].rm_eo >= matches[ref].rm_so) {
            if (!ds_string_append_range(out, base + matches[ref].rm_so, (size_t)(matches[ref].rm_eo - matches[ref].rm_so))) return false;
        }
    }
    return true;
}

static bool stdlib_regex_replace(Vm *vm, Instr *ins, DsValue *out) {
    const char *text = NULL, *pattern_data = NULL, *replacement = NULL, *flags_data = "";
    size_t text_len = 0, pattern_len = 0, replacement_len = 0, flags_len = 0;
    if (!vm_string_arg(vm, ins, 0, &text, &text_len) ||
        !vm_string_arg(vm, ins, 1, &pattern_data, &pattern_len) ||
        !vm_string_arg(vm, ins, 2, &replacement, &replacement_len)) return false;
    if (ins->arg_count > 3 && !vm_string_arg(vm, ins, 3, &flags_data, &flags_len)) return false;

    regex_t re;
    size_t capture_count = 0;
    if (!regex_compile_runtime(vm, ins, (DsStr){(char *)pattern_data, pattern_len}, (DsStr){(char *)flags_data, flags_len}, &re, &capture_count)) return false;
    DsRegexStatus replacement_status = ds_regex_validate_replacement((DsStr){(char *)replacement, replacement_len}, capture_count, true);
    if (replacement_status != DS_REGEX_OK) {
        regfree(&re);
        ds_diag_error(vm->diag, ins->span, "%s", ds_regex_status_message(replacement_status));
        return false;
    }

    DsString result;
    ds_string_init(&result);
    size_t offset = 0;
    regmatch_t matches[10];
    size_t nmatch = capture_count + 1;
    if (nmatch > 10) nmatch = 10;
    while (offset <= text_len) {
        const char *base = text + offset;
        int rc = regexec(&re, base, nmatch, matches, 0);
        if (rc != 0) {
            if (rc != REG_NOMATCH) {
                regfree(&re);
                ds_string_free(&result);
                ds_diag_error(vm->diag, ins->span, "failed to evaluate regex in v0.32.0");
                return false;
            }
            ds_string_append_range(&result, base, text_len - offset);
            break;
        }
        if (matches[0].rm_so < 0 || matches[0].rm_eo <= matches[0].rm_so) {
            regfree(&re);
            ds_string_free(&result);
            ds_diag_error(vm->diag, ins->span, "regex.replace patterns that match empty strings are unsupported in v0.32.0");
            return false;
        }
        ds_string_append_range(&result, base, (size_t)matches[0].rm_so);
        if (!regex_expand_replacement(vm, ins, &result, replacement, replacement_len, base, matches, capture_count)) {
            regfree(&re);
            ds_string_free(&result);
            return false;
        }
        offset += (size_t)matches[0].rm_eo;
        if (offset > text_len) break;
    }
    regfree(&re);
    *out = ds_value_string_take(&result);
    return true;
}


bool ds_vm_stdlib_call(Vm *vm, Instr *ins, DsValue *out) {
    *out = ds_value_null();

    const char *name = ins->name ? ins->name : "";
    DsStr helper_name = {(char *)name, strlen(name)};
    const DsStdlibHelper *helper = ds_stdlib_lookup(helper_name);
    if (!helper) {
        ds_diag_error(vm->diag, ins->span, "internal VM stdlib invariant failed: unknown standard-library helper `%s` after lowering", name);
        return false;
    }

    if (helper_is(ins, "file.exists")) return stdlib_path_status(vm, ins, out, VM_PATH_KIND_EXISTS);
    if (helper_is(ins, "file.is_file")) return stdlib_path_status(vm, ins, out, VM_PATH_KIND_FILE);
    if (helper_is(ins, "dir.exists")) return stdlib_path_status(vm, ins, out, VM_PATH_KIND_DIR);
    if (helper_is(ins, "file.read")) return stdlib_file_read(vm, ins, out);
    if (helper_is(ins, "file.write")) return stdlib_file_write_or_append(vm, ins, false);
    if (helper_is(ins, "file.append")) return stdlib_file_write_or_append(vm, ins, true);

    if (helper_is(ins, "path.cwd")) {
        char *cwd = getcwd(NULL, 0);
        if (!cwd) {
            ds_diag_error(vm->diag, ins->span, "failed to get current directory: %s", strerror(errno));
            return false;
        }
        return value_string_from_owned_cstr(out, cwd);
    }
    if (helper_is(ins, "path.join")) {
        char *joined = path_join_parts(vm, ins);
        if (!joined) return false;
        return value_string_from_owned_cstr(out, joined);
    }
    if (helper_is(ins, "path.basename") || helper_is(ins, "path.dirname") || helper_is(ins, "path.ext")) {
        return stdlib_path_part(vm, ins, out);
    }

    if (helper_is(ins, "cmd.exists") || helper_is(ins, "cmd.require")) return stdlib_cmd(vm, ins, out);
    if (strncmp(name, "string.", 7) == 0) return string_method(vm, ins, out);
    if (helper_is(ins, "env.get")) return stdlib_env_get(vm, ins, out);
    if (helper_is(ins, "env.set")) return stdlib_env_set(vm, ins);
    if (helper_is(ins, "env.unset")) return stdlib_env_unset(vm, ins);
    if (helper_is(ins, "regex.match")) return stdlib_regex_match(vm, ins, out);
    if (helper_is(ins, "regex.replace")) return stdlib_regex_replace(vm, ins, out);
    if (helper_is(ins, "glob") || helper_is(ins, "glob!")) return stdlib_glob(vm, ins, out);
    if (helper_is(ins, "lines")) return stdlib_lines(vm, ins, out);

    ds_diag_error(vm->diag, ins->span, "internal VM stdlib invariant failed: unknown standard-library helper `%s` after lowering", name);
    return false;
}
