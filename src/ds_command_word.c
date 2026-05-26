#include "ds_command_word.h"

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
