#include "ds.h"

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *dup_cstr(const char *text) {
    size_t len = strlen(text);
    char *out = (char *)malloc(len + 1);
    assert(out != NULL);
    memcpy(out, text, len + 1);
    return out;
}

static bool str_eq(DsStr actual, const char *expected) {
    size_t len = strlen(expected);
    return actual.len == len && memcmp(actual.data, expected, len) == 0;
}

static DsLowerProgram *lower_ok(const char *source_text) {
    DsSource source = {"<lower-test>", dup_cstr(source_text), strlen(source_text)};
    DsDiag diag;
    ds_diag_init(&diag, &source);
    DsTokenVec tokens = {0};
    assert(ds_lex(&source, &tokens, &diag));
    DsAst *ast = ds_parse(&tokens, &diag);
    assert(ast != NULL);
    assert(!diag.has_error);
    DsLowerProgram *program = ds_lower_program(ast, &diag);
    assert(program != NULL);
    assert(!diag.has_error);
    ds_ast_free(ast);
    ds_tokens_free(&tokens);
    free(source.data);
    return program;
}

static void lower_fail(const char *source_text) {
    DsSource source = {"<lower-test>", dup_cstr(source_text), strlen(source_text)};
    DsDiag diag;
    ds_diag_init(&diag, &source);
    DsTokenVec tokens = {0};
    assert(ds_lex(&source, &tokens, &diag));
    DsAst *ast = ds_parse(&tokens, &diag);
    assert(ast != NULL);
    DsLowerProgram *program = ds_lower_program(ast, &diag);
    assert(program == NULL);
    assert(diag.has_error);
    ds_ast_free(ast);
    ds_tokens_free(&tokens);
    free(source.data);
}

static void test_lower_empty_and_comments(void) {
    DsLowerProgram *empty = lower_ok("");
    assert(empty->statements.len == 0);
    ds_lower_program_free(empty);

    DsLowerProgram *comments = lower_ok("# comment only\n# second\n");
    assert(comments->statements.len == 0);
    ds_lower_program_free(comments);
}

static void test_lower_mixed_tree(void) {
    DsLowerProgram *program = lower_ok(
        "let name = \"Danh\"\n"
        "let count = 2\n"
        "let ok = true\n"
        "if ok {\n"
        "  echo \"Hello {name}\"\n"
        "} else {\n"
        "  echo $count\n"
        "}\n"
    );
    assert(program->statements.len == 4);
    assert(program->statements.items[0]->kind == DS_LOWER_STMT_LET);
    assert(str_eq(program->statements.items[0]->as.let_stmt.name, "name"));
    assert(program->statements.items[0]->as.let_stmt.value->kind == DS_LOWER_EXPR_STRING);
    assert(program->statements.items[1]->as.let_stmt.value->kind == DS_LOWER_EXPR_INT);
    assert(program->statements.items[2]->as.let_stmt.value->kind == DS_LOWER_EXPR_BOOL);

    DsLowerStmt *ifs = program->statements.items[3];
    assert(ifs->kind == DS_LOWER_STMT_IF);
    assert(ifs->as.if_stmt.condition->kind == DS_LOWER_EXPR_IDENT);
    assert(ifs->as.if_stmt.then_branch != NULL);
    assert(ifs->as.if_stmt.else_branch != NULL);
    assert(ifs->as.if_stmt.then_branch->kind == DS_LOWER_STMT_BLOCK);
    assert(ifs->as.if_stmt.then_branch->as.block_stmt.statements.len == 1);
    assert(ifs->as.if_stmt.then_branch->as.block_stmt.statements.items[0]->kind == DS_LOWER_STMT_CMD);
    assert(ifs->as.if_stmt.else_branch->as.block_stmt.statements.len == 1);
    assert(ifs->as.if_stmt.else_branch->as.block_stmt.statements.items[0]->kind == DS_LOWER_STMT_CMD);
    ds_lower_program_free(program);
}

static void test_lower_unary_binary_and_scope(void) {
    DsLowerProgram *program = lower_ok(
        "let ok = true\n"
        "if !ok { echo \"bad\" } else {\n"
        "  let inside = \"ok\"\n"
        "  if inside == \"ok\" { echo $inside }\n"
        "}\n"
    );
    DsLowerStmt *ifs = program->statements.items[1];
    assert(ifs->as.if_stmt.condition->kind == DS_LOWER_EXPR_UNARY);
    DsLowerStmt *nested = ifs->as.if_stmt.else_branch->as.block_stmt.statements.items[1];
    assert(nested->kind == DS_LOWER_STMT_IF);
    assert(nested->as.if_stmt.condition->kind == DS_LOWER_EXPR_BINARY);
    assert(strcmp(ds_binary_op_name(nested->as.if_stmt.condition->as.binary.op), "==") == 0);
    ds_lower_program_free(program);

    lower_fail("if true { let hidden = \"x\" }\necho $hidden\n");
    lower_fail("let name = \"one\"\nlet name = \"two\"\n");
    lower_fail("echo \"Hello {missing}\"\n");
    lower_fail("let total = \"a\" + \"b\"\n");
}

int main(void) {
    test_lower_empty_and_comments();
    test_lower_mixed_tree();
    test_lower_unary_binary_and_scope();
    return 0;
}
