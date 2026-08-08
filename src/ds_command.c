#include "ds_command.h"

#include <stdlib.h>
#include <string.h>

const char *ds_redirect_source_op(DsRedirectKind kind) {
    static const char *const ops[] = {"", "|>", "|>>", "!>", "!>>", "&>", "&>>"};
    return (unsigned)kind < DS_ARRAY_LEN(ops) ? ops[kind] : "";
}

const char *ds_redirect_shell_op(DsRedirectKind kind) {
    static const char *const ops[] = {NULL, ">", ">>", "2>", "2>>", "&>", "&>>"};
    return (unsigned)kind < DS_ARRAY_LEN(ops) ? ops[kind] : NULL;
}

void ds_word_vec_init(DsWordVec *vec) {
    vec->items = NULL;
    vec->len = 0;
    vec->cap = 0;
}

bool ds_word_vec_clone(DsWordVec *dst, const DsWordVec *src) {
    ds_word_vec_init(dst);
    dst->len = src->len;
    dst->cap = src->len;
    dst->items = (DsWord *)ds_xcalloc(dst->cap, sizeof(DsWord));
    for (size_t i = 0; i < src->len; i++) {
        dst->items[i].text = ds_str_clone(src->items[i].text);
        dst->items[i].span = src->items[i].span;
    }
    return true;
}

void ds_word_vec_free(DsWordVec *vec) {
    if (!vec) return;
    for (size_t i = 0; i < vec->len; i++) free(vec->items[i].text.data);
    free(vec->items);
    ds_word_vec_init(vec);
}

void ds_command_stage_init(DsCommandStage *stage) {
    memset(stage, 0, sizeof(*stage));
    ds_word_vec_init(&stage->words);
}

bool ds_command_stage_clone(DsCommandStage *dst, const DsCommandStage *src) {
    ds_command_stage_init(dst);
    dst->span = src->span;
    ds_word_vec_clone(&dst->words, &src->words);
    return true;
}

void ds_command_stage_free(DsCommandStage *stage) {
    if (!stage) return;
    ds_word_vec_free(&stage->words);
    memset(&stage->span, 0, sizeof(stage->span));
}

void ds_command_stage_vec_init(DsCommandStageVec *vec) {
    vec->items = NULL;
    vec->len = 0;
    vec->cap = 0;
}

bool ds_command_stage_vec_clone(DsCommandStageVec *dst, const DsCommandStageVec *src) {
    ds_command_stage_vec_init(dst);
    dst->len = src->len;
    dst->cap = src->len;
    dst->items = (DsCommandStage *)ds_xcalloc(dst->cap, sizeof(DsCommandStage));
    for (size_t i = 0; i < src->len; i++) ds_command_stage_clone(&dst->items[i], &src->items[i]);
    return true;
}

void ds_command_stage_vec_free(DsCommandStageVec *vec) {
    if (!vec) return;
    for (size_t i = 0; i < vec->len; i++) ds_command_stage_free(&vec->items[i]);
    free(vec->items);
    ds_command_stage_vec_init(vec);
}

void ds_redirect_init(DsRedirect *redirect) {
    memset(redirect, 0, sizeof(*redirect));
    redirect->kind = DS_REDIRECT_NONE;
}

bool ds_redirect_clone(DsRedirect *dst, const DsRedirect *src) {
    ds_redirect_init(dst);
    dst->kind = src->kind;
    dst->op_span = src->op_span;
    dst->target_span = src->target_span;
    if (src->target.len > 0) dst->target = ds_str_clone(src->target);
    return true;
}

void ds_redirect_free(DsRedirect *redirect) {
    if (!redirect) return;
    free(redirect->target.data);
    ds_redirect_init(redirect);
}

void ds_command_init(DsCommand *command, DsCommandKind kind, DsSpan span) {
    memset(command, 0, sizeof(*command));
    command->kind = kind;
    command->span = span;
    ds_command_stage_vec_init(&command->stages);
    ds_redirect_init(&command->redirect);
}

bool ds_command_clone(DsCommand *dst, const DsCommand *src) {
    ds_command_init(dst, src->kind, src->span);
    ds_command_stage_vec_clone(&dst->stages, &src->stages);
    ds_redirect_clone(&dst->redirect, &src->redirect);
    return true;
}

void ds_command_free(DsCommand *command) {
    if (!command) return;
    ds_command_stage_vec_free(&command->stages);
    ds_redirect_free(&command->redirect);
    command->kind = DS_COMMAND_PLAIN;
    memset(&command->span, 0, sizeof(command->span));
}
