#include "frontend.h"
#include "backend.h"
#include "ds_signal.h"

#include <ctype.h>
#include <stdarg.h>

typedef struct {
    DsString out;
} Formatter;

static void format_expr_prec(Formatter *fmt, const DsExpr *expr, int parent_prec);

static void append_str(Formatter *fmt, DsStr s) {
    ds_string_append_range(&fmt->out, s.data, s.len);
}

static void append_cstr(Formatter *fmt, const char *s) {
    ds_string_append_cstr(&fmt->out, s);
}

static void indent(Formatter *fmt, int level) {
    for (int i = 0; i < level; i++) append_cstr(fmt, "  ");
}

static void append_quoted(Formatter *fmt, DsStr value) {
    append_cstr(fmt, "\"");
    ds_string_append_escaped(&fmt->out, value.data, value.len);
    append_cstr(fmt, "\"");
}

static int expr_prec(const DsExpr *expr) {
    if (!expr) return 99;
    switch (expr->kind) {
        case DS_EXPR_BINARY:
            if (ds_binary_op_is_logical(expr->as.binary.op)) return 0;
            if (expr->as.binary.op == DS_BINARY_IN || expr->as.binary.op == DS_BINARY_MATCHES ||
                expr->as.binary.op == DS_BINARY_EQ || expr->as.binary.op == DS_BINARY_NE) return 1;
            if (ds_binary_op_is_comparison(expr->as.binary.op)) return 2;
            if (expr->as.binary.op == DS_BINARY_ADD || expr->as.binary.op == DS_BINARY_SUB) return 3;
            if (expr->as.binary.op >= DS_BINARY_MUL && expr->as.binary.op <= DS_BINARY_MOD) return 4;
            if (expr->as.binary.op == DS_BINARY_POW) return 5;
            return 1;
        case DS_EXPR_RANGE: return 2;
        case DS_EXPR_UNARY: return 5;
        case DS_EXPR_FIELD:
        case DS_EXPR_CALL:
        case DS_EXPR_INDEX: return 6;
        default: return 7;
    }
}

static bool expr_binary_op_is_logical(const DsExpr *expr) {
    return expr && expr->kind == DS_EXPR_BINARY && ds_binary_op_is_logical(expr->as.binary.op);
}

static bool expr_binary_op_is_comparison_like(const DsExpr *expr) {
    return expr && expr->kind == DS_EXPR_BINARY && ds_binary_op_is_comparison_like(expr->as.binary.op);
}

static void format_logical_operand(Formatter *fmt, const DsExpr *expr, int prec) {
    bool parens = expr_binary_op_is_comparison_like(expr);
    if (parens) append_cstr(fmt, "(");
    format_expr_prec(fmt, expr, prec);
    if (parens) append_cstr(fmt, ")");
}

static void format_expr_list_from(Formatter *fmt, const DsExprVec *args, size_t start) {
    for (size_t i = start; i < args->len; i++) {
        if (i > start) append_cstr(fmt, ", ");
        format_expr_prec(fmt, args->items[i], 0);
    }
}

static void format_expr_list(Formatter *fmt, const DsExprVec *args) {
    format_expr_list_from(fmt, args, 0);
}

static void format_words(Formatter *fmt, const DsWordVec *words) {
    for (size_t i = 0; i < words->len; i++) {
        if (i > 0) append_cstr(fmt, " ");
        append_str(fmt, words->items[i].text);
    }
}

static void format_redirect(Formatter *fmt, const DsRedirect *redirect);

static void format_command(Formatter *fmt, const DsCommand *command) {
    for (size_t s = 0; s < command->stages.len; s++) {
        if (s) append_cstr(fmt, " | ");
        format_words(fmt, &command->stages.items[s].words);
    }
    format_redirect(fmt, &command->redirect);
}

