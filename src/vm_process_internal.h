#ifndef DS_VM_PROCESS_INTERNAL_H
#define DS_VM_PROCESS_INTERNAL_H

#include "vm_internal.h"

typedef struct {
    char **items;
    size_t len;
} VmArgv;

typedef struct {
    DsString stdout_text;
    DsString stderr_text;
    int code;
    bool terminated_by_sigpipe;
    bool has_non_sigpipe_failure;
} VmProcessResult;

typedef struct {
    VmArgv argv;
    DsRedirect redirect;
    char *redirect_path;
    DsSpan span;
    bool capture;
    int exec_error_fd;
} VmProcessSpec;

bool vm_process_spec_from_instr(Vm *vm, Instr *ins, bool capture, VmProcessSpec *spec);
bool vm_process_spec_from_stage(Vm *vm, Instr *ins, size_t stage_index, bool capture,
                                VmProcessSpec *spec);
bool vm_process_redirect_path_from_instr(Vm *vm, Instr *ins, char **out);
void vm_process_spec_free(VmProcessSpec *spec);
void vm_process_trace_spec(Vm *vm, const VmProcessSpec *spec);
bool vm_process_run_control_command(Vm *vm, const VmProcessSpec *spec, int *out_code);

#endif
