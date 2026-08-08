#include "vm_internal.h"

static void print_span_comment(FILE *out, const DsSource *fallback, DsSpan span) {
    const DsSource *source = span.source ? span.source : fallback;
    fprintf(out, "    # %s:%d:%d\n", source && source->path ? source->path : "<source>", span.start.line, span.start.column);
}

static void print_reg_args(FILE *out, const Instr *ins) {
    for (size_t i = 0; i < ins->arg_count; i++) {
        if (i) fputs(", ", out);
        fprintf(out, "r%d", ins->args[i]);
    }
}

static void print_instr_command(FILE *out, const Instr *ins) {
    size_t word = 0;
    size_t stages = ins->stage_count ? ins->stage_count : 1;
    fputs(" [", out);
    for (size_t s = 0; s < stages; s++) {
        if (s) fputs(" |", out);
        size_t count = ins->stage_count ? ins->stage_word_counts[s] : ins->word_count;
        for (size_t j = 0; j < count; j++, word++) {
            if (j || s) fputs(", ", out);
            fputc('"', out);
            ds_fprint_escaped(out, ins->words[word].data, ins->words[word].len, DS_ESCAPE_BASIC);
            fputc('"', out);
        }
    }
    fputc(']', out);
}

const char *op_name(OpCode op) {
    switch (op) {
        case OP_LOAD_CONST: return "LOAD_CONST";
        case OP_LOAD_VAR: return "LOAD_VAR";
        case OP_STORE_VAR: return "STORE_VAR";
        case OP_SET_ENV: return "SET_ENV";
        case OP_NOT: return "NOT";
        case OP_BINARY: return "BINARY";
        case OP_COMPARE: return "COMPARE";
        case OP_MEMBERSHIP: return "MEMBERSHIP";
        case OP_REGEX_MATCH: return "REGEX_MATCH";
        case OP_INTERPOLATE: return "INTERPOLATE";
        case OP_INTERP_JOIN: return "INTERP_JOIN";
        case OP_RUN_CAPTURE: return "RUN_CAPTURE";
        case OP_GET_FIELD: return "GET_FIELD";
        case OP_JUMP: return "JUMP";
        case OP_JUMP_POP: return "JUMP_POP";
        case OP_JUMP_IF_FALSE: return "JUMP_IF_FALSE";
        case OP_PUSH_SCOPE: return "PUSH_SCOPE";
        case OP_POP_SCOPE: return "POP_SCOPE";
        case OP_RUN_CMD: return "RUN_CMD";
        case OP_CALL: return "CALL";
        case OP_STDLIB_CALL: return "STDLIB_CALL";
        case OP_ARRAY_LITERAL: return "ARRAY_LITERAL";
        case OP_MAP_LITERAL: return "MAP_LITERAL";
        case OP_GET_INDEX: return "GET_INDEX";
        case OP_SET_INDEX: return "SET_INDEX";
        case OP_PUSH_ARRAY: return "PUSH_ARRAY";
        case OP_FOR_ARRAY: return "FOR_ARRAY";
        case OP_FOR_MAP: return "FOR_MAP";
        case OP_FOR_RANGE: return "FOR_RANGE";
        case OP_RESET_FOR: return "RESET_FOR";
        case OP_ASSERT: return "ASSERT";
        case OP_RETURN_VALUE: return "RETURN_VALUE";
        case OP_RETURN_FUNC: return "RETURN_FUNC";
        case OP_REGISTER_HANDLER: return "REGISTER_HANDLER";
        case OP_END_HANDLER: return "END_HANDLER";
        case OP_RETURN: return "RETURN";
        case OP_NOP: return "NOP";
    }
    return "NOP";
}

const char *span_path(const DsSource *fallback, DsSpan span) {
    const DsSource *source = span.source ? span.source : fallback;
    return source && source->path ? source->path : "<source>";
}