static void format_expr_prec(Formatter *fmt, const DsExpr *expr, int parent_prec) {
    if (!expr) return;
    int prec = expr_prec(expr);
    bool parens = prec < parent_prec;
    if (parens) append_cstr(fmt, "(");
    switch (expr->kind) {
        case DS_EXPR_IDENT:
        case DS_EXPR_STRING:
        case DS_EXPR_INT:
            append_str(fmt, expr->as.text);
            break;
        case DS_EXPR_BOOL:
            append_cstr(fmt, expr->as.boolean ? "true" : "false");
            break;
        case DS_EXPR_REGEX:
            append_str(fmt, expr->as.regex);
            break;
        case DS_EXPR_RUN:
            append_cstr(fmt, "run ");
            format_command(fmt, &expr->as.run);
            break;
        case DS_EXPR_FIELD:
            format_expr_prec(fmt, expr->as.field.object, prec);
            append_cstr(fmt, ".");
            append_str(fmt, expr->as.field.field);
            break;
        case DS_EXPR_UNARY:
            append_cstr(fmt, ds_unary_op_name(expr->as.unary.op));
            format_expr_prec(fmt, expr->as.unary.right, prec);
            break;
        case DS_EXPR_BINARY:
            if (expr_binary_op_is_logical(expr)) format_logical_operand(fmt, expr->as.binary.left, prec);
            else format_expr_prec(fmt, expr->as.binary.left, prec);
            append_cstr(fmt, " ");
            append_cstr(fmt, ds_binary_op_name(expr->as.binary.op));
            append_cstr(fmt, " ");
            if (expr_binary_op_is_logical(expr)) format_logical_operand(fmt, expr->as.binary.right, prec + 1);
            else format_expr_prec(fmt, expr->as.binary.right, prec + 1);
            break;
        case DS_EXPR_CALL:
            if (expr->as.call.name.len > 7 && ds_str_has_prefix_cstr(expr->as.call.name, "string.") && expr->as.call.args.len > 0) {
                format_expr_prec(fmt, expr->as.call.args.items[0], prec);
                append_cstr(fmt, ".");
                append_str(fmt, (DsStr){expr->as.call.name.data + 7, expr->as.call.name.len - 7});
                append_cstr(fmt, "(");
                format_expr_list_from(fmt, &expr->as.call.args, 1);
                append_cstr(fmt, ")");
            } else {
                append_str(fmt, expr->as.call.name);
                append_cstr(fmt, "(");
                format_expr_list(fmt, &expr->as.call.args);
                append_cstr(fmt, ")");
            }
            break;
        case DS_EXPR_ARRAY:
            append_cstr(fmt, "[");
            format_expr_list(fmt, &expr->as.array.elements);
            append_cstr(fmt, "]");
            break;
        case DS_EXPR_MAP:
            append_cstr(fmt, "{");
            for (size_t i = 0; i < expr->as.map.entries.len; i++) {
                if (i > 0) append_cstr(fmt, ", ");
                append_str(fmt, expr->as.map.entries.items[i].key);
                append_cstr(fmt, ": ");
                format_expr_prec(fmt, expr->as.map.entries.items[i].value, 0);
            }
            append_cstr(fmt, "}");
            break;
        case DS_EXPR_INDEX:
            format_expr_prec(fmt, expr->as.index.object, prec);
            append_cstr(fmt, "[");
            format_expr_prec(fmt, expr->as.index.index, 0);
            append_cstr(fmt, "]");
            break;
        case DS_EXPR_RANGE:
            format_expr_prec(fmt, expr->as.range.start, prec);
            append_cstr(fmt, "..");
            format_expr_prec(fmt, expr->as.range.end, prec + 1);
            break;
        case DS_EXPR_ERROR:
            append_cstr(fmt, "<error>");
            break;
    }
    if (parens) append_cstr(fmt, ")");
}

static void format_expr(Formatter *fmt, const DsExpr *expr) { format_expr_prec(fmt, expr, 0); }

static void format_stmt(Formatter *fmt, const DsStmt *stmt, int level);

static void format_block_body(Formatter *fmt, const DsStmt *block, int level) {
    if (!block || block->kind != DS_STMT_BLOCK) return;
    for (size_t i = 0; i < block->as.block_stmt.statements.len; i++) {
        format_stmt(fmt, block->as.block_stmt.statements.items[i], level);
    }
}

