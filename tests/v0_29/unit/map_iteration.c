#include "ds.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static DsStr view(const char *text) {
    DsStr out = {(char *)text, strlen(text)};
    return out;
}

static void expect_key(DsStr key, const char *expected) {
    assert(key.len == strlen(expected));
    assert(memcmp(key.data ? key.data : "", expected, key.len) == 0);
}

static void test_empty_map_sorted_keys(void) {
    DsMap map;
    ds_map_init(&map);

    DsStr *keys = (DsStr *)1;
    size_t len = 99;
    ds_map_sorted_keys(&map, &keys, &len);
    assert(len == 0);
    assert(keys == NULL);

    ds_map_sorted_keys_free(keys, len);
    ds_map_free(&map);
}

static void test_sorted_keys_are_bytewise(void) {
    DsMap map;
    ds_map_init(&map);
    assert(ds_map_set(&map, view("2"), ds_value_int(2)));
    assert(ds_map_set(&map, view("10"), ds_value_int(10)));
    assert(ds_map_set(&map, view("1"), ds_value_int(1)));
    assert(ds_map_set(&map, view("b"), ds_value_int(4)));
    assert(ds_map_set(&map, view("A"), ds_value_int(5)));

    DsStr *keys = NULL;
    size_t len = 0;
    ds_map_sorted_keys(&map, &keys, &len);
    assert(len == 5);
    expect_key(keys[0], "1");
    expect_key(keys[1], "10");
    expect_key(keys[2], "2");
    expect_key(keys[3], "A");
    expect_key(keys[4], "b");

    ds_map_sorted_keys_free(keys, len);
    ds_map_free(&map);
}

static DsStr owned(const char *text) {
    return (DsStr){ds_str_dup_range(text, strlen(text)), strlen(text)};
}

static DsLowerExpr *lower_expr_new(DsLowerExprKind kind) {
    DsLowerExpr *expr = (DsLowerExpr *)ds_xcalloc(1, sizeof(DsLowerExpr));
    expr->kind = kind;
    return expr;
}

static DsLowerStmt *lower_stmt_new(DsLowerStmtKind kind) {
    DsLowerStmt *stmt = (DsLowerStmt *)ds_xcalloc(1, sizeof(DsLowerStmt));
    stmt->kind = kind;
    return stmt;
}

static DsLowerExpr *empty_map_expr(void) {
    return lower_expr_new(DS_LOWER_EXPR_MAP);
}

static DsLowerExpr *ident_expr(const char *name) {
    DsLowerExpr *expr = lower_expr_new(DS_LOWER_EXPR_IDENT);
    expr->as.text = owned(name);
    return expr;
}

static DsLowerStmt *let_empty_map_stmt(void) {
    DsLowerStmt *stmt = lower_stmt_new(DS_LOWER_STMT_LET);
    stmt->as.let_stmt.name = owned("values");
    stmt->as.let_stmt.value = empty_map_expr();
    return stmt;
}

static DsLowerStmt *fail_unreachable_stmt(void) {
    DsLowerStmt *stmt = lower_stmt_new(DS_LOWER_STMT_CMD);
    stmt->as.cmd_stmt.kind = DS_COMMAND_PLAIN;
    stmt->as.cmd_stmt.stages.len = 1;
    stmt->as.cmd_stmt.stages.items = (DsLowerCommandStage *)ds_xcalloc(1, sizeof(DsLowerCommandStage));
    stmt->as.cmd_stmt.stages.items[0].words.len = 2;
    stmt->as.cmd_stmt.stages.items[0].words.items =
        (DsLowerCommandWord *)ds_xcalloc(2, sizeof(DsLowerCommandWord));
    stmt->as.cmd_stmt.stages.items[0].words.items[0].kind = DS_LOWER_COMMAND_WORD_LITERAL;
    stmt->as.cmd_stmt.stages.items[0].words.items[0].source_text = owned("fail");
    stmt->as.cmd_stmt.stages.items[0].words.items[0].literal_text = owned("fail");
    stmt->as.cmd_stmt.stages.items[0].words.items[1].kind = DS_LOWER_COMMAND_WORD_LITERAL;
    stmt->as.cmd_stmt.stages.items[0].words.items[1].source_text =
        owned("empty map loop body unexpectedly executed");
    stmt->as.cmd_stmt.stages.items[0].words.items[1].literal_text =
        owned("empty map loop body unexpectedly executed");
    return stmt;
}

