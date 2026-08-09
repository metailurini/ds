#include "ds_hir.h"
#include "ds_runtime.h"
#include "ds_signal.h"

static void print_span(FILE *out, DsSpan span) {
    const DsSource *source = span.source;
    fprintf(out, " @ %s:%d:%d", source && source->path ? source->path : "<source>", span.start.line, span.start.column);
}

const char *ds_lower_value_kind_name(DsLowerValueKind kind);

static void print_row_schema(FILE *out, const DsLowerRowSchema *schema) {
    fputs(" row_schema={", out);
    for (size_t i = 0; schema && i < schema->len; i++) {
        if (i) fputs(", ", out);
        ds_fprint_str(out, schema->items[i].name);
        fputc(':', out);
        fputs(ds_lower_value_kind_name(schema->items[i].kind), out);
    }
    fputc('}', out);
}

const char *ds_lower_value_kind_name(DsLowerValueKind kind) {
    static const char *const names[] = {"unknown", "bool", "int", "string", "array", "map", "command_result"};
    return (unsigned)kind < DS_ARRAY_LEN(names) ? names[kind] : "unknown";
}

DsStr ds_lower_program_script_help(const DsSource *source, const DsLowerProgram *program) {
    DsString out;
    ds_string_init(&out);
    ds_string_appendf(&out, "Usage: %s", ds_source_basename(source));
    bool has_args = false;
    bool has_options = false;
    for (size_t i = 0; i < program->script_decls.len; i++) {
        const DsLowerScriptDecl *decl = &program->script_decls.items[i];
        if (decl->kind == DS_SCRIPT_DECL_ARG) {
            has_args = true;
            ds_string_appendf(&out, " <%.*s>", (int)decl->name.len, ds_str_data(decl->name));
        } else {
            has_options = true;
        }
    }
    if (has_options) ds_string_append_cstr(&out, " [options]");
    ds_string_append_char(&out, '\n');
    if (has_args) {
        ds_string_append_cstr(&out, "\nArguments:\n");
        for (size_t i = 0; i < program->script_decls.len; i++) {
            const DsLowerScriptDecl *decl = &program->script_decls.items[i];
            if (decl->kind != DS_SCRIPT_DECL_ARG) continue;
            ds_string_appendf(&out, "  %.*s %s\n", (int)decl->name.len, ds_str_data(decl->name), ds_script_type_name(decl->type));
        }
    }
    ds_string_append_cstr(&out, "\nOptions:\n");
    for (size_t i = 0; i < program->script_decls.len; i++) {
        const DsLowerScriptDecl *decl = &program->script_decls.items[i];
        if (decl->kind == DS_SCRIPT_DECL_OPTION) {
            ds_string_appendf(&out, "  --%.*s %s    default: ", (int)decl->name.len, ds_str_data(decl->name), ds_script_type_name(decl->type));
            if (decl->type == DS_SCRIPT_TYPE_STRING) {
                ds_string_append_range(&out, ds_str_data(decl->default_text), decl->default_text.len);
            } else if (decl->type == DS_SCRIPT_TYPE_INT) {
                ds_string_appendf(&out, "%lld", (long long)decl->default_int);
            } else {
                ds_string_append_cstr(&out, decl->default_bool ? "true" : "false");
            }
            ds_string_append_char(&out, '\n');
        } else if (decl->kind == DS_SCRIPT_DECL_FLAG) {
            ds_string_appendf(&out, "  --%.*s            boolean flag\n", (int)decl->name.len, ds_str_data(decl->name));
        }
    }
    ds_string_append_cstr(&out, "  --help             show this help\n");
    return (DsStr){out.data, out.len};
}

static const char *script_decl_kind(DsScriptDeclKind kind) {
    static const char *const names[] = {"Arg", "Option", "Flag"};
    return (unsigned)kind < DS_ARRAY_LEN(names) ? names[kind] : "Decl";
}

static void dump_expr(FILE *out, const DsLowerExpr *expr, int level);
static void dump_stmt(FILE *out, const DsLowerStmt *stmt, int level);

static void print_literal_expr(FILE *out, const DsLowerExpr *expr) {
    if (!expr) {
        fputs("<default>", out);
        return;
    }
    switch (expr->kind) {
        case DS_LOWER_EXPR_STRING:
            ds_fprint_str(out, expr->as.text);
            break;
        case DS_LOWER_EXPR_INT:
            ds_fprint_str(out, expr->as.text);
            break;
        case DS_LOWER_EXPR_BOOL:
            fputs(expr->as.boolean ? "true" : "false", out);
            break;
        default:
            fputs("<default>", out);
            break;
    }
}