static void format_block_after_header(Formatter *fmt, const DsStmt *block, int level) {
    append_cstr(fmt, " {\n");
    format_block_body(fmt, block, level + 1);
    indent(fmt, level);
    append_cstr(fmt, "}");
}

static void format_params(Formatter *fmt, const DsFnParamVec *params) {
    for (size_t i = 0; i < params->len; i++) {
        if (i > 0) append_cstr(fmt, ", ");
        const DsFnParam *param = &params->items[i];
        append_str(fmt, param->name);
        if (param->has_type) {
            append_cstr(fmt, ": ");
            append_cstr(fmt, ds_script_type_name(param->type));
        }
        if (param->default_value) {
            append_cstr(fmt, " = ");
            format_expr(fmt, param->default_value);
        }
    }
}

static void format_redirect(Formatter *fmt, const DsRedirect *redirect) {
    if (redirect->kind == DS_REDIRECT_NONE) return;
    append_cstr(fmt, " ");
    append_cstr(fmt, ds_redirect_source_op(redirect->kind));
    append_cstr(fmt, " ");
    append_str(fmt, redirect->target);
}

static void format_stmt(Formatter *fmt, const DsStmt *stmt, int level) {
    if (!stmt) return;
    indent(fmt, level);
    switch (stmt->kind) {
        case DS_STMT_LET:
            append_cstr(fmt, "let ");
            append_str(fmt, stmt->as.let_stmt.name);
            append_cstr(fmt, " = ");
            format_expr(fmt, stmt->as.let_stmt.value);
            append_cstr(fmt, "\n");
            break;
        case DS_STMT_ASSIGN:
            append_str(fmt, stmt->as.assign_stmt.name);
            append_cstr(fmt, " ");
            append_cstr(fmt, ds_assign_op_name(stmt->as.assign_stmt.op));
            append_cstr(fmt, " ");
            format_expr(fmt, stmt->as.assign_stmt.value);
            append_cstr(fmt, "\n");
            break;
        case DS_STMT_INDEX_ASSIGN:
            format_expr(fmt, stmt->as.index_assign_stmt.target);
            append_cstr(fmt, " ");
            append_cstr(fmt, ds_assign_op_name(stmt->as.index_assign_stmt.op));
            append_cstr(fmt, " ");
            format_expr(fmt, stmt->as.index_assign_stmt.value);
            append_cstr(fmt, "\n");
            break;
        case DS_STMT_IF:
            append_cstr(fmt, "if ");
            format_expr(fmt, stmt->as.if_stmt.condition);
            format_block_after_header(fmt, stmt->as.if_stmt.then_branch, level);
            if (stmt->as.if_stmt.else_branch) {
                append_cstr(fmt, " else");
                format_block_after_header(fmt, stmt->as.if_stmt.else_branch, level);
            }
            append_cstr(fmt, "\n");
            break;
        case DS_STMT_BLOCK:
            append_cstr(fmt, "{\n");
            format_block_body(fmt, stmt, level + 1);
            indent(fmt, level);
            append_cstr(fmt, "}\n");
            break;
        case DS_STMT_CMD:
            format_command(fmt, &stmt->as.cmd_stmt);
            append_cstr(fmt, "\n");
            break;
        case DS_STMT_IMPORT:
            append_cstr(fmt, "import ");
            append_str(fmt, stmt->as.import_stmt.path);
            append_cstr(fmt, "\n");
            break;
        case DS_STMT_FN:
            append_cstr(fmt, "fn ");
            append_str(fmt, stmt->as.fn_stmt.name);
            append_cstr(fmt, "(");
            format_params(fmt, &stmt->as.fn_stmt.params);
            append_cstr(fmt, ")");
            format_block_after_header(fmt, stmt->as.fn_stmt.body, level);
            append_cstr(fmt, "\n");
            break;
        case DS_STMT_CALL:
            append_str(fmt, stmt->as.call_stmt.name);
            append_cstr(fmt, "(");
            format_expr_list(fmt, &stmt->as.call_stmt.args);
            append_cstr(fmt, ")\n");
            break;
        case DS_STMT_FOR:
            append_cstr(fmt, "for ");
            append_str(fmt, stmt->as.for_stmt.key_name);
            if (stmt->as.for_stmt.has_value_name) {
                append_cstr(fmt, ", ");
                append_str(fmt, stmt->as.for_stmt.value_name);
            }
            append_cstr(fmt, " in ");
            format_expr(fmt, stmt->as.for_stmt.iterable);
            format_block_after_header(fmt, stmt->as.for_stmt.body, level);
            append_cstr(fmt, "\n");
            break;
        case DS_STMT_WHILE:
            append_cstr(fmt, "while ");
            format_expr(fmt, stmt->as.while_stmt.condition);
            format_block_after_header(fmt, stmt->as.while_stmt.body, level);
            append_cstr(fmt, "\n");
            break;
        case DS_STMT_BREAK:
            append_cstr(fmt, "break\n");
            break;
        case DS_STMT_CONTINUE:
            append_cstr(fmt, "continue\n");
            break;
        case DS_STMT_CASE:
            append_cstr(fmt, "case ");
            format_expr(fmt, stmt->as.case_stmt.selector);
            append_cstr(fmt, " {\n");
            for (size_t i = 0; i < stmt->as.case_stmt.arms.len; i++) {
                const DsCaseArm *arm = &stmt->as.case_stmt.arms.items[i];
                indent(fmt, level + 1);
                for (size_t j = 0; j < arm->patterns.len; j++) {
                    if (j) append_cstr(fmt, " | ");
                    const DsCasePattern *p = &arm->patterns.items[j];
                    if (p->kind == DS_CASE_PATTERN_DEFAULT) append_cstr(fmt, "_");
                    else if (p->kind == DS_CASE_PATTERN_BOOL) append_cstr(fmt, p->boolean ? "true" : "false");
                    else append_str(fmt, p->text);
                }
                format_block_after_header(fmt, arm->body, level + 1);
                append_cstr(fmt, "\n");
            }
            indent(fmt, level);
            append_cstr(fmt, "}\n");
            break;
        case DS_STMT_PUSH:
            append_str(fmt, stmt->as.push_stmt.name);
            append_cstr(fmt, ".push(");
            format_expr(fmt, stmt->as.push_stmt.value);
            append_cstr(fmt, ")\n");
            break;
        case DS_STMT_TEST:
            append_cstr(fmt, "test ");
            append_quoted(fmt, stmt->as.test_stmt.name);
            format_block_after_header(fmt, stmt->as.test_stmt.body, level);
            append_cstr(fmt, "\n");
            break;
        case DS_STMT_ASSERT:
            append_cstr(fmt, "assert ");
            format_expr(fmt, stmt->as.assert_stmt.condition);
            append_cstr(fmt, "\n");
            break;
        case DS_STMT_RETURN:
            append_cstr(fmt, "return ");
            format_expr(fmt, stmt->as.return_stmt.value);
            append_cstr(fmt, "\n");
            break;
        case DS_STMT_DEFER:
            append_cstr(fmt, "defer");
            if (stmt->as.handler_stmt.signal != DS_HANDLER_EXIT) {
                append_cstr(fmt, " on: \"");
                if (stmt->as.handler_stmt.signal == DS_HANDLER_INVALID) append_str(fmt, stmt->as.handler_stmt.signal_text);
                else append_cstr(fmt, ds_handler_signal_name(stmt->as.handler_stmt.signal));
                append_cstr(fmt, "\"");
            }
            format_block_after_header(fmt, stmt->as.handler_stmt.body, level);
            append_cstr(fmt, "\n");
            break;
        case DS_STMT_TRAP:
            append_cstr(fmt, "trap \"");
            if (stmt->as.handler_stmt.signal == DS_HANDLER_INVALID) append_str(fmt, stmt->as.handler_stmt.signal_text);
            else append_cstr(fmt, ds_handler_signal_name(stmt->as.handler_stmt.signal));
            append_cstr(fmt, "\"");
            format_block_after_header(fmt, stmt->as.handler_stmt.body, level);
            append_cstr(fmt, "\n");
            break;
    }
}

