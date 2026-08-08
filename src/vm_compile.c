#define _POSIX_C_SOURCE 200809L

#include "ds_regex.h"
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

static OpCmp op_cmp_from_str(const char *s, size_t len) {
    if (len == 1) {
        switch (s[0]) {
            case '+': return OP_CMP_ADD;
            case '-': return OP_CMP_SUB;
            case '*': return OP_CMP_MUL;
            case '/': return OP_CMP_DIV;
            case '%': return OP_CMP_MOD;
            case '>': return OP_CMP_GT;
            case '<': return OP_CMP_LT;
        }
    } else if (len == 2) {
        if (s[0] == '=' && s[1] == '=') return OP_CMP_EQ_EQ;
        if (s[0] == '!' && s[1] == '=') return OP_CMP_NE;
        if (s[0] == '>' && s[1] == '=') return OP_CMP_GE;
        if (s[0] == '<' && s[1] == '=') return OP_CMP_LE;
        if (s[0] == '*' && s[1] == '*') return OP_CMP_POW;
    } else if (len == 3) {
        if (s[0] == '=' && s[1] == '=' && s[2] == '=') return OP_CMP_EQ_EQ_EQ;
        if (s[0] == '!' && s[1] == '=' && s[2] == '=') return OP_CMP_NE_EQ;
    }
    return OP_CMP_ADD;
}

static void instr_free(Instr *ins) {
    free(ins->name);
    free(ins->value_name);
    free(ins->cmp);
    free(ins->field);
    free(ins->args);
    for (size_t i = 0; i < ins->word_count; i++) free(ins->words[i].data);
    free(ins->words);
    free(ins->stage_word_counts);
    free(ins->redirect.target.data);
    ds_map_sorted_keys_free(ins->loop_keys, ins->loop_key_count);
}

