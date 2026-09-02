#ifndef DS_COMMAND_FACTS_H
#define DS_COMMAND_FACTS_H

#include "ds_command.h"
#include "ds_common.h"

/*
 * Policy-neutral command facts shared by lowering, VM, and Bash emission.
 * This module intentionally does not own command payload storage; that stays in
 * ds_command.c. It also does not decide source-language legality; lowering owns
 * acceptance and diagnostics.
 */

typedef enum {
    DS_COMMAND_WORD_LITERAL,
    DS_COMMAND_WORD_QUOTED,
    DS_COMMAND_WORD_VARIABLE,
    DS_COMMAND_WORD_FIELD
} DsCommandWordKind;

typedef struct {
    DsCommandWordKind kind;
    DsStr name;
    DsStr field;
} DsCommandWordForm;

bool ds_command_name_char(char c);
DsCommandWordForm ds_command_word_analyze(DsStr word);

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

size_t ds_command_stage_count(const DsCommand *command);
bool ds_command_is_pipeline(const DsCommand *command);
int ds_command_pipeline_status(const int *stage_codes, size_t stage_count);

#endif
