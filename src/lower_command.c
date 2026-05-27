#include "lower_internal.h"
#include "ds_interpolation.h"
#include "ds_command_facts.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static DsInterpValueKind interp_kind_from_sym(SymKind kind) {
    switch (kind) {
        case SYM_BOOL: return DS_INTERP_VALUE_BOOL;
        case SYM_INT: return DS_INTERP_VALUE_INT;
        case SYM_STRING: return DS_INTERP_VALUE_STRING;
        case SYM_COMMAND_RESULT: return DS_INTERP_VALUE_COMMAND_RESULT;
        default: return DS_INTERP_VALUE_UNKNOWN;
    }
}

static bool is_supported_format_spec(DsStr spec, SymKind kind) {
    return ds_interp_parse_format_spec_for_kind(spec, interp_kind_from_sym(kind), NULL);
}

static bool lower_validate_arithmetic_interpolation_text(Lower *lower, DsStr body, DsSpan span) {
    /*
     * Keep arithmetic interpolation acceptance in lowering. The VM/Bash
     * arithmetic renderers still parse accepted text at runtime/emission time,
     * but malformed source shapes must not be discovered first by a backend.
     */
    bool expect_operand = true;
    int depth = 0;
    for (size_t i = 0; i < body.len;) {
        char c = body.data[i];
        if (c == ' ' || c == '\t') { i++; continue; }
        if (c >= '0' && c <= '9') {
            while (i < body.len && body.data[i] >= '0' && body.data[i] <= '9') i++;
            expect_operand = false;
            continue;
        }
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_') {
            size_t name_start = i++;
            while (i < body.len && ds_command_name_char(body.data[i])) i++;
            if (i < body.len && body.data[i] == '(') {
                ds_diag_error(lower->diag, span, "function-call interpolation in command words must be bound to a string expression first in v0.21.0");
                return false;
            }
            DsStr name = {body.data + name_start, i - name_start};
            Symbol *sym = scope_find(lower->scope, name);
            if (!sym) {
                ds_diag_error(lower->diag, span, "unknown interpolation variable `%.*s`", (int)name.len, name.data);
                return false;
            }
            lower_validate_handler_capture(lower, sym, name, span);
            if (sym->kind != SYM_INT) {
                ds_diag_error(lower->diag, span, "arithmetic interpolation operands must be integers in v0.21.0");
                return false;
            }
            expect_operand = false;
            continue;
        }
        if (c == '(') {
            if (!expect_operand) return false;
            depth++;
            i++;
            continue;
        }
        if (c == ')') {
            if (expect_operand || depth <= 0) return false;
            depth--;
            i++;
            expect_operand = false;
            continue;
        }
        if (c == '+' || c == '-' || c == '*' || c == '/' || c == '%') {
            if (expect_operand) {
                if (c == '-') { i++; continue; }
                return false;
            }
            if (c == '*' && i + 1 < body.len && body.data[i + 1] == '*') i += 2;
            else i++;
            expect_operand = true;
            continue;
        }
        return false;
    }
    return !expect_operand && depth == 0;
}

