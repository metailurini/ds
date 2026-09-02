#ifndef DS_LOWER_SCHEMA_H
#define DS_LOWER_SCHEMA_H

#include "ds_hir.h"

void row_schema_init(DsLowerRowSchema *schema);
void row_schema_free(DsLowerRowSchema *schema);
void row_schema_clone(const DsLowerRowSchema *src, DsLowerRowSchema *dst);
void row_schema_push(DsLowerRowSchema *schema, DsStr name, DsLowerValueKind kind);
const DsLowerRowField *row_schema_find(const DsLowerRowSchema *schema, DsStr name);
bool row_schema_equal(const DsLowerRowSchema *a, const DsLowerRowSchema *b);

#endif
