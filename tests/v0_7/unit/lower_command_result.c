#include "ds.h"

#include <assert.h>
#include <stdbool.h>
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
    DsSource source = {"<v0.7-lower-test>", dup_cstr(source_text), strlen(source_text)};
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
    DsSource source = {"<v0.7-lower-test>", dup_cstr(source_text), strlen(source_text)};
    DsDiag diag;
    ds_diag_init(&diag, &source);
    DsTokenVec tokens = {0};
    assert(ds_lex(&source, &tokens, &diag));
    DsAst *ast = ds_parse(&tokens, &diag);
    assert(ast != NULL || diag.has_error);
    DsLowerProgram *program = ast ? ds_lower_program(ast, &diag) : NULL;
    assert(program == NULL);
    assert(diag.has_error);
    if (ast) ds_ast_free(ast);
    ds_tokens_free(&tokens);
    free(source.data);
}

static void test_run_and_fields_lower(void) {
    DsLowerProgram *program = lower_ok(
        "let r = run printf \"hello\"\n"
        "if r.failed { echo r.stderr } else { echo r.stdout }\n"
    );
    assert(program->statements.len == 2);
    DsLowerStmt *let = program->statements.items[0];
    assert(let->kind == DS_LOWER_STMT_LET);
    assert(str_eq(let->as.let_stmt.name, "r"));
    assert(let->as.let_stmt.value->kind == DS_LOWER_EXPR_RUN);
    assert(let->as.let_stmt.value->as.run.stages.len == 1);
    assert(let->as.let_stmt.value->as.run.stages.items[0].words.len == 2);
    assert(str_eq(let->as.let_stmt.value->as.run.stages.items[0].words.items[0].text, "printf"));

    DsLowerStmt *ifs = program->statements.items[1];
    assert(ifs->kind == DS_LOWER_STMT_IF);
    assert(ifs->as.if_stmt.condition->kind == DS_LOWER_EXPR_FIELD);
    assert(str_eq(ifs->as.if_stmt.condition->as.field.field, "failed"));
    assert(ifs->as.if_stmt.condition->as.field.object->kind == DS_LOWER_EXPR_IDENT);
    ds_lower_program_free(program);
}

static void test_redirection_lowers(void) {
    DsLowerProgram *program = lower_ok(
        "printf \"out\" |> \"out.txt\"\n"
        "sh -c \"printf err >&2\" !>> \"err.txt\"\n"
        "sh -c \"printf all\" &> \"all.txt\"\n"
    );
    assert(program->statements.len == 3);
    assert(program->statements.items[0]->as.cmd_stmt.redirect.kind == DS_REDIRECT_OUT);
    assert(program->statements.items[1]->as.cmd_stmt.redirect.kind == DS_REDIRECT_ERR_APPEND);
    assert(program->statements.items[2]->as.cmd_stmt.redirect.kind == DS_REDIRECT_ALL);
    assert(str_eq(program->statements.items[2]->as.cmd_stmt.redirect.target, "\"all.txt\""));
    ds_lower_program_free(program);
}

static void test_lower_rejections(void) {
    lower_fail("let r = run printf $missing\n");
    lower_fail("let r = run printf \"ok\"\necho r.nope\n");
    lower_fail("let s = \"text\"\necho s.stdout\n");
}

int main(void) {
    test_run_and_fields_lower();
    test_redirection_lowers();
    test_lower_rejections();
    return 0;
}
