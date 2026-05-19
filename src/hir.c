#include "ds.h"

#include <stdio.h>
#include <string.h>

static void indent(FILE *out, int n) {
    for (int i = 0; i < n; i++) fputs("  ", out);
}

static void print_span(FILE *out, DsSpan span) {
    const DsSource *source = span.source;
    fprintf(out, " @ %s:%d:%d", source && source->path ? source->path : "<source>", span.start.line, span.start.column);
}

static void print_str(FILE *out, DsStr s) {
    fprintf(out, "%.*s", (int)s.len, s.data ? s.data : "");
}

static const char *script_decl_kind(DsScriptDeclKind kind) {
    switch (kind) {
        case DS_SCRIPT_DECL_ARG: return "Arg";
        case DS_SCRIPT_DECL_OPTION: return "Option";
        case DS_SCRIPT_DECL_FLAG: return "Flag";
    }
    return "Decl";
}

static const char *script_type(DsScriptType type) {
    switch (type) {
        case DS_SCRIPT_TYPE_STRING: return "string";
        case DS_SCRIPT_TYPE_INT: return "int";
        case DS_SCRIPT_TYPE_BOOL: return "bool";
    }
    return "unknown";
}

static void print_escaped(FILE *out, const char *data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)data[i];
        if (c == '\\') fputs("\\\\", out);
        else if (c == '"') fputs("\\\"", out);
        else if (c == '\n') fputs("\\n", out);
        else if (c == '\t') fputs("\\t", out);
        else if (c < 32 || c == 127) fprintf(out, "\\x%02x", c);
        else fputc((int)c, out);
    }
}

static void dump_expr(FILE *out, const DsLowerExpr *expr, int level);
static void dump_stmt(FILE *out, const DsLowerStmt *stmt, int level);

static void dump_word_vec(FILE *out, const DsWordVec *words) {
    fputc('[', out);
    for (size_t i = 0; i < words->len; i++) {
        if (i) fputs(", ", out);
        fputc('"', out);
        print_escaped(out, words->items[i].text.data, words->items[i].text.len);
        fputc('"', out);
    }
    fputc(']', out);
}

static const char *redirect_op(DsRedirectKind kind) {
    switch (kind) {
        case DS_REDIRECT_OUT: return ">";
        case DS_REDIRECT_OUT_APPEND: return ">>";
        case DS_REDIRECT_ERR: return "2>";
        case DS_REDIRECT_ERR_APPEND: return "2>>";
        case DS_REDIRECT_ALL: return "&>";
        case DS_REDIRECT_ALL_APPEND: return "&>>";
        case DS_REDIRECT_NONE: return NULL;
    }
    return NULL;
}

static void dump_redirect(FILE *out, const DsRedirect *redirect) {
    const char *op = redirect_op(redirect->kind);
    if (!op) return;
    fprintf(out, " Redirect %s ", op);
    fputc('"', out);
    print_escaped(out, redirect->target.data ? redirect->target.data : "", redirect->target.len);
    fputc('"', out);
    print_span(out, redirect->target_span);
}

static void dump_expr_vec(FILE *out, const DsLowerExprVec *args, int level) {
    if (args->len == 0) return;
    fputc('\n', out);
    for (size_t i = 0; i < args->len; i++) dump_expr(out, args->items[i], level + 1);
}

static void dump_expr(FILE *out, const DsLowerExpr *expr, int level) {
    if (!expr) {
        indent(out, level); fputs("<null expr>\n", out); return;
    }
    indent(out, level);
    switch (expr->kind) {
        case DS_LOWER_EXPR_IDENT:
            fputs("Ident ", out); print_str(out, expr->as.text); print_span(out, expr->span); fputc('\n', out); break;
        case DS_LOWER_EXPR_STRING:
            fputs("String \"", out); print_escaped(out, expr->as.text.data, expr->as.text.len); fputs("\"", out); print_span(out, expr->span); fputc('\n', out); break;
        case DS_LOWER_EXPR_INT:
            fputs("Int ", out); print_str(out, expr->as.text); print_span(out, expr->span); fputc('\n', out); break;
        case DS_LOWER_EXPR_BOOL:
            fprintf(out, "Bool %s", expr->as.boolean ? "true" : "false"); print_span(out, expr->span); fputc('\n', out); break;
        case DS_LOWER_EXPR_RUN:
            fputs("Run ", out); dump_word_vec(out, &expr->as.run.words); print_span(out, expr->span); fputc('\n', out); break;
        case DS_LOWER_EXPR_FIELD:
            fputs("Field .", out); print_str(out, expr->as.field.field); print_span(out, expr->span); fputc('\n', out);
            dump_expr(out, expr->as.field.object, level + 1);
            break;
        case DS_LOWER_EXPR_UNARY:
            fputs("Unary ", out); print_str(out, expr->as.unary.op); print_span(out, expr->span); fputc('\n', out);
            dump_expr(out, expr->as.unary.right, level + 1);
            break;
        case DS_LOWER_EXPR_BINARY:
            fputs("Binary ", out); print_str(out, expr->as.binary.op); print_span(out, expr->span); fputc('\n', out);
            dump_expr(out, expr->as.binary.left, level + 1);
            dump_expr(out, expr->as.binary.right, level + 1);
            break;
        case DS_LOWER_EXPR_CALL:
            fputs("Call ", out); print_str(out, expr->as.call.name); print_span(out, expr->span); dump_expr_vec(out, &expr->as.call.args, level); if (expr->as.call.args.len == 0) fputc('\n', out); break;
        case DS_LOWER_EXPR_ARRAY:
            fputs("Array", out); print_span(out, expr->span); dump_expr_vec(out, &expr->as.array.elements, level); if (expr->as.array.elements.len == 0) fputc('\n', out); break;
        case DS_LOWER_EXPR_MAP:
            fputs("Map", out); print_span(out, expr->span); fputc('\n', out);
            for (size_t i = 0; i < expr->as.map.entries.len; i++) {
                indent(out, level + 1); fputs("Entry ", out); print_str(out, expr->as.map.entries.items[i].key); print_span(out, expr->as.map.entries.items[i].span); fputc('\n', out);
                dump_expr(out, expr->as.map.entries.items[i].value, level + 2);
            }
            break;
        case DS_LOWER_EXPR_INDEX:
            fputs("Index", out); if (expr->as.index.object_is_array) fputs(" array", out); if (expr->as.index.object_is_map) fputs(" map", out); print_span(out, expr->span); fputc('\n', out);
            dump_expr(out, expr->as.index.object, level + 1);
            dump_expr(out, expr->as.index.index, level + 1);
            break;
        case DS_LOWER_EXPR_ERROR:
            fputs("ErrorExpr", out); print_span(out, expr->span); fputc('\n', out); break;
    }
}

