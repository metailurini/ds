#define _POSIX_C_SOURCE 200809L

#include "vm_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

void program_init(Program *p) { memset(p, 0, sizeof(*p)); }

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

void program_free(Program *p) {
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

bool decode_string_text(DsStr text, DsString *out);

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

bool decode_string_text(DsStr text, DsString *out) {
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
        case DS_LOWER_STMT_ASSERT: {
            int cond = compile_expr(p, stmt->as.assert_stmt.condition);
            Instr ins = {0};
            ins.op = OP_ASSERT;
            ins.span = stmt->span;
            ins.a = cond;
            emit_instr(p, ins);
            break;
        }
    }
}

bool compile_program(const DsLowerProgram *lowered, Program *p, DsDiag *diag) {
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
