#include "ds.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

typedef enum {
    OP_LOAD_CONST,
    OP_LOAD_VAR,
    OP_STORE_VAR,
    OP_NOT,
    OP_COMPARE,
    OP_INTERPOLATE,
    OP_JUMP,
    OP_JUMP_IF_FALSE,
    OP_RUN_CMD,
    OP_RETURN,
    OP_NOP
} OpCode;

typedef struct {
    OpCode op;
    DsSpan span;
    int dst;
    int a;
    int b;
    int target;
    char *name;
    char *cmp;
    DsStr *words;
    size_t word_count;
} Instr;

typedef struct {
    DsValue *consts;
    size_t const_len;
    size_t const_cap;
    Instr *instrs;
    size_t instr_len;
    size_t instr_cap;
    int next_reg;
} Program;

static void program_init(Program *p) { memset(p, 0, sizeof(*p)); }

static void instr_free(Instr *ins) {
    free(ins->name);
    free(ins->cmp);
    for (size_t i = 0; i < ins->word_count; i++) free(ins->words[i].data);
    free(ins->words);
}

static void program_free(Program *p) {
    for (size_t i = 0; i < p->const_len; i++) ds_value_free(&p->consts[i]);
    for (size_t i = 0; i < p->instr_len; i++) instr_free(&p->instrs[i]);
    free(p->consts);
    free(p->instrs);
}

static int new_reg(Program *p) { return p->next_reg++; }

static int add_const(Program *p, DsValue value) {
    if (p->const_len == p->const_cap) {
        p->const_cap = p->const_cap ? p->const_cap * 2 : 16;
        p->consts = (DsValue *)ds_xrealloc(p->consts, p->const_cap * sizeof(DsValue));
    }
    p->consts[p->const_len] = value;
    return (int)p->const_len++;
}

static size_t emit_instr(Program *p, Instr ins) {
    if (p->instr_len == p->instr_cap) {
        p->instr_cap = p->instr_cap ? p->instr_cap * 2 : 32;
        p->instrs = (Instr *)ds_xrealloc(p->instrs, p->instr_cap * sizeof(Instr));
    }
    p->instrs[p->instr_len] = ins;
    return p->instr_len++;
}

static bool decode_string_text(DsStr text, DsString *out) {
    ds_string_init(out);
    if (text.len < 2 || text.data[0] != '"' || text.data[text.len - 1] != '"') return false;
    for (size_t i = 1; i + 1 < text.len; i++) {
        char c = text.data[i];
        if (c == '\\' && i + 1 < text.len - 1) {
            char escaped = text.data[++i];
            if (escaped == 'n') c = '\n';
            else if (escaped == 't') c = '\t';
            else if (escaped == '"') c = '"';
            else if (escaped == '\\') c = '\\';
            else c = escaped;
        }
        ds_string_append_char(out, c);
    }
    return true;
}

static int compile_expr(Program *p, const DsExpr *expr);

static int compile_string_expr(Program *p, const DsExpr *expr) {
    DsString decoded;
    decode_string_text(expr->as.text, &decoded);
    int c = add_const(p, ds_value_string_take(&decoded));
    int r = new_reg(p);
    Instr ins = {0};
    ins.op = OP_INTERPOLATE;
    ins.span = expr->span;
    ins.dst = r;
    ins.a = c;
    emit_instr(p, ins);
    return r;
}

