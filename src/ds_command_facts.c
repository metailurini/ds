#include "ds_command_facts.h"

bool ds_command_name_char(char c) {
    return ds_is_ident_continue(c);
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
        if (i + 1 < decoded.len && decoded.data[i + 1] == '{') { i++; continue; }
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
    for (size_t i = 0; i < DS_ARRAY_LEN(k_fields); i++) {
        size_t len = strlen(k_fields[i].name);
        if (field.len == len && memcmp(field.data, k_fields[i].name, len) == 0) return &k_fields[i];
    }
    return NULL;
}

size_t ds_command_result_field_count(void) {
    return DS_ARRAY_LEN(k_fields);
}

const DsCommandResultField *ds_command_result_field_at(size_t index) {
    if (index >= ds_command_result_field_count()) return NULL;
    return &k_fields[index];
}

const char *ds_command_result_field_kind_name(DsCommandResultFieldKind kind) {
    static const char *const names[] = {"string", "int", "bool"};
    return (unsigned)kind < DS_ARRAY_LEN(names) ? names[kind] : "unknown";
}

size_t ds_command_stage_count(const DsCommand *command) {
    return command ? command->stages.len : 0;
}

bool ds_command_is_pipeline(const DsCommand *command) {
    return ds_command_stage_count(command) > 1;
}

int ds_command_pipeline_status(const int *stage_codes, size_t stage_count) {
    /*
     * VM/Bash parity contract: pipeline status follows Bash pipefail semantics,
     * where the pipeline status is the rightmost non-zero stage status, or zero
     * when every stage succeeds. Bash uses `set -o pipefail`; the VM consumes
     * this shared helper so future pipeline status changes have one C owner.
     */
    int code = 0;
    if (!stage_codes) return code;
    for (size_t i = 0; i < stage_count; i++) if (stage_codes[i] != 0) code = stage_codes[i];
    return code;
}
