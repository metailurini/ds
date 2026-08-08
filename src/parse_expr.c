#include "parser_internal.h"
#include "ds_stdlib.h"

static int precedence(DsTokenKind kind) {
    switch (kind) {
        case DS_TOK_OR_OR: return 1;
        case DS_TOK_AND_AND: return 2;
        case DS_TOK_IN:
        case DS_TOK_MATCHES: return 3;
        case DS_TOK_EQUAL_EQUAL:
        case DS_TOK_BANG_EQUAL: return 3;
        case DS_TOK_DOT_DOT: return 3;
        case DS_TOK_GREATER:
        case DS_TOK_GREATER_EQUAL:
        case DS_TOK_LESS:
        case DS_TOK_LESS_EQUAL: return 4;
        case DS_TOK_PLUS:
        case DS_TOK_MINUS: return 5;
        case DS_TOK_STAR:
        case DS_TOK_SLASH:
        case DS_TOK_PERCENT: return 6;
        case DS_TOK_STAR_STAR: return 7;
        default: return 0;
    }
}

static DsExpr *parse_expr_prec(Parser *p, int min_prec);
DsExpr *parse_expr(Parser *p);
void parse_call_args(Parser *p, DsExprVec *args);
typedef bool (*ParseCollectionItemFn)(Parser *p, DsExpr *expr);

static DsExpr *parser_new_text_expr(DsExprKind kind, const DsToken *token) {
    DsExpr *expr = ds_expr_new(kind, token->span);
    expr->as.text = parser_copy_token_text(token);
    return expr;
}

static DsExpr *parse_collection_literal(Parser *p, DsExprKind kind, ParseCollectionItemFn parse_item) {
    bool map = kind == DS_EXPR_MAP;
    DsTokenKind closing = map ? DS_TOK_RBRACE : DS_TOK_RBRACKET;
    const char *trailing_message = map ? "expected map entry after `,`" : "expected array element after `,`";
    const char *closing_message = map ? "expected `}` to close map literal" : "expected `]` to close array literal";
    DsToken *open = parser_previous(p);
    DsExpr *expr = ds_expr_new(kind, open->span);
    parser_skip_newlines(p);
    if (map && parser_at(p, closing)) {
        ds_diag_error(p->diag, parser_peek(p)->span, "empty map literals are deferred in v0.10.0");
    } else {
        while (!parser_at_end(p) && !parser_at(p, closing)) {
            if (!parse_item(p, expr)) break;
            parser_skip_newlines(p);
            if (!parser_advance_if(p, DS_TOK_COMMA)) break;
            parser_skip_newlines(p);
            if (parser_reject_trailing_comma(p, closing, trailing_message)) break;
        }
    }
    if (parser_expect(p, closing, closing_message)) expr->span.end = parser_previous(p)->span.end;
    return expr;
}

static bool parse_array_element(Parser *p, DsExpr *expr) {
    DS_VEC_PUSH(&expr->as.array.elements, parse_expr(p), 8);
    return true;
}

static bool parse_map_entry(Parser *p, DsExpr *expr) {
    DsMapEntry entry = {0};
    if (parser_advance_if(p, DS_TOK_IDENT) || parser_advance_if(p, DS_TOK_STRING)) {
        DsToken *key = parser_previous(p);
        entry.key = parser_copy_token_text(key);
        entry.quoted_key = key->kind == DS_TOK_STRING;
        entry.span = key->span;
    } else {
        ds_diag_error(p->diag, parser_peek(p)->span, "expected map key before `:`");
        return false;
    }
    if (!parser_expect(p, DS_TOK_COLON, "expected `:` after map key")) goto fail;
    parser_skip_newlines(p);
    if (parser_at(p, DS_TOK_COMMA) || parser_at(p, DS_TOK_RBRACE) || parser_at_end(p)) {
        ds_diag_error(p->diag, parser_peek(p)->span, "expected map value after `:`");
        goto fail;
    }
    entry.value = parse_expr(p);
    if (entry.value) entry.span.end = entry.value->span.end;
    DS_VEC_PUSH(&expr->as.map.entries, entry, 8);
    return true;

fail:
    free(entry.key.data);
    return false;
}


