#include "ds_command_result.h"

#include <string.h>

static const DsCommandResultField k_fields[] = {
    {"stdout", "stdout", DS_COMMAND_RESULT_FIELD_STDOUT, DS_COMMAND_RESULT_FIELD_STRING},
    {"stderr", "stderr", DS_COMMAND_RESULT_FIELD_STDERR, DS_COMMAND_RESULT_FIELD_STRING},
    {"status", "code", DS_COMMAND_RESULT_FIELD_STATUS, DS_COMMAND_RESULT_FIELD_INT},
    {"code", "code", DS_COMMAND_RESULT_FIELD_CODE, DS_COMMAND_RESULT_FIELD_INT},
    {"ok", "ok", DS_COMMAND_RESULT_FIELD_OK, DS_COMMAND_RESULT_FIELD_BOOL},
    {"failed", "failed", DS_COMMAND_RESULT_FIELD_FAILED, DS_COMMAND_RESULT_FIELD_BOOL},
};

const DsCommandResultField *ds_command_result_field_lookup(DsStr field) {
    for (size_t i = 0; i < sizeof(k_fields) / sizeof(k_fields[0]); i++) {
        size_t len = strlen(k_fields[i].name);
        if (field.len == len && memcmp(field.data, k_fields[i].name, len) == 0) return &k_fields[i];
    }
    return NULL;
}

size_t ds_command_result_field_count(void) {
    return sizeof(k_fields) / sizeof(k_fields[0]);
}

const DsCommandResultField *ds_command_result_field_at(size_t index) {
    if (index >= ds_command_result_field_count()) return NULL;
    return &k_fields[index];
}

const char *ds_command_result_field_kind_name(DsCommandResultFieldKind kind) {
    switch (kind) {
        case DS_COMMAND_RESULT_FIELD_STRING: return "string";
        case DS_COMMAND_RESULT_FIELD_INT: return "int";
        case DS_COMMAND_RESULT_FIELD_BOOL: return "bool";
    }
    return "unknown";
}
