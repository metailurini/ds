#ifndef DS_VM_INTERNAL_H
#define DS_VM_INTERNAL_H

#include "backend.h"
#include "ds_stdlib.h"

#include <stdbool.h>
#include <stddef.h>

typedef enum {
    OP_LOAD_CONST,
    OP_LOAD_VAR,
    OP_STORE_VAR,
    OP_NOT,
    OP_COMPARE,
    OP_INTERPOLATE,
    OP_RUN_CAPTURE,
    OP_GET_FIELD,
    OP_JUMP,
    OP_JUMP_IF_FALSE,
    OP_PUSH_SCOPE,
    OP_POP_SCOPE,
    OP_RUN_CMD,
    OP_CALL,
    OP_STDLIB_CALL,
    OP_ARRAY_LITERAL,
    OP_MAP_LITERAL,
    OP_GET_INDEX,
    OP_PUSH_ARRAY,
    OP_FOR_ARRAY,
    OP_ASSERT,
    OP_RETURN_FUNC,
    OP_RETURN,
    OP_NOP
} OpCode;

typedef struct {
    char *name;
    DsValue default_value;
    bool has_default;
} FnParamMeta;

typedef struct {
    char *name;
    FnParamMeta *params;
    size_t param_count;
    size_t required_count;
    size_t target;
} FnMeta;

typedef struct {
    OpCode op;
    DsSpan span;
    int dst;
    int a;
    int b;
    int target;
    char *name;
    char *cmp;
    char *field;
    int *args;
    size_t arg_count;
    DsStr *words;
    size_t word_count;
    DsRedirect redirect;
    size_t loop_index;
    bool loop_active;
} Instr;

typedef struct {
    DsValue *consts;
    size_t const_len;
    size_t const_cap;
    Instr *instrs;
    size_t instr_len;
    size_t instr_cap;
    int next_reg;
    FnMeta *functions;
    size_t function_len;
    size_t function_cap;
} Program;

typedef struct VmScope VmScope;
struct VmScope {
    DsMap vars;
    VmScope *parent;
};

typedef struct {
    Program *program;
    DsValue *regs;
    VmScope *scope;
    DsDiag *diag;
    const DsSource *source;
    DsVmOptions options;
    bool test_done;
    size_t *return_ips;
    size_t return_len;
    size_t return_cap;
} Vm;

void program_free(Program *p);
bool compile_program(const DsLowerProgram *lowered, Program *p, DsDiag *diag);
bool decode_string_text(DsStr text, DsString *out);

const char *op_name(OpCode op);
const char *span_path(const DsSource *fallback, DsSpan span);
void trace_vm_instr(Vm *vm, size_t ip, const Instr *ins);

int bind_script_args(Vm *vm, const DsLowerProgram *program, int argc, char **argv);

VmScope *scope_new(VmScope *parent);
void scope_free_chain(VmScope *scope);
void vm_push_scope(Vm *vm);
void vm_pop_scope(Vm *vm);
bool vm_pop_return(Vm *vm, size_t *out);
bool call_function(Vm *vm, Instr *ins, size_t next_ip, size_t *target_ip);
bool lookup_var(Vm *vm, const char *name, DsValue *out, DsSpan span);
DsValue *lookup_var_ref(Vm *vm, const char *name);

bool interpolate_string(Vm *vm, const DsString *input, DsString *out, DsSpan span);
int run_command(Vm *vm, Instr *ins);
int run_capture(Vm *vm, Instr *ins, DsValue *out_value);
bool command_result_field(Vm *vm, const DsValue *value, const char *field, DsSpan span, DsValue *out);

bool ds_vm_stdlib_call(Vm *vm, Instr *ins, DsValue *out);

#endif