static void dump_word_vec(FILE *out, const DsWordVec *words) {
    fputc('[', out);
    for (size_t i = 0; i < words->len; i++) {
        if (i) fputs(", ", out);
        fputc('"', out);
        ds_fprint_escaped(out, words->items[i].text.data, words->items[i].text.len, DS_ESCAPE_HEX_CONTROLS);
        fputc('"', out);
    }
    fputc(']', out);
}

static void dump_redirect(FILE *out, const DsRedirect *redirect);

static void dump_command(FILE *out, const DsCommand *command) {
    for (size_t s = 0; s < command->stages.len; s++) {
        if (s) fputs(" | ", out);
        dump_word_vec(out, &command->stages.items[s].words);
    }
    dump_redirect(out, &command->redirect);
}

static void dump_redirect(FILE *out, const DsRedirect *redirect) {
    const char *op = ds_redirect_shell_op(redirect->kind);
    if (!op) return;
    fprintf(out, " Redirect %s ", op);
    fputc('"', out);
    ds_fprint_escaped(out, ds_str_data(redirect->target), redirect->target.len, DS_ESCAPE_HEX_CONTROLS);
    fputc('"', out);
    print_span(out, redirect->target_span);
}

static void dump_expr_vec(FILE *out, const DsLowerExprVec *args, int level) {
    if (args->len == 0) return;
    fputc('\n', out);
    for (size_t i = 0; i < args->len; i++) dump_expr(out, args->items[i], level + 1);
}

static void dump_expr(FILE *out, const DsLowerExpr *expr, int level) {
    if (!expr) {
        ds_fprint_indent(out, level); fputs("<null expr>\n", out); return;
    }
    ds_fprint_indent(out, level);
    switch (expr->kind) {
        case DS_LOWER_EXPR_IDENT:
            fputs("Ident ", out); ds_fprint_str(out, expr->as.text); print_span(out, expr->span); fputc('\n', out); break;
        case DS_LOWER_EXPR_STRING:
            fputs("String \"", out); ds_fprint_escaped(out, expr->as.text.data, expr->as.text.len, DS_ESCAPE_HEX_CONTROLS); fputs("\"", out); print_span(out, expr->span); fputc('\n', out); break;
        case DS_LOWER_EXPR_INT:
            fputs("Int ", out); ds_fprint_str(out, expr->as.text); print_span(out, expr->span); fputc('\n', out); break;
        case DS_LOWER_EXPR_BOOL:
            fprintf(out, "Bool %s", expr->as.boolean ? "true" : "false"); print_span(out, expr->span); fputc('\n', out); break;
        case DS_LOWER_EXPR_REGEX:
            fputs("Regex ", out); ds_fprint_str(out, expr->as.regex); print_span(out, expr->span); fputc('\n', out); break;
        case DS_LOWER_EXPR_RUN:
            fputs("Run ", out); dump_command(out, &expr->as.run); print_span(out, expr->span); fputc('\n', out); break;
        case DS_LOWER_EXPR_FIELD:
            fputs("Field .", out); ds_fprint_str(out, expr->as.field.field); print_span(out, expr->span); fputc('\n', out);
            dump_expr(out, expr->as.field.object, level + 1);
            break;
        case DS_LOWER_EXPR_UNARY:
            fprintf(out, "Unary %s", ds_unary_op_name(expr->as.unary.op)); print_span(out, expr->span); fputc('\n', out);
            dump_expr(out, expr->as.unary.right, level + 1);
            break;
        case DS_LOWER_EXPR_BINARY:
            fprintf(out, "Binary %s", ds_binary_op_name(expr->as.binary.op)); print_span(out, expr->span); fputc('\n', out);
            dump_expr(out, expr->as.binary.left, level + 1);
            dump_expr(out, expr->as.binary.right, level + 1);
            break;
        case DS_LOWER_EXPR_CALL:
            fputs("Call ", out); ds_fprint_str(out, expr->as.call.name); if (expr->as.call.returns_row || expr->as.call.returns_row_array) print_row_schema(out, &expr->as.call.row_schema); print_span(out, expr->span); dump_expr_vec(out, &expr->as.call.args, level); if (expr->as.call.args.len == 0) fputc('\n', out); break;
        case DS_LOWER_EXPR_INTERP:
            fputs("InterpolatedString", out); print_span(out, expr->span); dump_expr_vec(out, &expr->as.interp.parts, level); if (expr->as.interp.parts.len == 0) fputc('\n', out); break;
        case DS_LOWER_EXPR_ARRAY:
            fputs("Array", out); if (expr->as.array.is_row_array) print_row_schema(out, &expr->as.array.row_schema); print_span(out, expr->span); dump_expr_vec(out, &expr->as.array.elements, level); if (expr->as.array.elements.len == 0) fputc('\n', out); break;
        case DS_LOWER_EXPR_MAP:
            fputs("Map", out); if (expr->as.map.is_row) print_row_schema(out, &expr->as.map.row_schema); print_span(out, expr->span); fputc('\n', out);
            for (size_t i = 0; i < expr->as.map.entries.len; i++) {
                ds_fprint_indent(out, level + 1); fputs("Entry ", out); ds_fprint_str(out, expr->as.map.entries.items[i].key); print_span(out, expr->as.map.entries.items[i].span); fputc('\n', out);
                dump_expr(out, expr->as.map.entries.items[i].value, level + 2);
            }
            break;
        case DS_LOWER_EXPR_INDEX:
            fputs("Index", out); if (expr->as.index.object_is_array) fputs(" array", out); if (expr->as.index.object_is_map) fputs(" map", out); if (expr->as.index.returns_row) print_row_schema(out, &expr->as.index.row_schema); print_span(out, expr->span); fputc('\n', out);
            dump_expr(out, expr->as.index.object, level + 1);
            dump_expr(out, expr->as.index.index, level + 1);
            break;
        case DS_LOWER_EXPR_RANGE:
            fputs("Range", out); print_span(out, expr->span); fputc('\n', out);
            dump_expr(out, expr->as.range.start, level + 1);
            dump_expr(out, expr->as.range.end, level + 1);
            break;
        case DS_LOWER_EXPR_ERROR:
            fputs("ErrorExpr", out); print_span(out, expr->span); fputc('\n', out); break;
    }
}

