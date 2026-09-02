#include "formatter.h"
#include "backend.h"

bool ds_formatter_format_source(const DsSource *source, const DsAst *ast,
                                DsString *out, DsDiag *diag) {
    return ds_format_source(source, ast, out, diag);
}