static void dump_block(FILE *out, const DsLowerStmt *block, int level) {
    if (!block || block->kind != DS_LOWER_STMT_BLOCK) return;
    for (size_t i = 0; i < block->as.block_stmt.statements.len; i++) dump_stmt(out, block->as.block_stmt.statements.items[i], level);
}

static void dump_stmt(FILE *out, const DsLowerStmt *stmt, int level) {
    if (!stmt) return;
    indent(out, level);
    switch (stmt->kind) {
        case DS_LOWER_STMT_LET:
            fputs("Let ", out); print_str(out, stmt->as.let_stmt.name); print_span(out, stmt->span); fputc('\n', out);
            dump_expr(out, stmt->as.let_stmt.value, level + 1);
            break;
        case DS_LOWER_STMT_CMD:
            fputs("Command ", out); dump_word_vec(out, &stmt->as.cmd_stmt.words); dump_redirect(out, &stmt->as.cmd_stmt.redirect); print_span(out, stmt->span); fputc('\n', out);
            break;
        case DS_LOWER_STMT_CALL:
            fputs("CallStmt ", out); print_str(out, stmt->as.call_stmt.name); print_span(out, stmt->span); dump_expr_vec(out, &stmt->as.call_stmt.args, level); if (stmt->as.call_stmt.args.len == 0) fputc('\n', out); break;
        case DS_LOWER_STMT_PUSH:
            fputs("Push ", out); print_str(out, stmt->as.push_stmt.name); print_span(out, stmt->span); fputc('\n', out);
            dump_expr(out, stmt->as.push_stmt.value, level + 1);
            break;
        case DS_LOWER_STMT_FOR_ARRAY:
            fputs("For ", out); print_str(out, stmt->as.for_stmt.name); fputs(" in", out); print_span(out, stmt->span); fputc('\n', out);
            dump_expr(out, stmt->as.for_stmt.iterable, level + 1);
            indent(out, level + 1); fputs("Body\n", out);
            dump_block(out, stmt->as.for_stmt.body, level + 2);
            break;
        case DS_LOWER_STMT_IF:
            fputs("If", out); print_span(out, stmt->span); fputc('\n', out);
            indent(out, level + 1); fputs("Condition\n", out);
            dump_expr(out, stmt->as.if_stmt.condition, level + 2);
            indent(out, level + 1); fputs("Then\n", out);
            dump_block(out, stmt->as.if_stmt.then_branch, level + 2);
            if (stmt->as.if_stmt.else_branch) {
                indent(out, level + 1); fputs("Else\n", out);
                dump_block(out, stmt->as.if_stmt.else_branch, level + 2);
            }
            break;
        case DS_LOWER_STMT_BLOCK:
            fputs("Block", out); print_span(out, stmt->span); fputc('\n', out);
            dump_block(out, stmt, level + 1);
            break;
    }
}

bool ds_hir_dump_program(const DsLowerProgram *program, FILE *out) {
    if (!program || !out) return false;
    fputs("Program\n", out);
    if (program->has_script) {
        indent(out, 1); fputs("Script\n", out);
        for (size_t i = 0; i < program->script_decls.len; i++) {
            const DsLowerScriptDecl *decl = &program->script_decls.items[i];
            indent(out, 2);
            fprintf(out, "%s ", script_decl_kind(decl->kind));
            print_str(out, decl->name);
            fprintf(out, ": %s", script_type(decl->type));
            if (decl->has_default) {
                if (decl->type == DS_SCRIPT_TYPE_STRING) { fputs(" = \"", out); print_escaped(out, decl->default_text.data ? decl->default_text.data : "", decl->default_text.len); fputc('"', out); }
                else if (decl->type == DS_SCRIPT_TYPE_INT) fprintf(out, " = %lld", (long long)decl->default_int);
                else fprintf(out, " = %s", decl->default_bool ? "true" : "false");
            }
            print_span(out, decl->span); fputc('\n', out);
        }
    }
    for (size_t i = 0; i < program->functions.len; i++) {
        const DsLowerFn *fn = &program->functions.items[i];
        indent(out, 1); fputs("Function ", out); print_str(out, fn->name); fputc('(', out);
        for (size_t j = 0; j < fn->params.len; j++) {
            if (j) fputs(", ", out);
            print_str(out, fn->params.items[j].name);
            if (fn->params.items[j].has_default) fputs(" = <default>", out);
        }
        fputc(')', out); print_span(out, fn->span); fputc('\n', out);
        indent(out, 2); fputs("Body\n", out);
        dump_block(out, fn->body, 3);
    }
    indent(out, 1); fputs("Statements\n", out);
    for (size_t i = 0; i < program->statements.len; i++) dump_stmt(out, program->statements.items[i], 2);
    return true;
}