static void trim_extra_blank(DsString *s) {
    while (s->len >= 2 && s->data[s->len - 1] == '\n' && s->data[s->len - 2] == '\n') {
        s->data[--s->len] = '\0';
    }
}

static int top_stmt_group(const DsStmt *stmt) {
    if (!stmt) return 0;
    switch (stmt->kind) {
        case DS_STMT_IMPORT: return 1;
        case DS_STMT_LET:
        case DS_STMT_ASSIGN:
        case DS_STMT_INDEX_ASSIGN:
        case DS_STMT_CMD:
        case DS_STMT_CALL:
        case DS_STMT_PUSH:
        case DS_STMT_ASSERT:
        case DS_STMT_RETURN: return 2;
        case DS_STMT_DEFER:
        case DS_STMT_TRAP: return 3;
        case DS_STMT_IF:
        case DS_STMT_FOR:
        case DS_STMT_WHILE:
        case DS_STMT_BREAK:
        case DS_STMT_CONTINUE:
        case DS_STMT_CASE:
        case DS_STMT_BLOCK: return 3;
        case DS_STMT_FN: return 4;
        case DS_STMT_TEST: return 5;
    }
    return 0;
}

static bool has_comment(const DsSource *source, DsSpan *span) {
    bool in_string = false;
    bool escape = false;
    int line = 1;
    int col = 1;
    for (size_t i = 0; i < source->len; i++) {
        char c = source->data[i];
        if (in_string) {
            if (escape) escape = false;
            else if (c == '\\') escape = true;
            else if (c == '"') in_string = false;
        } else {
            if (c == '"') in_string = true;
            else if (c == '#') {
                DsLoc loc = {i, line, col};
                *span = (DsSpan){loc, {i + 1, line, col + 1}, source};
                return true;
            }
        }
        if (c == '\n') { line++; col = 1; }
        else col++;
    }
    return false;
}

