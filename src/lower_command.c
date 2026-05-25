#include "lower_internal.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool command_name_char(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
}

static bool parse_format_limit(DsStr spec, size_t start, size_t end, size_t *value) {
    if (start >= end) return false;
    size_t out = 0;
    for (size_t i = start; i < end; i++) {
        if (spec.data[i] < '0' || spec.data[i] > '9') return false;
        size_t digit = (size_t)(spec.data[i] - '0');
        if (out > 1024 / 10 || (out == 1024 / 10 && digit > 1024 % 10)) return false;
        out = out * 10 + digit;
    }
    if (out < 1 || out > 1024) return false;
    *value = out;
    return true;
}

static bool is_supported_format_spec(DsStr spec, SymKind kind) {
    if (spec.len == 0) return false;
    if ((spec.len == 5 && memcmp(spec.data, "upper", 5) == 0) ||
        (spec.len == 5 && memcmp(spec.data, "lower", 5) == 0) ||
        (spec.len == 4 && memcmp(spec.data, "trim", 4) == 0)) return kind == SYM_STRING;
    size_t i = 0;
    if (spec.data[0] == '<' || spec.data[0] == '>' || spec.data[0] == '^') {
        i = 1;
        if (i >= spec.len) return false;
        while (i < spec.len && spec.data[i] >= '0' && spec.data[i] <= '9') i++;
        size_t width = 0;
        return i == spec.len && parse_format_limit(spec, 1, i, &width) && kind == SYM_STRING;
    }
    bool zero = false;
    if (i < spec.len && spec.data[i] == '0') { zero = true; i++; }
    size_t digits_start = i;
    while (i < spec.len && spec.data[i] >= '0' && spec.data[i] <= '9') i++;
    if (i < spec.len && spec.data[i] == 'd') {
        size_t width = 0;
        return i + 1 == spec.len && parse_format_limit(spec, digits_start, i, &width) && kind == SYM_INT;
    }
    if (zero) return false;
    if (i < spec.len && spec.data[i] == '.') {
        i++;
        size_t prec_start = i;
        while (i < spec.len && spec.data[i] >= '0' && spec.data[i] <= '9') i++;
        size_t precision = 0;
        return i < spec.len && spec.data[i] == 'f' && i + 1 == spec.len && parse_format_limit(spec, prec_start, i, &precision) && kind == SYM_INT;
    }
    return false;
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
            while (i < body.len && ((body.data[i] >= 'A' && body.data[i] <= 'Z') ||
                                    (body.data[i] >= 'a' && body.data[i] <= 'z') ||
                                    (body.data[i] >= '0' && body.data[i] <= '9') ||
                                    body.data[i] == '_')) i++;
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
                    ds_diag_error(lower->diag, span, "unsupported interpolation format specifier `%.*s`; supported: upper, lower, trim, <N, >N, ^N, Nd, 0Nd, .Pf, N.Pf", (int)spec.len, spec.data);
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
    if (word.len >= 2 && word.data[0] == '$') {
        size_t name_len = 0;
        while (1 + name_len < word.len && command_name_char(word.data[1 + name_len])) name_len++;
        DsStr name = {word.data + 1, name_len};
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
        if (name_len + 1 < word.len) {
            char suffix = word.data[name_len + 1];
            if ((sym->kind == SYM_ARRAY || sym->kind == SYM_MAP) && (suffix == '[' || suffix == '.')) {
                ds_diag_error(lower->diag, span, "collection access command arguments are deferred in v0.10.0; bind the indexed value to a variable first");
                return false;
            }
            if (suffix != '.' || sym->kind != SYM_COMMAND_RESULT) {
                ds_diag_error(lower->diag, span, "unsupported command variable suffix in v0.10.0");
                return false;
            }
        }
        if (sym->kind == SYM_ARRAY || sym->kind == SYM_MAP) {
            ds_diag_error(lower->diag, span, "collection `%.*s` cannot be passed directly as a command argument in v0.10.0; index it first", (int)name.len, name.data);
            return false;
        }
    }
    if (word.len >= 2 && word.data[0] == '"' && word.data[word.len - 1] == '"') return lower_validate_word_interpolation(lower, word, span);
    for (size_t i = 1; i < word.len; i++) {
        if (word.data[i] == '.') {
            DsStr name = {word.data, i};
            DsStr field = {word.data + i + 1, word.len - i - 1};
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
                continue;
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
    }
    return true;
}

static bool command_quoted_word_needs_value_call_materialization(Lower *lower, DsStr word) {
    if (word.len < 2 || word.data[0] != '"' || word.data[word.len - 1] != '"') return false;
    DsStr decoded = {0};
    if (!lower_decode_string_text(word, &decoded)) return false;
    bool found = false;
    for (size_t i = 0; i < decoded.len && !found; i++) {
        if (decoded.data[i] != '{') continue;
        size_t j = i + 1;
        while (j < decoded.len && (decoded.data[j] == ' ' || decoded.data[j] == '\t')) j++;
        if (j < decoded.len && ((decoded.data[j] >= 'A' && decoded.data[j] <= 'Z') ||
                                (decoded.data[j] >= 'a' && decoded.data[j] <= 'z') ||
                                decoded.data[j] == '_')) {
            j++;
            while (j < decoded.len && ((decoded.data[j] >= 'A' && decoded.data[j] <= 'Z') ||
                                       (decoded.data[j] >= 'a' && decoded.data[j] <= 'z') ||
                                       (decoded.data[j] >= '0' && decoded.data[j] <= '9') ||
                                       decoded.data[j] == '_' || decoded.data[j] == '.')) j++;
            while (j < decoded.len && (decoded.data[j] == ' ' || decoded.data[j] == '\t')) j++;
            if (j < decoded.len && decoded.data[j] == '(') found = true;
        }
    }
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
