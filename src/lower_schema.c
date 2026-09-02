#include "lower_schema.h"

void row_schema_init(DsLowerRowSchema *schema) {
    if (!schema) return;
    *schema = (DsLowerRowSchema){0};
}

void row_schema_free(DsLowerRowSchema *schema) {
    if (!schema) return;
    for (size_t i = 0; i < schema->len; i++) free(schema->items[i].name.data);
    free(schema->items);
    *schema = (DsLowerRowSchema){0};
}

void row_schema_push(DsLowerRowSchema *schema, DsStr name, DsLowerValueKind kind) {
    if (!schema) return;
    DsLowerRowField field = {ds_str_clone(name), kind};
    DS_VEC_PUSH(schema, field, 4);
}

void row_schema_clone(const DsLowerRowSchema *src, DsLowerRowSchema *dst) {
    if (!dst) return;
    row_schema_init(dst);
    if (!src) return;
    for (size_t i = 0; i < src->len; i++) {
        row_schema_push(dst, src->items[i].name, src->items[i].kind);
    }
}

const DsLowerRowField *row_schema_find(const DsLowerRowSchema *schema, DsStr name) {
    if (!schema) return NULL;
    for (size_t i = 0; i < schema->len; i++) {
        if (ds_str_eq(schema->items[i].name, name)) return &schema->items[i];
    }
    return NULL;
}

bool row_schema_equal(const DsLowerRowSchema *a, const DsLowerRowSchema *b) {
    if (!a || !b) return false;
    if (a->len != b->len) return false;
    for (size_t i = 0; i < a->len; i++) {
        const DsLowerRowField *field = row_schema_find(b, a->items[i].name);
        if (!field || field->kind != a->items[i].kind) return false;
    }
    return true;
}
