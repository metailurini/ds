#include "lower_internal.h"

#include <string.h>

void collect_test(Lower *lower, const DsStmt *stmt, DsLowerProgram *program) {
    if (stmt->kind != DS_STMT_TEST) return;
    for (size_t i = 0; i < program->tests.len; i++) {
        if (program->tests.items[i].name.len == stmt->as.test_stmt.name.len &&
            memcmp(program->tests.items[i].name.data, stmt->as.test_stmt.name.data, stmt->as.test_stmt.name.len) == 0) {
            ds_diag_error(lower->diag, stmt->span, "duplicate test `%.*s`", (int)stmt->as.test_stmt.name.len, stmt->as.test_stmt.name.data);
            return;
        }
    }
    DsLowerTest test;
    memset(&test, 0, sizeof(test));
    test.name = str_clone(stmt->as.test_stmt.name);
    test.span = stmt->span;
    test.body = lower_block(lower, stmt->as.test_stmt.body, true);
    lower_test_vec_push(&program->tests, test);
}
