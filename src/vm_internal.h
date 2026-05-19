#ifndef DS_VM_INTERNAL_H
#define DS_VM_INTERNAL_H

#include "ds.h"

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
    size_t *return_ips;
    size_t return_len;
    size_t return_cap;
} Vm;

bool ds_vm_stdlib_call(Vm *vm, Instr *ins, DsValue *out);

#endif