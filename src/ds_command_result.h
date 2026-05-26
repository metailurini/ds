#ifndef DS_COMMAND_RESULT_H
#define DS_COMMAND_RESULT_H

#include "ds_common.h"

typedef enum {
    DS_COMMAND_RESULT_FIELD_STDOUT,
    DS_COMMAND_RESULT_FIELD_STDERR,
    DS_COMMAND_RESULT_FIELD_STATUS,
    DS_COMMAND_RESULT_FIELD_CODE,
    DS_COMMAND_RESULT_FIELD_OK,
    DS_COMMAND_RESULT_FIELD_FAILED
} DsCommandResultFieldId;

typedef enum {
    DS_COMMAND_RESULT_FIELD_STRING,
    DS_COMMAND_RESULT_FIELD_INT,
    DS_COMMAND_RESULT_FIELD_BOOL
} DsCommandResultFieldKind;

typedef struct {
    const char *name;
    const char *storage_name;
    DsCommandResultFieldId id;
    DsCommandResultFieldKind kind;
} DsCommandResultField;

const DsCommandResultField *ds_command_result_field_lookup(DsStr field);
size_t ds_command_result_field_count(void);
const DsCommandResultField *ds_command_result_field_at(size_t index);
const char *ds_command_result_field_kind_name(DsCommandResultFieldKind kind);

#endif
