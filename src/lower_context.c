#include "lower_internal.h"

void lower_context_init(Lower *lower, DsDiag *diag, DsLowerProgram *program) {
    if (!lower) return;
    memset(lower, 0, sizeof(*lower));
    lower->diag = diag;
    lower->program = program;
    scope_init(&lower->root_scope, NULL);
    lower->scope = &lower->root_scope;
}

void lower_context_free(Lower *lower) {
    if (!lower) return;
    scope_free(&lower->root_scope);
    free(lower->map_loop_symbols);
    lower->map_loop_symbols = NULL;
    lower->map_loop_len = 0;
    lower->map_loop_cap = 0;
    lower->scope = NULL;
}

void lower_diag_stdlib_arity_error(Lower *lower, DsSpan span, DsStr name,
                                   size_t min_arity, size_t max_arity, size_t actual) {
    if (min_arity == max_arity) {
        ds_diag_error(lower->diag, span, "helper `%.*s` expects %zu arguments but got %zu",
                      (int)name.len, name.data, min_arity, actual);
    } else {
        ds_diag_error(lower->diag, span, "helper `%.*s` expects %zu to %zu arguments but got %zu",
                      (int)name.len, name.data, min_arity, max_arity, actual);
    }
}

void lower_diag_unknown_function(Lower *lower, DsSpan span, DsStr name) {
    ds_diag_error(lower->diag, span, "unknown function `%.*s`", (int)name.len, name.data);
}

void lower_diag_unknown_stdlib_helper(Lower *lower, DsSpan span, DsStr name) {
    ds_diag_error(lower->diag, span, "unknown standard-library helper `%.*s`", (int)name.len, name.data);
}

void lower_diag_unknown_string_method(Lower *lower, DsSpan span, DsStr member) {
    ds_diag_error(lower->diag, span, "unknown string method `%.*s`; supported methods are %s",
                  (int)member.len, member.data, ds_stdlib_string_method_names());
}

bool lower_validate_env_name(Lower *lower, DsStr name, DsSpan span, const char *version) {
    if (is_env_name_text(name)) return true;
    ds_diag_error(lower->diag, span, "invalid environment variable name `%.*s` in %s",
                  (int)name.len, ds_str_data(name), version);
    return false;
}
