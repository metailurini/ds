#ifndef DS_LOWER_STMT_H
#define DS_LOWER_STMT_H

#include "lower_context.h"

DsLowerStmt *stmt_new(DsLowerStmtKind kind, DsSpan span);
DsLowerStmt *lower_call_stmt(Lower *lower, const DsStmt *stmt);
DsLowerStmt *lower_block(Lower *lower, const DsStmt *block, bool child_scope);
DsLowerStmt *lower_stmt(Lower *lower, const DsStmt *stmt);

#endif
