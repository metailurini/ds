#include "ds_ast.h"

#include <stdlib.h>

static void indent(FILE *out, int level) {
    for (int i = 0; i < level; i++) fputs("  ", out);
}

static const char *handler_signal_name(DsHandlerSignal signal) {
    switch (signal) {
        case DS_HANDLER_EXIT: return "EXIT";
        case DS_HANDLER_INT: return "INT";
        case DS_HANDLER_TERM: return "TERM";
        case DS_HANDLER_INVALID: return "<invalid>";
    }
    return "EXIT";
}

static void print_expr(const DsExpr *expr, FILE *out, int level) {
    if (!expr) {
        indent(out, level);
        fputs("<missing expr>\n", out);
        return;
    }

    indent(out, level);
    switch (expr->kind) {
        case DS_EXPR_IDENT:
            fprintf(out, "IdentExpr %.*s\n", (int)expr->as.text.len, expr->as.text.data);
            break;
        case DS_EXPR_STRING:
            fprintf(out, "StringExpr %.*s\n", (int)expr->as.text.len, expr->as.text.data);
            break;
        case DS_EXPR_INT:
            fprintf(out, "IntExpr %.*s\n", (int)expr->as.text.len, expr->as.text.data);
            break;
        case DS_EXPR_BOOL:
            fprintf(out, "BoolExpr %s\n", expr->as.boolean ? "true" : "false");
            break;
        case DS_EXPR_REGEX:
            fprintf(out, "RegexExpr %.*s\n", (int)expr->as.regex.len, expr->as.regex.data);
            break;
        case DS_EXPR_RUN:
            fputs("RunExpr\n", out);
            for (size_t s = 0; s < expr->as.run.stages.len; s++) {
                indent(out, level + 1);
                fprintf(out, "Stage %zu\n", s);
                for (size_t i = 0; i < expr->as.run.stages.items[s].words.len; i++) {
                    indent(out, level + 2);
                    fprintf(out, "Word %.*s\n", (int)expr->as.run.stages.items[s].words.items[i].text.len, expr->as.run.stages.items[s].words.items[i].text.data);
                }
            }
            break;
        case DS_EXPR_FIELD:
            fprintf(out, "FieldExpr %.*s\n", (int)expr->as.field.field.len, expr->as.field.field.data);
            print_expr(expr->as.field.object, out, level + 1);
            break;
        case DS_EXPR_UNARY:
            fprintf(out, "UnaryExpr %.*s\n", (int)expr->as.unary.op.len, expr->as.unary.op.data);
            print_expr(expr->as.unary.right, out, level + 1);
            break;
        case DS_EXPR_BINARY:
            fprintf(out, "BinaryExpr %.*s\n", (int)expr->as.binary.op.len, expr->as.binary.op.data);
            print_expr(expr->as.binary.left, out, level + 1);
            print_expr(expr->as.binary.right, out, level + 1);
            break;
        case DS_EXPR_CALL:
            fprintf(out, "CallExpr %.*s\n", (int)expr->as.call.name.len, expr->as.call.name.data);
            for (size_t i = 0; i < expr->as.call.args.len; i++) print_expr(expr->as.call.args.items[i], out, level + 1);
            break;
        case DS_EXPR_ARRAY:
            fputs("ArrayExpr\n", out);
            for (size_t i = 0; i < expr->as.array.elements.len; i++) print_expr(expr->as.array.elements.items[i], out, level + 1);
            break;
        case DS_EXPR_MAP:
            fputs("MapExpr\n", out);
            for (size_t i = 0; i < expr->as.map.entries.len; i++) {
                const DsMapEntry *entry = &expr->as.map.entries.items[i];
                indent(out, level + 1);
                fprintf(out, "Entry %.*s\n", (int)entry->key.len, entry->key.data);
                print_expr(entry->value, out, level + 2);
            }
            break;
        case DS_EXPR_INDEX:
            fputs("IndexExpr\n", out);
            print_expr(expr->as.index.object, out, level + 1);
            print_expr(expr->as.index.index, out, level + 1);
            break;
        case DS_EXPR_RANGE:
            fputs("RangeExpr\n", out);
            print_expr(expr->as.range.start, out, level + 1);
            print_expr(expr->as.range.end, out, level + 1);
            break;
        case DS_EXPR_ERROR:
            fputs("ErrorExpr\n", out);
            break;
    }
}

