#ifndef DS_VM_INTERNAL_H
#define DS_VM_INTERNAL_H

#include "backend.h"
#include "ds_stdlib.h"

static inline bool vm_i64_add_checked(int64_t lhs, int64_t rhs, int64_t *out) {
    if ((rhs > 0 && lhs > INT64_MAX - rhs) || (rhs < 0 && lhs < INT64_MIN - rhs)) return false;
    *out = lhs + rhs;
    return true;
}

static inline bool vm_i64_sub_checked(int64_t lhs, int64_t rhs, int64_t *out) {
    if ((rhs < 0 && lhs > INT64_MAX + rhs) || (rhs > 0 && lhs < INT64_MIN + rhs)) return false;
    *out = lhs - rhs;
    return true;
}

static inline bool vm_i64_mul_checked(int64_t lhs, int64_t rhs, int64_t *out) {
    if (lhs == 0 || rhs == 0) { *out = 0; return true; }
    if ((lhs == -1 && rhs == INT64_MIN) || (rhs == -1 && lhs == INT64_MIN)) return false;
    if (lhs > 0) {
        if (rhs > 0) { if (lhs > INT64_MAX / rhs) return false; }
        else if (rhs < INT64_MIN / lhs) return false;
    } else {
        if (rhs > 0) { if (lhs < INT64_MIN / rhs) return false; }
        else if (lhs < INT64_MAX / rhs) return false;
    }
    *out = lhs * rhs;
    return true;
}

static inline bool vm_i64_pow_checked(int64_t base, int64_t exp, int64_t *out) {
    if (exp < 0) return false;
    int64_t result = 1;
    int64_t factor = base;
    while (exp > 0) {
        if ((exp & 1) && !vm_i64_mul_checked(result, factor, &result)) return false;
        exp >>= 1;
        if (exp > 0 && !vm_i64_mul_checked(factor, factor, &factor)) return false;
    }
    *out = result;
    return true;
}

static inline bool vm_ascii_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}

static inline void vm_ascii_trim_bounds(const char *data, size_t len, size_t *start, size_t *end) {
    size_t a = 0, b = len;
    while (a < b && vm_ascii_space(data[a])) a++;
    while (b > a && vm_ascii_space(data[b - 1])) b--;
    *start = a;
    *end = b;
}

typedef enum {
    VM_READ_STREAM_OK,
    VM_READ_STREAM_IO_ERROR,
    VM_READ_STREAM_EMBEDDED_NUL,
} VmReadStreamStatus;

static inline VmReadStreamStatus vm_read_stream(FILE *fp, DsString *out, bool rewind_first,
                                                bool reject_nul) {
    ds_string_init(out);
    if (rewind_first && (fflush(fp) != 0 || fseek(fp, 0, SEEK_SET) != 0)) {
        return VM_READ_STREAM_IO_ERROR;
    }
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) {
        if (reject_nul && memchr(buf, '\0', n)) return VM_READ_STREAM_EMBEDDED_NUL;
        ds_string_append_range(out, buf, n);
    }
    return ferror(fp) ? VM_READ_STREAM_IO_ERROR : VM_READ_STREAM_OK;
}

typedef enum {
    OP_CMP_ADD,
    OP_CMP_SUB,
    OP_CMP_MUL,
    OP_CMP_DIV,
    OP_CMP_MOD,
    OP_CMP_POW,
    OP_CMP_EQ_EQ,
    OP_CMP_NE,
    OP_CMP_LT,
    OP_CMP_LE,
    OP_CMP_GT,
    OP_CMP_GE,
    OP_CMP_EQ_EQ_EQ,
    OP_CMP_NE_EQ,
} OpCmp;

