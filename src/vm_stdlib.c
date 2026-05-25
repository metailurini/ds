#define _POSIX_C_SOURCE 200809L

#include "vm_internal.h"

#include <errno.h>
#include <glob.h>
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

    DsValue *v = &vm->regs[ins->args[index]];
    if (v->kind != DS_VALUE_STRING) {
        ds_diag_error(vm->diag, ins->span, "standard-library helper `%s` expects string arguments", ins->name ? ins->name : "<helper>");
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
    ds_diag_error(vm->diag, ins->span, "invalid environment variable name `%s` in v0.11.0", name ? name : "");
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
    if (strstr(pattern, "**")) {
        ds_diag_error(vm->diag, ins->span, "runtime glob pattern contains recursive `**`; recursive glob patterns are deferred in v0.11.0");
        free(pattern);
        return false;
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
    if (helper_is(ins, "glob") || helper_is(ins, "glob!")) return stdlib_glob(vm, ins, out);
    if (helper_is(ins, "lines")) return stdlib_lines(vm, ins, out);

    ds_diag_error(vm->diag, ins->span, "internal VM stdlib invariant failed: unknown standard-library helper `%s` after lowering", name);
    return false;
}
