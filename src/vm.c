#define _POSIX_C_SOURCE 200809L

#include "ds.h"

#include <errno.h>
#include <fcntl.h>
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
    OP_RUN_CAPTURE,
    OP_GET_FIELD,
    OP_JUMP,
    OP_JUMP_IF_FALSE,
    OP_PUSH_SCOPE,
    OP_POP_SCOPE,
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
    char *field;
    DsStr *words;
    size_t word_count;
    DsRedirect redirect;
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
    free(ins->field);
    for (size_t i = 0; i < ins->word_count; i++) free(ins->words[i].data);
    free(ins->words);
    free(ins->redirect.target.data);
}

static void copy_words_to_instr(Instr *ins, const DsWordVec *words) {
    ins->word_count = words->len;
    ins->words = (DsStr *)ds_xcalloc(ins->word_count ? ins->word_count : 1, sizeof(DsStr));
    for (size_t i = 0; i < ins->word_count; i++) {
        DsStr w = words->items[i].text;
        ins->words[i].data = ds_str_dup_range(w.data, w.len);
        ins->words[i].len = w.len;
    }
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

static int compile_expr(Program *p, const DsLowerExpr *expr);

static int compile_string_expr(Program *p, const DsLowerExpr *expr) {
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

static int compile_expr(Program *p, const DsLowerExpr *expr) {
    switch (expr->kind) {
        case DS_LOWER_EXPR_STRING:
            return compile_string_expr(p, expr);
        case DS_LOWER_EXPR_INT: {
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
        case DS_LOWER_EXPR_BOOL: {
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
        case DS_LOWER_EXPR_IDENT: {
            int r = new_reg(p);
            Instr ins = {0};
            ins.op = OP_LOAD_VAR;
            ins.span = expr->span;
            ins.dst = r;
            ins.name = ds_str_dup_range(expr->as.text.data, expr->as.text.len);
            emit_instr(p, ins);
            return r;
        }
        case DS_LOWER_EXPR_RUN: {
            int r = new_reg(p);
            Instr ins = {0};
            ins.op = OP_RUN_CAPTURE;
            ins.span = expr->span;
            ins.dst = r;
            copy_words_to_instr(&ins, &expr->as.run.words);
            emit_instr(p, ins);
            return r;
        }
        case DS_LOWER_EXPR_FIELD: {
            int obj = compile_expr(p, expr->as.field.object);
            int r = new_reg(p);
            Instr ins = {0};
            ins.op = OP_GET_FIELD;
            ins.span = expr->span;
            ins.dst = r;
            ins.a = obj;
            ins.field = ds_str_dup_range(expr->as.field.field.data, expr->as.field.field.len);
            emit_instr(p, ins);
            return r;
        }
        case DS_LOWER_EXPR_UNARY: {
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
        case DS_LOWER_EXPR_BINARY: {
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
        case DS_LOWER_EXPR_ERROR:
            return new_reg(p);
    }
    return new_reg(p);
}

static void compile_stmt(Program *p, const DsLowerStmt *stmt);

static void compile_block(Program *p, const DsLowerStmt *block) {
    for (size_t i = 0; i < block->as.block_stmt.statements.len; i++) compile_stmt(p, block->as.block_stmt.statements.items[i]);
}

static void compile_scoped_block(Program *p, const DsLowerStmt *block) {
    Instr push = {0};
    push.op = OP_PUSH_SCOPE;
    push.span = block->span;
    emit_instr(p, push);
    compile_block(p, block);
    Instr pop = {0};
    pop.op = OP_POP_SCOPE;
    pop.span = block->span;
    emit_instr(p, pop);
}

static void compile_stmt(Program *p, const DsLowerStmt *stmt) {
    switch (stmt->kind) {
        case DS_LOWER_STMT_LET: {
            int src = compile_expr(p, stmt->as.let_stmt.value);
            Instr ins = {0};
            ins.op = OP_STORE_VAR;
            ins.span = stmt->span;
            ins.a = src;
            ins.name = ds_str_dup_range(stmt->as.let_stmt.name.data, stmt->as.let_stmt.name.len);
            emit_instr(p, ins);
            break;
        }
        case DS_LOWER_STMT_CMD: {
            Instr ins = {0};
            ins.op = OP_RUN_CMD;
            ins.span = stmt->span;
            copy_words_to_instr(&ins, &stmt->as.cmd_stmt.words);
            ins.redirect.kind = stmt->as.cmd_stmt.redirect.kind;
            ins.redirect.op_span = stmt->as.cmd_stmt.redirect.op_span;
            ins.redirect.target_span = stmt->as.cmd_stmt.redirect.target_span;
            if (stmt->as.cmd_stmt.redirect.target.len > 0) {
                ins.redirect.target.data = ds_str_dup_range(stmt->as.cmd_stmt.redirect.target.data, stmt->as.cmd_stmt.redirect.target.len);
                ins.redirect.target.len = stmt->as.cmd_stmt.redirect.target.len;
            }
            emit_instr(p, ins);
            break;
        }
        case DS_LOWER_STMT_IF: {
            int cond = compile_expr(p, stmt->as.if_stmt.condition);
            Instr jif = {0};
            jif.op = OP_JUMP_IF_FALSE;
            jif.span = stmt->as.if_stmt.condition->span;
            jif.a = cond;
            size_t jif_pos = emit_instr(p, jif);
            compile_scoped_block(p, stmt->as.if_stmt.then_branch);
            if (stmt->as.if_stmt.else_branch) {
                Instr jump = {0};
                jump.op = OP_JUMP;
                jump.span = stmt->span;
                size_t jump_pos = emit_instr(p, jump);
                p->instrs[jif_pos].target = (int)p->instr_len;
                compile_scoped_block(p, stmt->as.if_stmt.else_branch);
                p->instrs[jump_pos].target = (int)p->instr_len;
            } else {
                p->instrs[jif_pos].target = (int)p->instr_len;
            }
            break;
        }
        case DS_LOWER_STMT_BLOCK:
            compile_scoped_block(p, stmt);
            break;
    }
}

static bool compile_program(const DsLowerProgram *lowered, Program *p, DsDiag *diag) {
    (void)diag;
    program_init(p);
    for (size_t i = 0; i < lowered->statements.len; i++) compile_stmt(p, lowered->statements.items[i]);
    Instr ret = {0};
    ret.op = OP_RETURN;
    ret.target = 0;
    ret.span = lowered->span;
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
        case OP_RUN_CAPTURE: return "RUN_CAPTURE";
        case OP_GET_FIELD: return "GET_FIELD";
        case OP_JUMP: return "JUMP";
        case OP_JUMP_IF_FALSE: return "JUMP_IF_FALSE";
        case OP_PUSH_SCOPE: return "PUSH_SCOPE";
        case OP_POP_SCOPE: return "POP_SCOPE";
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

bool ds_bytecode_dump_program(const DsSource *source, const DsLowerProgram *lowered, FILE *out, DsDiag *diag) {
    Program p;
    if (!compile_program(lowered, &p, diag)) {
        return false;
    }

    fputs("args:\n", out);
    if (!lowered->has_script || lowered->script_decls.len == 0) {
        fputs("  <none>\n", out);
    } else {
        for (size_t i = 0; i < lowered->script_decls.len; i++) {
            const DsLowerScriptDecl *decl = &lowered->script_decls.items[i];
            const char *kind = decl->kind == DS_SCRIPT_DECL_ARG ? "arg" : (decl->kind == DS_SCRIPT_DECL_OPTION ? "option" : "flag");
            const char *type = decl->type == DS_SCRIPT_TYPE_STRING ? "string" : (decl->type == DS_SCRIPT_TYPE_INT ? "int" : "bool");
            fprintf(out, "  %s %.*s: %s", kind, (int)decl->name.len, decl->name.data, type);
            if (decl->has_default) {
                if (decl->type == DS_SCRIPT_TYPE_STRING) {
                    fputs(" = \"", out);
                    print_escaped(out, decl->default_text.data ? decl->default_text.data : "", decl->default_text.len);
                    fputc('"', out);
                } else if (decl->type == DS_SCRIPT_TYPE_INT) {
                    fprintf(out, " = %lld", (long long)decl->default_int);
                } else {
                    fprintf(out, " = %s", decl->default_bool ? "true" : "false");
                }
            }
            { const DsSource *span_source = decl->span.source ? decl->span.source : source; fprintf(out, "    # %s:%d:%d\n", span_source && span_source->path ? span_source->path : "<source>", decl->span.start.line, decl->span.start.column); }
        }
    }

    fputs("\nconstants:\n", out);
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
            case DS_VALUE_COMMAND_RESULT:
                fputs("command_result", out);
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
            case OP_RUN_CAPTURE:
                fprintf(out, " r%d, [", ins->dst);
                for (size_t j = 0; j < ins->word_count; j++) {
                    if (j) fputs(", ", out);
                    fputc('"', out);
                    print_escaped(out, ins->words[j].data, ins->words[j].len);
                    fputc('"', out);
                }
                fputc(']', out);
                break;
            case OP_GET_FIELD: fprintf(out, " r%d, r%d.%s", ins->dst, ins->a, ins->field); break;
            case OP_JUMP: fprintf(out, " %d", ins->target); break;
            case OP_JUMP_IF_FALSE: fprintf(out, " r%d, %d", ins->a, ins->target); break;
            case OP_PUSH_SCOPE: break;
            case OP_POP_SCOPE: break;
            case OP_RUN_CMD:
                fputs(" [", out);
                for (size_t j = 0; j < ins->word_count; j++) {
                    if (j) fputs(", ", out);
                    fputc('"', out);
                    print_escaped(out, ins->words[j].data, ins->words[j].len);
                    fputc('"', out);
                }
                fputc(']', out);
                if (ins->redirect.kind != DS_REDIRECT_NONE) {
                    static const char *names[] = {"", "|>", "|>>", "!>", "!>>", "&>", "&>>"};
                    fprintf(out, " %s \"", names[ins->redirect.kind]);
                    print_escaped(out, ins->redirect.target.data, ins->redirect.target.len);
                    fputc('"', out);
                }
                break;
            case OP_RETURN: fprintf(out, " %d", ins->target); break;
            case OP_NOP: break;
        }
        { const DsSource *span_source = ins->span.source ? ins->span.source : source; fprintf(out, "    # %s:%d:%d\n", span_source && span_source->path ? span_source->path : "<source>", ins->span.start.line, ins->span.start.column); }
    }
    program_free(&p);
    return true;
}

bool ds_bytecode_dump(const DsSource *source, const DsAst *ast, FILE *out, DsDiag *diag) {
    DsLowerProgram *lowered = ds_lower_program(ast, diag);
    if (!lowered) return false;
    bool ok = ds_bytecode_dump_program(source, lowered, out, diag);
    ds_lower_program_free(lowered);
    return ok;
}

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
} Vm;

static const char *script_basename(const DsSource *source) {
    const char *path = source && source->path ? source->path : "<script>";
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static const char *script_type_name(DsScriptType type) {
    switch (type) {
        case DS_SCRIPT_TYPE_STRING: return "string";
        case DS_SCRIPT_TYPE_INT: return "int";
        case DS_SCRIPT_TYPE_BOOL: return "bool";
    }
    return "unknown";
}

static bool parse_runtime_int(const char *text, int64_t *out) {
    if (!text || text[0] == '\0') return false;
    errno = 0;
    char *end = NULL;
    long long value = strtoll(text, &end, 10);
    if (errno == ERANGE || !end || *end != '\0') return false;
    if (text[0] == '+' || text[0] == '-') {
        if (text[1] == '\0') return false;
    }
    *out = (int64_t)value;
    return true;
}

static bool parse_runtime_bool(const char *text, bool *out) {
    if (strcmp(text, "true") == 0) { *out = true; return true; }
    if (strcmp(text, "false") == 0) { *out = false; return true; }
    return false;
}

static void print_script_help(const DsSource *source, const DsLowerProgram *program, FILE *out) {
    fprintf(out, "Usage: %s", script_basename(source));
    for (size_t i = 0; i < program->script_decls.len; i++) {
        const DsLowerScriptDecl *decl = &program->script_decls.items[i];
        if (decl->kind == DS_SCRIPT_DECL_ARG) fprintf(out, " <%.*s>", (int)decl->name.len, decl->name.data);
    }
    bool has_options = false;
    for (size_t i = 0; i < program->script_decls.len; i++) if (program->script_decls.items[i].kind != DS_SCRIPT_DECL_ARG) has_options = true;
    if (has_options) fputs(" [options]", out);
    fputs("\n", out);

    bool has_args = false;
    for (size_t i = 0; i < program->script_decls.len; i++) if (program->script_decls.items[i].kind == DS_SCRIPT_DECL_ARG) has_args = true;
    if (has_args) {
        fputs("\nArguments:\n", out);
        for (size_t i = 0; i < program->script_decls.len; i++) {
            const DsLowerScriptDecl *decl = &program->script_decls.items[i];
            if (decl->kind == DS_SCRIPT_DECL_ARG) fprintf(out, "  %.*s %s\n", (int)decl->name.len, decl->name.data, script_type_name(decl->type));
        }
    }

    fputs("\nOptions:\n", out);
    for (size_t i = 0; i < program->script_decls.len; i++) {
        const DsLowerScriptDecl *decl = &program->script_decls.items[i];
        if (decl->kind == DS_SCRIPT_DECL_OPTION) {
            fprintf(out, "  --%.*s %s    default: ", (int)decl->name.len, decl->name.data, script_type_name(decl->type));
            if (decl->type == DS_SCRIPT_TYPE_STRING) fprintf(out, "%.*s", (int)decl->default_text.len, decl->default_text.data ? decl->default_text.data : "");
            else if (decl->type == DS_SCRIPT_TYPE_INT) fprintf(out, "%lld", (long long)decl->default_int);
            else fprintf(out, "%s", decl->default_bool ? "true" : "false");
            fputc('\n', out);
        } else if (decl->kind == DS_SCRIPT_DECL_FLAG) {
            fprintf(out, "  --%.*s            boolean flag\n", (int)decl->name.len, decl->name.data);
        }
    }
    fputs("  --help             show this help\n", out);
}

static int find_decl_by_option(const DsLowerProgram *program, const char *name) {
    size_t len = strlen(name);
    for (size_t i = 0; i < program->script_decls.len; i++) {
        const DsLowerScriptDecl *decl = &program->script_decls.items[i];
        if (decl->kind != DS_SCRIPT_DECL_ARG && decl->name.len == len && memcmp(decl->name.data, name, len) == 0) return (int)i;
    }
    return -1;
}

static bool set_var_from_decl(Vm *vm, const DsLowerScriptDecl *decl, const char *text, DsSpan span) {
    DsValue value = ds_value_null();
    if (decl->type == DS_SCRIPT_TYPE_STRING) {
        DsString s;
        ds_string_from_cstr(&s, text ? text : "");
        value = ds_value_string_take(&s);
    } else if (decl->type == DS_SCRIPT_TYPE_INT) {
        int64_t parsed = 0;
        if (!parse_runtime_int(text, &parsed)) {
            ds_diag_error(vm->diag, span, "invalid int value `%s` for `%.*s`", text ? text : "", (int)decl->name.len, decl->name.data);
            return false;
        }
        value = ds_value_int(parsed);
    } else {
        bool parsed = false;
        if (!parse_runtime_bool(text, &parsed)) {
            ds_diag_error(vm->diag, span, "invalid bool value `%s` for `%.*s`", text ? text : "", (int)decl->name.len, decl->name.data);
            return false;
        }
        value = ds_value_bool(parsed);
    }
    ds_map_set(&vm->scope->vars, decl->name, value);
    return true;
}

static bool set_default_from_decl(Vm *vm, const DsLowerScriptDecl *decl) {
    DsValue value = ds_value_null();
    if (decl->type == DS_SCRIPT_TYPE_STRING) {
        DsString s;
        ds_string_from_range(&s, decl->default_text.data ? decl->default_text.data : "", decl->default_text.len);
        value = ds_value_string_take(&s);
    } else if (decl->type == DS_SCRIPT_TYPE_INT) {
        value = ds_value_int(decl->default_int);
    } else {
        value = ds_value_bool(decl->default_bool);
    }
    ds_map_set(&vm->scope->vars, decl->name, value);
    return true;
}

static int bind_script_args(Vm *vm, const DsLowerProgram *program, int argc, char **argv) {
    if (!program->has_script) {
        if (argc > 0) {
            fprintf(stderr, "%s: error: unexpected script arguments\n", script_basename(vm->source));
            return 1;
        }
        return 0;
    }

    bool *seen = (bool *)ds_xcalloc(program->script_decls.len ? program->script_decls.len : 1, sizeof(bool));
    for (size_t i = 0; i < program->script_decls.len; i++) {
        const DsLowerScriptDecl *decl = &program->script_decls.items[i];
        if (decl->kind != DS_SCRIPT_DECL_ARG) set_default_from_decl(vm, decl);
    }

    size_t next_arg = 0;
    bool end_options = false;
    for (int i = 0; i < argc; i++) {
        const char *arg = argv[i];
        if (!end_options && (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0)) {
            print_script_help(vm->source, program, stdout);
            free(seen);
            return 2;
        }
        if (!end_options && strcmp(arg, "--") == 0) {
            end_options = true;
            continue;
        }
        if (!end_options && strncmp(arg, "--", 2) == 0) {
            int idx = find_decl_by_option(program, arg + 2);
            if (idx < 0) {
                fprintf(stderr, "%s: error: unknown option `%s`\n", script_basename(vm->source), arg);
                free(seen);
                return 1;
            }
            if (seen[idx]) {
                fprintf(stderr, "%s: error: duplicate option `%s`\n", script_basename(vm->source), arg);
                free(seen);
                return 1;
            }
            seen[idx] = true;
            const DsLowerScriptDecl *decl = &program->script_decls.items[idx];
            if (decl->kind == DS_SCRIPT_DECL_FLAG) {
                ds_map_set(&vm->scope->vars, decl->name, ds_value_bool(true));
            } else {
                if (i + 1 >= argc || strncmp(argv[i + 1], "--", 2) == 0) {
                    fprintf(stderr, "%s: error: option `%s` requires a value\n", script_basename(vm->source), arg);
                    free(seen);
                    return 1;
                }
                i++;
                if (!set_var_from_decl(vm, decl, argv[i], decl->span)) {
                    free(seen);
                    return 1;
                }
            }
            continue;
        }
        while (next_arg < program->script_decls.len && program->script_decls.items[next_arg].kind != DS_SCRIPT_DECL_ARG) next_arg++;
        if (next_arg >= program->script_decls.len) {
            fprintf(stderr, "%s: error: unexpected extra positional argument `%s`\n", script_basename(vm->source), arg);
            free(seen);
            return 1;
        }
        if (!set_var_from_decl(vm, &program->script_decls.items[next_arg], arg, program->script_decls.items[next_arg].span)) {
            free(seen);
            return 1;
        }
        seen[next_arg] = true;
        next_arg++;
    }

    for (size_t i = 0; i < program->script_decls.len; i++) {
        const DsLowerScriptDecl *decl = &program->script_decls.items[i];
        if (decl->kind == DS_SCRIPT_DECL_ARG && !seen[i]) {
            fprintf(stderr, "%s: error: missing required argument `%.*s`\n", script_basename(vm->source), (int)decl->name.len, decl->name.data);
            free(seen);
            return 1;
        }
    }
    free(seen);
    return 0;
}

static VmScope *scope_new(VmScope *parent) {
    VmScope *scope = (VmScope *)ds_xcalloc(1, sizeof(VmScope));
    ds_map_init(&scope->vars);
    scope->parent = parent;
    return scope;
}

static void scope_free_one(VmScope *scope) {
    if (!scope) return;
    ds_map_free(&scope->vars);
    free(scope);
}

static void scope_free_chain(VmScope *scope) {
    while (scope) {
        VmScope *parent = scope->parent;
        scope_free_one(scope);
        scope = parent;
    }
}

static void vm_push_scope(Vm *vm) {
    vm->scope = scope_new(vm->scope);
}

static void vm_pop_scope(Vm *vm) {
    if (!vm->scope || !vm->scope->parent) return;
    VmScope *old = vm->scope;
    vm->scope = old->parent;
    old->parent = NULL;
    scope_free_one(old);
}

static bool lookup_var(Vm *vm, const char *name, DsValue *out, DsSpan span) {
    DsStr key = {(char *)name, strlen(name)};
    for (VmScope *scope = vm->scope; scope; scope = scope->parent) {
        DsValue *found = ds_map_get(&scope->vars, key);
        if (found) {
            *out = ds_value_copy(found);
            return true;
        }
    }
    {
        ds_diag_error(vm->diag, span, "unknown variable `%s`", name);
        return false;
    }
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
    for (size_t i = 1; i + 1 < word.len; i++) {
        if (word.data[i] == '.') {
            char *name = ds_str_dup_range(word.data, i);
            char *field = ds_str_dup_range(word.data + i + 1, word.len - i - 1);
            DsValue value;
            if (!lookup_var(vm, name, &value, span)) { free(name); free(field); return false; }
            if (value.kind != DS_VALUE_COMMAND_RESULT) {
                ds_diag_error(vm->diag, span, "field access is only supported on command results in v0.7.0");
                ds_value_free(&value); free(name); free(field); return false;
            }
            DsValue field_value = ds_value_null();
            if (strcmp(field, "stdout") == 0) ds_string_from_range(&field_value.as.string, value.as.command_result.stdout_text.data ? value.as.command_result.stdout_text.data : "", value.as.command_result.stdout_text.len), field_value.kind = DS_VALUE_STRING;
            else if (strcmp(field, "stderr") == 0) ds_string_from_range(&field_value.as.string, value.as.command_result.stderr_text.data ? value.as.command_result.stderr_text.data : "", value.as.command_result.stderr_text.len), field_value.kind = DS_VALUE_STRING;
            else if (strcmp(field, "code") == 0) field_value = ds_value_int(value.as.command_result.code);
            else if (strcmp(field, "ok") == 0) field_value = ds_value_bool(value.as.command_result.code == 0);
            else if (strcmp(field, "failed") == 0) field_value = ds_value_bool(value.as.command_result.code != 0);
            else ds_diag_error(vm->diag, span, "unknown command result field `%s`", field);
            DsString rendered;
            ds_value_to_string(&field_value, &rendered);
            *out = ds_str_dup_range(rendered.data ? rendered.data : "", rendered.len);
            ds_string_free(&rendered);
            ds_value_free(&field_value);
            ds_value_free(&value);
            free(name); free(field);
            return !vm->diag->has_error;
        }
    }
    *out = ds_str_dup_range(word.data, word.len);
    return true;
}

static bool render_redirect_target(Vm *vm, DsRedirect *redirect, char **out) {
    DsString decoded;
    if (!decode_string_text(redirect->target, &decoded)) {
        ds_diag_error(vm->diag, redirect->target_span, "invalid redirection target");
        return false;
    }
    DsString rendered;
    bool ok = interpolate_string(vm, &decoded, &rendered, redirect->target_span);
    ds_string_free(&decoded);
    if (!ok) return false;
    *out = ds_str_dup_range(rendered.data ? rendered.data : "", rendered.len);
    ds_string_free(&rendered);
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
        if (ins->redirect.kind != DS_REDIRECT_NONE) {
            char *path = NULL;
            if (!render_redirect_target(vm, &ins->redirect, &path)) _exit(1);
            int flags = O_CREAT | O_WRONLY;
            if (ins->redirect.kind == DS_REDIRECT_OUT_APPEND || ins->redirect.kind == DS_REDIRECT_ERR_APPEND || ins->redirect.kind == DS_REDIRECT_ALL_APPEND) flags |= O_APPEND;
            else flags |= O_TRUNC;
            int fd = open(path, flags, 0666);
            if (fd < 0) {
                fprintf(stderr, "ds: failed to open redirection target `%s`: %s\n", path, strerror(errno));
                _exit(1);
            }
            if (ins->redirect.kind == DS_REDIRECT_OUT || ins->redirect.kind == DS_REDIRECT_OUT_APPEND) dup2(fd, STDOUT_FILENO);
            else if (ins->redirect.kind == DS_REDIRECT_ERR || ins->redirect.kind == DS_REDIRECT_ERR_APPEND) dup2(fd, STDERR_FILENO);
            else { dup2(fd, STDOUT_FILENO); dup2(fd, STDERR_FILENO); }
            close(fd);
            free(path);
        }
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

static bool read_file_into_string(FILE *fp, DsString *out) {
    ds_string_init(out);
    fflush(fp);
    if (fseek(fp, 0, SEEK_SET) != 0) return false;
    char buf[4096];
    size_t n = 0;
    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) ds_string_append_range(out, buf, n);
    return ferror(fp) == 0;
}

static int run_capture(Vm *vm, Instr *ins, DsValue *out_value) {
    *out_value = ds_value_null();
    if (ins->word_count == 0) return 1;
    char **argv = (char **)ds_xcalloc(ins->word_count + 1, sizeof(char *));
    for (size_t i = 0; i < ins->word_count; i++) {
        if (!word_to_arg(vm, ins->words[i], ins->span, &argv[i])) {
            for (size_t j = 0; j < i; j++) free(argv[j]);
            free(argv);
            return 1;
        }
    }
    FILE *out_fp = tmpfile();
    FILE *err_fp = tmpfile();
    if (!out_fp || !err_fp) {
        ds_diag_error(vm->diag, ins->span, "failed to create command capture temporary files: %s", strerror(errno));
        if (out_fp) fclose(out_fp);
        if (err_fp) fclose(err_fp);
        for (size_t i = 0; i < ins->word_count; i++) free(argv[i]);
        free(argv);
        return 1;
    }
    pid_t pid = fork();
    if (pid < 0) {
        ds_diag_error(vm->diag, ins->span, "failed to launch command `%s`: %s", argv[0], strerror(errno));
        fclose(out_fp); fclose(err_fp);
        for (size_t i = 0; i < ins->word_count; i++) free(argv[i]);
        free(argv);
        return 1;
    }
    if (pid == 0) {
        dup2(fileno(out_fp), STDOUT_FILENO);
        dup2(fileno(err_fp), STDERR_FILENO);
        execvp(argv[0], argv);
        fprintf(stderr, "ds: failed to launch command `%s`: %s\n", argv[0], strerror(errno));
        _exit(127);
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) { status = 1; break; }
    }
    int code = 1;
    if (WIFEXITED(status)) code = WEXITSTATUS(status);
    else if (WIFSIGNALED(status)) code = 128 + WTERMSIG(status);
    DsString stdout_text, stderr_text;
    if (!read_file_into_string(out_fp, &stdout_text) || !read_file_into_string(err_fp, &stderr_text)) {
        ds_diag_error(vm->diag, ins->span, "failed to read command capture output");
        code = 1;
    }
    fclose(out_fp); fclose(err_fp);
    for (size_t i = 0; i < ins->word_count; i++) free(argv[i]);
    free(argv);
    *out_value = ds_value_command_result_take(&stdout_text, &stderr_text, code);
    return 0;
}

static bool command_result_field(Vm *vm, const DsValue *value, const char *field, DsSpan span, DsValue *out) {
    if (value->kind != DS_VALUE_COMMAND_RESULT) {
        ds_diag_error(vm->diag, span, "field access is only supported on command results in v0.7.0");
        return false;
    }
    if (strcmp(field, "stdout") == 0) { ds_string_from_range(&out->as.string, value->as.command_result.stdout_text.data ? value->as.command_result.stdout_text.data : "", value->as.command_result.stdout_text.len); out->kind = DS_VALUE_STRING; return true; }
    if (strcmp(field, "stderr") == 0) { ds_string_from_range(&out->as.string, value->as.command_result.stderr_text.data ? value->as.command_result.stderr_text.data : "", value->as.command_result.stderr_text.len); out->kind = DS_VALUE_STRING; return true; }
    if (strcmp(field, "code") == 0) { *out = ds_value_int(value->as.command_result.code); return true; }
    if (strcmp(field, "ok") == 0) { *out = ds_value_bool(value->as.command_result.code == 0); return true; }
    if (strcmp(field, "failed") == 0) { *out = ds_value_bool(value->as.command_result.code != 0); return true; }
    ds_diag_error(vm->diag, span, "unknown command result field `%s`", field);
    return false;
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

int ds_vm_run_program_args(const DsSource *source, const DsLowerProgram *lowered, int argc, char **argv, DsDiag *diag) {
    (void)source;
    Program p;
    if (!compile_program(lowered, &p, diag)) {
        return 1;
    }
    Vm vm;
    memset(&vm, 0, sizeof(vm));
    vm.program = &p;
    vm.diag = diag;
    vm.source = source;
    vm.scope = scope_new(NULL);
    ensure_regs(&vm);

    int rc = 0;
    int bind_rc = bind_script_args(&vm, lowered, argc, argv);
    if (bind_rc == 2) { rc = 0; goto done; }
    if (bind_rc != 0) { rc = bind_rc; goto done; }
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
                ds_map_set(&vm.scope->vars, key, ds_value_copy(&vm.regs[ins->a]));
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
            case OP_RUN_CAPTURE: {
                DsValue value;
                if (run_capture(&vm, ins, &value) != 0) { rc = 1; goto done; }
                set_reg(&vm, ins->dst, value);
                ip++;
                break;
            }
            case OP_GET_FIELD: {
                DsValue field = ds_value_null();
                if (!command_result_field(&vm, &vm.regs[ins->a], ins->field, ins->span, &field)) { rc = 1; goto done; }
                set_reg(&vm, ins->dst, field);
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
            case OP_PUSH_SCOPE:
                vm_push_scope(&vm);
                ip++;
                break;
            case OP_POP_SCOPE:
                vm_pop_scope(&vm);
                ip++;
                break;
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
    scope_free_chain(vm.scope);
    program_free(&p);
    return rc;
}

int ds_vm_run_program(const DsSource *source, const DsLowerProgram *lowered, DsDiag *diag) {
    return ds_vm_run_program_args(source, lowered, 0, NULL, diag);
}

int ds_vm_run(const DsSource *source, const DsAst *ast, DsDiag *diag) {
    DsLowerProgram *lowered = ds_lower_program(ast, diag);
    if (!lowered) return 1;
    int rc = ds_vm_run_program(source, lowered, diag);
    ds_lower_program_free(lowered);
    return rc;
}
