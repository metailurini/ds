#ifndef DS_LOWER_FREE_H
#define DS_LOWER_FREE_H

#include "ds_hir.h"

void lower_expr_free(DsLowerExpr *expr);
void lower_stmt_free(DsLowerStmt *stmt);

#endif