static int compile_expr(Program *p, const DsExpr *expr) {
    switch (expr->kind) {
        case DS_EXPR_STRING:
            return compile_string_expr(p, expr);
        case DS_EXPR_INT: {
            char *tmp = ds_str_dup_range(expr->as.text.data, expr->as.text.len);
            int64_t value = strtoll(tmp, NULL, 10);
            free(tmp);
            int c = add_const(p, ds_value_int(value));
            int r = new_reg(p);
            Instr ins = {0};
            ins.op = OP_LOAD_CONST;
            ins.span = expr->span;
            ins.dst = r;
            ins.a = c;
            emit_instr(p, ins);
            return r;
        }
        case DS_EXPR_BOOL: {
            int c = add_const(p, ds_value_bool(expr->as.boolean));
            int r = new_reg(p);
            Instr ins = {0};
            ins.op = OP_LOAD_CONST;
            ins.span = expr->span;
            ins.dst = r;
            ins.a = c;
            emit_instr(p, ins);
            return r;
        }
        case DS_EXPR_IDENT: {
            int r = new_reg(p);
            Instr ins = {0};
            ins.op = OP_LOAD_VAR;
            ins.span = expr->span;
            ins.dst = r;
            ins.name = ds_str_dup_range(expr->as.text.data, expr->as.text.len);
            emit_instr(p, ins);
            return r;
        }
        case DS_EXPR_UNARY: {
            int right = compile_expr(p, expr->as.unary.right);
            int r = new_reg(p);
            Instr ins = {0};
            ins.op = OP_NOT;
            ins.span = expr->span;
            ins.dst = r;
            ins.a = right;
            emit_instr(p, ins);
            return r;
        }
        case DS_EXPR_BINARY: {
            int left = compile_expr(p, expr->as.binary.left);
            int right = compile_expr(p, expr->as.binary.right);
            int r = new_reg(p);
            Instr ins = {0};
            ins.op = OP_COMPARE;
            ins.span = expr->span;
            ins.dst = r;
            ins.a = left;
            ins.b = right;
            ins.cmp = ds_str_dup_range(expr->as.binary.op.data, expr->as.binary.op.len);
            emit_instr(p, ins);
            return r;
        }
        case DS_EXPR_ERROR:
            return new_reg(p);
    }
    return new_reg(p);
}

static void compile_stmt(Program *p, const DsStmt *stmt);

static void compile_block(Program *p, const DsStmt *block) {
    for (size_t i = 0; i < block->as.block_stmt.statements.len; i++) compile_stmt(p, block->as.block_stmt.statements.items[i]);
}

static void compile_stmt(Program *p, const DsStmt *stmt) {
    switch (stmt->kind) {
        case DS_STMT_LET: {
            int src = compile_expr(p, stmt->as.let_stmt.value);
            Instr ins = {0};
            ins.op = OP_STORE_VAR;
            ins.span = stmt->span;
            ins.a = src;
            ins.name = ds_str_dup_range(stmt->as.let_stmt.name.data, stmt->as.let_stmt.name.len);
            emit_instr(p, ins);
            break;
        }
        case DS_STMT_CMD: {
            Instr ins = {0};
            ins.op = OP_RUN_CMD;
            ins.span = stmt->span;
            ins.word_count = stmt->as.cmd_stmt.words.len;
            ins.words = (DsStr *)ds_xcalloc(ins.word_count ? ins.word_count : 1, sizeof(DsStr));
            for (size_t i = 0; i < ins.word_count; i++) {
                DsStr w = stmt->as.cmd_stmt.words.items[i];
                ins.words[i].data = ds_str_dup_range(w.data, w.len);
                ins.words[i].len = w.len;
            }
            emit_instr(p, ins);
            break;
        }
        case DS_STMT_IF: {
            int cond = compile_expr(p, stmt->as.if_stmt.condition);
            Instr jif = {0};
            jif.op = OP_JUMP_IF_FALSE;
            jif.span = stmt->as.if_stmt.condition->span;
            jif.a = cond;
            size_t jif_pos = emit_instr(p, jif);
            compile_block(p, stmt->as.if_stmt.then_branch);
            if (stmt->as.if_stmt.else_branch) {
                Instr jump = {0};
                jump.op = OP_JUMP;
                jump.span = stmt->span;
                size_t jump_pos = emit_instr(p, jump);
                p->instrs[jif_pos].target = (int)p->instr_len;
                compile_block(p, stmt->as.if_stmt.else_branch);
                p->instrs[jump_pos].target = (int)p->instr_len;
            } else {
                p->instrs[jif_pos].target = (int)p->instr_len;
            }
            break;
        }
        case DS_STMT_BLOCK:
            compile_block(p, stmt);
            break;
    }
}

