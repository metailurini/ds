#define _POSIX_C_SOURCE 200809L

#include "ds.h"

#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
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
    OP_CALL,
    OP_STDLIB_CALL,
    OP_ARRAY_LITERAL,
    OP_MAP_LITERAL,
    OP_GET_INDEX,
    OP_PUSH_ARRAY,
    OP_FOR_ARRAY,
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

static void program_init(Program *p) { memset(p, 0, sizeof(*p)); }

static void instr_free(Instr *ins) {
    free(ins->name);
    free(ins->cmp);
    free(ins->field);
    free(ins->args);
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
    for (size_t i = 0; i < p->function_len; i++) {
        free(p->functions[i].name);
        for (size_t j = 0; j < p->functions[i].param_count; j++) {
            free(p->functions[i].params[j].name);
            ds_value_free(&p->functions[i].params[j].default_value);
        }
        free(p->functions[i].params);
    }
    free(p->consts);
    free(p->instrs);
    free(p->functions);
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

static bool decode_string_text(DsStr text, DsString *out);

static DsValue literal_default_value(const DsLowerExpr *expr) {
    if (!expr) return ds_value_null();
    switch (expr->kind) {
        case DS_LOWER_EXPR_STRING: {
            DsString decoded;
            decode_string_text(expr->as.text, &decoded);
            return ds_value_string_take(&decoded);
        }
        case DS_LOWER_EXPR_INT: {
            char *tmp = ds_str_dup_range(expr->as.text.data, expr->as.text.len);
            int64_t value = strtoll(tmp, NULL, 10);
            free(tmp);
            return ds_value_int(value);
        }
        case DS_LOWER_EXPR_BOOL:
            return ds_value_bool(expr->as.boolean);
        default:
            return ds_value_null();
    }
}

static int add_function_meta(Program *p, const DsLowerFn *fn) {
    if (p->function_len == p->function_cap) {
        p->function_cap = p->function_cap ? p->function_cap * 2 : 8;
        p->functions = (FnMeta *)ds_xrealloc(p->functions, p->function_cap * sizeof(FnMeta));
    }
    FnMeta *meta = &p->functions[p->function_len];
    memset(meta, 0, sizeof(*meta));
    meta->name = ds_str_dup_range(fn->name.data, fn->name.len);
    meta->param_count = fn->params.len;
    meta->required_count = fn->required_count;
    meta->params = (FnParamMeta *)ds_xcalloc(meta->param_count ? meta->param_count : 1, sizeof(FnParamMeta));
    for (size_t i = 0; i < fn->params.len; i++) {
        meta->params[i].name = ds_str_dup_range(fn->params.items[i].name.data, fn->params.items[i].name.len);
        meta->params[i].has_default = fn->params.items[i].has_default;
        meta->params[i].default_value = fn->params.items[i].has_default ? literal_default_value(fn->params.items[i].default_value) : ds_value_null();
    }
    return (int)p->function_len++;
}

static int find_function_meta(Program *p, DsStr name) {
    for (size_t i = 0; i < p->function_len; i++) {
        if (strlen(p->functions[i].name) == name.len && memcmp(p->functions[i].name, name.data, name.len) == 0) return (int)i;
    }
    return -1;
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
        case DS_LOWER_EXPR_ARRAY: {
            int r = new_reg(p);
            Instr ins = {0};
            ins.op = OP_ARRAY_LITERAL;
            ins.span = expr->span;
            ins.dst = r;
            ins.arg_count = expr->as.array.elements.len;
            ins.args = (int *)ds_xcalloc(ins.arg_count ? ins.arg_count : 1, sizeof(int));
            for (size_t i = 0; i < expr->as.array.elements.len; i++) ins.args[i] = compile_expr(p, expr->as.array.elements.items[i]);
            emit_instr(p, ins);
            return r;
        }
        case DS_LOWER_EXPR_MAP: {
            int r = new_reg(p);
            Instr ins = {0};
            ins.op = OP_MAP_LITERAL;
            ins.span = expr->span;
            ins.dst = r;
            ins.arg_count = expr->as.map.entries.len;
            ins.args = (int *)ds_xcalloc(ins.arg_count ? ins.arg_count : 1, sizeof(int));
            ins.word_count = expr->as.map.entries.len;
            ins.words = (DsStr *)ds_xcalloc(ins.word_count ? ins.word_count : 1, sizeof(DsStr));
            for (size_t i = 0; i < expr->as.map.entries.len; i++) {
                ins.args[i] = compile_expr(p, expr->as.map.entries.items[i].value);
                ins.words[i].data = ds_str_dup_range(expr->as.map.entries.items[i].key.data, expr->as.map.entries.items[i].key.len);
                ins.words[i].len = expr->as.map.entries.items[i].key.len;
            }
            emit_instr(p, ins);
            return r;
        }
        case DS_LOWER_EXPR_INDEX: {
            int obj = compile_expr(p, expr->as.index.object);
            int idx = compile_expr(p, expr->as.index.index);
            int r = new_reg(p);
            Instr ins = {0};
            ins.op = OP_GET_INDEX;
            ins.span = expr->span;
            ins.dst = r;
            ins.a = obj;
            ins.b = idx;
            emit_instr(p, ins);
            return r;
        }
        case DS_LOWER_EXPR_CALL:
        {
            int r = new_reg(p);
            Instr ins = {0};
            ins.op = OP_STDLIB_CALL;
            ins.span = expr->span;
            ins.dst = r;
            ins.name = ds_str_dup_range(expr->as.call.name.data, expr->as.call.name.len);
            ins.arg_count = expr->as.call.args.len;
            ins.args = (int *)ds_xcalloc(ins.arg_count ? ins.arg_count : 1, sizeof(int));
            for (size_t i = 0; i < expr->as.call.args.len; i++) ins.args[i] = compile_expr(p, expr->as.call.args.items[i]);
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
        case DS_LOWER_STMT_CALL: {
            Instr ins = {0};
            ins.op = find_function_meta(p, stmt->as.call_stmt.name) >= 0 ? OP_CALL : OP_STDLIB_CALL;
            ins.span = stmt->span;
            ins.target = find_function_meta(p, stmt->as.call_stmt.name);
            if (ins.op == OP_STDLIB_CALL) ins.name = ds_str_dup_range(stmt->as.call_stmt.name.data, stmt->as.call_stmt.name.len);
            ins.arg_count = stmt->as.call_stmt.args.len;
            ins.args = (int *)ds_xcalloc(ins.arg_count ? ins.arg_count : 1, sizeof(int));
            for (size_t i = 0; i < stmt->as.call_stmt.args.len; i++) ins.args[i] = compile_expr(p, stmt->as.call_stmt.args.items[i]);
            emit_instr(p, ins);
            break;
        }
        case DS_LOWER_STMT_PUSH: {
            int value = compile_expr(p, stmt->as.push_stmt.value);
            Instr ins = {0};
            ins.op = OP_PUSH_ARRAY;
            ins.span = stmt->span;
            ins.a = value;
            ins.name = ds_str_dup_range(stmt->as.push_stmt.name.data, stmt->as.push_stmt.name.len);
            emit_instr(p, ins);
            break;
        }
        case DS_LOWER_STMT_FOR_ARRAY: {
            int iterable = compile_expr(p, stmt->as.for_stmt.iterable);
            Instr begin = {0};
            begin.op = OP_FOR_ARRAY;
            begin.span = stmt->span;
            begin.a = iterable;
            begin.name = ds_str_dup_range(stmt->as.for_stmt.name.data, stmt->as.for_stmt.name.len);
            size_t begin_pos = emit_instr(p, begin);
            compile_block(p, stmt->as.for_stmt.body);
            Instr pop = {0};
            pop.op = OP_POP_SCOPE;
            pop.span = stmt->span;
            emit_instr(p, pop);
            Instr jump = {0};
            jump.op = OP_JUMP;
            jump.span = stmt->span;
            jump.target = (int)begin_pos;
            emit_instr(p, jump);
            p->instrs[begin_pos].target = (int)p->instr_len;
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
    for (size_t i = 0; i < lowered->functions.len; i++) add_function_meta(p, &lowered->functions.items[i]);
    size_t jump_main_pos = 0;
    if (lowered->functions.len > 0) {
        Instr jump_main = {0};
        jump_main.op = OP_JUMP;
        jump_main.span = lowered->span;
        jump_main_pos = emit_instr(p, jump_main);
        for (size_t i = 0; i < lowered->functions.len; i++) {
            p->functions[i].target = p->instr_len;
            compile_scoped_block(p, lowered->functions.items[i].body);
            Instr ret_fn = {0};
            ret_fn.op = OP_RETURN_FUNC;
            ret_fn.span = lowered->functions.items[i].span;
            emit_instr(p, ret_fn);
        }
        p->instrs[jump_main_pos].target = (int)p->instr_len;
    }
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
        case OP_CALL: return "CALL";
        case OP_STDLIB_CALL: return "STDLIB_CALL";
        case OP_ARRAY_LITERAL: return "ARRAY_LITERAL";
        case OP_MAP_LITERAL: return "MAP_LITERAL";
        case OP_GET_INDEX: return "GET_INDEX";
        case OP_PUSH_ARRAY: return "PUSH_ARRAY";
        case OP_FOR_ARRAY: return "FOR_ARRAY";
        case OP_RETURN_FUNC: return "RETURN_FUNC";
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

static void print_value_literal(FILE *out, const DsValue *v) {
    switch (v->kind) {
        case DS_VALUE_NULL:
            fputs("null", out);
            break;
        case DS_VALUE_BOOL:
            fprintf(out, "bool %s", v->as.boolean ? "true" : "false");
            break;
        case DS_VALUE_INT:
            fprintf(out, "int %lld", (long long)v->as.integer);
            break;
        case DS_VALUE_STRING:
            fputs("string \"", out);
            print_escaped(out, v->as.string.data ? v->as.string.data : "", v->as.string.len);
            fputc('"', out);
            break;
        case DS_VALUE_COMMAND_RESULT:
            fputs("command_result", out);
            break;
        case DS_VALUE_ARRAY:
            fprintf(out, "array[%zu]", v->as.array.len);
            break;
        case DS_VALUE_MAP:
            fprintf(out, "map[%zu]", ds_map_len(&v->as.map));
            break;
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

    fputs("\nfunctions:\n", out);
    if (p.function_len == 0) {
        fputs("  <none>\n", out);
    } else {
        for (size_t i = 0; i < p.function_len; i++) {
            FnMeta *fn = &p.functions[i];
            fprintf(out, "  fn%zu %s(required=%zu, params=%zu)\n", i, fn->name, fn->required_count, fn->param_count);
            for (size_t j = 0; j < fn->param_count; j++) {
                FnParamMeta *param = &fn->params[j];
                fprintf(out, "    param %zu %s", j, param->name);
                if (param->has_default) {
                    fputs(" = ", out);
                    print_value_literal(out, &param->default_value);
                }
                fputc('\n', out);
            }
        }
    }

    fputs("\nconstants:\n", out);
    for (size_t i = 0; i < p.const_len; i++) {
        fprintf(out, "  %zu: ", i);
        DsValue *v = &p.consts[i];
        print_value_literal(out, v);
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
            case OP_CALL:
                fprintf(out, " fn%d(", ins->target);
                for (size_t j = 0; j < ins->arg_count; j++) {
                    if (j) fputs(", ", out);
                    fprintf(out, "r%d", ins->args[j]);
                }
                fputc(')', out);
                break;
            case OP_STDLIB_CALL:
                fprintf(out, " r%d, %s(", ins->dst, ins->name ? ins->name : "<helper>");
                for (size_t j = 0; j < ins->arg_count; j++) { if (j) fputs(", ", out); fprintf(out, "r%d", ins->args[j]); }
                fputc(')', out);
                break;
            case OP_ARRAY_LITERAL:
                fprintf(out, " r%d, [", ins->dst);
                for (size_t j = 0; j < ins->arg_count; j++) { if (j) fputs(", ", out); fprintf(out, "r%d", ins->args[j]); }
                fputc(']', out);
                break;
            case OP_MAP_LITERAL:
                fprintf(out, " r%d, {", ins->dst);
                for (size_t j = 0; j < ins->arg_count; j++) { if (j) fputs(", ", out); fprintf(out, "%.*s: r%d", (int)ins->words[j].len, ins->words[j].data, ins->args[j]); }
                fputc('}', out);
                break;
            case OP_GET_INDEX: fprintf(out, " r%d, r%d[r%d]", ins->dst, ins->a, ins->b); break;
            case OP_PUSH_ARRAY: fprintf(out, " %s, r%d", ins->name, ins->a); break;
            case OP_FOR_ARRAY: fprintf(out, " %s in r%d -> %d", ins->name, ins->a, ins->target); break;
            case OP_RETURN_FUNC: break;
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
    size_t *return_ips;
    size_t return_len;
    size_t return_cap;
} Vm;

static bool command_result_field(Vm *vm, const DsValue *value, const char *field, DsSpan span, DsValue *out);

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

static void vm_push_return(Vm *vm, size_t ip) {
    if (vm->return_len == vm->return_cap) {
        vm->return_cap = vm->return_cap ? vm->return_cap * 2 : 8;
        vm->return_ips = (size_t *)ds_xrealloc(vm->return_ips, vm->return_cap * sizeof(size_t));
    }
    vm->return_ips[vm->return_len++] = ip;
}

static bool vm_pop_return(Vm *vm, size_t *out) {
    if (vm->return_len == 0) return false;
    *out = vm->return_ips[--vm->return_len];
    return true;
}

static bool call_function(Vm *vm, Instr *ins, size_t next_ip, size_t *target_ip) {
    if (ins->target < 0 || (size_t)ins->target >= vm->program->function_len) {
        ds_diag_error(vm->diag, ins->span, "unknown function call target");
        return false;
    }
    FnMeta *fn = &vm->program->functions[ins->target];
    VmScope *scope = scope_new(vm->scope);
    for (size_t i = 0; i < fn->param_count; i++) {
        DsValue value = ds_value_null();
        if (i < ins->arg_count) value = ds_value_copy(&vm->regs[ins->args[i]]);
        else if (fn->params[i].has_default) value = ds_value_copy(&fn->params[i].default_value);
        else {
            scope_free_one(scope);
            ds_diag_error(vm->diag, ins->span, "function `%s` missing argument `%s`", fn->name, fn->params[i].name);
            return false;
        }
        DsStr key = {fn->params[i].name, strlen(fn->params[i].name)};
        ds_map_set(&scope->vars, key, value);
    }
    vm->scope = scope;
    vm_push_return(vm, next_ip);
    *target_ip = fn->target;
    return true;
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

static DsValue *lookup_var_ref(Vm *vm, const char *name) {
    DsStr key = {(char *)name, strlen(name)};
    for (VmScope *scope = vm->scope; scope; scope = scope->parent) {
        DsValue *found = ds_map_get(&scope->vars, key);
        if (found) return found;
    }
    return NULL;
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
                if (j < input->len && (input->data[j] == '}' || input->data[j] == '.')) {
                    char *name = ds_str_dup_range(input->data + start, j - start);
                    DsValue value;
                    if (!lookup_var(vm, name, &value, span)) {
                        free(name);
                        ds_string_free(out);
                        return false;
                    }
                    if (input->data[j] == '.') {
                        size_t field_start = ++j;
                        if (j < input->len && ((input->data[j] >= 'A' && input->data[j] <= 'Z') || (input->data[j] >= 'a' && input->data[j] <= 'z') || input->data[j] == '_')) {
                            j++;
                            while (j < input->len && ((input->data[j] >= 'A' && input->data[j] <= 'Z') || (input->data[j] >= 'a' && input->data[j] <= 'z') || (input->data[j] >= '0' && input->data[j] <= '9') || input->data[j] == '_')) j++;
                        }
                        if (j >= input->len || input->data[j] != '}') {
                            ds_diag_error(vm->diag, span, "unsupported string interpolation; expected `{name}` or `{name.field}`");
                            ds_value_free(&value);
                            free(name);
                            ds_string_free(out);
                            return false;
                        }
                        char *field = ds_str_dup_range(input->data + field_start, j - field_start);
                        DsValue field_value = ds_value_null();
                        bool ok = command_result_field(vm, &value, field, span, &field_value);
                        free(field);
                        ds_value_free(&value);
                        if (!ok) {
                            free(name);
                            ds_string_free(out);
                            return false;
                        }
                        value = field_value;
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
            ds_diag_error(vm->diag, span, "unsupported string interpolation; expected `{name}` or `{name.field}`");
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
            DsValue field_value = ds_value_null();
            bool ok = command_result_field(vm, &value, field, span, &field_value);
            if (!ok) {
                ds_value_free(&value);
                free(name); free(field);
                return false;
            }
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

static bool render_redirect_target(Vm *vm, const DsRedirect *redirect, char **out) {
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

typedef struct {
    char **items;
    size_t len;
} VmArgv;

typedef struct {
    DsString stdout_text;
    DsString stderr_text;
    int code;
} VmProcessResult;

typedef struct {
    VmArgv argv;
    DsRedirect redirect;
    DsSpan span;
    bool capture;
    int exec_error_fd;
} VmProcessSpec;

static void argv_free(VmArgv *argv) {
    for (size_t i = 0; i < argv->len; i++) free(argv->items[i]);
    free(argv->items);
    argv->items = NULL;
    argv->len = 0;
}

static bool argv_build(Vm *vm, Instr *ins, VmArgv *argv) {
    argv->items = NULL;
    argv->len = 0;
    if (ins->word_count == 0) return false;
    argv->items = (char **)ds_xcalloc(ins->word_count + 1, sizeof(char *));
    argv->len = ins->word_count;
    for (size_t i = 0; i < ins->word_count; i++) {
        if (!word_to_arg(vm, ins->words[i], ins->span, &argv->items[i])) {
            argv->len = i;
            argv_free(argv);
            return false;
        }
    }
    return true;
}

static int process_status_code(int status) {
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return 1;
}

static void process_result_init(VmProcessResult *result) {
    ds_string_init(&result->stdout_text);
    ds_string_init(&result->stderr_text);
    result->code = 0;
}

static void process_result_free(VmProcessResult *result) {
    ds_string_free(&result->stdout_text);
    ds_string_free(&result->stderr_text);
    result->code = 0;
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

static bool open_redirect_target(Vm *vm, const DsRedirect *redirect, int *out_fd) {
    *out_fd = -1;
    char *redirect_path = NULL;
    if (!render_redirect_target(vm, redirect, &redirect_path)) return false;

    int flags = O_CREAT | O_WRONLY;
    if (redirect->kind == DS_REDIRECT_OUT_APPEND || redirect->kind == DS_REDIRECT_ERR_APPEND || redirect->kind == DS_REDIRECT_ALL_APPEND) flags |= O_APPEND;
    else flags |= O_TRUNC;

    int fd = open(redirect_path, flags, 0666);
    if (fd < 0) {
        ds_diag_error(vm->diag, redirect->target_span, "failed to open redirection target `%s`: %s", redirect_path, strerror(errno));
        free(redirect_path);
        return false;
    }
    free(redirect_path);
    *out_fd = fd;
    return true;
}

static bool process_spec_from_instr(Vm *vm, Instr *ins, bool capture, VmProcessSpec *spec) {
    memset(spec, 0, sizeof(*spec));
    spec->span = ins->span;
    spec->redirect = ins->redirect;
    spec->capture = capture;
    return argv_build(vm, ins, &spec->argv);
}

static void process_spec_free(VmProcessSpec *spec) {
    argv_free(&spec->argv);
}

static bool fd_set_cloexec(int fd) {
    int flags = fcntl(fd, F_GETFD);
    if (flags < 0) return false;
    return fcntl(fd, F_SETFD, flags | FD_CLOEXEC) == 0;
}

static bool process_exec_error_pipe(Vm *vm, const VmProcessSpec *spec, int pipe_fds[2]) {
    pipe_fds[0] = -1;
    pipe_fds[1] = -1;
    if (pipe(pipe_fds) != 0) {
        ds_diag_error(vm->diag, spec->span, "failed to prepare command `%s`: %s", spec->argv.items[0], strerror(errno));
        return false;
    }
    if (!fd_set_cloexec(pipe_fds[1])) {
        ds_diag_error(vm->diag, spec->span, "failed to prepare command `%s`: %s", spec->argv.items[0], strerror(errno));
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        pipe_fds[0] = -1;
        pipe_fds[1] = -1;
        return false;
    }
    return true;
}

static void process_child_exec(const VmProcessSpec *spec, int redirect_fd, FILE *out_fp, FILE *err_fp) {
    if (spec->capture) {
        dup2(fileno(out_fp), STDOUT_FILENO);
        dup2(fileno(err_fp), STDERR_FILENO);
    } else if (spec->redirect.kind != DS_REDIRECT_NONE) {
        if (spec->redirect.kind == DS_REDIRECT_OUT || spec->redirect.kind == DS_REDIRECT_OUT_APPEND) dup2(redirect_fd, STDOUT_FILENO);
        else if (spec->redirect.kind == DS_REDIRECT_ERR || spec->redirect.kind == DS_REDIRECT_ERR_APPEND) dup2(redirect_fd, STDERR_FILENO);
        else { dup2(redirect_fd, STDOUT_FILENO); dup2(redirect_fd, STDERR_FILENO); }
    }
    if (redirect_fd >= 0) close(redirect_fd);
    execvp(spec->argv.items[0], spec->argv.items);
    int exec_errno = errno;
    if (spec->exec_error_fd >= 0) {
        ssize_t ignored = write(spec->exec_error_fd, &exec_errno, sizeof(exec_errno));
        (void)ignored;
        close(spec->exec_error_fd);
    }
    if (spec->capture) fprintf(stderr, "ds: failed to launch command `%s`: %s\n", spec->argv.items[0], strerror(exec_errno));
    _exit(127);
}

static bool process_execute(Vm *vm, VmProcessSpec *spec, VmProcessResult *result) {
    process_result_init(result);
    int redirect_fd = -1;
    FILE *out_fp = NULL;
    FILE *err_fp = NULL;
    int exec_error_pipe[2] = {-1, -1};
    spec->exec_error_fd = -1;

    if (!spec->capture && spec->redirect.kind != DS_REDIRECT_NONE) {
        if (!open_redirect_target(vm, &spec->redirect, &redirect_fd)) return false;
    }

    if (spec->capture) {
        out_fp = tmpfile();
        err_fp = tmpfile();
        if (!out_fp || !err_fp) {
            ds_diag_error(vm->diag, spec->span, "failed to create command capture temporary files: %s", strerror(errno));
            if (out_fp) fclose(out_fp);
            if (err_fp) fclose(err_fp);
            return false;
        }
    }

    if (!process_exec_error_pipe(vm, spec, exec_error_pipe)) {
        if (redirect_fd >= 0) close(redirect_fd);
        if (out_fp) fclose(out_fp);
        if (err_fp) fclose(err_fp);
        return false;
    }
    spec->exec_error_fd = exec_error_pipe[1];

    pid_t pid = fork();
    if (pid < 0) {
        ds_diag_error(vm->diag, spec->span, "failed to launch command `%s`: %s", spec->argv.items[0], strerror(errno));
        if (redirect_fd >= 0) close(redirect_fd);
        if (out_fp) fclose(out_fp);
        if (err_fp) fclose(err_fp);
        close(exec_error_pipe[0]);
        close(exec_error_pipe[1]);
        spec->exec_error_fd = -1;
        return false;
    }

    if (pid == 0) {
        close(exec_error_pipe[0]);
        process_child_exec(spec, redirect_fd, out_fp, err_fp);
    }

    close(exec_error_pipe[1]);
    spec->exec_error_fd = -1;
    if (redirect_fd >= 0) close(redirect_fd);
    int exec_errno = 0;
    ssize_t exec_error_len = read(exec_error_pipe[0], &exec_errno, sizeof(exec_errno));
    close(exec_error_pipe[0]);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            ds_diag_error(vm->diag, spec->span, "failed waiting for command `%s`: %s", spec->argv.items[0], strerror(errno));
            if (out_fp) fclose(out_fp);
            if (err_fp) fclose(err_fp);
            return false;
        }
    }
    result->code = process_status_code(status);

    if (!spec->capture && exec_error_len == (ssize_t)sizeof(exec_errno)) {
        ds_diag_error(vm->diag, spec->span, "failed to launch command `%s`: %s", spec->argv.items[0], strerror(exec_errno));
        return true;
    }

    if (spec->capture) {
        if (!read_file_into_string(out_fp, &result->stdout_text) || !read_file_into_string(err_fp, &result->stderr_text)) {
            ds_diag_error(vm->diag, spec->span, "failed to read command capture output");
            fclose(out_fp);
            fclose(err_fp);
            return false;
        }
        fclose(out_fp);
        fclose(err_fp);
    }
    return true;
}

static int run_command(Vm *vm, Instr *ins) {
    if (ins->word_count == 0) return 0;
    VmProcessSpec spec;
    if (!process_spec_from_instr(vm, ins, false, &spec)) return 1;
    VmProcessResult result;
    bool ok = process_execute(vm, &spec, &result);
    int code = ok ? result.code : 1;
    process_result_free(&result);
    process_spec_free(&spec);
    return code;
}

static int run_capture(Vm *vm, Instr *ins, DsValue *out_value) {
    *out_value = ds_value_null();
    if (ins->word_count == 0) return 1;
    VmProcessSpec spec;
    if (!process_spec_from_instr(vm, ins, true, &spec)) return 1;
    VmProcessResult result;
    bool ok = process_execute(vm, &spec, &result);
    process_spec_free(&spec);
    if (!ok) {
        process_result_free(&result);
        return 1;
    }
    *out_value = ds_value_command_result_take(&result.stdout_text, &result.stderr_text, result.code);
    return 0;
}

static bool command_result_field(Vm *vm, const DsValue *value, const char *field, DsSpan span, DsValue *out) {
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
        ds_diag_error(vm->diag, span, "field access is only supported on command results and maps in v0.10.0");
        return false;
    }
    DsStr field_view = {(char *)field, strlen(field)};
    const DsCommandResultField *desc = ds_command_result_field_lookup(field_view);
    if (desc && desc->kind == DS_COMMAND_RESULT_FIELD_STRING && strcmp(desc->name, "stdout") == 0) { ds_string_from_range(&out->as.string, value->as.command_result.stdout_text.data ? value->as.command_result.stdout_text.data : "", value->as.command_result.stdout_text.len); out->kind = DS_VALUE_STRING; return true; }
    if (desc && desc->kind == DS_COMMAND_RESULT_FIELD_STRING && strcmp(desc->name, "stderr") == 0) { ds_string_from_range(&out->as.string, value->as.command_result.stderr_text.data ? value->as.command_result.stderr_text.data : "", value->as.command_result.stderr_text.len); out->kind = DS_VALUE_STRING; return true; }
    if (desc && desc->kind == DS_COMMAND_RESULT_FIELD_INT) { *out = ds_value_int(value->as.command_result.code); return true; }
    if (desc && desc->kind == DS_COMMAND_RESULT_FIELD_BOOL && strcmp(desc->name, "ok") == 0) { *out = ds_value_bool(value->as.command_result.code == 0); return true; }
    if (desc && desc->kind == DS_COMMAND_RESULT_FIELD_BOOL && strcmp(desc->name, "failed") == 0) { *out = ds_value_bool(value->as.command_result.code != 0); return true; }
    ds_diag_error(vm->diag, span, "unknown command result field `%s`", field);
    return false;
}

static bool ensure_regs(Vm *vm) {
    if (vm->program->next_reg <= 0) vm->program->next_reg = 1;
    vm->regs = (DsValue *)ds_xcalloc((size_t)vm->program->next_reg, sizeof(DsValue));
    for (int i = 0; i < vm->program->next_reg; i++) vm->regs[i] = ds_value_null();
    return true;
}

static bool vm_string_arg(Vm *vm, Instr *ins, size_t index, const char **out, size_t *len) {
    if (index >= ins->arg_count) return false;
    DsValue *v = &vm->regs[ins->args[index]];
    if (v->kind != DS_VALUE_STRING) {
        ds_diag_error(vm->diag, ins->span, "standard-library helper `%s` expects string arguments", ins->name ? ins->name : "<helper>");
        return false;
    }
    *out = v->as.string.data ? v->as.string.data : "";
    *len = v->as.string.len;
    if (memchr(*out, '\0', *len)) {
        ds_diag_error(vm->diag, ins->span, "standard-library helper `%s` does not support embedded NUL bytes", ins->name ? ins->name : "<helper>");
        return false;
    }
    return true;
}

static char *vm_string_arg_dup(Vm *vm, Instr *ins, size_t index) {
    const char *s = NULL; size_t len = 0;
    if (!vm_string_arg(vm, ins, index, &s, &len)) return NULL;
    return ds_str_dup_range(s, len);
}

static bool vm_valid_env_name(const char *name) {
    if (!name || !name[0]) return false;
    unsigned char c = (unsigned char)name[0];
    if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_')) return false;
    for (size_t i = 1; name[i]; i++) {
        c = (unsigned char)name[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return false;
    }
    return true;
}

static bool vm_require_env_name(Vm *vm, Instr *ins, const char *name) {
    if (vm_valid_env_name(name)) return true;
    ds_diag_error(vm->diag, ins->span, "invalid environment variable name `%s` in v0.11.0", name ? name : "");
    return false;
}

static bool value_string_from_owned_cstr(DsValue *out, char *text) {
    DsString s;
    ds_string_init(&s);
    if (!ds_string_from_cstr(&s, text ? text : "")) { free(text); return false; }
    free(text);
    *out = ds_value_string_take(&s);
    return true;
}

static int cmp_cstr_ptr(const void *a, const void *b) {
    const char *const *sa = (const char *const *)a;
    const char *const *sb = (const char *const *)b;
    return strcmp(*sa, *sb);
}

static bool array_push_string(DsValue *array, const char *data, size_t len) {
    DsString s;
    ds_string_from_range(&s, data ? data : "", len);
    DsValue *item = (DsValue *)ds_xcalloc(1, sizeof(DsValue));
    *item = ds_value_string_take(&s);
    return ds_array_push(&array->as.array, item);
}

static bool is_executable_file(const char *path) {
    struct stat st;
    return path && stat(path, &st) == 0 && !S_ISDIR(st.st_mode) && access(path, X_OK) == 0;
}

static bool read_path_to_string_msg(Vm *vm, Instr *ins, const char *path, const char *what, DsValue *out) {
    FILE *fp = fopen(path, "rb");
    if (!fp) { ds_diag_error(vm->diag, ins->span, "failed to read %s `%s`: %s", what, path, strerror(errno)); return false; }
    DsString s;
    ds_string_init(&s);
    char buf[4096];
    size_t n = 0;
    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) {
        if (memchr(buf, '\0', n)) { ds_diag_error(vm->diag, ins->span, "%s `%s` contains embedded NUL bytes", what, path); fclose(fp); ds_string_free(&s); return false; }
        ds_string_append_range(&s, buf, n);
    }
    if (ferror(fp)) { ds_diag_error(vm->diag, ins->span, "failed to read %s `%s`: %s", what, path, strerror(errno)); fclose(fp); ds_string_free(&s); return false; }
    fclose(fp);
    *out = ds_value_string_take(&s);
    return true;
}

static bool read_path_to_string(Vm *vm, Instr *ins, const char *path, DsValue *out) {
    return read_path_to_string_msg(vm, ins, path, "file", out);
}

static bool write_string_to_path(Vm *vm, Instr *ins, const char *path, const char *text, size_t len, bool append) {
    FILE *fp = fopen(path, append ? "ab" : "wb");
    const char *op = append ? "append" : "write";
    if (!fp) { ds_diag_error(vm->diag, ins->span, "failed to %s file `%s`: %s", op, path, strerror(errno)); return false; }
    if (len && fwrite(text, 1, len, fp) != len) { ds_diag_error(vm->diag, ins->span, "failed to %s file `%s`: %s", op, path, strerror(errno)); fclose(fp); return false; }
    if (fclose(fp) != 0) { ds_diag_error(vm->diag, ins->span, "failed to %s file `%s`: %s", op, path, strerror(errno)); return false; }
    return true;
}

static char *path_join_parts(Vm *vm, Instr *ins) {
    DsString s;
    ds_string_init(&s);
    for (size_t i = 0; i < ins->arg_count; i++) {
        const char *part = NULL; size_t len = 0;
        if (!vm_string_arg(vm, ins, i, &part, &len)) { ds_string_free(&s); return NULL; }
        if (i == 0) ds_string_append_range(&s, part, len);
        else {
            while (s.len > 0 && s.data[s.len - 1] == '/') s.len--;
            while (len > 0 && *part == '/') { part++; len--; }
            ds_string_append_char(&s, '/');
            ds_string_append_range(&s, part, len);
        }
    }
    char *out = ds_str_dup_range(s.data ? s.data : "", s.len);
    ds_string_free(&s);
    return out;
}

static bool vm_stdlib_call(Vm *vm, Instr *ins, DsValue *out) {
    *out = ds_value_null();
    const char *name = ins->name ? ins->name : "";
    DsStr helper_name = {(char *)name, strlen(name)};
    const DsStdlibHelper *helper = ds_stdlib_lookup(helper_name);
    if (!helper) {
        ds_diag_error(vm->diag, ins->span, "unknown standard-library helper `%s`", name);
        return false;
    }
    if (strcmp(name, "file.exists") == 0 || strcmp(name, "file.is_file") == 0 || strcmp(name, "dir.exists") == 0) {
        char *path = vm_string_arg_dup(vm, ins, 0); if (!path) return false;
        struct stat st;
        bool ok = stat(path, &st) == 0;
        bool result = false;
        if (!ok && errno != ENOENT && errno != ENOTDIR) { ds_diag_error(vm->diag, ins->span, "failed to stat `%s`: %s", path, strerror(errno)); free(path); return false; }
        if (strcmp(name, "file.exists") == 0) result = ok;
        else if (strcmp(name, "file.is_file") == 0) result = ok && S_ISREG(st.st_mode);
        else result = ok && S_ISDIR(st.st_mode);
        free(path); *out = ds_value_bool(result); return true;
    }
    if (strcmp(name, "file.read") == 0) { char *path = vm_string_arg_dup(vm, ins, 0); if (!path) return false; bool ok = read_path_to_string(vm, ins, path, out); free(path); return ok; }
    if (strcmp(name, "file.write") == 0 || strcmp(name, "file.append") == 0) { char *path = vm_string_arg_dup(vm, ins, 0); const char *txt = NULL; size_t len = 0; if (!path || !vm_string_arg(vm, ins, 1, &txt, &len)) { free(path); return false; } bool ok = write_string_to_path(vm, ins, path, txt, len, strcmp(name, "file.append") == 0); free(path); return ok; }
    if (strcmp(name, "path.cwd") == 0) { char *cwd = getcwd(NULL, 0); if (!cwd) { ds_diag_error(vm->diag, ins->span, "failed to get current directory: %s", strerror(errno)); return false; } return value_string_from_owned_cstr(out, cwd); }
    if (strcmp(name, "path.join") == 0) { char *joined = path_join_parts(vm, ins); if (!joined) return false; return value_string_from_owned_cstr(out, joined); }
    if (strcmp(name, "path.basename") == 0 || strcmp(name, "path.dirname") == 0 || strcmp(name, "path.ext") == 0) {
        char *path = vm_string_arg_dup(vm, ins, 0); if (!path) return false; char *res = NULL;
        if (strcmp(name, "path.basename") == 0) { char *slash = strrchr(path, '/'); res = ds_str_dup_range(slash ? slash + 1 : path, strlen(slash ? slash + 1 : path)); }
        else if (strcmp(name, "path.dirname") == 0) { char *slash = strrchr(path, '/'); if (!slash) res = ds_str_dup_range(".", 1); else if (slash == path) res = ds_str_dup_range("/", 1); else res = ds_str_dup_range(path, (size_t)(slash - path)); }
        else { char *base = strrchr(path, '/'); base = base ? base + 1 : path; char *dot = strrchr(base, '.'); if (!dot || dot == base) res = ds_str_dup_range("", 0); else res = ds_str_dup_range(dot, strlen(dot)); }
        free(path); return value_string_from_owned_cstr(out, res);
    }
    if (strcmp(name, "cmd.exists") == 0 || strcmp(name, "cmd.require") == 0) {
        char *cmd = vm_string_arg_dup(vm, ins, 0); if (!cmd) return false;
        char *path = getenv("PATH"); bool found = false;
        if (strchr(cmd, '/')) found = is_executable_file(cmd);
        else if (path) { char *copy = strdup(path); for (char *save = NULL, *dir = strtok_r(copy, ":", &save); dir; dir = strtok_r(NULL, ":", &save)) { DsString full; ds_string_init(&full); ds_string_append_cstr(&full, *dir ? dir : "."); ds_string_append_char(&full, '/'); ds_string_append_cstr(&full, cmd); if (is_executable_file(full.data)) found = true; ds_string_free(&full); if (found) break; } free(copy); }
        if (!found && strcmp(name, "cmd.require") == 0) { ds_diag_error(vm->diag, ins->span, "required command `%s` was not found on PATH", cmd); free(cmd); return false; }
        free(cmd); *out = strcmp(name, "cmd.exists") == 0 ? ds_value_bool(found) : ds_value_null(); return true;
    }
    if (strcmp(name, "env.get") == 0) { char *key = vm_string_arg_dup(vm, ins, 0); if (!key) return false; if (!vm_require_env_name(vm, ins, key)) { free(key); return false; } char *val = getenv(key); if (!val && ins->arg_count == 2) { const char *def = NULL; size_t len = 0; if (!vm_string_arg(vm, ins, 1, &def, &len)) { free(key); return false; } DsString s; ds_string_from_range(&s, def, len); *out = ds_value_string_take(&s); free(key); return true; } DsString s; ds_string_from_cstr(&s, val ? val : ""); *out = ds_value_string_take(&s); free(key); return true; }
    if (strcmp(name, "env.set") == 0) { char *key = vm_string_arg_dup(vm, ins, 0); char *val = vm_string_arg_dup(vm, ins, 1); if (!key || !val) { free(key); free(val); return false; } if (!vm_require_env_name(vm, ins, key)) { free(key); free(val); return false; } if (setenv(key, val, 1) != 0) { ds_diag_error(vm->diag, ins->span, "failed to set environment `%s`: %s", key, strerror(errno)); free(key); free(val); return false; } free(key); free(val); return true; }
    if (strcmp(name, "env.unset") == 0) { char *key = vm_string_arg_dup(vm, ins, 0); if (!key) return false; if (!vm_require_env_name(vm, ins, key)) { free(key); return false; } if (unsetenv(key) != 0) { ds_diag_error(vm->diag, ins->span, "failed to unset environment `%s`: %s", key, strerror(errno)); free(key); return false; } free(key); return true; }
    if (strcmp(name, "glob") == 0 || strcmp(name, "glob!") == 0) { char *pat = vm_string_arg_dup(vm, ins, 0); if (!pat) return false; if (strstr(pat, "**")) { ds_diag_error(vm->diag, ins->span, "recursive `**` glob patterns are deferred in v0.11.0"); free(pat); return false; } glob_t g; memset(&g, 0, sizeof(g)); int grc = glob(pat, 0, NULL, &g); DsValue arr = ds_value_null(); arr.kind = DS_VALUE_ARRAY; ds_array_init(&arr.as.array); if (grc == GLOB_NOMATCH) { if (strcmp(name, "glob!") == 0) { ds_diag_error(vm->diag, ins->span, "required glob `%s` had no matches", pat); free(pat); globfree(&g); ds_value_free(&arr); return false; } } else if (grc != 0) { ds_diag_error(vm->diag, ins->span, "failed to evaluate glob `%s`", pat); free(pat); globfree(&g); ds_value_free(&arr); return false; } else { qsort(g.gl_pathv, g.gl_pathc, sizeof(char *), cmp_cstr_ptr); for (size_t i = 0; i < g.gl_pathc; i++) array_push_string(&arr, g.gl_pathv[i], strlen(g.gl_pathv[i])); } free(pat); globfree(&g); *out = arr; return true; }
    if (strcmp(name, "lines") == 0) { char *path = vm_string_arg_dup(vm, ins, 0); if (!path) return false; DsValue text = ds_value_null(); if (!read_path_to_string_msg(vm, ins, path, "lines from", &text)) { free(path); return false; } free(path); DsValue arr = ds_value_null(); arr.kind = DS_VALUE_ARRAY; ds_array_init(&arr.as.array); size_t start = 0; for (size_t i = 0; i < text.as.string.len; i++) { if (text.as.string.data[i] == '\n') { size_t end = i > start && text.as.string.data[i - 1] == '\r' ? i - 1 : i; array_push_string(&arr, text.as.string.data + start, end - start); start = i + 1; } } if (start < text.as.string.len) array_push_string(&arr, text.as.string.data + start, text.as.string.len - start); ds_value_free(&text); *out = arr; return true; }
    ds_diag_error(vm->diag, ins->span, "unknown standard-library helper `%s`", name);
    return false;
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
            case OP_CALL: {
                size_t target_ip = 0;
                if (!call_function(&vm, ins, ip + 1, &target_ip)) { rc = 1; goto done; }
                ip = target_ip;
                break;
            }
            case OP_STDLIB_CALL: {
                DsValue value = ds_value_null();
                if (!vm_stdlib_call(&vm, ins, &value)) { rc = 1; goto done; }
                set_reg(&vm, ins->dst, value);
                ip++;
                break;
            }
            case OP_ARRAY_LITERAL: {
                DsValue array = ds_value_null();
                array.kind = DS_VALUE_ARRAY;
                ds_array_init(&array.as.array);
                for (size_t i = 0; i < ins->arg_count; i++) {
                    DsValue *item = (DsValue *)ds_xcalloc(1, sizeof(DsValue));
                    *item = ds_value_copy(&vm.regs[ins->args[i]]);
                    ds_array_push(&array.as.array, item);
                }
                set_reg(&vm, ins->dst, array);
                ip++;
                break;
            }
            case OP_MAP_LITERAL: {
                DsValue map = ds_value_null();
                map.kind = DS_VALUE_MAP;
                ds_map_init(&map.as.map);
                for (size_t i = 0; i < ins->arg_count; i++) ds_map_set(&map.as.map, ins->words[i], ds_value_copy(&vm.regs[ins->args[i]]));
                set_reg(&vm, ins->dst, map);
                ip++;
                break;
            }
            case OP_GET_INDEX: {
                DsValue *obj = &vm.regs[ins->a];
                DsValue *idx = &vm.regs[ins->b];
                if (obj->kind == DS_VALUE_ARRAY) {
                    if (idx->kind != DS_VALUE_INT) { ds_diag_error(diag, ins->span, "array index must be an int"); rc = 1; goto done; }
                    if (idx->as.integer < 0 || (size_t)idx->as.integer >= obj->as.array.len) { ds_diag_error(diag, ins->span, "array index %lld out of range", (long long)idx->as.integer); rc = 1; goto done; }
                    set_reg(&vm, ins->dst, ds_value_copy((DsValue *)obj->as.array.items[idx->as.integer]));
                } else if (obj->kind == DS_VALUE_MAP) {
                    DsString key;
                    ds_value_to_string(idx, &key);
                    DsStr key_view = {key.data ? key.data : "", key.len};
                    DsValue *found = ds_map_get(&obj->as.map, key_view);
                    if (!found) { ds_diag_error(diag, ins->span, "missing map key `%.*s`", (int)key_view.len, key_view.data); ds_string_free(&key); rc = 1; goto done; }
                    set_reg(&vm, ins->dst, ds_value_copy(found));
                    ds_string_free(&key);
                } else { ds_diag_error(diag, ins->span, "indexing requires an array or map"); rc = 1; goto done; }
                ip++;
                break;
            }
            case OP_PUSH_ARRAY: {
                DsValue *array = lookup_var_ref(&vm, ins->name);
                if (!array) { ds_diag_error(diag, ins->span, "unknown array `%s`", ins->name); rc = 1; goto done; }
                if (array->kind != DS_VALUE_ARRAY) { ds_diag_error(diag, ins->span, "`push` requires an array variable"); rc = 1; goto done; }
                DsValue *item = (DsValue *)ds_xcalloc(1, sizeof(DsValue));
                *item = ds_value_copy(&vm.regs[ins->a]);
                ds_array_push(&array->as.array, item);
                ip++;
                break;
            }
            case OP_FOR_ARRAY: {
                DsValue *iter = &vm.regs[ins->a];
                if (iter->kind != DS_VALUE_ARRAY) { ds_diag_error(diag, ins->span, "for loop iterable must be an array"); rc = 1; goto done; }
                if (!ins->loop_active) { ins->loop_active = true; ins->loop_index = 0; }
                if (ins->loop_index >= iter->as.array.len) { ins->loop_active = false; ip = (size_t)ins->target; break; }
                vm_push_scope(&vm);
                DsStr key = {ins->name, strlen(ins->name)};
                ds_map_set(&vm.scope->vars, key, ds_value_copy((DsValue *)iter->as.array.items[ins->loop_index]));
                ins->loop_index++;
                ip++;
                break;
            }
            case OP_RETURN_FUNC: {
                size_t return_ip = 0;
                vm_pop_scope(&vm);
                if (!vm_pop_return(&vm, &return_ip)) { rc = 1; goto done; }
                ip = return_ip;
                break;
            }
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
    free(vm.return_ips);
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
