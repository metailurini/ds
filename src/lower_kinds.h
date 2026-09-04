#ifndef DS_LOWER_KINDS_H
#define DS_LOWER_KINDS_H

#include "lower_symbols.h"
#include "ds_stdlib.h"

bool lower_sym_kind_is_scalar(SymKind kind);
bool lower_value_kind_is_scalar(DsLowerValueKind kind);
DsLowerValueKind lower_value_kind_from_sym(SymKind kind);
SymKind sym_kind_from_lower_value_kind(DsLowerValueKind kind);
DsLowerValueKind lower_fn_param_expected_kind(const DsLowerFnParam *param);
DsLowerValueKind lower_stdlib_return_value_kind(const DsStdlibHelper *helper);
bool stdlib_return_kind(const DsStdlibHelper *helper, SymKind *kind);
bool command_result_field_kind(DsStr field, SymKind *kind_out);

#endif
