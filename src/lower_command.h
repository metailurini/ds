#ifndef DS_LOWER_COMMAND_H
#define DS_LOWER_COMMAND_H

#include "lower_context.h"

bool lower_validate_command_word(Lower *lower, DsStr word, DsSpan span);
bool lower_validate_word_interpolation(Lower *lower, DsStr text, DsSpan span);
void lower_command_to_hir(Lower *lower, const DsCommand *command, DsLowerCommand *out);
bool lower_materialize_command_value_call_interpolation(Lower *lower, DsCommand *command,
                                                        DsLowerStmt *block);

#endif
