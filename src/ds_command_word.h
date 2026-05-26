#ifndef DS_COMMAND_WORD_H
#define DS_COMMAND_WORD_H

#include "ds_common.h"

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
bool ds_command_word_contains_direct_call_interpolation(DsStr decoded);

#endif
