#include "vm_internal.h"

#include <string.h>

/*
 * VM command-result field ownership boundary.
 *
 * Lowering owns source-language field legality and VM/Bash parity gates.
 * This file only materializes accepted command-result fields at runtime and
 * preserves map field lookup behavior for accepted HIR. Unknown fields here are
 * internal VM invariants after lowering, not semantic diagnostics.
 */
bool vm_command_result_field(Vm *vm, const DsValue *value, const char *field, DsSpan span, DsValue *out) {
    if (value->kind == DS_VALUE_MAP) {
        DsStr key = {(char *)field, strlen(field)};
        DsValue *found = ds_map_get((DsMap *)&value->as.map, key);
        if (!found) {
            ds_diag_error(vm->diag, span, "missing map key `%s`", field);
            return false;
        }
        *out = ds_value_copy(found);
        return true;
    }
    if (value->kind != DS_VALUE_COMMAND_RESULT) {
        ds_diag_error(vm->diag, span, "internal VM field invariant failed: field receiver should be a command result or map after lowering");
        return false;
    }

    DsStr field_view = {(char *)field, strlen(field)};
    const DsCommandResultField *desc = ds_command_result_field_lookup(field_view);
    if (desc && desc->kind == DS_COMMAND_RESULT_FIELD_STRING && strcmp(desc->name, "stdout") == 0) {
        ds_string_from_range(&out->as.string, value->as.command_result.stdout_text.data ? value->as.command_result.stdout_text.data : "", value->as.command_result.stdout_text.len);
        out->kind = DS_VALUE_STRING;
        return true;
    }
    if (desc && desc->kind == DS_COMMAND_RESULT_FIELD_STRING && strcmp(desc->name, "stderr") == 0) {
        ds_string_from_range(&out->as.string, value->as.command_result.stderr_text.data ? value->as.command_result.stderr_text.data : "", value->as.command_result.stderr_text.len);
        out->kind = DS_VALUE_STRING;
        return true;
    }
    if (desc && desc->kind == DS_COMMAND_RESULT_FIELD_INT) {
        *out = ds_value_int(value->as.command_result.code);
        return true;
    }
    if (desc && desc->kind == DS_COMMAND_RESULT_FIELD_BOOL && strcmp(desc->name, "ok") == 0) {
        *out = ds_value_bool(value->as.command_result.code == 0);
        return true;
    }
    if (desc && desc->kind == DS_COMMAND_RESULT_FIELD_BOOL && strcmp(desc->name, "failed") == 0) {
        *out = ds_value_bool(value->as.command_result.code != 0);
        return true;
    }

    ds_diag_error(vm->diag, span, "internal VM field invariant failed: unknown command result field `%s` after lowering", field);
    return false;
}