static DsExpr *parse_primary(Parser *p) {
    DsToken *tok = parser_peek(p);
    if (parser_advance_if(p, DS_TOK_RUN)) {
        return parse_run_expr(p);
    }
    if (parser_advance_if(p, DS_TOK_IDENT)) {
        return parser_new_text_expr(DS_EXPR_IDENT, tok);
    }
    if (parser_advance_if(p, DS_TOK_TYPE_STRING) || parser_advance_if(p, DS_TOK_TYPE_INT) ||
        parser_advance_if(p, DS_TOK_TYPE_BOOL) || parser_advance_if(p, DS_TOK_SCRIPT) ||
        parser_advance_if(p, DS_TOK_IMPORT) || parser_advance_if(p, DS_TOK_ARG) || parser_advance_if(p, DS_TOK_OPTION) || parser_advance_if(p, DS_TOK_FLAG)) {
        DsToken *used = parser_previous(p);
        return parser_new_text_expr(DS_EXPR_IDENT, used);
    }
    if (parser_advance_if(p, DS_TOK_STRING)) {
        return parser_new_text_expr(DS_EXPR_STRING, tok);
    }
    if (parser_advance_if(p, DS_TOK_INT)) {
        return parser_new_text_expr(DS_EXPR_INT, tok);
    }
    if (parser_advance_if(p, DS_TOK_TRUE) || parser_advance_if(p, DS_TOK_FALSE)) {
        DsToken *used = parser_previous(p);
        DsExpr *expr = ds_expr_new(DS_EXPR_BOOL, used->span);
        expr->as.boolean = used->kind == DS_TOK_TRUE;
        return expr;
    }
    if (parser_advance_if(p, DS_TOK_REGEX)) {
        return parser_new_text_expr(DS_EXPR_REGEX, tok);
    }
    if (parser_advance_if(p, DS_TOK_LPAREN)) {
        DsToken *open = parser_previous(p);
        DsExpr *expr = parse_expr_prec(p, 1);
        if (!parser_expect(p, DS_TOK_RPAREN, "expected `)` after expression")) {
            return expr ? expr : ds_expr_new(DS_EXPR_ERROR, open->span);
        }
        if (expr) expr->span.end = parser_previous(p)->span.end;
        return expr;
    }
    if (parser_advance_if(p, DS_TOK_LBRACKET)) {
        return parse_collection_literal(p, DS_EXPR_ARRAY, parse_array_element);
    }
    if (parser_advance_if(p, DS_TOK_LBRACE)) {
        return parse_collection_literal(p, DS_EXPR_MAP, parse_map_entry);
    }

    ds_diag_error(p->diag, tok->span, "expected expression");
    return ds_expr_new(DS_EXPR_ERROR, tok->span);
}

static bool parser_expr_is_stdlib_namespace(DsExpr *expr) {
    return expr && expr->kind == DS_EXPR_IDENT && ds_stdlib_is_namespace(expr->as.text);
}

static DsStr parser_copy_dotted_bang_name(DsToken *left, DsToken *right) {
    size_t len = left->text.len + 1 + right->text.len + 1;
    char *buf = (char *)ds_xcalloc(len + 1, 1);
    memcpy(buf, left->text.data, left->text.len);
    buf[left->text.len] = '.';
    memcpy(buf + left->text.len + 1, right->text.data, right->text.len);
    buf[len - 1] = '!';
    return (DsStr){buf, len};
}

static DsExpr *parser_take_field_call(DsExpr *field_expr, const DsToken *field, const DsToken *opener, bool bang) {
    bool stdlib = parser_expr_is_stdlib_namespace(field_expr->as.field.object);
    DsExpr *call = ds_expr_new(DS_EXPR_CALL,
                                   (DsSpan){field_expr->span.start, opener->span.end, field_expr->span.source});
    DsToken left = {.text = stdlib ? field_expr->as.field.object->as.text : (DsStr){"string", 6},
                    .span = field_expr->as.field.object->span};
    DsToken right = {.text = field_expr->as.field.field, .span = field->span};
    call->as.call.name = bang ? parser_copy_dotted_bang_name(&left, &right) : parser_copy_dotted_name(&left, &right);
    if (stdlib) {
        free(field_expr->as.field.object->as.text.data);
        free(field_expr->as.field.object);
    } else {
        DS_VEC_PUSH(&call->as.call.args, field_expr->as.field.object, 8);
    }
    free(field_expr->as.field.field.data);
    free(field_expr);
    return call;
}

static DsExpr *parser_take_ident_call(DsExpr *ident, const DsToken *opener, bool bang) {
    DsExpr *call = ds_expr_new(DS_EXPR_CALL, (DsSpan){ident->span.start, opener->span.end, ident->span.source});
    if (bang) {
        DsToken token = {.text = ident->as.text, .span = ident->span};
        call->as.call.name = parser_copy_bang_name(&token);
        free(ident->as.text.data);
    } else {
        call->as.call.name = ident->as.text;
    }
    free(ident);
    return call;
}

