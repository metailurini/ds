#include "ds_command_pipeline.h"

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