static DsLowerStmt *block_with_unreachable_control(DsLowerStmtKind control_kind) {
    DsLowerStmt *block = lower_stmt_new(DS_LOWER_STMT_BLOCK);
    block->as.block_stmt.scoped = true;
    block->as.block_stmt.statements.len = 2;
    block->as.block_stmt.statements.items = (DsLowerStmt **)ds_xcalloc(2, sizeof(DsLowerStmt *));
    block->as.block_stmt.statements.items[0] = fail_unreachable_stmt();
    block->as.block_stmt.statements.items[1] = lower_stmt_new(control_kind);
    return block;
}

static DsLowerStmt *empty_map_loop_stmt(DsLowerStmtKind control_kind) {
    DsLowerStmt *stmt = lower_stmt_new(DS_LOWER_STMT_FOR_MAP);
    stmt->as.for_stmt.name = owned("key");
    stmt->as.for_stmt.value_name = owned("value");
    stmt->as.for_stmt.iterable = ident_expr("values");
    stmt->as.for_stmt.body = block_with_unreachable_control(control_kind);
    stmt->as.for_stmt.element_kind = DS_LOWER_VALUE_UNKNOWN;
    return stmt;
}

static DsLowerProgram *empty_map_loop_program(DsLowerStmtKind control_kind) {
    DsLowerProgram *program = (DsLowerProgram *)ds_xcalloc(1, sizeof(DsLowerProgram));
    program->statements.len = 2;
    program->statements.items = (DsLowerStmt **)ds_xcalloc(2, sizeof(DsLowerStmt *));
    program->statements.items[0] = let_empty_map_stmt();
    program->statements.items[1] = empty_map_loop_stmt(control_kind);
    return program;
}

static void source_and_diag(DsSource *source, DsDiag *diag) {
    *source = (DsSource){"<v0.29 empty-map unit>", "", 0};
    ds_diag_init(diag, source);
}

static void test_empty_map_loop_body_with_break_is_inert(const char *tmp_dir) {
    DsSource source;
    DsDiag diag;
    source_and_diag(&source, &diag);
    DsLowerProgram *program = empty_map_loop_program(DS_LOWER_STMT_BREAK);
    assert(ds_vm_run_program(&source, program, &diag) == 0);
    assert(!diag.has_error);
    if (tmp_dir) {
        char path[4096];
        snprintf(path, sizeof(path), "%s/empty_map_break.sh", tmp_dir);
        assert(ds_emit_bash_program(&source, program, path, &diag));
        assert(!diag.has_error);
    }
    ds_lower_program_free(program);
}

static void test_empty_map_loop_body_with_continue_is_inert(const char *tmp_dir) {
    DsSource source;
    DsDiag diag;
    source_and_diag(&source, &diag);
    DsLowerProgram *program = empty_map_loop_program(DS_LOWER_STMT_CONTINUE);
    assert(ds_vm_run_program(&source, program, &diag) == 0);
    assert(!diag.has_error);
    if (tmp_dir) {
        char path[4096];
        snprintf(path, sizeof(path), "%s/empty_map_continue.sh", tmp_dir);
        assert(ds_emit_bash_program(&source, program, path, &diag));
        assert(!diag.has_error);
    }
    ds_lower_program_free(program);
}

int main(int argc, char **argv) {
    const char *tmp_dir = argc > 1 ? argv[1] : NULL;
    test_empty_map_sorted_keys();
    test_sorted_keys_are_bytewise();
    test_empty_map_loop_body_with_break_is_inert(tmp_dir);
    test_empty_map_loop_body_with_continue_is_inert(tmp_dir);
    return 0;
}