static DsExpr *parse_postfix(Parser *p) {
    DsExpr *expr = parse_primary(p);
    while (expr) {
        if (expr->kind == DS_EXPR_IDENT && ds_str_eq_cstr(expr->as.text, "glob") && parser_advance_if(p, DS_TOK_BANG)) {
            DsToken *bang = parser_previous(p);
            if (!parser_expect(p, DS_TOK_LPAREN, "expected `(` after `glob!`")) break;
            DsExpr *call = parser_take_ident_call(expr, bang, true);
            parse_call_args(p, &call->as.call.args);
            if (parser_expect(p, DS_TOK_RPAREN, "expected `)` after function call arguments")) call->span.end = parser_previous(p)->span.end;
            expr = call;
            continue;
        }

        if (expr->kind == DS_EXPR_IDENT && parser_advance_if(p, DS_TOK_LPAREN)) {
            DsToken *open = parser_previous(p);
            DsExpr *call = parser_take_ident_call(expr, open, false);
            parse_call_args(p, &call->as.call.args);
            expr = call;
            if (!parser_expect(p, DS_TOK_RPAREN, "expected `)` after function call arguments")) break;
            call->span.end = parser_previous(p)->span.end;
            continue;
        }

        if (parser_advance_if(p, DS_TOK_LBRACKET)) {
            DsToken *open = parser_previous(p);
            DsExpr *idx = NULL;
            if (parser_at(p, DS_TOK_RBRACKET)) {
                ds_diag_error(p->diag, open->span, "expected index expression after `[` ");
            } else {
                idx = parse_expr(p);
            }
            if (!parser_expect(p, DS_TOK_RBRACKET, "expected `]` after index expression")) break;
            DsExpr *index_expr = ds_expr_new(DS_EXPR_INDEX, (DsSpan){expr ? expr->span.start : open->span.start, parser_previous(p)->span.end, open->span.source});
            index_expr->as.index.object = expr;
            index_expr->as.index.index = idx;
            expr = index_expr;
            continue;
        }

        if (parser_advance_if(p, DS_TOK_DOT)) {
            DsToken *dot = parser_previous(p);
            if (!parser_expect_identifier_like(p, "expected field name after `.`")) break;
            DsToken *field = parser_previous(p);
            DsExpr *field_expr = ds_expr_new(DS_EXPR_FIELD, (DsSpan){expr ? expr->span.start : dot->span.start, field->span.end, dot->span.source});
            field_expr->as.field.object = expr;
            field_expr->as.field.field = parser_copy_token_text(field);
            expr = field_expr;
            if (parser_expr_is_stdlib_namespace(field_expr->as.field.object) && parser_advance_if(p, DS_TOK_BANG)) {
                DsToken *bang = parser_previous(p);
                if (!parser_expect(p, DS_TOK_LPAREN, "expected `(` after bang standard-library helper name")) break;
                DsExpr *call = parser_take_field_call(field_expr, field, bang, true);
                parse_call_args(p, &call->as.call.args);
                if (parser_expect(p, DS_TOK_RPAREN, "expected `)` after function call arguments")) call->span.end = parser_previous(p)->span.end;
                expr = call;
                continue;
            }
            if (parser_advance_if(p, DS_TOK_LPAREN)) {
                DsToken *open = parser_previous(p);
                DsExpr *call = parser_take_field_call(field_expr, field, open, false);
                parse_call_args(p, &call->as.call.args);
                if (parser_expect(p, DS_TOK_RPAREN, "expected `)` after method call arguments")) call->span.end = parser_previous(p)->span.end;
                expr = call;
            }
            continue;
        }

        break;
    }
    return expr;
}

static DsExpr *parse_unary(Parser *p) {
    if (parser_advance_if(p, DS_TOK_BANG) || parser_advance_if(p, DS_TOK_MINUS)) {
        DsToken *op = parser_previous(p);
        DsExpr *right = parse_unary(p);
        DsExpr *expr = ds_expr_new(DS_EXPR_UNARY, (DsSpan){op->span.start, right ? right->span.end : op->span.end, op->span.source});
        expr->as.unary.op = parser_copy_token_text(op);
        expr->as.unary.right = right;
        return expr;
    }
    return parse_postfix(p);
}

static DsExpr *parse_expr_prec(Parser *p, int min_prec) {
    DsExpr *left = parse_unary(p);

    while (!parser_is_stmt_end(p) && !parser_at(p, DS_TOK_LBRACE) && !parser_at(p, DS_TOK_RBRACE) && !parser_at(p, DS_TOK_RPAREN) && !parser_at(p, DS_TOK_RBRACKET) && !parser_at(p, DS_TOK_COMMA)) {
        DsTokenKind op_kind = parser_peek(p)->kind;
        int prec = precedence(op_kind);
        if (prec < min_prec || prec == 0) break;

        DsToken *op = parser_advance(p);
        DsExpr *right = parse_expr_prec(p, op_kind == DS_TOK_STAR_STAR ? prec : prec + 1);
        DsSpan span = {left ? left->span.start : op->span.start, right ? right->span.end : op->span.end, left ? left->span.source : op->span.source};
        if (op_kind == DS_TOK_DOT_DOT) {
            DsExpr *range = ds_expr_new(DS_EXPR_RANGE, span);
            range->as.range.start = left;
            range->as.range.end = right;
            left = range;
            continue;
        }
        DsExpr *binary = ds_expr_new(DS_EXPR_BINARY, span);
        binary->as.binary.left = left;
        binary->as.binary.op = parser_copy_token_text(op);
        binary->as.binary.right = right;
        left = binary;
    }

    return left;
}

DsExpr *parse_expr(Parser *p) {
    return parse_expr_prec(p, 1);
}

void parse_call_args(Parser *p, DsExprVec *args) {
    if (parser_at(p, DS_TOK_RPAREN)) return;
    while (!parser_at_end(p) && !parser_at(p, DS_TOK_RPAREN)) {
        DS_VEC_PUSH(args, parse_expr(p), 8);
        if (!parser_advance_if(p, DS_TOK_COMMA)) break;
        if (parser_reject_trailing_comma(p, DS_TOK_RPAREN, "expected function call argument after `,`")) break;
    }
}
