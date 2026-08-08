#include "ds_ast.h"
#include "ds_signal.h"

#include <stdlib.h>

void ds_case_pattern_fprint(FILE *out, const DsCasePattern *pattern) {
    if (pattern->kind == DS_CASE_PATTERN_DEFAULT) fputs("_", out);
    else if (pattern->kind == DS_CASE_PATTERN_BOOL) fputs(pattern->boolean ? "true" : "false", out);
    else ds_fprint_str(out, pattern->text);
}

void ds_case_pattern_vec_free(DsCasePatternVec *patterns) {
    if (!patterns) return;
    for (size_t i = 0; i < patterns->len; i++) free(patterns->items[i].text.data);
    free(patterns->items);
    *patterns = (DsCasePatternVec){0};
}

static void print_expr(const DsExpr *expr, FILE *out, int level) {
    if (!expr) {
        ds_fprint_indent(out, level);
        fputs("<missing expr>\n", out);
        return;
    }

    ds_fprint_indent(out, level);
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
                ds_fprint_indent(out, level + 1);
                fprintf(out, "Stage %zu\n", s);
                for (size_t i = 0; i < expr->as.run.stages.items[s].words.len; i++) {
                    ds_fprint_indent(out, level + 2);
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
                ds_fprint_indent(out, level + 1);
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
    ds_fprint_indent(out, level);
    switch (stmt->kind) {
        case DS_STMT_LET:
            fprintf(out, "LetStmt %.*s\n", (int)stmt->as.let_stmt.name.len, stmt->as.let_stmt.name.data);
            print_expr(stmt->as.let_stmt.value, out, level + 1);
            break;
        case DS_STMT_ASSIGN: {
            const char *op = ds_assign_op_name(stmt->as.assign_stmt.op);
            fprintf(out, "AssignStmt %.*s %s\n", (int)stmt->as.assign_stmt.name.len, stmt->as.assign_stmt.name.data, op);
            print_expr(stmt->as.assign_stmt.value, out, level + 1);
            break;
        }
        case DS_STMT_INDEX_ASSIGN: {
            const char *op = ds_assign_op_name(stmt->as.index_assign_stmt.op);
            fprintf(out, "IndexAssignStmt %s\n", op);
            print_expr(stmt->as.index_assign_stmt.target, out, level + 1);
            print_expr(stmt->as.index_assign_stmt.value, out, level + 1);
            break;
        }
        case DS_STMT_IF:
            fputs("IfStmt\n", out);
            ds_fprint_indent(out, level + 1);
            fputs("Condition\n", out);
            print_expr(stmt->as.if_stmt.condition, out, level + 2);
            ds_fprint_indent(out, level + 1);
            fputs("Then\n", out);
            print_stmt(stmt->as.if_stmt.then_branch, out, level + 2);
            if (stmt->as.if_stmt.else_branch) {
                ds_fprint_indent(out, level + 1);
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
                ds_fprint_indent(out, level + 1);
                fprintf(out, "Stage %zu\n", s);
                for (size_t i = 0; i < stmt->as.cmd_stmt.stages.items[s].words.len; i++) {
                    ds_fprint_indent(out, level + 2);
                    fprintf(out, "Word %.*s\n", (int)stmt->as.cmd_stmt.stages.items[s].words.items[i].text.len,
                            stmt->as.cmd_stmt.stages.items[s].words.items[i].text.data);
                }
            }
            if (stmt->as.cmd_stmt.redirect.kind != DS_REDIRECT_NONE) {
                ds_fprint_indent(out, level + 1);
                fprintf(out, "Redirect %s %.*s\n", ds_redirect_source_op(stmt->as.cmd_stmt.redirect.kind),
                        (int)stmt->as.cmd_stmt.redirect.target.len, stmt->as.cmd_stmt.redirect.target.data);
            }
            break;
        case DS_STMT_FN:
            fprintf(out, "FnStmt %.*s\n", (int)stmt->as.fn_stmt.name.len, stmt->as.fn_stmt.name.data);
            for (size_t i = 0; i < stmt->as.fn_stmt.params.len; i++) {
                const DsFnParam *param = &stmt->as.fn_stmt.params.items[i];
                ds_fprint_indent(out, level + 1);
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
            ds_fprint_indent(out, level + 1);
            fputs("Iterable\n", out);
            print_expr(stmt->as.for_stmt.iterable, out, level + 2);
            ds_fprint_indent(out, level + 1);
            fputs("Body\n", out);
            print_stmt(stmt->as.for_stmt.body, out, level + 2);
            break;
        case DS_STMT_WHILE:
            fputs("WhileStmt\n", out);
            ds_fprint_indent(out, level + 1);
            fputs("Condition\n", out);
            print_expr(stmt->as.while_stmt.condition, out, level + 2);
            ds_fprint_indent(out, level + 1);
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
            ds_fprint_indent(out, level + 1);
            fputs("Selector\n", out);
            print_expr(stmt->as.case_stmt.selector, out, level + 2);
            for (size_t i = 0; i < stmt->as.case_stmt.arms.len; i++) {
                const DsCaseArm *arm = &stmt->as.case_stmt.arms.items[i];
                ds_fprint_indent(out, level + 1);
                fputs("Arm", out);
                for (size_t j = 0; j < arm->patterns.len; j++) {
                    fputc(' ', out);
                    ds_case_pattern_fprint(out, &arm->patterns.items[j]);
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
            else fprintf(out, "DeferStmt %s\n", ds_handler_signal_name(stmt->as.handler_stmt.signal));
            print_stmt(stmt->as.handler_stmt.body, out, level + 1);
            break;
        case DS_STMT_TRAP:
            if (stmt->as.handler_stmt.signal == DS_HANDLER_INVALID) fprintf(out, "TrapStmt %.*s\n", (int)stmt->as.handler_stmt.signal_text.len, stmt->as.handler_stmt.signal_text.data);
            else fprintf(out, "TrapStmt %s\n", ds_handler_signal_name(stmt->as.handler_stmt.signal));
            print_stmt(stmt->as.handler_stmt.body, out, level + 1);
            break;
    }
}

const char *ds_assign_op_name(DsAssignOp op) {
    static const char *const names[] = {"=", "+=", "-=", "*=", "/=", "%="};
    return (unsigned)op < DS_ARRAY_LEN(names) ? names[op] : "=";
}

const char *ds_assign_binary_op(DsAssignOp op) {
    static const char *const names[] = {"=", "+", "-", "*", "/", "%"};
    return (unsigned)op < DS_ARRAY_LEN(names) ? names[op] : "=";
}

static const char *decl_kind_name(DsScriptDeclKind kind) {
    static const char *const names[] = {"ArgDecl", "OptionDecl", "FlagDecl"};
    return (unsigned)kind < DS_ARRAY_LEN(names) ? names[kind] : "Decl";
}

const char *ds_script_type_name(DsScriptType type) {
    static const char *const names[] = {"string", "int", "bool"};
    return (unsigned)type < DS_ARRAY_LEN(names) ? names[type] : "unknown";
}

void ds_ast_print(const DsAst *ast, FILE *out) {
    fputs("Script\n", out);
    if (ast->has_script) {
        ds_fprint_indent(out, 1);
        fputs("ScriptBlock\n", out);
        for (size_t i = 0; i < ast->script.declarations.len; i++) {
            const DsScriptDecl *decl = &ast->script.declarations.items[i];
            ds_fprint_indent(out, 2);
            fprintf(out, "%s %.*s: %s\n", decl_kind_name(decl->kind), (int)decl->name.len, decl->name.data, ds_script_type_name(decl->type));
            if (decl->default_value) {
                ds_fprint_indent(out, 3);
                fputs("Default\n", out);
                print_expr(decl->default_value, out, 4);
            }
        }
    }
    for (size_t i = 0; i < ast->statements.len; i++) {
        print_stmt(ast->statements.items[i], out, 1);
    }
}

void ds_expr_free(DsExpr *expr) {
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
            ds_expr_free(expr->as.field.object);
            free(expr->as.field.field.data);
            break;
        case DS_EXPR_UNARY:
            free(expr->as.unary.op.data);
            ds_expr_free(expr->as.unary.right);
            break;
        case DS_EXPR_BINARY:
            ds_expr_free(expr->as.binary.left);
            free(expr->as.binary.op.data);
            ds_expr_free(expr->as.binary.right);
            break;
        case DS_EXPR_CALL:
            free(expr->as.call.name.data);
            DS_FREE_PTR_VEC(expr->as.call.args, ds_expr_free);
            break;
        case DS_EXPR_ARRAY:
            DS_FREE_PTR_VEC(expr->as.array.elements, ds_expr_free);
            break;
        case DS_EXPR_MAP:
            DS_FREE_KEYED_PTR_VEC(expr->as.map.entries, ds_expr_free);
            break;
        case DS_EXPR_INDEX:
            ds_expr_free(expr->as.index.object);
            ds_expr_free(expr->as.index.index);
            break;
        case DS_EXPR_RANGE:
            ds_expr_free(expr->as.range.start);
            ds_expr_free(expr->as.range.end);
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
            ds_expr_free(stmt->as.let_stmt.value);
            break;
        case DS_STMT_ASSIGN:
            free(stmt->as.assign_stmt.name.data);
            ds_expr_free(stmt->as.assign_stmt.value);
            break;
        case DS_STMT_INDEX_ASSIGN:
            ds_expr_free(stmt->as.index_assign_stmt.target);
            ds_expr_free(stmt->as.index_assign_stmt.value);
            break;
        case DS_STMT_IF:
            ds_expr_free(stmt->as.if_stmt.condition);
            free_stmt(stmt->as.if_stmt.then_branch);
            free_stmt(stmt->as.if_stmt.else_branch);
            break;
        case DS_STMT_BLOCK:
            DS_FREE_PTR_VEC(stmt->as.block_stmt.statements, free_stmt);
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
                ds_expr_free(stmt->as.fn_stmt.params.items[i].default_value);
            }
            free(stmt->as.fn_stmt.params.items);
            free_stmt(stmt->as.fn_stmt.body);
            break;
        case DS_STMT_CALL:
            free(stmt->as.call_stmt.name.data);
            DS_FREE_PTR_VEC(stmt->as.call_stmt.args, ds_expr_free);
            break;
        case DS_STMT_FOR:
            free(stmt->as.for_stmt.key_name.data);
            free(stmt->as.for_stmt.value_name.data);
            ds_expr_free(stmt->as.for_stmt.iterable);
            free_stmt(stmt->as.for_stmt.body);
            break;
        case DS_STMT_WHILE:
            ds_expr_free(stmt->as.while_stmt.condition);
            free_stmt(stmt->as.while_stmt.body);
            break;
        case DS_STMT_BREAK:
        case DS_STMT_CONTINUE:
            break;
        case DS_STMT_CASE:
            ds_expr_free(stmt->as.case_stmt.selector);
            DS_FREE_CASE_ARM_VEC(stmt->as.case_stmt.arms, free_stmt);
            break;
        case DS_STMT_PUSH:
            free(stmt->as.push_stmt.name.data);
            ds_expr_free(stmt->as.push_stmt.value);
            break;
        case DS_STMT_TEST:
            free(stmt->as.test_stmt.name.data);
            free_stmt(stmt->as.test_stmt.body);
            break;
        case DS_STMT_ASSERT:
            ds_expr_free(stmt->as.assert_stmt.condition);
            break;
        case DS_STMT_RETURN:
            ds_expr_free(stmt->as.return_stmt.value);
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
        ds_expr_free(ast->script.declarations.items[i].default_value);
    }
    free(ast->script.declarations.items);
    DS_FREE_PTR_VEC(ast->statements, free_stmt);
    free(ast);
}