static void copy_command_to_instr(Instr *ins, const DsCommand *command) {
    ins->stage_count = command->stages.len;
    ins->stage_word_counts = (size_t *)ds_xcalloc(ins->stage_count ? ins->stage_count : 1, sizeof(size_t));
    ins->word_count = 0;
    for (size_t s = 0; s < command->stages.len; s++) {
        ins->stage_word_counts[s] = command->stages.items[s].words.len;
        ins->word_count += command->stages.items[s].words.len;
    }
    ins->words = (DsStr *)ds_xcalloc(ins->word_count ? ins->word_count : 1, sizeof(DsStr));
    size_t idx = 0;
    for (size_t s = 0; s < command->stages.len; s++) {
        const DsWordVec *words = &command->stages.items[s].words;
        for (size_t i = 0; i < words->len; i++) {
            DsStr w = words->items[i].text;
            ins->words[idx].data = ds_str_dup_range(w.data, w.len);
            ins->words[idx].len = w.len;
            idx++;
        }
    }
    ins->redirect.kind = command->redirect.kind;
    ins->redirect.op_span = command->redirect.op_span;
    ins->redirect.target_span = command->redirect.target_span;
    if (command->redirect.target.len > 0) {
        ins->redirect.target.data = ds_str_dup_range(command->redirect.target.data, command->redirect.target.len);
        ins->redirect.target.len = command->redirect.target.len;
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
    for (size_t i = 0; i < p->loop_len; i++) {
        free(p->loop_stack[i].breaks);
        free(p->loop_stack[i].continues);
    }
    free(p->consts);
    free(p->instrs);
    free(p->functions);
    free(p->loop_stack);
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
        meta->params[i].expected_kind = fn->params.items[i].has_default ? fn->params.items[i].default_kind : fn->params.items[i].inferred_kind;
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

static void emit_load_const(Program *p, DsSpan span, int dst, DsValue value) {
    Instr ins = {0};
    ins.op = OP_LOAD_CONST;
    ins.span = span;
    ins.dst = dst;
    ins.a = add_const(p, value);
    emit_instr(p, ins);
}

static int compile_const(Program *p, DsSpan span, DsValue value) {
    int reg = new_reg(p);
    emit_load_const(p, span, reg, value);
    return reg;
}

static void loop_patch_vec_push(size_t **items, size_t *len, size_t *cap, size_t value) {
    if (*len == *cap) {
        *cap = *cap ? *cap * 2 : 4;
        *items = (size_t *)ds_xrealloc(*items, *cap * sizeof(size_t));
    }
    (*items)[(*len)++] = value;
}

static LoopPatch *push_loop(Program *p, size_t start, int base_scope_depth) {
    if (p->loop_len == p->loop_cap) {
        p->loop_cap = p->loop_cap ? p->loop_cap * 2 : 4;
        p->loop_stack = (LoopPatch *)ds_xrealloc(p->loop_stack, p->loop_cap * sizeof(LoopPatch));
    }
    LoopPatch *loop = &p->loop_stack[p->loop_len++];
    memset(loop, 0, sizeof(*loop));
    loop->start = start;
    loop->base_scope_depth = base_scope_depth;
    return loop;
}

static LoopPatch *current_loop(Program *p) {
    return p->loop_len ? &p->loop_stack[p->loop_len - 1] : NULL;
}

static void pop_loop(Program *p, size_t end) {
    if (!p->loop_len) return;
    LoopPatch *loop = &p->loop_stack[p->loop_len - 1];
    for (size_t i = 0; i < loop->break_len; i++) p->instrs[loop->breaks[i]].target = (int)end;
    for (size_t i = 0; i < loop->continue_len; i++) p->instrs[loop->continues[i]].target = (int)loop->start;
    free(loop->breaks);
    free(loop->continues);
    p->loop_len--;
}

bool decode_string_text(DsStr text, DsString *out) {
    ds_string_init(out);
    if (text.len >= 6 && memcmp(text.data, "\"\"\"", 3) == 0 && memcmp(text.data + text.len - 3, "\"\"\"", 3) == 0) {
        ds_string_append_range(out, text.data + 3, text.len - 6);
        return true;
    }
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

static int compile_interp_expr(Program *p, const DsLowerExpr *expr) {
    int r = new_reg(p);
    Instr ins = {0};
    ins.op = OP_INTERP_JOIN;
    ins.span = expr->span;
    ins.dst = r;
    ins.arg_count = expr->as.interp.parts.len;
    ins.args = (int *)ds_xcalloc(ins.arg_count ? ins.arg_count : 1, sizeof(int));
    for (size_t i = 0; i < expr->as.interp.parts.len; i++) ins.args[i] = compile_expr(p, expr->as.interp.parts.items[i]);
    emit_instr(p, ins);
    return r;
}

static int compile_expr(Program *p, const DsLowerExpr *expr) {
    switch (expr->kind) {
        case DS_LOWER_EXPR_STRING:
            return compile_string_expr(p, expr);
        case DS_LOWER_EXPR_INTERP:
            return compile_interp_expr(p, expr);
        case DS_LOWER_EXPR_INT: {
            char *tmp = ds_str_dup_range(expr->as.text.data, expr->as.text.len);
            int64_t value = strtoll(tmp, NULL, 10);
            free(tmp);
            return compile_const(p, expr->span, ds_value_int(value));
        }
        case DS_LOWER_EXPR_BOOL:
            return compile_const(p, expr->span, ds_value_bool(expr->as.boolean));
        case DS_LOWER_EXPR_REGEX: {
            DsString s; ds_string_init(&s);
            ds_string_append_range(&s, expr->as.regex.data, expr->as.regex.len);
            return compile_const(p, expr->span, ds_value_string_take(&s));
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
            copy_command_to_instr(&ins, &expr->as.run);
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
            if (expr->as.unary.op.len == 1 && expr->as.unary.op.data[0] == '-') {
                int zero = compile_const(p, expr->span, ds_value_int(0));
                ins.op = OP_BINARY;
                ins.b = right;
                ins.cmp = ds_str_dup_range("-", 1);
            ins.cmp_enum = OP_CMP_SUB;
                ins.a = zero;
            } else {
                ins.op = OP_NOT;
                ins.a = right;
            }
            ins.span = expr->span;
            ins.dst = r;
            emit_instr(p, ins);
            return r;
        }
        case DS_LOWER_EXPR_BINARY: {
            if ((expr->as.binary.op.len == 2 && memcmp(expr->as.binary.op.data, "&&", 2) == 0) ||
                (expr->as.binary.op.len == 2 && memcmp(expr->as.binary.op.data, "||", 2) == 0)) {
                bool is_and = expr->as.binary.op.data[0] == '&';
                int r = new_reg(p);
                int left = compile_expr(p, expr->as.binary.left);
                Instr left_false = {0};
                left_false.op = OP_JUMP_IF_FALSE;
                left_false.span = expr->as.binary.left->span;
                left_false.a = left;
                size_t left_false_pos = emit_instr(p, left_false);

                if (!is_and) {
                    emit_load_const(p, expr->span, r, ds_value_bool(true));
                    Instr jump_end = {0};
                    jump_end.op = OP_JUMP;
                    jump_end.span = expr->span;
                    size_t true_jump_pos = emit_instr(p, jump_end);
                    p->instrs[left_false_pos].target = (int)p->instr_len;
                    int right = compile_expr(p, expr->as.binary.right);
                    Instr right_false = {0};
                    right_false.op = OP_JUMP_IF_FALSE;
                    right_false.span = expr->as.binary.right->span;
                    right_false.a = right;
                    size_t right_false_pos = emit_instr(p, right_false);
                    emit_load_const(p, expr->span, r, ds_value_bool(true));
                    Instr jump_end_right = {0};
                    jump_end_right.op = OP_JUMP;
                    jump_end_right.span = expr->span;
                    size_t right_true_jump_pos = emit_instr(p, jump_end_right);
                    p->instrs[right_false_pos].target = (int)p->instr_len;
                    emit_load_const(p, expr->span, r, ds_value_bool(false));
                    p->instrs[true_jump_pos].target = (int)p->instr_len;
                    p->instrs[right_true_jump_pos].target = (int)p->instr_len;
                    return r;
                }

                int right = compile_expr(p, expr->as.binary.right);
                Instr right_false = {0};
                right_false.op = OP_JUMP_IF_FALSE;
                right_false.span = expr->as.binary.right->span;
                right_false.a = right;
                size_t right_false_pos = emit_instr(p, right_false);
                emit_load_const(p, expr->span, r, ds_value_bool(true));
                Instr jump_end = {0};
                jump_end.op = OP_JUMP;
                jump_end.span = expr->span;
                size_t true_jump_pos = emit_instr(p, jump_end);
                p->instrs[left_false_pos].target = (int)p->instr_len;
                p->instrs[right_false_pos].target = (int)p->instr_len;
                emit_load_const(p, expr->span, r, ds_value_bool(false));
                p->instrs[true_jump_pos].target = (int)p->instr_len;
                return r;
            }
            int left = compile_expr(p, expr->as.binary.left);
            int right = -1;
            bool regex_literal_rhs = expr->as.binary.op.len == 7 && memcmp(expr->as.binary.op.data, "matches", 7) == 0 &&
                                     expr->as.binary.right->kind == DS_LOWER_EXPR_REGEX;
            bool regex_insensitive = false;
            if (regex_literal_rhs) {
                DsStr raw_pattern = {0};
                if (ds_regex_literal_parts(expr->as.binary.right->as.regex, &raw_pattern, &regex_insensitive)) {
                    DsString decoded;
                    if (ds_regex_decode_literal_pattern(raw_pattern, &decoded)) {
                        right = compile_const(p, expr->as.binary.right->span, ds_value_string_take(&decoded));
                    }
                }
            }
            if (right < 0) right = compile_expr(p, expr->as.binary.right);
            int r = new_reg(p);
            Instr ins = {0};
            if (expr->as.binary.op.len == 2 && memcmp(expr->as.binary.op.data, "in", 2) == 0) ins.op = OP_MEMBERSHIP;
            else if (expr->as.binary.op.len == 7 && memcmp(expr->as.binary.op.data, "matches", 7) == 0) ins.op = OP_REGEX_MATCH;
            else ins.op = (expr->as.binary.op.len == 1 && (expr->as.binary.op.data[0] == '+' || expr->as.binary.op.data[0] == '-' || expr->as.binary.op.data[0] == '*' || expr->as.binary.op.data[0] == '/' || expr->as.binary.op.data[0] == '%')) ||
                     (expr->as.binary.op.len == 2 && memcmp(expr->as.binary.op.data, "**", 2) == 0) ? OP_BINARY : OP_COMPARE;
            ins.span = expr->span;
            ins.dst = r;
            ins.a = left;
            ins.b = right;
            ins.regex_case_insensitive = regex_insensitive;
            ins.cmp = ds_str_dup_range(expr->as.binary.op.data, expr->as.binary.op.len);
            ins.cmp_enum = op_cmp_from_str(expr->as.binary.op.data, expr->as.binary.op.len);
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
        case DS_LOWER_EXPR_RANGE:
            return compile_expr(p, expr->as.range.start);
        case DS_LOWER_EXPR_CALL:
        {
            int r = new_reg(p);
            Instr ins = {0};
            int fn_index = find_function_meta(p, expr->as.call.name);
            ins.op = fn_index >= 0 ? OP_CALL : OP_STDLIB_CALL;
            ins.span = expr->span;
            ins.dst = r;
            ins.target = fn_index;
            if (fn_index < 0) ins.name = ds_str_dup_range(expr->as.call.name.data, expr->as.call.name.len);
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
    p->scope_depth++;
    compile_block(p, block);
    Instr pop = {0};
    pop.op = OP_POP_SCOPE;
    pop.span = block->span;
    emit_instr(p, pop);
    p->scope_depth--;
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
        case DS_LOWER_STMT_ASSIGN: {
            bool env_assign = stmt->as.assign_stmt.name.len > 4 && memcmp(stmt->as.assign_stmt.name.data, "env.", 4) == 0;
            int src = -1;
            if (stmt->as.assign_stmt.op == DS_LOWER_ASSIGN_SET) {
                src = compile_expr(p, stmt->as.assign_stmt.value);
            } else {
                int left = new_reg(p);
                Instr load = {0};
                load.op = OP_LOAD_VAR;
                load.span = stmt->span;
                load.dst = left;
                load.name = ds_str_dup_range(stmt->as.assign_stmt.name.data, stmt->as.assign_stmt.name.len);
                emit_instr(p, load);
                int right = compile_expr(p, stmt->as.assign_stmt.value);
                src = new_reg(p);
                Instr bin = {0};
                bin.op = OP_BINARY;
                bin.span = stmt->span;
                bin.dst = src;
                bin.a = left;
                bin.b = right;
                const char *op = ds_lower_assign_binary_op(stmt->as.assign_stmt.op);
                bin.cmp = ds_str_dup_cstr(op);
                bin.cmp_enum = op_cmp_from_str(op, strlen(op));
                emit_instr(p, bin);
            }
            Instr ins = {0};
            ins.op = env_assign ? OP_SET_ENV : OP_STORE_VAR;
            ins.span = stmt->span;
            ins.a = src;
            if (env_assign) ins.name = ds_str_dup_range(stmt->as.assign_stmt.name.data + 4, stmt->as.assign_stmt.name.len - 4);
            else ins.name = ds_str_dup_range(stmt->as.assign_stmt.name.data, stmt->as.assign_stmt.name.len);
            emit_instr(p, ins);
            break;
        }
        case DS_LOWER_STMT_INDEX_ASSIGN: {
            int index = compile_expr(p, stmt->as.index_assign_stmt.index);
            int value = compile_expr(p, stmt->as.index_assign_stmt.value);
            Instr ins = {0};
            ins.op = OP_SET_INDEX;
            ins.span = stmt->span;
            ins.a = index;
            ins.b = value;
            ins.name = ds_str_dup_range(stmt->as.index_assign_stmt.name.data, stmt->as.index_assign_stmt.name.len);
            emit_instr(p, ins);
            break;
        }
        case DS_LOWER_STMT_CMD: {
            Instr ins = {0};
            ins.op = OP_RUN_CMD;
            ins.span = stmt->span;
            copy_command_to_instr(&ins, &stmt->as.cmd_stmt);
            emit_instr(p, ins);
            break;
        }
        case DS_LOWER_STMT_CALL: {
            Instr ins = {0};
            ins.op = find_function_meta(p, stmt->as.call_stmt.name) >= 0 ? OP_CALL : OP_STDLIB_CALL;
            ins.span = stmt->span;
            ins.dst = new_reg(p);
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
            LoopPatch *loop = push_loop(p, begin_pos, p->scope_depth);
            p->scope_depth++;
            compile_block(p, stmt->as.for_stmt.body);
            Instr pop = {0};
            pop.op = OP_POP_SCOPE;
            pop.span = stmt->span;
            emit_instr(p, pop);
            p->scope_depth--;
            Instr jump = {0};
            jump.op = OP_JUMP;
            jump.span = stmt->span;
            jump.target = (int)begin_pos;
            emit_instr(p, jump);
            p->instrs[begin_pos].target = (int)p->instr_len;
            (void)loop;
            pop_loop(p, p->instr_len);
            break;
        }
        case DS_LOWER_STMT_FOR_MAP: {
            int iterable = compile_expr(p, stmt->as.for_stmt.iterable);
            Instr begin = {0};
            begin.op = OP_FOR_MAP;
            begin.span = stmt->span;
            begin.a = iterable;
            begin.name = ds_str_dup_range(stmt->as.for_stmt.name.data, stmt->as.for_stmt.name.len);
            begin.value_name = ds_str_dup_range(stmt->as.for_stmt.value_name.data, stmt->as.for_stmt.value_name.len);
            size_t begin_pos = emit_instr(p, begin);
            LoopPatch *loop = push_loop(p, begin_pos, p->scope_depth);
            p->scope_depth++;
            compile_block(p, stmt->as.for_stmt.body);
            Instr pop = {0};
            pop.op = OP_POP_SCOPE;
            pop.span = stmt->span;
            emit_instr(p, pop);
            p->scope_depth--;
            Instr jump = {0};
            jump.op = OP_JUMP;
            jump.span = stmt->span;
            jump.target = (int)begin_pos;
            emit_instr(p, jump);
            p->instrs[begin_pos].target = (int)p->instr_len;
            (void)loop;
            pop_loop(p, p->instr_len);
            break;
        }
        case DS_LOWER_STMT_FOR_RANGE: {
            int start = compile_expr(p, stmt->as.for_stmt.iterable->as.range.start);
            int end = compile_expr(p, stmt->as.for_stmt.iterable->as.range.end);
            Instr begin = {0};
            begin.op = OP_FOR_RANGE;
            begin.span = stmt->span;
            begin.a = start;
            begin.b = end;
            begin.name = ds_str_dup_range(stmt->as.for_stmt.name.data, stmt->as.for_stmt.name.len);
            size_t begin_pos = emit_instr(p, begin);
            LoopPatch *loop = push_loop(p, begin_pos, p->scope_depth);
            p->scope_depth++;
            compile_block(p, stmt->as.for_stmt.body);
            Instr pop = {0};
            pop.op = OP_POP_SCOPE;
            pop.span = stmt->span;
            emit_instr(p, pop);
            p->scope_depth--;
            Instr jump = {0};
            jump.op = OP_JUMP;
            jump.span = stmt->span;
            jump.target = (int)begin_pos;
            emit_instr(p, jump);
            p->instrs[begin_pos].target = (int)p->instr_len;
            (void)loop;
            pop_loop(p, p->instr_len);
            break;
        }
        case DS_LOWER_STMT_WHILE: {
            size_t begin_pos = p->instr_len;
            int cond = compile_expr(p, stmt->as.while_stmt.condition);
            Instr jif = {0};
            jif.op = OP_JUMP_IF_FALSE;
            jif.span = stmt->as.while_stmt.condition->span;
            jif.a = cond;
            size_t jif_pos = emit_instr(p, jif);
            Instr push = {0};
            push.op = OP_PUSH_SCOPE;
            push.span = stmt->span;
            emit_instr(p, push);
            LoopPatch *loop = push_loop(p, begin_pos, p->scope_depth);
            p->scope_depth++;
            compile_block(p, stmt->as.while_stmt.body);
            Instr pop = {0};
            pop.op = OP_POP_SCOPE;
            pop.span = stmt->span;
            emit_instr(p, pop);
            p->scope_depth--;
            Instr jump = {0};
            jump.op = OP_JUMP;
            jump.span = stmt->span;
            jump.target = (int)begin_pos;
            emit_instr(p, jump);
            p->instrs[jif_pos].target = (int)p->instr_len;
            (void)loop;
            pop_loop(p, p->instr_len);
            break;
        }
        case DS_LOWER_STMT_BREAK:
        case DS_LOWER_STMT_CONTINUE: {
            LoopPatch *loop = current_loop(p);
            if (stmt->kind == DS_LOWER_STMT_BREAK && loop && (p->instrs[loop->start].op == OP_FOR_ARRAY || p->instrs[loop->start].op == OP_FOR_MAP)) {
                Instr reset = {0};
                reset.op = OP_RESET_FOR;
                reset.span = stmt->span;
                reset.target = (int)loop->start;
                emit_instr(p, reset);
            }
            Instr jump = {0};
            jump.op = OP_JUMP_POP;
            jump.span = stmt->span;
            jump.target = 0;
            jump.a = loop ? p->scope_depth - loop->base_scope_depth : 0;
            size_t pos = emit_instr(p, jump);
            if (loop) {
                if (stmt->kind == DS_LOWER_STMT_BREAK) loop_patch_vec_push(&loop->breaks, &loop->break_len, &loop->break_cap, pos);
                else loop_patch_vec_push(&loop->continues, &loop->continue_len, &loop->continue_cap, pos);
            }
            break;
        }
        case DS_LOWER_STMT_CASE: {
            int selector = compile_expr(p, stmt->as.case_stmt.selector);
            size_t *end_jumps = NULL;
            size_t end_len = 0, end_cap = 0;
            size_t next_arm_target = 0;
            for (size_t i = 0; i < stmt->as.case_stmt.arms.len; i++) {
                if (next_arm_target) p->instrs[next_arm_target].target = (int)p->instr_len;
                const DsLowerCaseArm *arm = &stmt->as.case_stmt.arms.items[i];
                bool is_default = false;
                for (size_t j = 0; j < arm->patterns.len; j++) {
                    const DsLowerCasePattern *pat = &arm->patterns.items[j];
                    if (pat->kind == DS_LOWER_CASE_PATTERN_DEFAULT) { is_default = true; break; }
                    DsValue value;
                    if (pat->kind == DS_LOWER_CASE_PATTERN_STRING) {
                        DsString decoded;
                        decode_string_text(pat->text, &decoded);
                        value = ds_value_string_take(&decoded);
                    } else if (pat->kind == DS_LOWER_CASE_PATTERN_INT) {
                        char *tmp = ds_str_dup_range(pat->text.data, pat->text.len);
                        value = ds_value_int(strtoll(tmp, NULL, 10));
                        free(tmp);
                    } else {
                        value = ds_value_bool(pat->boolean);
                    }
                    int lit = compile_const(p, pat->span, value);
                    int cmp_reg = new_reg(p);
                    Instr cmp = {0};
                    cmp.op = OP_COMPARE;
                    cmp.span = pat->span;
                    cmp.dst = cmp_reg;
                    cmp.a = selector;
                    cmp.b = lit;
                    cmp.cmp = ds_str_dup_range("===", 3);
                    cmp.cmp_enum = OP_CMP_EQ_EQ_EQ;
                    emit_instr(p, cmp);
                    Instr jif = {0};
                    jif.op = OP_JUMP_IF_FALSE;
                    jif.span = pat->span;
                    jif.a = cmp_reg;
                    size_t false_pos = emit_instr(p, jif);
                    compile_scoped_block(p, arm->body);
                    Instr end_jump = {0};
                    end_jump.op = OP_JUMP;
                    end_jump.span = arm->span;
                    size_t end_pos = emit_instr(p, end_jump);
                    loop_patch_vec_push(&end_jumps, &end_len, &end_cap, end_pos);
                    p->instrs[false_pos].target = (int)p->instr_len;
                }
                if (is_default) {
                    compile_scoped_block(p, arm->body);
                    Instr end_jump = {0};
                    end_jump.op = OP_JUMP;
                    end_jump.span = arm->span;
                    size_t end_pos = emit_instr(p, end_jump);
                    loop_patch_vec_push(&end_jumps, &end_len, &end_cap, end_pos);
                }
                next_arm_target = 0;
            }
            for (size_t i = 0; i < end_len; i++) p->instrs[end_jumps[i]].target = (int)p->instr_len;
            free(end_jumps);
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
            if (stmt->as.block_stmt.scoped) compile_scoped_block(p, stmt);
            else for (size_t i = 0; i < stmt->as.block_stmt.statements.len; i++) compile_stmt(p, stmt->as.block_stmt.statements.items[i]);
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
        case DS_LOWER_STMT_RETURN: {
            int value = compile_expr(p, stmt->as.return_stmt.value);
            Instr ins = {0};
            ins.op = OP_RETURN_VALUE;
            ins.span = stmt->span;
            ins.a = value;
            emit_instr(p, ins);
            break;
        }
        case DS_LOWER_STMT_DEFER:
        case DS_LOWER_STMT_TRAP: {
            Instr reg = {0};
            reg.op = OP_REGISTER_HANDLER;
            reg.span = stmt->span;
            reg.a = (int)stmt->as.handler_stmt.signal;
            reg.b = stmt->kind == DS_LOWER_STMT_TRAP ? 1 : 0;
            size_t reg_pos = emit_instr(p, reg);
            Instr skip = {0};
            skip.op = OP_JUMP;
            skip.span = stmt->span;
            size_t skip_pos = emit_instr(p, skip);
            p->instrs[reg_pos].target = (int)p->instr_len;
            compile_scoped_block(p, stmt->as.handler_stmt.body);
            Instr end = {0};
            end.op = OP_END_HANDLER;
            end.span = stmt->span;
            emit_instr(p, end);
            p->instrs[skip_pos].target = (int)p->instr_len;
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