static bool compile_program(const DsAst *ast, Program *p, DsDiag *diag) {
    (void)diag;
    program_init(p);
    for (size_t i = 0; i < ast->statements.len; i++) compile_stmt(p, ast->statements.items[i]);
    Instr ret = {0};
    ret.op = OP_RETURN;
    ret.target = 0;
    ret.span = ast->span;
    emit_instr(p, ret);
    return true;
}

static const char *op_name(OpCode op) {
    switch (op) {
        case OP_LOAD_CONST: return "LOAD_CONST";
        case OP_LOAD_VAR: return "LOAD_VAR";
        case OP_STORE_VAR: return "STORE_VAR";
        case OP_NOT: return "NOT";
        case OP_COMPARE: return "COMPARE";
        case OP_INTERPOLATE: return "INTERPOLATE";
        case OP_JUMP: return "JUMP";
        case OP_JUMP_IF_FALSE: return "JUMP_IF_FALSE";
        case OP_RUN_CMD: return "RUN_CMD";
        case OP_RETURN: return "RETURN";
        case OP_NOP: return "NOP";
    }
    return "NOP";
}

static void print_escaped(FILE *out, const char *data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        char c = data[i];
        if (c == '\\') fputs("\\\\", out);
        else if (c == '"') fputs("\\\"", out);
        else if (c == '\n') fputs("\\n", out);
        else if (c == '\t') fputs("\\t", out);
        else fputc(c, out);
    }
}

bool ds_bytecode_dump(const DsSource *source, const DsAst *ast, FILE *out, DsDiag *diag) {
    if (!ds_lower_validate(ast, diag)) return false;
    Program p;
    if (!compile_program(ast, &p, diag)) return false;

    fputs("constants:\n", out);
    for (size_t i = 0; i < p.const_len; i++) {
        fprintf(out, "  %zu: ", i);
        DsValue *v = &p.consts[i];
        switch (v->kind) {
            case DS_VALUE_NULL: fputs("null", out); break;
            case DS_VALUE_BOOL: fprintf(out, "bool %s", v->as.boolean ? "true" : "false"); break;
            case DS_VALUE_INT: fprintf(out, "int %lld", (long long)v->as.integer); break;
            case DS_VALUE_STRING:
                fputs("string \"", out);
                print_escaped(out, v->as.string.data ? v->as.string.data : "", v->as.string.len);
                fputc('"', out);
                break;
        }
        fputc('\n', out);
    }
    fputs("\ninstructions:\n", out);
    for (size_t i = 0; i < p.instr_len; i++) {
        Instr *ins = &p.instrs[i];
        fprintf(out, "  %04zu %-14s", i, op_name(ins->op));
        switch (ins->op) {
            case OP_LOAD_CONST: fprintf(out, " r%d, const %d", ins->dst, ins->a); break;
            case OP_LOAD_VAR: fprintf(out, " r%d, %s", ins->dst, ins->name); break;
            case OP_STORE_VAR: fprintf(out, " %s, r%d", ins->name, ins->a); break;
            case OP_NOT: fprintf(out, " r%d, r%d", ins->dst, ins->a); break;
            case OP_COMPARE: fprintf(out, " r%d, r%d %s r%d", ins->dst, ins->a, ins->cmp, ins->b); break;
            case OP_INTERPOLATE: fprintf(out, " r%d, const %d", ins->dst, ins->a); break;
            case OP_JUMP: fprintf(out, " %d", ins->target); break;
            case OP_JUMP_IF_FALSE: fprintf(out, " r%d, %d", ins->a, ins->target); break;
            case OP_RUN_CMD:
                fputs(" [", out);
                for (size_t j = 0; j < ins->word_count; j++) {
                    if (j) fputs(", ", out);
                    fputc('"', out);
                    print_escaped(out, ins->words[j].data, ins->words[j].len);
                    fputc('"', out);
                }
                fputc(']', out);
                break;
            case OP_RETURN: fprintf(out, " %d", ins->target); break;
            case OP_NOP: break;
        }
        fprintf(out, "    # %s:%d:%d\n", source && source->path ? source->path : "<source>", ins->span.start.line, ins->span.start.column);
    }
    program_free(&p);
    return true;
}

