#include "lower_kinds.h"

#include "ds_command_facts.h"

bool lower_sym_kind_is_scalar(SymKind kind) {
    return kind == SYM_STRING || kind == SYM_INT || kind == SYM_BOOL;
}

bool lower_value_kind_is_scalar(DsLowerValueKind kind) {
    return kind == DS_LOWER_VALUE_STRING || kind == DS_LOWER_VALUE_INT ||
           kind == DS_LOWER_VALUE_BOOL;
}

DsLowerValueKind lower_value_kind_from_sym(SymKind kind) {
    switch (kind) {
        case SYM_BOOL: return DS_LOWER_VALUE_BOOL;
        case SYM_INT: return DS_LOWER_VALUE_INT;
        case SYM_STRING: return DS_LOWER_VALUE_STRING;
        case SYM_COMMAND_RESULT: return DS_LOWER_VALUE_COMMAND_RESULT;
        case SYM_ARRAY: return DS_LOWER_VALUE_ARRAY;
        case SYM_MAP: return DS_LOWER_VALUE_MAP;
        case SYM_FUNCTION:
        case SYM_TOPLEVEL_PREDECLARED:
        case SYM_UNKNOWN:
            return DS_LOWER_VALUE_UNKNOWN;
    }
    return DS_LOWER_VALUE_UNKNOWN;
}

SymKind sym_kind_from_lower_value_kind(DsLowerValueKind kind) {
    switch (kind) {
        case DS_LOWER_VALUE_BOOL: return SYM_BOOL;
        case DS_LOWER_VALUE_INT: return SYM_INT;
        case DS_LOWER_VALUE_STRING: return SYM_STRING;
        case DS_LOWER_VALUE_COMMAND_RESULT: return SYM_COMMAND_RESULT;
        case DS_LOWER_VALUE_ARRAY: return SYM_ARRAY;
        case DS_LOWER_VALUE_MAP: return SYM_MAP;
        case DS_LOWER_VALUE_UNKNOWN: return SYM_UNKNOWN;
    }
    return SYM_UNKNOWN;
}

DsLowerValueKind lower_fn_param_expected_kind(const DsLowerFnParam *param) {
    if (!param) return DS_LOWER_VALUE_UNKNOWN;
    return param->has_default ? param->default_kind : param->inferred_kind;
}

DsLowerValueKind lower_stdlib_return_value_kind(const DsStdlibHelper *helper) {
    if (!helper) return DS_LOWER_VALUE_UNKNOWN;
    switch (helper->return_kind) {
        case DS_STDLIB_RETURN_BOOL: return DS_LOWER_VALUE_BOOL;
        case DS_STDLIB_RETURN_INT: return DS_LOWER_VALUE_INT;
        case DS_STDLIB_RETURN_STRING: return DS_LOWER_VALUE_STRING;
        case DS_STDLIB_RETURN_ARRAY: return DS_LOWER_VALUE_ARRAY;
        case DS_STDLIB_RETURN_MAP: return DS_LOWER_VALUE_MAP;
        case DS_STDLIB_RETURN_COMMAND_RESULT: return DS_LOWER_VALUE_COMMAND_RESULT;
        case DS_STDLIB_RETURN_STATEMENT_ONLY: return DS_LOWER_VALUE_UNKNOWN;
    }
    return DS_LOWER_VALUE_UNKNOWN;
}

bool stdlib_return_kind(const DsStdlibHelper *helper, SymKind *kind) {
    if (!helper || !kind) return false;
    *kind = sym_kind_from_lower_value_kind(lower_stdlib_return_value_kind(helper));
    return true;
}

bool command_result_field_kind(DsStr field, SymKind *kind_out) {
    if (!kind_out) return false;
    const DsCommandResultField *desc = ds_command_result_field_lookup(field);
    if (!desc) return false;
    switch (desc->kind) {
        case DS_COMMAND_RESULT_FIELD_STRING: *kind_out = SYM_STRING; return true;
        case DS_COMMAND_RESULT_FIELD_INT: *kind_out = SYM_INT; return true;
        case DS_COMMAND_RESULT_FIELD_BOOL: *kind_out = SYM_BOOL; return true;
    }
    return false;
}
