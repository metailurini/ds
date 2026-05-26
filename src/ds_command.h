#ifndef DS_COMMAND_H
#define DS_COMMAND_H

#include "ds_common.h"

typedef struct {
    DsStr text;
    DsSpan span;
} DsWord;

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

typedef struct {
    DsWord *items;
    size_t len;
    size_t cap;
} DsWordVec;

typedef struct {
    DsWordVec words;
    DsSpan span;
} DsCommandStage;

typedef struct {
    DsCommandStage *items;
    size_t len;
    size_t cap;
} DsCommandStageVec;

typedef enum {
    DS_REDIRECT_NONE,
    DS_REDIRECT_OUT,
    DS_REDIRECT_OUT_APPEND,
    DS_REDIRECT_ERR,
    DS_REDIRECT_ERR_APPEND,
    DS_REDIRECT_ALL,
    DS_REDIRECT_ALL_APPEND
} DsRedirectKind;

typedef struct {
    DsRedirectKind kind;
    DsStr target;
    DsSpan op_span;
    DsSpan target_span;
} DsRedirect;

typedef enum {
    DS_COMMAND_PLAIN,
    DS_COMMAND_CAPTURE
} DsCommandKind;

typedef struct {
    DsCommandKind kind;
    DsCommandStageVec stages;
    DsRedirect redirect;
    DsSpan span;
} DsCommand;

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

void ds_word_vec_init(DsWordVec *vec);
bool ds_word_vec_clone(DsWordVec *dst, const DsWordVec *src);
void ds_word_vec_free(DsWordVec *vec);
void ds_command_stage_init(DsCommandStage *stage);
bool ds_command_stage_clone(DsCommandStage *dst, const DsCommandStage *src);
void ds_command_stage_free(DsCommandStage *stage);
void ds_command_stage_vec_init(DsCommandStageVec *vec);
bool ds_command_stage_vec_clone(DsCommandStageVec *dst, const DsCommandStageVec *src);
void ds_command_stage_vec_free(DsCommandStageVec *vec);
void ds_redirect_init(DsRedirect *redirect);
bool ds_redirect_clone(DsRedirect *dst, const DsRedirect *src);
void ds_redirect_free(DsRedirect *redirect);
void ds_command_init(DsCommand *command, DsCommandKind kind, DsSpan span);
bool ds_command_clone(DsCommand *dst, const DsCommand *src);
void ds_command_free(DsCommand *command);
size_t ds_command_stage_count(const DsCommand *command);
bool ds_command_is_pipeline(const DsCommand *command);
int ds_command_pipeline_status(const int *stage_codes, size_t stage_count);
bool ds_command_name_char(char c);
DsCommandWordForm ds_command_word_analyze(DsStr word);
bool ds_command_word_contains_direct_call_interpolation(DsStr decoded);
const DsCommandResultField *ds_command_result_field_lookup(DsStr field);
size_t ds_command_result_field_count(void);
const DsCommandResultField *ds_command_result_field_at(size_t index);
const char *ds_command_result_field_kind_name(DsCommandResultFieldKind kind);

#endif
