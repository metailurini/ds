#include "ds_ast.h"

static const char *const k_unary_op_names[] = {"!", "-"};
static const char *const k_binary_op_names[] = {
    "+", "-", "*", "/", "%", "**",
    "&&", "||",
    "==", "!=", ">", ">=", "<", "<=",
    "in", "matches"
};

const char *ds_unary_op_name(DsUnaryOp op) {
    return (unsigned)op < DS_ARRAY_LEN(k_unary_op_names) ? k_unary_op_names[op] : "?";
}

const char *ds_binary_op_name(DsBinaryOp op) {
    return (unsigned)op < DS_ARRAY_LEN(k_binary_op_names) ? k_binary_op_names[op] : "?";
}

DsBinaryOp ds_binary_op_from_text(DsStr text) {
    for (size_t i = 0; i < DS_ARRAY_LEN(k_binary_op_names); i++) {
        if (ds_str_eq_cstr(text, k_binary_op_names[i])) return (DsBinaryOp)i;
    }
    return DS_BINARY_INVALID;
}

bool ds_binary_op_is_arithmetic(DsBinaryOp op) {
    return op >= DS_BINARY_ADD && op <= DS_BINARY_POW;
}

bool ds_binary_op_is_logical(DsBinaryOp op) {
    return op >= DS_BINARY_AND && op <= DS_BINARY_OR;
}

bool ds_binary_op_is_comparison(DsBinaryOp op) {
    return op >= DS_BINARY_EQ && op <= DS_BINARY_LE;
}

bool ds_binary_op_is_comparison_like(DsBinaryOp op) {
    return ds_binary_op_is_comparison(op) || op == DS_BINARY_IN || op == DS_BINARY_MATCHES;
}

DsExpr *ds_expr_new(DsExprKind kind, DsSpan span) {
    DsExpr *expr = (DsExpr *)ds_xcalloc(1, sizeof(*expr));
    expr->kind = kind;
    expr->span = span;
    return expr;
}

DsStmt *ds_stmt_new(DsStmtKind kind, DsSpan span) {
    DsStmt *stmt = (DsStmt *)ds_xcalloc(1, sizeof(*stmt));
    stmt->kind = kind;
    stmt->span = span;
    return stmt;
}
#include "ds_signal.h"

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

static void print_command_stages(FILE *out, const DsCommand *command, int level) {
    for (size_t s = 0; s < command->stages.len; s++) {
        ds_fprint_indent(out, level);
        fprintf(out, "Stage %zu\n", s);
        for (size_t i = 0; i < command->stages.items[s].words.len; i++) {
            ds_fprint_indent(out, level + 1);
            fputs("Word ", out);
            ds_fprint_str(out, command->stages.items[s].words.items[i].text);
            fputc('\n', out);
        }
    }
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
            print_command_stages(out, &expr->as.run, level + 1);
            break;
        case DS_EXPR_FIELD:
            fprintf(out, "FieldExpr %.*s\n", (int)expr->as.field.field.len, expr->as.field.field.data);
            print_expr(expr->as.field.object, out, level + 1);
            break;
        case DS_EXPR_UNARY:
            fprintf(out, "UnaryExpr %s\n", ds_unary_op_name(expr->as.unary.op));
            print_expr(expr->as.unary.right, out, level + 1);
            break;
        case DS_EXPR_BINARY:
            fprintf(out, "BinaryExpr %s\n", ds_binary_op_name(expr->as.binary.op));
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
            print_command_stages(out, &stmt->as.cmd_stmt, level + 1);
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

static void free_expr_vec(DsExprVec *vec) {
    for (size_t i = 0; i < vec->len; i++) ds_expr_free(vec->items[i]);
    free(vec->items);
}

static void free_map_entry_vec(DsMapEntryVec *vec) {
    for (size_t i = 0; i < vec->len; i++) {
        free(vec->items[i].key.data);
        ds_expr_free(vec->items[i].value);
    }
    free(vec->items);
}

static void free_fn_param_vec(DsFnParamVec *vec) {
    for (size_t i = 0; i < vec->len; i++) {
        free(vec->items[i].name.data);
        ds_expr_free(vec->items[i].default_value);
    }
    free(vec->items);
}

static void free_stmt(DsStmt *stmt);

static void free_stmt_vec(DsStmtVec *vec) {
    for (size_t i = 0; i < vec->len; i++) free_stmt(vec->items[i]);
    free(vec->items);
}

static void free_case_arm_vec(DsCaseArmVec *vec) {
    for (size_t i = 0; i < vec->len; i++) {
        ds_case_pattern_vec_free(&vec->items[i].patterns);
        free_stmt(vec->items[i].body);
    }
    free(vec->items);
}

void ds_expr_free(DsExpr *expr) {
    if (!expr) return;
    switch (expr->kind) {
#include "generated/ast_expr_free.inc"
    }
    free(expr);
}

static void free_stmt(DsStmt *stmt) {
    if (!stmt) return;
    switch (stmt->kind) {
#include "generated/ast_stmt_free.inc"
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
    free_stmt_vec(&ast->statements);
    free(ast);
}