typedef struct {
    Program *program;
    DsValue *regs;
    DsMap vars;
    DsDiag *diag;
} Vm;

static bool lookup_var(Vm *vm, const char *name, DsValue *out, DsSpan span) {
    DsStr key = {(char *)name, strlen(name)};
    DsValue *found = ds_map_get(&vm->vars, key);
    if (!found) {
        ds_diag_error(vm->diag, span, "unknown variable `%s`", name);
        return false;
    }
    *out = ds_value_copy(found);
    return true;
}

static bool interpolate_string(Vm *vm, const DsString *input, DsString *out, DsSpan span) {
    ds_string_init(out);
    for (size_t i = 0; i < input->len; i++) {
        char c = input->data[i];
        if (c == '{') {
            size_t start = i + 1;
            size_t j = start;
            if (j < input->len && ((input->data[j] >= 'A' && input->data[j] <= 'Z') || (input->data[j] >= 'a' && input->data[j] <= 'z') || input->data[j] == '_')) {
                j++;
                while (j < input->len && ((input->data[j] >= 'A' && input->data[j] <= 'Z') || (input->data[j] >= 'a' && input->data[j] <= 'z') || (input->data[j] >= '0' && input->data[j] <= '9') || input->data[j] == '_')) j++;
                if (j < input->len && input->data[j] == '}') {
                    char *name = ds_str_dup_range(input->data + start, j - start);
                    DsValue value;
                    if (!lookup_var(vm, name, &value, span)) {
                        free(name);
                        ds_string_free(out);
                        return false;
                    }
                    DsString rendered;
                    ds_value_to_string(&value, &rendered);
                    ds_string_append_range(out, rendered.data ? rendered.data : "", rendered.len);
                    ds_string_free(&rendered);
                    ds_value_free(&value);
                    free(name);
                    i = j;
                    continue;
                }
            }
            ds_diag_error(vm->diag, span, "unsupported string interpolation; expected `{name}`");
            ds_string_free(out);
            return false;
        }
        ds_string_append_char(out, c);
    }
    return true;
}

static bool word_to_arg(Vm *vm, DsStr word, DsSpan span, char **out) {
    if (word.len >= 2 && word.data[0] == '"' && word.data[word.len - 1] == '"') {
        DsString decoded;
        if (!decode_string_text(word, &decoded)) return false;
        DsString rendered;
        bool ok = interpolate_string(vm, &decoded, &rendered, span);
        ds_string_free(&decoded);
        if (!ok) return false;
        *out = ds_str_dup_range(rendered.data ? rendered.data : "", rendered.len);
        ds_string_free(&rendered);
        return true;
    }
    if (word.len >= 2 && word.data[0] == '$') {
        char *name = ds_str_dup_range(word.data + 1, word.len - 1);
        DsValue value;
        if (!lookup_var(vm, name, &value, span)) {
            free(name);
            return false;
        }
        DsString rendered;
        ds_value_to_string(&value, &rendered);
        *out = ds_str_dup_range(rendered.data ? rendered.data : "", rendered.len);
        ds_string_free(&rendered);
        ds_value_free(&value);
        free(name);
        return true;
    }
    *out = ds_str_dup_range(word.data, word.len);
    return true;
}

static int run_command(Vm *vm, Instr *ins) {
    if (ins->word_count == 0) return 0;
    char **argv = (char **)ds_xcalloc(ins->word_count + 1, sizeof(char *));
    for (size_t i = 0; i < ins->word_count; i++) {
        if (!word_to_arg(vm, ins->words[i], ins->span, &argv[i])) {
            for (size_t j = 0; j < i; j++) free(argv[j]);
            free(argv);
            return 1;
        }
    }
    pid_t pid = fork();
    if (pid < 0) {
        ds_diag_error(vm->diag, ins->span, "failed to launch command `%s`: %s", argv[0], strerror(errno));
        for (size_t i = 0; i < ins->word_count; i++) free(argv[i]);
        free(argv);
        return 1;
    }
    if (pid == 0) {
        execvp(argv[0], argv);
        fprintf(stderr, "ds: failed to launch command `%s`: %s\n", argv[0], strerror(errno));
        _exit(127);
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            ds_diag_error(vm->diag, ins->span, "failed waiting for command `%s`: %s", argv[0], strerror(errno));
            status = 1;
            break;
        }
    }
    for (size_t i = 0; i < ins->word_count; i++) free(argv[i]);
    free(argv);
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return 1;
}