static void print_stmt(const DsStmt *stmt, FILE *out, int level) {
    indent(out, level);
    switch (stmt->kind) {
        case DS_STMT_LET:
            fprintf(out, "LetStmt %.*s\n", (int)stmt->as.let_stmt.name.len, stmt->as.let_stmt.name.data);
            print_expr(stmt->as.let_stmt.value, out, level + 1);
            break;
        case DS_STMT_ASSIGN: {
            const char *op = stmt->as.assign_stmt.op == DS_ASSIGN_ADD ? "+=" : (stmt->as.assign_stmt.op == DS_ASSIGN_SUB ? "-=" : "=");
            fprintf(out, "AssignStmt %.*s %s\n", (int)stmt->as.assign_stmt.name.len, stmt->as.assign_stmt.name.data, op);
            print_expr(stmt->as.assign_stmt.value, out, level + 1);
            break;
        }
        case DS_STMT_IF:
            fputs("IfStmt\n", out);
            indent(out, level + 1);
            fputs("Condition\n", out);
            print_expr(stmt->as.if_stmt.condition, out, level + 2);
            indent(out, level + 1);
            fputs("Then\n", out);
            print_stmt(stmt->as.if_stmt.then_branch, out, level + 2);
            if (stmt->as.if_stmt.else_branch) {
                indent(out, level + 1);
                fputs("Else\n", out);
                print_stmt(stmt->as.if_stmt.else_branch, out, level + 2);
            }
            break;
        case DS_STMT_BLOCK:
            fputs("BlockStmt\n", out);
            for (size_t i = 0; i < stmt->as.block_stmt.statements.len; i++) {
                print_stmt(stmt->as.block_stmt.statements.items[i], out, level + 1);
            }
            break;
        case DS_STMT_IMPORT:
            fprintf(out, "ImportStmt %.*s\n", (int)stmt->as.import_stmt.path.len, stmt->as.import_stmt.path.data);
            break;
        case DS_STMT_CMD:
            fputs("CmdStmt\n", out);
            for (size_t s = 0; s < stmt->as.cmd_stmt.stages.len; s++) {
                indent(out, level + 1);
                fprintf(out, "Stage %zu\n", s);
                for (size_t i = 0; i < stmt->as.cmd_stmt.stages.items[s].words.len; i++) {
                    indent(out, level + 2);
                    fprintf(out, "Word %.*s\n", (int)stmt->as.cmd_stmt.stages.items[s].words.items[i].text.len,
                            stmt->as.cmd_stmt.stages.items[s].words.items[i].text.data);
                }
            }
            if (stmt->as.cmd_stmt.redirect.kind != DS_REDIRECT_NONE) {
                static const char *names[] = {"none", "|>", "|>>", "!>", "!>>", "&>", "&>>"};
                indent(out, level + 1);
                fprintf(out, "Redirect %s %.*s\n", names[stmt->as.cmd_stmt.redirect.kind],
                        (int)stmt->as.cmd_stmt.redirect.target.len, stmt->as.cmd_stmt.redirect.target.data);
            }
            break;
        case DS_STMT_FN:
            fprintf(out, "FnStmt %.*s\n", (int)stmt->as.fn_stmt.name.len, stmt->as.fn_stmt.name.data);
            for (size_t i = 0; i < stmt->as.fn_stmt.params.len; i++) {
                const DsFnParam *param = &stmt->as.fn_stmt.params.items[i];
                indent(out, level + 1);
                fprintf(out, "Param %.*s%s\n", (int)param->name.len, param->name.data, param->default_value ? " =" : "");
                if (param->default_value) print_expr(param->default_value, out, level + 2);
            }
            print_stmt(stmt->as.fn_stmt.body, out, level + 1);
            break;
        case DS_STMT_CALL:
            fprintf(out, "CallStmt %.*s\n", (int)stmt->as.call_stmt.name.len, stmt->as.call_stmt.name.data);
            for (size_t i = 0; i < stmt->as.call_stmt.args.len; i++) print_expr(stmt->as.call_stmt.args.items[i], out, level + 1);
            break;
        case DS_STMT_FOR:
            fprintf(out, "ForStmt %.*s", (int)stmt->as.for_stmt.key_name.len, stmt->as.for_stmt.key_name.data);
            if (stmt->as.for_stmt.has_value_name) fprintf(out, ", %.*s", (int)stmt->as.for_stmt.value_name.len, stmt->as.for_stmt.value_name.data);
            fputc('\n', out);
            indent(out, level + 1);
            fputs("Iterable\n", out);
            print_expr(stmt->as.for_stmt.iterable, out, level + 2);
            indent(out, level + 1);
            fputs("Body\n", out);
            print_stmt(stmt->as.for_stmt.body, out, level + 2);
            break;
        case DS_STMT_WHILE:
            fputs("WhileStmt\n", out);
            indent(out, level + 1);
            fputs("Condition\n", out);
            print_expr(stmt->as.while_stmt.condition, out, level + 2);
            indent(out, level + 1);
            fputs("Body\n", out);
            print_stmt(stmt->as.while_stmt.body, out, level + 2);
            break;
        case DS_STMT_BREAK:
            fputs("BreakStmt\n", out);
            break;
        case DS_STMT_CONTINUE:
            fputs("ContinueStmt\n", out);
            break;
        case DS_STMT_CASE:
            fputs("CaseStmt\n", out);
            indent(out, level + 1);
            fputs("Selector\n", out);
            print_expr(stmt->as.case_stmt.selector, out, level + 2);
            for (size_t i = 0; i < stmt->as.case_stmt.arms.len; i++) {
                const DsCaseArm *arm = &stmt->as.case_stmt.arms.items[i];
                indent(out, level + 1);
                fputs("Arm", out);
                for (size_t j = 0; j < arm->patterns.len; j++) {
                    const DsCasePattern *p = &arm->patterns.items[j];
                    fputc(' ', out);
                    if (p->kind == DS_CASE_PATTERN_DEFAULT) fputs("_", out);
                    else if (p->kind == DS_CASE_PATTERN_BOOL) fputs(p->boolean ? "true" : "false", out);
                    else fprintf(out, "%.*s", (int)p->text.len, p->text.data);
                }
                fputc('\n', out);
                print_stmt(arm->body, out, level + 2);
            }
            break;
        case DS_STMT_PUSH:
            fprintf(out, "PushStmt %.*s\n", (int)stmt->as.push_stmt.name.len, stmt->as.push_stmt.name.data);
            print_expr(stmt->as.push_stmt.value, out, level + 1);
            break;
        case DS_STMT_TEST:
            fprintf(out, "TestStmt \"%.*s\"\n", (int)stmt->as.test_stmt.name.len, stmt->as.test_stmt.name.data);
            print_stmt(stmt->as.test_stmt.body, out, level + 1);
            break;
        case DS_STMT_ASSERT:
            fputs("AssertStmt\n", out);
            print_expr(stmt->as.assert_stmt.condition, out, level + 1);
            break;
        case DS_STMT_RETURN:
            fputs("ReturnStmt\n", out);
            print_expr(stmt->as.return_stmt.value, out, level + 1);
            break;
        case DS_STMT_DEFER:
            if (stmt->as.handler_stmt.signal == DS_HANDLER_INVALID) fprintf(out, "DeferStmt %.*s\n", (int)stmt->as.handler_stmt.signal_text.len, stmt->as.handler_stmt.signal_text.data);
            else fprintf(out, "DeferStmt %s\n", handler_signal_name(stmt->as.handler_stmt.signal));
            print_stmt(stmt->as.handler_stmt.body, out, level + 1);
            break;
        case DS_STMT_TRAP:
            if (stmt->as.handler_stmt.signal == DS_HANDLER_INVALID) fprintf(out, "TrapStmt %.*s\n", (int)stmt->as.handler_stmt.signal_text.len, stmt->as.handler_stmt.signal_text.data);
            else fprintf(out, "TrapStmt %s\n", handler_signal_name(stmt->as.handler_stmt.signal));
            print_stmt(stmt->as.handler_stmt.body, out, level + 1);
            break;
    }
}

