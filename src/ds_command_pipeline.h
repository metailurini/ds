#ifndef DS_COMMAND_PIPELINE_H
#define DS_COMMAND_PIPELINE_H

#include "ds_command.h"

size_t ds_command_stage_count(const DsCommand *command);
bool ds_command_is_pipeline(const DsCommand *command);
int ds_command_pipeline_status(const int *stage_codes, size_t stage_count);

#endif