static bool ensure_regs(Vm *vm) {
    if (vm->program->next_reg <= 0) vm->program->next_reg = 1;
    vm->regs = (DsValue *)ds_xcalloc((size_t)vm->program->next_reg, sizeof(DsValue));
    for (int i = 0; i < vm->program->next_reg; i++) vm->regs[i] = ds_value_null();
    return true;
}

static void set_reg(Vm *vm, int reg, DsValue value) {
    ds_value_free(&vm->regs[reg]);
    vm->regs[reg] = value;
}

int ds_vm_run(const DsSource *source, const DsAst *ast, DsDiag *diag) {
    (void)source;
    if (!ds_lower_validate(ast, diag)) return 1;
    Program p;
    if (!compile_program(ast, &p, diag)) return 1;
    Vm vm;
    memset(&vm, 0, sizeof(vm));
    vm.program = &p;
    vm.diag = diag;
    ds_map_init(&vm.vars);
    ensure_regs(&vm);

    int rc = 0;
    size_t ip = 0;
    while (ip < p.instr_len) {
        Instr *ins = &p.instrs[ip];
        switch (ins->op) {
            case OP_LOAD_CONST:
                set_reg(&vm, ins->dst, ds_value_copy(&p.consts[ins->a]));
                ip++;
                break;
            case OP_LOAD_VAR: {
                DsValue value;
                if (!lookup_var(&vm, ins->name, &value, ins->span)) { rc = 1; goto done; }
                set_reg(&vm, ins->dst, value);
                ip++;
                break;
            }
            case OP_STORE_VAR: {
                DsStr key = {ins->name, strlen(ins->name)};
                ds_map_set(&vm.vars, key, ds_value_copy(&vm.regs[ins->a]));
                ip++;
                break;
            }
            case OP_NOT: {
                bool truth = false;
                ds_value_truthy(&vm.regs[ins->a], &truth);
                set_reg(&vm, ins->dst, ds_value_bool(!truth));
                ip++;
                break;
            }
            case OP_COMPARE: {
                int cmp = ds_value_compare(&vm.regs[ins->a], &vm.regs[ins->b]);
                bool result = false;
                if (strcmp(ins->cmp, "==") == 0) result = cmp == 0;
                else if (strcmp(ins->cmp, "!=") == 0) result = cmp != 0;
                else if (strcmp(ins->cmp, ">") == 0) result = cmp > 0;
                else if (strcmp(ins->cmp, ">=") == 0) result = cmp >= 0;
                else if (strcmp(ins->cmp, "<") == 0) result = cmp < 0;
                else if (strcmp(ins->cmp, "<=") == 0) result = cmp <= 0;
                set_reg(&vm, ins->dst, ds_value_bool(result));
                ip++;
                break;
            }
            case OP_INTERPOLATE: {
                DsString rendered;
                if (!interpolate_string(&vm, &p.consts[ins->a].as.string, &rendered, ins->span)) { rc = 1; goto done; }
                set_reg(&vm, ins->dst, ds_value_string_take(&rendered));
                ip++;
                break;
            }
            case OP_JUMP:
                ip = (size_t)ins->target;
                break;
            case OP_JUMP_IF_FALSE: {
                bool truth = false;
                ds_value_truthy(&vm.regs[ins->a], &truth);
                ip = truth ? ip + 1 : (size_t)ins->target;
                break;
            }
            case OP_RUN_CMD:
                rc = run_command(&vm, ins);
                if (rc != 0) goto done;
                ip++;
                break;
            case OP_RETURN:
                rc = ins->target;
                goto done;
            case OP_NOP:
                ip++;
                break;
        }
    }

done:
    for (int i = 0; i < p.next_reg; i++) ds_value_free(&vm.regs[i]);
    free(vm.regs);
    ds_map_free(&vm.vars);
    program_free(&p);
    return rc;
}