bool ds_format_source(const DsSource *source, const DsAst *ast, DsString *out, DsDiag *diag) {
    ds_string_init(out);
    DsSpan comment_span;
    if (has_comment(source, &comment_span)) {
        ds_diag_error(diag, comment_span, "formatter cannot preserve comments yet; remove comments or defer formatting this file");
        return false;
    }

    Formatter fmt = {0};
    ds_string_init(&fmt.out);

    bool wrote_group = false;
    if (ast->has_script) {
        append_cstr(&fmt, "script {\n");
        for (size_t i = 0; i < ast->script.declarations.len; i++) {
            const DsScriptDecl *decl = &ast->script.declarations.items[i];
            append_cstr(&fmt, "  ");
            append_cstr(&fmt, decl->kind == DS_SCRIPT_DECL_ARG ? "arg " : decl->kind == DS_SCRIPT_DECL_OPTION ? "option " : "flag ");
            append_str(&fmt, decl->name);
            append_cstr(&fmt, ": ");
            append_cstr(&fmt, ds_script_type_name(decl->type));
            if (decl->default_value) {
                append_cstr(&fmt, " = ");
                format_expr(&fmt, decl->default_value);
            }
            append_cstr(&fmt, "\n");
        }
        append_cstr(&fmt, "}\n");
        wrote_group = true;
    }

    int prev_group = wrote_group ? -1 : 0;
    for (size_t i = 0; i < ast->statements.len; i++) {
        int group = top_stmt_group(ast->statements.items[i]);
        if (wrote_group && (prev_group < 0 || group != prev_group || group >= 3 || prev_group >= 3)) append_cstr(&fmt, "\n");
        format_stmt(&fmt, ast->statements.items[i], 0);
        trim_extra_blank(&fmt.out);
        wrote_group = true;
        prev_group = group;
    }

    if (fmt.out.len > 0 && fmt.out.data[fmt.out.len - 1] != '\n') append_cstr(&fmt, "\n");
    *out = fmt.out;
    return true;
}
