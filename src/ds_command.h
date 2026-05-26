#ifndef DS_COMMAND_H
#define DS_COMMAND_H

#include "ds_common.h"

typedef struct {
    DsStr text;
    DsSpan span;
} DsWord;

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

#endif