bool lower_validate_word_interpolation(Lower *lower, DsStr text, DsSpan span) {
    DsStr decoded = {0};
    if (!lower_decode_string_text(text, &decoded)) return true;
    for (size_t i = 0; i < decoded.len; i++) {
        char c = decoded.data[i];
        if (c != '{') continue;
        size_t start = i + 1;
        size_t j = start;
        if (j < decoded.len && ((decoded.data[j] >= 'A' && decoded.data[j] <= 'Z') || (decoded.data[j] >= 'a' && decoded.data[j] <= 'z') || decoded.data[j] == '_')) {
            j++;
            while (j < decoded.len && ((decoded.data[j] >= 'A' && decoded.data[j] <= 'Z') || (decoded.data[j] >= 'a' && decoded.data[j] <= 'z') || (decoded.data[j] >= '0' && decoded.data[j] <= '9') || decoded.data[j] == '_')) j++;
            DsStr name = {decoded.data + start, j - start};
            if (j < decoded.len && decoded.data[j] == '(') {
                ds_diag_error(lower->diag, span, "function-call interpolation in command words must be bound to a string expression first in v0.21.0");
                free(decoded.data);
                return false;
            }
            if (name.len == 3 && memcmp(name.data, "env", 3) == 0 && j < decoded.len && decoded.data[j] == '.') {
                size_t field_start = ++j;
                if (j < decoded.len && ((decoded.data[j] >= 'A' && decoded.data[j] <= 'Z') || (decoded.data[j] >= 'a' && decoded.data[j] <= 'z') || decoded.data[j] == '_')) {
                    j++;
                    while (j < decoded.len && ((decoded.data[j] >= 'A' && decoded.data[j] <= 'Z') || (decoded.data[j] >= 'a' && decoded.data[j] <= 'z') || (decoded.data[j] >= '0' && decoded.data[j] <= '9') || decoded.data[j] == '_')) j++;
                }
                DsStr field = {decoded.data + field_start, j - field_start};
                if (!is_env_name_text(field)) {
                    ds_diag_error(lower->diag, span, "invalid environment variable name `%.*s` in v0.27.0", (int)field.len, field.data);
                    free(decoded.data);
                    return false;
                }
                if (j < decoded.len && decoded.data[j] == '}') { i = j; continue; }
            }
            Symbol *sym = scope_find(lower->scope, name);
            if (!sym) {
                ds_diag_error(lower->diag, span, "unknown interpolation variable `%.*s`", (int)name.len, name.data);
                free(decoded.data);
                return false;
            }
            lower_validate_handler_capture(lower, sym, name, span);
            SymKind value_kind = sym->kind;
            if (j < decoded.len && decoded.data[j] == '.') {
                size_t field_start = ++j;
                if (j < decoded.len && ((decoded.data[j] >= 'A' && decoded.data[j] <= 'Z') || (decoded.data[j] >= 'a' && decoded.data[j] <= 'z') || decoded.data[j] == '_')) {
                    j++;
                    while (j < decoded.len && ((decoded.data[j] >= 'A' && decoded.data[j] <= 'Z') || (decoded.data[j] >= 'a' && decoded.data[j] <= 'z') || (decoded.data[j] >= '0' && decoded.data[j] <= '9') || decoded.data[j] == '_')) j++;
                }
                DsStr field = {decoded.data + field_start, j - field_start};
                if (sym->kind != SYM_COMMAND_RESULT) {
                    ds_diag_error(lower->diag, span, "field interpolation is only supported on command results in v0.7.0");
                    free(decoded.data);
                    return false;
                }
                if (!command_result_field_kind(field, &value_kind)) {
                    ds_diag_error(lower->diag, span, "unknown command result field `%.*s`", (int)field.len, field.data);
                    free(decoded.data);
                    return false;
                }
            }
            if (j < decoded.len && decoded.data[j] == ':') {
                size_t spec_start = ++j;
                while (j < decoded.len && decoded.data[j] != '}') j++;
                if (j >= decoded.len) break;
                DsStr spec = {decoded.data + spec_start, j - spec_start};
                if (!is_supported_format_spec(spec, value_kind)) {
                    ds_diag_error(lower->diag, span, "unsupported interpolation format specifier `%.*s`; supported: %s", (int)spec.len, spec.data, ds_interp_supported_format_specs());
                    free(decoded.data);
                    return false;
                }
            }
            if (j < decoded.len && decoded.data[j] == '}') { i = j; continue; }
        }
        size_t k = start;
        bool maybe_arith = false;
        while (k < decoded.len && decoded.data[k] != '}') {
            char ac = decoded.data[k++];
            if (ac == '+' || ac == '-' || ac == '*' || ac == '/' || ac == '%' || ac == '(' || ac == ')') maybe_arith = true;
        }
        if (maybe_arith && k < decoded.len && decoded.data[k] == '}') {
            DsStr body = {decoded.data + start, k - start};
            if (!lower_validate_arithmetic_interpolation_text(lower, body, span)) {
                if (!lower->diag->has_error) ds_diag_error(lower->diag, span, "invalid arithmetic interpolation in v0.21.0");
                free(decoded.data);
                return false;
            }
            i = k;
            continue;
        }
        ds_diag_error(lower->diag, span, "unsupported string interpolation; expected `{name}`, `{name.field}`, arithmetic, or a supported `:specifier`");
        free(decoded.data);
        return false;
    }
    free(decoded.data);
    return true;
}


