#include "ds_command.h"

#include <stdlib.h>
#include <string.h>

static DsStr ds_str_clone_view(DsStr s) {
    DsStr out = {ds_str_dup_range(s.data ? s.data : "", s.len), s.len};
    return out;
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
    dst->items = (DsWord *)ds_xcalloc(dst->cap ? dst->cap : 1, sizeof(DsWord));
    for (size_t i = 0; i < src->len; i++) {
        dst->items[i].text = ds_str_clone_view(src->items[i].text);
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
    dst->items = (DsCommandStage *)ds_xcalloc(dst->cap ? dst->cap : 1, sizeof(DsCommandStage));
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
    if (src->target.len > 0) dst->target = ds_str_clone_view(src->target);
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

bool ds_command_name_char(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
}

DsCommandWordForm ds_command_word_analyze(DsStr word) {
    DsCommandWordForm form = {DS_COMMAND_WORD_LITERAL, {0}, {0}};
    if (word.len >= 2 && word.data[0] == '"' && word.data[word.len - 1] == '"') {
        form.kind = DS_COMMAND_WORD_QUOTED;
        return form;
    }
    if (word.len >= 2 && word.data[0] == '$') {
        size_t name_len = 0;
        while (1 + name_len < word.len && ds_command_name_char(word.data[1 + name_len])) name_len++;
        form.kind = DS_COMMAND_WORD_VARIABLE;
        form.name = (DsStr){word.data + 1, name_len};
        if (name_len + 1 < word.len && word.data[name_len + 1] == '.') {
            form.kind = DS_COMMAND_WORD_FIELD;
            form.field = (DsStr){word.data + name_len + 2, word.len - name_len - 2};
        }
        return form;
    }
    for (size_t i = 1; i < word.len; i++) {
        if (word.data[i] == '.') {
            form.kind = DS_COMMAND_WORD_FIELD;
            form.name = (DsStr){word.data, i};
            form.field = (DsStr){word.data + i + 1, word.len - i - 1};
            return form;
        }
    }
    return form;
}

bool ds_command_word_contains_direct_call_interpolation(DsStr decoded) {
    for (size_t i = 0; i < decoded.len; i++) {
        if (decoded.data[i] != '{') continue;
        size_t j = i + 1;
        while (j < decoded.len && (decoded.data[j] == ' ' || decoded.data[j] == '\t')) j++;
        if (j >= decoded.len || !((decoded.data[j] >= 'A' && decoded.data[j] <= 'Z') ||
                                  (decoded.data[j] >= 'a' && decoded.data[j] <= 'z') ||
                                  decoded.data[j] == '_')) continue;
        j++;
        while (j < decoded.len && (ds_command_name_char(decoded.data[j]) || decoded.data[j] == '.')) j++;
        while (j < decoded.len && (decoded.data[j] == ' ' || decoded.data[j] == '\t')) j++;
        if (j < decoded.len && decoded.data[j] == '(') return true;
    }
    return false;
}

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

const char *ds_command_result_field_kind_name(DsCommandResultFieldKind kind) {
    switch (kind) {
        case DS_COMMAND_RESULT_FIELD_STRING: return "string";
        case DS_COMMAND_RESULT_FIELD_INT: return "int";
        case DS_COMMAND_RESULT_FIELD_BOOL: return "bool";
    }
    return "unknown";
}
