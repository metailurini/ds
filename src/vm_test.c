#include "vm_internal.h"

#include <string.h>

int ds_vm_run_test(const DsSource *source, const DsLowerProgram *lowered, const DsLowerTest *test, DsDiag *diag) {
    DsLowerProgram view;
    memset(&view, 0, sizeof(view));
    view.functions = lowered->functions;
    view.span = test->span;
    view.statements.len = 1;
    view.statements.cap = 1;
    view.statements.items = (DsLowerStmt **)&test->body;
    DsVmOptions options = {0};
    options.test_mode = true;
    options.test_name = test->name;
    return ds_vm_run_program_args_options(source, &view, 0, NULL, diag, options);
}