bool lower_validate_command_word(Lower *lower, DsStr word, DsSpan span) {
    if ((word.len == 2 && ((word.data[0] == '*' && word.data[1] == '=') ||
                           (word.data[0] == '/' && word.data[1] == '=') ||
                           (word.data[0] == '%' && word.data[1] == '=')))) {
        ds_diag_error(lower->diag, span, "compound assignment target must be a variable in v0.21.0");
        return false;
    }
    DsCommandWordForm form = ds_command_word_analyze(word);
    if (word.data[0] == '$' && (form.kind == DS_COMMAND_WORD_VARIABLE || form.kind == DS_COMMAND_WORD_FIELD)) {
        DsStr name = form.name;
        Symbol *sym = scope_find(lower->scope, name);
        if (!sym) {
            if (find_function(lower->program, name)) {
                ds_diag_error(lower->diag, span, "function `%.*s` cannot be used as a variable in v0.9.0", (int)name.len, name.data);
            } else {
                ds_diag_error(lower->diag, span, "unknown command variable `%.*s`", (int)name.len, name.data);
            }
            return false;
        }
        if (sym->kind == SYM_FUNCTION) {
            ds_diag_error(lower->diag, span, "function `%.*s` cannot be used as a variable in v0.9.0", (int)name.len, name.data);
            return false;
        }
        lower_validate_handler_capture(lower, sym, name, span);
        if (word.data[0] == '$' && form.kind == DS_COMMAND_WORD_VARIABLE && name.len + 1 < word.len) {
            char suffix = word.data[name.len + 1];
            if ((sym->kind == SYM_ARRAY || sym->kind == SYM_MAP) && (suffix == '[' || suffix == '.')) {
                ds_diag_error(lower->diag, span, "collection access command arguments are deferred in v0.10.0; bind the indexed value to a variable first");
                return false;
            }
            ds_diag_error(lower->diag, span, "unsupported command variable suffix in v0.10.0");
            return false;
        }
        if (word.data[0] == '$' && form.kind == DS_COMMAND_WORD_FIELD) {
            if (sym->kind == SYM_ARRAY || sym->kind == SYM_MAP) {
                ds_diag_error(lower->diag, span, "collection access command arguments are deferred in v0.10.0; bind the indexed value to a variable first");
                return false;
            }
            if (sym->kind != SYM_COMMAND_RESULT) {
                ds_diag_error(lower->diag, span, "unsupported command variable suffix in v0.10.0");
                return false;
            }
            SymKind field_kind = SYM_UNKNOWN;
            if (!command_result_field_kind(form.field, &field_kind)) {
                DsSpan field_span = span;
                field_span.start.offset = span.start.offset + (int)name.len + 2;
                field_span.start.column = span.start.column + (int)name.len + 2;
                field_span.end.offset = field_span.start.offset + (int)form.field.len;
                field_span.end.column = field_span.start.column + (int)form.field.len;
                ds_diag_error(lower->diag, field_span, "unsupported command result field `%.*s`", (int)form.field.len, form.field.data);
                return false;
            }
        }
        if (sym->kind == SYM_ARRAY || sym->kind == SYM_MAP) {
            ds_diag_error(lower->diag, span, "collection `%.*s` cannot be passed directly as a command argument in v0.10.0; index it first", (int)name.len, name.data);
            return false;
        }
    }
    if (form.kind == DS_COMMAND_WORD_QUOTED) return lower_validate_word_interpolation(lower, word, span);
    if (form.kind == DS_COMMAND_WORD_FIELD && word.data[0] != '$') {
        DsStr name = form.name;
        DsStr field = form.field;
        size_t i = (size_t)(field.data - word.data - 1);
        DsSpan field_span = span;
        field_span.start.offset = span.start.offset + (int)i + 1;
        field_span.start.column = span.start.column + (int)i + 1;
        field_span.end.offset = field_span.start.offset + (int)field.len;
        field_span.end.column = field_span.start.column + (int)field.len;
        if (field.len == 0) {
            ds_diag_error(lower->diag, field_span, "expected field name after `.`");
            return false;
        }
        if (name.len == 3 && memcmp(name.data, "env", 3) == 0) {
            if (!is_env_name_text(field)) {
                ds_diag_error(lower->diag, field_span, "invalid environment variable name `%.*s` in v0.27.0", (int)field.len, field.data);
                return false;
            }
            return true;
        }
        SymKind field_kind = SYM_UNKNOWN;
        Symbol *sym = scope_find(lower->scope, name);
        if (!sym) {
            DsSpan name_span = span;
            name_span.end.offset = name_span.start.offset + (int)name.len;
            name_span.end.column = name_span.start.column + (int)name.len;
            ds_diag_error(lower->diag, name_span, "unknown command variable `%.*s`", (int)name.len, name.data);
            return false;
        }
        lower_validate_handler_capture(lower, sym, name, span);
        if (sym->kind != SYM_COMMAND_RESULT) {
            if (sym->kind == SYM_MAP) {
                ds_diag_error(lower->diag, field_span, "map field command arguments are deferred in v0.10.0; bind the field to a variable first");
                return false;
            }
            ds_diag_error(lower->diag, field_span, "field access is only supported on command results and maps in v0.10.0");
            return false;
        }
        if (!command_result_field_kind(field, &field_kind)) {
            ds_diag_error(lower->diag, field_span, "unsupported command result field `%.*s`", (int)field.len, field.data);
            return false;
        }
    }
    return true;
}

