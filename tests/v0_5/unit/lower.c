#include "ds.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int checks = 0;

static void check(int cond, const char *msg) {
    if (!cond) {
        fprintf(stderr, "CHECK failed: %s\n", msg);
        exit(1);
    }
    checks++;
}

static DsLowerProgram *lower_text(const char *text, DsDiag *diag, DsSource *source) {
    source->path = "unit.ds";
    source->data = (char *)text;
    source->len = strlen(text);
    ds_diag_init(diag, source);
    DsTokenVec tokens = {0};
    check(ds_lex(source, &tokens, diag), "lex should succeed");
    DsAst *ast = ds_parse(&tokens, diag);
    check(ast != NULL, "parse returns ast");
    DsLowerProgram *program = ds_lower_program(ast, diag);
    ds_ast_free(ast);
    ds_tokens_free(&tokens);
    return program;
}

static void expect_success_contract(void) {
    const char *src =
        "script {\n"
        "  arg app: string\n"
        "  arg count: int\n"
        "  option target: string = \"staging\"\n"
        "  option retries: int = 3\n"
        "  option dry: bool = false\n"
        "  flag force: bool = false\n"
        "}\n"
        "echo \"{app}:{target}:{retries}:{force}:{dry}:{count}\"\n";
    DsDiag diag;
    DsSource source;
    DsLowerProgram *program = lower_text(src, &diag, &source);
    check(program != NULL, "lowering returns program");
    check(!diag.has_error, "lowering has no diagnostics");
    check(program->has_script, "program has script contract");
    check(program->script_decls.len == 6, "contract has six declarations");
    check(program->script_decls.items[0].kind == DS_SCRIPT_DECL_ARG, "first decl is arg");
    check(program->script_decls.items[0].type == DS_SCRIPT_TYPE_STRING, "first decl type string");
    check(program->script_decls.items[1].type == DS_SCRIPT_TYPE_INT, "second positional type int");
    check(program->script_decls.items[2].kind == DS_SCRIPT_DECL_OPTION, "third decl is option");
    check(program->script_decls.items[2].has_default, "string option has default");
    check(program->script_decls.items[2].default_text.len == strlen("staging"), "string default decoded length");
    check(memcmp(program->script_decls.items[2].default_text.data, "staging", strlen("staging")) == 0, "string default decoded text");
    check(program->script_decls.items[3].default_int == 3, "int option default lowered");
    check(program->script_decls.items[4].type == DS_SCRIPT_TYPE_BOOL, "bool option type lowered");
    check(program->script_decls.items[4].default_bool == false, "bool option default false");
    check(program->script_decls.items[5].kind == DS_SCRIPT_DECL_FLAG, "last decl is flag");
    check(program->script_decls.items[5].default_bool == false, "flag default false");
    check(program->statements.len == 1, "body statement lowered");
    ds_lower_program_free(program);
}

static void expect_lower_error(const char *src, const char *label) {
    DsDiag diag;
    DsSource source;
    DsLowerProgram *program = lower_text(src, &diag, &source);
    check(program == NULL, label);
    check(diag.has_error, "diagnostic is marked for lower error");
}

int main(void) {
    expect_success_contract();
    expect_lower_error("script {\n  arg app: string\n  option app: string = \"x\"\n}\n", "duplicate names rejected");
    expect_lower_error("script {\n  arg app: string\n}\nlet app = \"x\"\n", "script decl conflicts with later let");
    expect_lower_error("script {\n  option retries: int = \"three\"\n}\n", "default type mismatch rejected");
    expect_lower_error("script {\n  flag force: bool = true\n}\n", "flag true default rejected");
    expect_lower_error("script {\n  arg enabled: bool\n}\n", "bool positional rejected");

    printf("v0.5 lowering unit checks passed: %d\n", checks);
    return 0;
}