static void dump_block(FILE *out, const DsLowerStmt *block, int level) {
    if (!block || block->kind != DS_LOWER_STMT_BLOCK) return;
    for (size_t i = 0; i < block->as.block_stmt.statements.len; i++) dump_stmt(out, block->as.block_stmt.statements.items[i], level);
}

static void dump_stmt(FILE *out, const DsLowerStmt *stmt, int level) {
    if (!stmt) return;
    ds_fprint_indent(out, level);
    switch (stmt->kind) {
        case DS_LOWER_STMT_LET:
            fputs("Let ", out); ds_fprint_str(out, stmt->as.let_stmt.name); if (stmt->as.let_stmt.is_row || stmt->as.let_stmt.is_row_array) print_row_schema(out, &stmt->as.let_stmt.row_schema); print_span(out, stmt->span); fputc('\n', out);
            dump_expr(out, stmt->as.let_stmt.value, level + 1);
            break;
        case DS_LOWER_STMT_ASSIGN: {
            const char *op = ds_assign_op_name(stmt->as.assign_stmt.op);
            fprintf(out, "Assign "); ds_fprint_str(out, stmt->as.assign_stmt.name); fprintf(out, " %s", op); print_span(out, stmt->span); fputc('\n', out);
            dump_expr(out, stmt->as.assign_stmt.value, level + 1);
            break;
        }
        case DS_LOWER_STMT_INDEX_ASSIGN:
            fprintf(out, "IndexAssign "); ds_fprint_str(out, stmt->as.index_assign_stmt.name);
            fprintf(out, stmt->as.index_assign_stmt.target_is_map ? " map[] =" : " array[] =");
            print_span(out, stmt->span); fputc('\n', out);
            ds_fprint_indent(out, level + 1); fputs("Index\n", out);
            dump_expr(out, stmt->as.index_assign_stmt.index, level + 2);
            ds_fprint_indent(out, level + 1); fputs("Value\n", out);
            dump_expr(out, stmt->as.index_assign_stmt.value, level + 2);
            break;
        case DS_LOWER_STMT_CMD:
            fputs("Command ", out); dump_command(out, &stmt->as.cmd_stmt); print_span(out, stmt->span); fputc('\n', out);
            break;
        case DS_LOWER_STMT_CALL:
            fputs("CallStmt ", out); ds_fprint_str(out, stmt->as.call_stmt.name); print_span(out, stmt->span); dump_expr_vec(out, &stmt->as.call_stmt.args, level); if (stmt->as.call_stmt.args.len == 0) fputc('\n', out); break;
        case DS_LOWER_STMT_PUSH:
            fputs("Push ", out); ds_fprint_str(out, stmt->as.push_stmt.name); if (stmt->as.push_stmt.target_is_row_array) print_row_schema(out, &stmt->as.push_stmt.row_schema); print_span(out, stmt->span); fputc('\n', out);
            dump_expr(out, stmt->as.push_stmt.value, level + 1);
            break;
        case DS_LOWER_STMT_FOR_ARRAY:
            fputs("For ", out); ds_fprint_str(out, stmt->as.for_stmt.name); fputs(" in", out); if (stmt->as.for_stmt.iterates_row_array) print_row_schema(out, &stmt->as.for_stmt.row_schema); print_span(out, stmt->span); fputc('\n', out);
            dump_expr(out, stmt->as.for_stmt.iterable, level + 1);
            ds_fprint_indent(out, level + 1); fputs("Body\n", out);
            dump_block(out, stmt->as.for_stmt.body, level + 2);
            break;
        case DS_LOWER_STMT_FOR_MAP:
            fputs("ForMap ", out); ds_fprint_str(out, stmt->as.for_stmt.name); fputs(", ", out); ds_fprint_str(out, stmt->as.for_stmt.value_name); fputs(" in", out); print_span(out, stmt->span); fputc('\n', out);
            dump_expr(out, stmt->as.for_stmt.iterable, level + 1);
            ds_fprint_indent(out, level + 1); fputs("Body\n", out);
            dump_block(out, stmt->as.for_stmt.body, level + 2);
            break;
        case DS_LOWER_STMT_FOR_RANGE:
            fputs("ForRange ", out); ds_fprint_str(out, stmt->as.for_stmt.name); fputs(" in", out); print_span(out, stmt->span); fputc('\n', out);
            dump_expr(out, stmt->as.for_stmt.iterable, level + 1);
            ds_fprint_indent(out, level + 1); fputs("Body\n", out);
            dump_block(out, stmt->as.for_stmt.body, level + 2);
            break;
        case DS_LOWER_STMT_WHILE:
            fputs("While", out); print_span(out, stmt->span); fputc('\n', out);
            ds_fprint_indent(out, level + 1); fputs("Condition\n", out);
            dump_expr(out, stmt->as.while_stmt.condition, level + 2);
            ds_fprint_indent(out, level + 1); fputs("Body\n", out);
            dump_block(out, stmt->as.while_stmt.body, level + 2);
            break;
        case DS_LOWER_STMT_BREAK:
            fputs("Break", out); print_span(out, stmt->span); fputc('\n', out);
            break;
        case DS_LOWER_STMT_CONTINUE:
            fputs("Continue", out); print_span(out, stmt->span); fputc('\n', out);
            break;
        case DS_LOWER_STMT_CASE:
            fputs("Case", out); print_span(out, stmt->span); fputc('\n', out);
            ds_fprint_indent(out, level + 1); fputs("Selector\n", out);
            dump_expr(out, stmt->as.case_stmt.selector, level + 2);
            for (size_t i = 0; i < stmt->as.case_stmt.arms.len; i++) {
                const DsLowerCaseArm *arm = &stmt->as.case_stmt.arms.items[i];
                ds_fprint_indent(out, level + 1); fputs("Arm", out);
                for (size_t j = 0; j < arm->patterns.len; j++) {
                    fputc(' ', out);
                    ds_case_pattern_fprint(out, &arm->patterns.items[j]);
                }
                print_span(out, arm->span); fputc('\n', out);
                dump_block(out, arm->body, level + 2);
            }
            break;
        case DS_LOWER_STMT_IF:
            fputs("If", out); print_span(out, stmt->span); fputc('\n', out);
            ds_fprint_indent(out, level + 1); fputs("Condition\n", out);
            dump_expr(out, stmt->as.if_stmt.condition, level + 2);
            ds_fprint_indent(out, level + 1); fputs("Then\n", out);
            dump_block(out, stmt->as.if_stmt.then_branch, level + 2);
            if (stmt->as.if_stmt.else_branch) {
                ds_fprint_indent(out, level + 1); fputs("Else\n", out);
                dump_block(out, stmt->as.if_stmt.else_branch, level + 2);
            }
            break;
        case DS_LOWER_STMT_BLOCK:
            fputs("Block", out); print_span(out, stmt->span); fputc('\n', out);
            dump_block(out, stmt, level + 1);
            break;
        case DS_LOWER_STMT_ASSERT:
            fputs("Assert", out); print_span(out, stmt->span); fputc('\n', out);
            dump_expr(out, stmt->as.assert_stmt.condition, level + 1);
            break;
        case DS_LOWER_STMT_RETURN:
            fputs("Return", out); if (stmt->as.return_stmt.returns_row_array) print_row_schema(out, &stmt->as.return_stmt.row_schema); print_span(out, stmt->span); fputc('\n', out);
            dump_expr(out, stmt->as.return_stmt.value, level + 1);
            break;
        case DS_LOWER_STMT_DEFER:
            fprintf(out, "Defer %s", ds_handler_signal_name(stmt->as.handler_stmt.signal)); print_span(out, stmt->span); fputc('\n', out);
            dump_block(out, stmt->as.handler_stmt.body, level + 1);
            break;
        case DS_LOWER_STMT_TRAP:
            fprintf(out, "Trap %s", ds_handler_signal_name(stmt->as.handler_stmt.signal)); print_span(out, stmt->span); fputc('\n', out);
            dump_block(out, stmt->as.handler_stmt.body, level + 1);
            break;
    }
}