static const char *decl_kind_name(DsScriptDeclKind kind) {
    switch (kind) {
        case DS_SCRIPT_DECL_ARG: return "ArgDecl";
        case DS_SCRIPT_DECL_OPTION: return "OptionDecl";
        case DS_SCRIPT_DECL_FLAG: return "FlagDecl";
    }
    return "Decl";
}

const char *ds_script_type_name(DsScriptType type) {
    switch (type) {
        case DS_SCRIPT_TYPE_STRING: return "string";
        case DS_SCRIPT_TYPE_INT: return "int";
        case DS_SCRIPT_TYPE_BOOL: return "bool";
    }
    return "unknown";
}

void ds_ast_print(const DsAst *ast, FILE *out) {
    fputs("Script\n", out);
    if (ast->has_script) {
        indent(out, 1);
        fputs("ScriptBlock\n", out);
        for (size_t i = 0; i < ast->script.declarations.len; i++) {
            const DsScriptDecl *decl = &ast->script.declarations.items[i];
            indent(out, 2);
            fprintf(out, "%s %.*s: %s\n", decl_kind_name(decl->kind), (int)decl->name.len, decl->name.data, ds_script_type_name(decl->type));
            if (decl->default_value) {
                indent(out, 3);
                fputs("Default\n", out);
                print_expr(decl->default_value, out, 4);
            }
        }
    }
    for (size_t i = 0; i < ast->statements.len; i++) {
        print_stmt(ast->statements.items[i], out, 1);
    }
}

