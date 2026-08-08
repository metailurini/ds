#include "parser_internal.h"

bool parse_script_type(Parser *p, DsScriptType *out) {
    if (parser_advance_if(p, DS_TOK_TYPE_STRING)) { *out = DS_SCRIPT_TYPE_STRING; return true; }
    if (parser_advance_if(p, DS_TOK_TYPE_INT)) { *out = DS_SCRIPT_TYPE_INT; return true; }
    if (parser_advance_if(p, DS_TOK_TYPE_BOOL)) { *out = DS_SCRIPT_TYPE_BOOL; return true; }
    ds_diag_error(p->diag, parser_peek(p)->span, "expected type name `string`, `int`, or `bool`");
    return false;
}

static bool parse_script_decl(Parser *p, DsScriptBlock *script) {
    DsToken *start = parser_peek(p);
    DsScriptDecl decl;
    memset(&decl, 0, sizeof(decl));

    if (parser_advance_if(p, DS_TOK_ARG)) {
        decl.kind = DS_SCRIPT_DECL_ARG;
    } else if (parser_advance_if(p, DS_TOK_OPTION)) {
        decl.kind = DS_SCRIPT_DECL_OPTION;
    } else if (parser_advance_if(p, DS_TOK_FLAG)) {
        decl.kind = DS_SCRIPT_DECL_FLAG;
    } else {
        ds_diag_error(p->diag, parser_peek(p)->span, "expected `arg`, `option`, or `flag` declaration");
        parser_skip_to_stmt_end(p);
        parser_consume_statement_end(p);
        return false;
    }

    if (!parser_expect_identifier_like(p, "expected declaration name")) return false;
    DsToken *name = parser_previous(p);
    decl.name = parser_copy_token_text(name);
    if (!parser_expect(p, DS_TOK_COLON, "expected `:` after declaration name")) return false;
    if (!parse_script_type(p, &decl.type)) return false;

    if (decl.kind == DS_SCRIPT_DECL_ARG) {
        if (parser_advance_if(p, DS_TOK_EQUAL)) {
            ds_diag_error(p->diag, parser_previous(p)->span, "`arg` declarations do not support defaults in v0.5.0");
            if (!parser_is_stmt_end(p)) decl.default_value = parse_expr(p);
        }
    } else {
        if (!parser_expect(p, DS_TOK_EQUAL, decl.kind == DS_SCRIPT_DECL_OPTION ?
                    "expected `=` after option type" : "expected `=` after flag type")) return false;
        if (parser_is_stmt_end(p)) {
            ds_diag_error(p->diag, parser_peek(p)->span, "expected default value after `=`");
            return false;
        }
        decl.default_value = parse_expr(p);
    }

    decl.span = (DsSpan){start->span.start, (decl.default_value ? decl.default_value->span.end : parser_previous(p)->span.end), start->span.source};
    parser_expect_stmt_end(p, "declaration");
    parser_script_decl_vec_push(&script->declarations, decl);
    return true;
}

bool parse_script_block(Parser *p, DsAst *ast) {
    DsToken *start = parser_previous(p);
    if (ast->statements.len > 0) {
        ds_diag_error(p->diag, start->span, "`script` block must appear before executable statements");
    }
    if (ast->has_script) {
        ds_diag_error(p->diag, start->span, "duplicate `script` block");
    }
    ast->has_script = true;
    ast->script.span = start->span;
    if (!parser_expect(p, DS_TOK_LBRACE, "expected `{` after `script`")) return false;
    parser_skip_newlines(p);
    while (!parser_at_end(p) && !parser_at(p, DS_TOK_RBRACE)) {
        parse_script_decl(p, &ast->script);
        parser_skip_newlines(p);
    }
    if (!parser_expect(p, DS_TOK_RBRACE, "expected `}` to close script block")) return false;
    ast->script.span = (DsSpan){start->span.start, parser_previous(p)->span.end, start->span.source};
    parser_consume_statement_end(p);
    return true;
}