#define DS_VM_OPCODE_LIST(X) \
    X(LOAD_CONST) X(LOAD_VAR) X(STORE_VAR) X(SET_ENV) X(NOT) X(BINARY) X(COMPARE) \
    X(MEMBERSHIP) X(REGEX_MATCH) X(INTERPOLATE) X(INTERP_JOIN) X(RUN_CAPTURE) X(GET_FIELD) \
    X(JUMP) X(JUMP_POP) X(JUMP_IF_FALSE) X(PUSH_SCOPE) X(POP_SCOPE) X(RUN_CMD) X(CALL) \
    X(STDLIB_CALL) X(ARRAY_LITERAL) X(MAP_LITERAL) X(GET_INDEX) X(SET_INDEX) X(PUSH_ARRAY) \
    X(FOR_ARRAY) X(FOR_MAP) X(FOR_RANGE) X(RESET_FOR) X(ASSERT) X(RETURN_VALUE) X(RETURN_FUNC) \
    X(REGISTER_HANDLER) X(END_HANDLER) X(RETURN) X(NOP)

typedef enum {
#define DS_VM_OPCODE_ENUM(name) OP_##name,
    DS_VM_OPCODE_LIST(DS_VM_OPCODE_ENUM)
#undef DS_VM_OPCODE_ENUM
} OpCode;

typedef struct {
    size_t start;
    size_t *breaks;
    size_t break_len;
    size_t break_cap;
    size_t *continues;
    size_t continue_len;
    size_t continue_cap;
    int base_scope_depth;
} LoopPatch;

typedef struct {
    char *name;
    DsValue default_value;
    bool has_default;
    DsLowerValueKind expected_kind;
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
    OpCmp cmp_enum;
    char *cmp;
    char *field;
    int *args;
    size_t arg_count;
    DsStr *words;
    size_t word_count;
    size_t *stage_word_counts;
    size_t stage_count;
    DsRedirect redirect;
    size_t loop_index;
    int64_t loop_current;
    bool loop_active;
    bool regex_case_insensitive;
    char *value_name;
    DsStr *loop_keys;
    size_t loop_key_count;
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
    int scope_depth;
    LoopPatch *loop_stack;
    size_t loop_len;
    size_t loop_cap;
} Program;

typedef struct VmScope VmScope;
struct VmScope {
    DsMap vars;
    VmScope *parent;
};

typedef struct {
    DsHandlerSignal signal;
    size_t target;
    bool is_trap;
} VmHandler;

typedef struct {
    size_t ip;
    int dst;
    VmScope *scope;
} VmReturnFrame;

typedef struct {
    Program *program;
    DsValue *regs;
    VmScope *scope;
    DsDiag *diag;
    const DsSource *source;
    DsVmOptions options;
    bool test_done;
    VmReturnFrame *returns;
    size_t return_len;
    size_t return_cap;
    VmHandler *handlers;
    size_t handler_len;
    size_t handler_cap;
    bool cleanup_running;
    bool control_exit_requested;
    int interrupted_signal;
} Vm;

void program_free(Program *p);
bool compile_program(const DsLowerProgram *lowered, Program *p, DsDiag *diag);
bool decode_string_text(DsStr text, DsString *out);

const char *op_name(OpCode op);
const char *op_cmp_name(OpCmp op);
OpCmp op_cmp_from_str(const char *text, size_t len);
const char *span_path(const DsSource *fallback, DsSpan span);
void trace_vm_instr(Vm *vm, size_t ip, const Instr *ins);

int bind_script_args(Vm *vm, const DsLowerProgram *program, int argc, char **argv);

VmScope *scope_new(VmScope *parent);
void scope_free_chain(VmScope *scope);
bool vm_push_scope(Vm *vm);
void vm_pop_scope(Vm *vm);
bool vm_pop_return(Vm *vm, VmReturnFrame *out);
void vm_pop_to_scope(Vm *vm, VmScope *target);
bool call_function(Vm *vm, Instr *ins, size_t next_ip, size_t *target_ip);
bool lookup_var(Vm *vm, const char *name, DsValue *out, DsSpan span);
DsValue *lookup_var_ref(Vm *vm, const char *name);

bool interpolate_string(Vm *vm, const DsString *input, DsString *out, DsSpan span);
int run_command(Vm *vm, Instr *ins);
int run_capture(Vm *vm, Instr *ins, DsValue *out_value);
bool vm_command_result_field(Vm *vm, const DsValue *value, const char *field, DsSpan span, DsValue *out);

int vm_take_pending_signal(void);
void vm_note_interrupted_signal(Vm *vm, int sig);

bool ds_vm_stdlib_call(Vm *vm, Instr *ins, DsValue *out);

#endif