static void free_expr(DsExpr *expr) {
    if (!expr) return;
    switch (expr->kind) {
        case DS_EXPR_IDENT:
        case DS_EXPR_STRING:
        case DS_EXPR_INT:
            free(expr->as.text.data);
            break;
        case DS_EXPR_REGEX:
            free(expr->as.regex.data);
            break;
        case DS_EXPR_RUN:
            ds_command_free(&expr->as.run);
            break;
        case DS_EXPR_FIELD:
            free_expr(expr->as.field.object);
            free(expr->as.field.field.data);
            break;
        case DS_EXPR_UNARY:
            free(expr->as.unary.op.data);
            free_expr(expr->as.unary.right);
            break;
        case DS_EXPR_BINARY:
            free_expr(expr->as.binary.left);
            free(expr->as.binary.op.data);
            free_expr(expr->as.binary.right);
            break;
        case DS_EXPR_CALL:
            free(expr->as.call.name.data);
            for (size_t i = 0; i < expr->as.call.args.len; i++) free_expr(expr->as.call.args.items[i]);
            free(expr->as.call.args.items);
            break;
        case DS_EXPR_ARRAY:
            for (size_t i = 0; i < expr->as.array.elements.len; i++) free_expr(expr->as.array.elements.items[i]);
            free(expr->as.array.elements.items);
            break;
        case DS_EXPR_MAP:
            for (size_t i = 0; i < expr->as.map.entries.len; i++) {
                free(expr->as.map.entries.items[i].key.data);
                free_expr(expr->as.map.entries.items[i].value);
            }
            free(expr->as.map.entries.items);
            break;
        case DS_EXPR_INDEX:
            free_expr(expr->as.index.object);
            free_expr(expr->as.index.index);
            break;
        case DS_EXPR_RANGE:
            free_expr(expr->as.range.start);
            free_expr(expr->as.range.end);
            break;
        case DS_EXPR_BOOL:
        case DS_EXPR_ERROR:
            break;
    }
    free(expr);
}