bool ds_hir_dump_program(const DsLowerProgram *program, FILE *out) {
    if (!program || !out) return false;
    fputs("Program\n", out);
    if (program->has_script) {
        ds_fprint_indent(out, 1); fputs("Script\n", out);
        for (size_t i = 0; i < program->script_decls.len; i++) {
            const DsLowerScriptDecl *decl = &program->script_decls.items[i];
            ds_fprint_indent(out, 2);
            fprintf(out, "%s ", script_decl_kind(decl->kind));
            ds_fprint_str(out, decl->name);
            fprintf(out, ": %s", ds_script_type_name(decl->type));
            if (decl->has_default) {
                if (decl->type == DS_SCRIPT_TYPE_STRING) { fputs(" = \"", out); ds_fprint_escaped(out, ds_str_data(decl->default_text), decl->default_text.len, DS_ESCAPE_HEX_CONTROLS); fputc('"', out); }
                else if (decl->type == DS_SCRIPT_TYPE_INT) fprintf(out, " = %lld", (long long)decl->default_int);
                else fprintf(out, " = %s", decl->default_bool ? "true" : "false");
            }
            print_span(out, decl->span); fputc('\n', out);
        }
    }
    for (size_t i = 0; i < program->functions.len; i++) {
        const DsLowerFn *fn = &program->functions.items[i];
        ds_fprint_indent(out, 1); fputs("Function ", out); ds_fprint_str(out, fn->name); fputc('(', out);
        for (size_t j = 0; j < fn->params.len; j++) {
            if (j) fputs(", ", out);
            ds_fprint_str(out, fn->params.items[j].name);
            if (!fn->params.items[j].has_default && fn->params.items[j].inferred_kind != DS_LOWER_VALUE_UNKNOWN) {
                fprintf(out, ": inferred %s", ds_lower_value_kind_name(fn->params.items[j].inferred_kind));
            } else if (!fn->params.items[j].has_default) {
                fputs(": unknown", out);
            }
            if (fn->params.items[j].has_default) {
                fprintf(out, ": default %s", ds_lower_value_kind_name(fn->params.items[j].default_kind));
                fputs(" = ", out);
                print_literal_expr(out, fn->params.items[j].default_value);
            }
        }
        fputc(')', out); print_span(out, fn->span); fputc('\n', out);
        ds_fprint_indent(out, 2); fputs("Body\n", out);
        dump_block(out, fn->body, 3);
    }
    if (program->tests.len > 0) {
        ds_fprint_indent(out, 1); fputs("Tests\n", out);
        for (size_t i = 0; i < program->tests.len; i++) {
            const DsLowerTest *test = &program->tests.items[i];
            ds_fprint_indent(out, 2); fputs("Test ", out); ds_fprint_str(out, test->name); print_span(out, test->span); fputc('\n', out);
            dump_block(out, test->body, 3);
        }
    }
    ds_fprint_indent(out, 1); fputs("Statements\n", out);
    for (size_t i = 0; i < program->statements.len; i++) dump_stmt(out, program->statements.items[i], 2);
    return true;
}