void trace_vm_instr(Vm *vm, size_t ip, const Instr *ins) {
    if (!vm->options.trace_vm) return;
    fprintf(stderr, "trace: vm ip=%zu op=%s", ip, op_name(ins->op));
    if (ins->dst >= 0) fprintf(stderr, " dst=r%d", ins->dst);
    if (ins->name) fprintf(stderr, " name=%s", ins->name);
    if (ins->target >= 0) fprintf(stderr, " target=%d", ins->target);
    fprintf(stderr, " @ %s:%d:%d\n", span_path(vm->source, ins->span), ins->span.start.line, ins->span.start.column);
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
            ds_fprint_escaped(out, ds_string_data(&v->as.string), v->as.string.len, DS_ESCAPE_BASIC);
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
                    ds_fprint_escaped(out, ds_str_data(decl->default_text), decl->default_text.len, DS_ESCAPE_BASIC);
                    fputc('"', out);
                } else if (decl->type == DS_SCRIPT_TYPE_INT) {
                    fprintf(out, " = %lld", (long long)decl->default_int);
                } else {
                    fprintf(out, " = %s", decl->default_bool ? "true" : "false");
                }
            }
            print_span_comment(out, source, decl->span);
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
                fprintf(out, "    param %zu %s: %s", j, param->name, ds_lower_value_kind_name(param->expected_kind));
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
            case OP_SET_ENV: fprintf(out, " %s, r%d", ins->name, ins->a); break;
            case OP_NOT: fprintf(out, " r%d, r%d", ins->dst, ins->a); break;
            case OP_BINARY: fprintf(out, " r%d, r%d %s r%d", ins->dst, ins->a, ins->cmp, ins->b); break;
            case OP_COMPARE: fprintf(out, " r%d, r%d %s r%d", ins->dst, ins->a, ins->cmp, ins->b); break;
            case OP_MEMBERSHIP: fprintf(out, " r%d, r%d in r%d", ins->dst, ins->a, ins->b); break;
            case OP_REGEX_MATCH: fprintf(out, " r%d, r%d matches r%d", ins->dst, ins->a, ins->b); break;
            case OP_INTERPOLATE: fprintf(out, " r%d, const %d", ins->dst, ins->a); break;
            case OP_INTERP_JOIN:
                fprintf(out, " r%d, join(", ins->dst);
                print_reg_args(out, ins);
                fputc(')', out);
                break;
            case OP_RUN_CAPTURE:
                fprintf(out, " r%d,", ins->dst);
                print_instr_command(out, ins);
                break;
            case OP_GET_FIELD: fprintf(out, " r%d, r%d.%s", ins->dst, ins->a, ins->field); break;
            case OP_JUMP: fprintf(out, " %d", ins->target); break;
            case OP_JUMP_POP: fprintf(out, " pop %d -> %d", ins->a, ins->target); break;
            case OP_JUMP_IF_FALSE: fprintf(out, " r%d, %d", ins->a, ins->target); break;
            case OP_PUSH_SCOPE: break;
            case OP_POP_SCOPE: break;
            case OP_RUN_CMD:
                print_instr_command(out, ins);
                if (ins->redirect.kind != DS_REDIRECT_NONE) {
                    fprintf(out, " %s \"", ds_redirect_source_op(ins->redirect.kind));
                    ds_fprint_escaped(out, ins->redirect.target.data, ins->redirect.target.len, DS_ESCAPE_BASIC);
                    fputc('"', out);
                }
                break;
            case OP_CALL:
                fprintf(out, " fn%d(", ins->target);
                print_reg_args(out, ins);
                fputc(')', out);
                break;
            case OP_STDLIB_CALL:
                fprintf(out, " r%d, %s(", ins->dst, ins->name ? ins->name : "<helper>");
                print_reg_args(out, ins);
                fputc(')', out);
                break;
            case OP_ARRAY_LITERAL:
                fprintf(out, " r%d, [", ins->dst);
                print_reg_args(out, ins);
                fputc(']', out);
                break;
            case OP_MAP_LITERAL:
                fprintf(out, " r%d, {", ins->dst);
                for (size_t j = 0; j < ins->arg_count; j++) { if (j) fputs(", ", out); fprintf(out, "%.*s: r%d", (int)ins->words[j].len, ins->words[j].data, ins->args[j]); }
                fputc('}', out);
                break;
            case OP_GET_INDEX: fprintf(out, " r%d, r%d[r%d]", ins->dst, ins->a, ins->b); break;
            case OP_SET_INDEX: fprintf(out, " %s[r%d] = r%d", ins->name, ins->a, ins->b); break;
            case OP_PUSH_ARRAY: fprintf(out, " %s, r%d", ins->name, ins->a); break;
            case OP_FOR_ARRAY: fprintf(out, " %s in r%d -> %d", ins->name, ins->a, ins->target); break;
            case OP_FOR_MAP: fprintf(out, " %s, %s in r%d -> %d", ins->name, ins->value_name, ins->a, ins->target); break;
            case OP_FOR_RANGE: fprintf(out, " %s in r%d..r%d -> %d", ins->name, ins->a, ins->b, ins->target); break;
            case OP_RESET_FOR: fprintf(out, " %d", ins->target); break;
            case OP_ASSERT: fprintf(out, " r%d", ins->a); break;
            case OP_RETURN_VALUE: fprintf(out, " r%d", ins->a); break;
            case OP_RETURN_FUNC: break;
            case OP_REGISTER_HANDLER:
                fprintf(out, " %s %s -> %d", ins->b ? "trap" : "defer",
                        ds_handler_signal_name((DsHandlerSignal)ins->a), ins->target);
                break;
            case OP_END_HANDLER: break;
            case OP_RETURN: fprintf(out, " %d", ins->target); break;
            case OP_NOP: break;
        }
        print_span_comment(out, source, ins->span);
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