static void free_stmt(DsStmt *stmt) {
    if (!stmt) return;
    switch (stmt->kind) {
        case DS_STMT_LET:
            free(stmt->as.let_stmt.name.data);
            free_expr(stmt->as.let_stmt.value);
            break;
        case DS_STMT_ASSIGN:
            free(stmt->as.assign_stmt.name.data);
            free_expr(stmt->as.assign_stmt.value);
            break;
        case DS_STMT_IF:
            free_expr(stmt->as.if_stmt.condition);
            free_stmt(stmt->as.if_stmt.then_branch);
            free_stmt(stmt->as.if_stmt.else_branch);
            break;
        case DS_STMT_BLOCK:
            for (size_t i = 0; i < stmt->as.block_stmt.statements.len; i++) {
                free_stmt(stmt->as.block_stmt.statements.items[i]);
            }
            free(stmt->as.block_stmt.statements.items);
            break;
        case DS_STMT_IMPORT:
            free(stmt->as.import_stmt.path.data);
            break;
        case DS_STMT_CMD:
            ds_command_free(&stmt->as.cmd_stmt);
            break;
        case DS_STMT_FN:
            free(stmt->as.fn_stmt.name.data);
            for (size_t i = 0; i < stmt->as.fn_stmt.params.len; i++) {
                free(stmt->as.fn_stmt.params.items[i].name.data);
                free_expr(stmt->as.fn_stmt.params.items[i].default_value);
            }
            free(stmt->as.fn_stmt.params.items);
            free_stmt(stmt->as.fn_stmt.body);
            break;
        case DS_STMT_CALL:
            free(stmt->as.call_stmt.name.data);
            for (size_t i = 0; i < stmt->as.call_stmt.args.len; i++) free_expr(stmt->as.call_stmt.args.items[i]);
            free(stmt->as.call_stmt.args.items);
            break;
        case DS_STMT_FOR:
            free(stmt->as.for_stmt.key_name.data);
            free(stmt->as.for_stmt.value_name.data);
            free_expr(stmt->as.for_stmt.iterable);
            free_stmt(stmt->as.for_stmt.body);
            break;
        case DS_STMT_WHILE:
            free_expr(stmt->as.while_stmt.condition);
            free_stmt(stmt->as.while_stmt.body);
            break;
        case DS_STMT_BREAK:
        case DS_STMT_CONTINUE:
            break;
        case DS_STMT_CASE:
            free_expr(stmt->as.case_stmt.selector);
            for (size_t i = 0; i < stmt->as.case_stmt.arms.len; i++) {
                DsCaseArm *arm = &stmt->as.case_stmt.arms.items[i];
                for (size_t j = 0; j < arm->patterns.len; j++) free(arm->patterns.items[j].text.data);
                free(arm->patterns.items);
                free_stmt(arm->body);
            }
            free(stmt->as.case_stmt.arms.items);
            break;
        case DS_STMT_PUSH:
            free(stmt->as.push_stmt.name.data);
            free_expr(stmt->as.push_stmt.value);
            break;
        case DS_STMT_TEST:
            free(stmt->as.test_stmt.name.data);
            free_stmt(stmt->as.test_stmt.body);
            break;
        case DS_STMT_ASSERT:
            free_expr(stmt->as.assert_stmt.condition);
            break;
        case DS_STMT_RETURN:
            free_expr(stmt->as.return_stmt.value);
            break;
        case DS_STMT_DEFER:
        case DS_STMT_TRAP:
            free(stmt->as.handler_stmt.signal_text.data);
            free_stmt(stmt->as.handler_stmt.body);
            break;
    }
    free(stmt);
}

void ds_ast_free(DsAst *ast) {
    if (!ast) return;
    for (size_t i = 0; i < ast->script.declarations.len; i++) {
        free(ast->script.declarations.items[i].name.data);
        free_expr(ast->script.declarations.items[i].default_value);
    }
    free(ast->script.declarations.items);
    for (size_t i = 0; i < ast->statements.len; i++) {
        free_stmt(ast->statements.items[i]);
    }
    free(ast->statements.items);
    free(ast);
}