static bool command_quoted_word_needs_value_call_materialization(Lower *lower, DsStr word) {
    if (word.len < 2 || word.data[0] != '"' || word.data[word.len - 1] != '"') return false;
    DsStr decoded = {0};
    if (!lower_decode_string_text(word, &decoded)) return false;
    bool found = ds_command_word_contains_direct_call_interpolation(decoded);
    free(decoded.data);
    (void)lower;
    return found;
}

static DsStr lower_make_temp_name(Lower *lower, const char *prefix) {
    char buf[96];
    do {
        snprintf(buf, sizeof(buf), "__ds_%s_%zu", prefix, lower->temp_counter++);
        DsStr candidate = {buf, strlen(buf)};
        if (!scope_find(lower->scope, candidate)) break;
    } while (true);
    return (DsStr){ds_str_dup_range(buf, strlen(buf)), strlen(buf)};
}

static DsLowerStmt *lower_command_interpolation_temp_string_let(Lower *lower, DsStr name, DsStr quoted_text, DsSpan span) {
    DsExpr fake;
    memset(&fake, 0, sizeof(fake));
    fake.kind = DS_EXPR_STRING;
    fake.span = span;
    fake.as.text = quoted_text;
    SymKind kind = SYM_UNKNOWN;
    DsLowerStmt *let = stmt_new(DS_LOWER_STMT_LET, span);
    let->as.let_stmt.name = str_clone(name);
    let->as.let_stmt.value = lower_expr(lower, &fake, &kind);
    if (kind != SYM_STRING && kind != SYM_UNKNOWN) {
        ds_diag_error(lower->diag, span, "function call in command interpolation must return a scalar string-renderable value in v0.27.0");
    }
    scope_define(lower, lower->scope, name, SYM_STRING, span);
    return let;
}

static DsStr lower_command_temp_word(DsStr name) {
    DsString s;
    ds_string_init(&s);
    ds_string_append_char(&s, '$');
    ds_string_append_range(&s, name.data, name.len);
    return (DsStr){s.data, s.len};
}

static DsStr lower_redirect_temp_target(DsStr name) {
    DsString s;
    ds_string_init(&s);
    ds_string_append_char(&s, '"');
    ds_string_append_char(&s, '{');
    ds_string_append_range(&s, name.data, name.len);
    ds_string_append_char(&s, '}');
    ds_string_append_char(&s, '"');
    return (DsStr){s.data, s.len};
}

bool lower_materialize_command_value_call_interpolation(Lower *lower, DsCommand *command, DsLowerStmt *block) {
    /*
     * M3.4 command-word contract: direct scalar value-call interpolation in a
     * quoted command word is not handed to VM/Bash as a backend-specific
     * command substitution problem. Lowering evaluates the interpolation as a
     * normal string expression into a private temporary, then rewrites the
     * command word to an ordinary `$temp` argument. Unsupported return kinds are
     * diagnosed while lowering the temporary string expression.
     */
    bool changed = false;
    for (size_t s = 0; s < command->stages.len; s++) {
        for (size_t i = 0; i < command->stages.items[s].words.len; i++) {
            DsWord *word = &command->stages.items[s].words.items[i];
            if (!command_quoted_word_needs_value_call_materialization(lower, word->text)) continue;
            DsStr tmp = lower_make_temp_name(lower, "cmd_interp");
            lower_stmt_vec_push(&block->as.block_stmt.statements, lower_command_interpolation_temp_string_let(lower, tmp, word->text, word->span));
            free(word->text.data);
            word->text = lower_command_temp_word(tmp);
            free(tmp.data);
            changed = true;
        }
    }
    if (command->redirect.kind != DS_REDIRECT_NONE && command_quoted_word_needs_value_call_materialization(lower, command->redirect.target)) {
        DsStr tmp = lower_make_temp_name(lower, "redir_interp");
        lower_stmt_vec_push(&block->as.block_stmt.statements, lower_command_interpolation_temp_string_let(lower, tmp, command->redirect.target, command->redirect.target_span));
        free(command->redirect.target.data);
        command->redirect.target = lower_redirect_temp_target(tmp);
        free(tmp.data);
        changed = true;
    }
    return changed;
}
