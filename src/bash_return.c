#include "bash_internal.h"
#include "ds_command_pipeline.h"

#include <string.h>

static bool emit_command_result_return(BashEmitter *e, const DsLowerExpr *value, DsSpan span, int indent) {
    if (value->kind == DS_LOWER_EXPR_RUN) {
        if (ds_command_is_pipeline(&value->as.run)) {
            DsStr ret_name = {"return", strlen("return")};
            return bash_emit_capture_pipeline_assignment(e, ret_name, &value->as.run, value->span, indent);
        }
        emit_indent(&e->out, indent);
        buf_append(&e->out, "__ds_capture __ds_return");
        if (!emit_capture_command(e, &value->as.run, &e->out, value->span)) return false;
        buf_append(&e->out, "\n");
        return true;
    }
    if (value->kind == DS_LOWER_EXPR_IDENT) {
        bash_emit_command_result_copy_to_return(e, value->as.text, indent);
        return true;
    }
    ds_diag_error(e->diag, span, "internal Bash invariant failed: command-result return should be run, named, or forwarded after lowering");
    return false;
}

bool bash_emit_return_stmt(BashEmitter *e, const DsLowerStmt *stmt, int indent) {
    const DsLowerExpr *value = stmt->as.return_stmt.value;
    DsLowerValueKind kind = stmt->as.return_stmt.return_kind;

    bash_emit_return_type(e, kind, indent);
    emit_indent(&e->out, indent);

    if (value->kind == DS_LOWER_EXPR_CALL && value->as.call.is_user_function) {
        if (!bash_emit_user_call_capture_return(e, value, kind, indent)) return false;
        bash_emit_return_type(e, kind, indent);
    } else if (kind == DS_LOWER_VALUE_ARRAY) {
        if (!bash_emit_array_return_payload(e, value, stmt->span, indent)) return false;
    } else if (kind == DS_LOWER_VALUE_MAP) {
        if (!bash_emit_map_return_payload(e, value, stmt->span, indent)) return false;
    } else if (kind == DS_LOWER_VALUE_COMMAND_RESULT) {
        if (!emit_command_result_return(e, value, stmt->span, indent)) return false;
    } else {
        buf_append(&e->out, "__ds_return_value=");
        if (!emit_value_expr(e, value, &e->out)) return false;
        buf_append(&e->out, " || return $?\n");
    }

    emit_indent(&e->out, indent);
    buf_append(&e->out, "return 0\n\n");
    return true;
}
