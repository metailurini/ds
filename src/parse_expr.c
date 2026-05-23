#include "parser_internal.h"

static int precedence(DsTokenKind kind) {
    switch (kind) {
        case DS_TOK_IN:
        case DS_TOK_MATCHES: return 1;
        case DS_TOK_EQUAL_EQUAL:
        case DS_TOK_BANG_EQUAL: return 1;
        case DS_TOK_DOT_DOT: return 2;
        case DS_TOK_GREATER:
        case DS_TOK_GREATER_EQUAL:
        case DS_TOK_LESS:
        case DS_TOK_LESS_EQUAL: return 2;
        case DS_TOK_PLUS:
        case DS_TOK_MINUS: return 3;
        case DS_TOK_STAR:
        case DS_TOK_SLASH:
        case DS_TOK_PERCENT: return 4;
        case DS_TOK_STAR_STAR: return 5;
        default: return 0;
    }
}

static DsExpr *parse_expr_prec(Parser *p, int min_prec);
DsExpr *parse_expr(Parser *p);
void parse_call_args(Parser *p, DsExprVec *args);

static void skip_collection_newlines(Parser *p) { parser_skip_newlines(p); }

static DsExpr *parse_array_literal(Parser *p) {
    DsToken *open = parser_previous(p);
    DsExpr *expr = parser_new_expr(DS_EXPR_ARRAY, open->span);
    skip_collection_newlines(p);
    if (!parser_at(p, DS_TOK_RBRACKET)) {
        while (!parser_at_end(p) && !parser_at(p, DS_TOK_RBRACKET)) {
            parser_expr_vec_push(&expr->as.array.elements, parse_expr(p));
            skip_collection_newlines(p);
            if (!parser_advance_if(p, DS_TOK_COMMA)) break;
            skip_collection_newlines(p);
            if (parser_at(p, DS_TOK_RBRACKET)) {
                ds_diag_error(p->diag, parser_peek(p)->span, "expected array element after `,`");
                break;
            }
        }
    }
    if (!parser_expect(p, DS_TOK_RBRACKET, "expected `]` to close array literal")) return expr;
    expr->span.end = parser_previous(p)->span.end;
    return expr;
}

static DsExpr *parse_map_literal(Parser *p) {
    DsToken *open = parser_previous(p);
    DsExpr *expr = parser_new_expr(DS_EXPR_MAP, open->span);
    skip_collection_newlines(p);
    if (parser_at(p, DS_TOK_RBRACE)) {
        ds_diag_error(p->diag, parser_peek(p)->span, "empty map literals are deferred in v0.10.0");
        parser_advance(p);
        expr->span.end = parser_previous(p)->span.end;
        return expr;
    }
    while (!parser_at_end(p) && !parser_at(p, DS_TOK_RBRACE)) {
        DsMapEntry entry;
        memset(&entry, 0, sizeof(entry));
        if (parser_advance_if(p, DS_TOK_IDENT) || parser_advance_if(p, DS_TOK_STRING)) {
            DsToken *key = parser_previous(p);
            entry.key = parser_copy_token_text(key);
            entry.quoted_key = key->kind == DS_TOK_STRING;
            entry.span = key->span;
        } else {
            ds_diag_error(p->diag, parser_peek(p)->span, "expected map key before `:`");
            break;
        }
        if (!parser_expect(p, DS_TOK_COLON, "expected `:` after map key")) break;
        skip_collection_newlines(p);
        if (parser_at(p, DS_TOK_COMMA) || parser_at(p, DS_TOK_RBRACE) || parser_at_end(p)) {
            ds_diag_error(p->diag, parser_peek(p)->span, "expected map value after `:`");
            break;
        }
        entry.value = parse_expr(p);
        if (entry.value) entry.span.end = entry.value->span.end;
        parser_map_entry_vec_push(&expr->as.map.entries, entry);
        skip_collection_newlines(p);
        if (!parser_advance_if(p, DS_TOK_COMMA)) break;
        skip_collection_newlines(p);
        if (parser_at(p, DS_TOK_RBRACE)) {
            ds_diag_error(p->diag, parser_peek(p)->span, "expected map entry after `,`");
            break;
        }
    }
    if (!parser_expect(p, DS_TOK_RBRACE, "expected `}` to close map literal")) return expr;
    expr->span.end = parser_previous(p)->span.end;
    return expr;
}


static DsExpr *parse_primary(Parser *p) {
    DsToken *tok = parser_peek(p);
    if (parser_advance_if(p, DS_TOK_RUN)) {
        return parse_run_expr(p);
    }
    if (parser_advance_if(p, DS_TOK_IDENT)) {
        DsExpr *expr = parser_new_expr(DS_EXPR_IDENT, tok->span);
        expr->as.text = parser_copy_token_text(tok);
        return expr;
    }
    if (parser_advance_if(p, DS_TOK_TYPE_STRING) || parser_advance_if(p, DS_TOK_TYPE_INT) ||
        parser_advance_if(p, DS_TOK_TYPE_BOOL) || parser_advance_if(p, DS_TOK_SCRIPT) ||
        parser_advance_if(p, DS_TOK_IMPORT) || parser_advance_if(p, DS_TOK_ARG) || parser_advance_if(p, DS_TOK_OPTION) || parser_advance_if(p, DS_TOK_FLAG)) {
        DsToken *used = parser_previous(p);
        DsExpr *expr = parser_new_expr(DS_EXPR_IDENT, used->span);
        expr->as.text = parser_copy_token_text(used);
        return expr;
    }
    if (parser_advance_if(p, DS_TOK_STRING)) {
        DsExpr *expr = parser_new_expr(DS_EXPR_STRING, tok->span);
        expr->as.text = parser_copy_token_text(tok);
        return expr;
    }
    if (parser_advance_if(p, DS_TOK_INT)) {
        DsExpr *expr = parser_new_expr(DS_EXPR_INT, tok->span);
        expr->as.text = parser_copy_token_text(tok);
        return expr;
    }
    if (parser_advance_if(p, DS_TOK_TRUE) || parser_advance_if(p, DS_TOK_FALSE)) {
        DsToken *used = parser_previous(p);
        DsExpr *expr = parser_new_expr(DS_EXPR_BOOL, used->span);
        expr->as.boolean = used->kind == DS_TOK_TRUE;
        return expr;
    }
    if (parser_advance_if(p, DS_TOK_REGEX)) {
        DsExpr *expr = parser_new_expr(DS_EXPR_REGEX, tok->span);
        expr->as.regex = parser_copy_token_text(tok);
        return expr;
    }
    if (parser_advance_if(p, DS_TOK_LPAREN)) {
        DsToken *open = parser_previous(p);
        DsExpr *expr = parse_expr_prec(p, 1);
        if (!parser_expect(p, DS_TOK_RPAREN, "expected `)` after expression")) {
            return expr ? expr : parser_new_expr(DS_EXPR_ERROR, open->span);
        }
        if (expr) expr->span.end = parser_previous(p)->span.end;
        return expr;
    }
    if (parser_advance_if(p, DS_TOK_LBRACKET)) {
        return parse_array_literal(p);
    }
    if (parser_advance_if(p, DS_TOK_LBRACE)) {
        return parse_map_literal(p);
    }

    ds_diag_error(p->diag, tok->span, "expected expression");
    return parser_new_expr(DS_EXPR_ERROR, tok->span);
}

static DsExpr *parse_postfix(Parser *p) {
    DsExpr *expr = parse_primary(p);
    if (expr && expr->kind == DS_EXPR_IDENT && strncmp(expr->as.text.data, "glob", expr->as.text.len) == 0 && expr->as.text.len == 4 && parser_advance_if(p, DS_TOK_BANG)) {
        DsToken *bang = parser_previous(p);
        if (parser_expect(p, DS_TOK_LPAREN, "expected `(` after `glob!`")) {
            DsExpr *call = parser_new_expr(DS_EXPR_CALL, (DsSpan){expr->span.start, bang->span.end, expr->span.source});
            DsToken tmp = {.text = expr->as.text, .span = expr->span};
            call->as.call.name = parser_copy_bang_name(&tmp);
            free(expr->as.text.data);
            free(expr);
            parse_call_args(p, &call->as.call.args);
            if (parser_expect(p, DS_TOK_RPAREN, "expected `)` after function call arguments")) call->span.end = parser_previous(p)->span.end;
            expr = call;
        }
    }
    while (expr && expr->kind == DS_EXPR_IDENT && parser_advance_if(p, DS_TOK_LPAREN)) {
        DsToken *open = parser_previous(p);
        DsExpr *call = parser_new_expr(DS_EXPR_CALL, (DsSpan){expr->span.start, open->span.end, expr->span.source});
        call->as.call.name = parser_copy_token_text(&(DsToken){.text = expr->as.text, .span = expr->span});
        free(expr->as.text.data);
        free(expr);
        parse_call_args(p, &call->as.call.args);
        expr = call;
        if (!parser_expect(p, DS_TOK_RPAREN, "expected `)` after function call arguments")) break;
        call->span.end = parser_previous(p)->span.end;
    }
    while (parser_advance_if(p, DS_TOK_LBRACKET)) {
        DsToken *open = parser_previous(p);
        DsExpr *idx = NULL;
        if (parser_at(p, DS_TOK_RBRACKET)) {
            ds_diag_error(p->diag, open->span, "expected index expression after `[` ");
        } else {
            idx = parse_expr(p);
        }
        if (!parser_expect(p, DS_TOK_RBRACKET, "expected `]` after index expression")) break;
        DsExpr *index_expr = parser_new_expr(DS_EXPR_INDEX, (DsSpan){expr ? expr->span.start : open->span.start, parser_previous(p)->span.end, open->span.source});
        index_expr->as.index.object = expr;
        index_expr->as.index.index = idx;
        expr = index_expr;
    }
    while (parser_advance_if(p, DS_TOK_DOT)) {
        DsToken *dot = parser_previous(p);
        if (!parser_expect_identifier_like(p, "expected field name after `.`")) {
            break;
        }
        DsToken *field = parser_previous(p);
        DsExpr *field_expr = parser_new_expr(DS_EXPR_FIELD, (DsSpan){expr ? expr->span.start : dot->span.start, field->span.end, dot->span.source});
        field_expr->as.field.object = expr;
        field_expr->as.field.field = parser_copy_token_text(field);
        expr = field_expr;
        if (parser_advance_if(p, DS_TOK_LPAREN)) {
            DsToken *open = parser_previous(p);
            DsExpr *call = parser_new_expr(DS_EXPR_CALL, (DsSpan){field_expr->span.start, open->span.end, dot->span.source});
            bool namespace_call = field_expr->as.field.object && field_expr->as.field.object->kind == DS_EXPR_IDENT &&
                ((field_expr->as.field.object->as.text.len == 4 && memcmp(field_expr->as.field.object->as.text.data, "file", 4) == 0) ||
                 (field_expr->as.field.object->as.text.len == 3 && memcmp(field_expr->as.field.object->as.text.data, "dir", 3) == 0) ||
                 (field_expr->as.field.object->as.text.len == 4 && memcmp(field_expr->as.field.object->as.text.data, "path", 4) == 0) ||
                 (field_expr->as.field.object->as.text.len == 3 && memcmp(field_expr->as.field.object->as.text.data, "cmd", 3) == 0) ||
                 (field_expr->as.field.object->as.text.len == 3 && memcmp(field_expr->as.field.object->as.text.data, "env", 3) == 0));
            if (namespace_call) {
                DsToken left = {.text = field_expr->as.field.object->as.text, .span = field_expr->as.field.object->span};
                DsToken right = {.text = field_expr->as.field.field, .span = field->span};
                call->as.call.name = parser_copy_dotted_name(&left, &right);
                free(field_expr->as.field.object->as.text.data);
                free(field_expr->as.field.object);
            } else {
                DsToken left = {.text = (DsStr){"string", 6}, .span = field->span};
                DsToken right = {.text = field_expr->as.field.field, .span = field->span};
                call->as.call.name = parser_copy_dotted_name(&left, &right);
                parser_expr_vec_push(&call->as.call.args, field_expr->as.field.object);
            }
            free(field_expr->as.field.field.data);
            free(field_expr);
            parse_call_args(p, &call->as.call.args);
            if (parser_expect(p, DS_TOK_RPAREN, "expected `)` after method call arguments")) call->span.end = parser_previous(p)->span.end;
            expr = call;
        }
    }
    return expr;
}

static DsExpr *parse_unary(Parser *p) {
    if (parser_advance_if(p, DS_TOK_BANG) || parser_advance_if(p, DS_TOK_MINUS)) {
        DsToken *op = parser_previous(p);
        DsExpr *right = parse_unary(p);
        DsExpr *expr = parser_new_expr(DS_EXPR_UNARY, (DsSpan){op->span.start, right ? right->span.end : op->span.end, op->span.source});
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
            DsExpr *range = parser_new_expr(DS_EXPR_RANGE, span);
            range->as.range.start = left;
            range->as.range.end = right;
            left = range;
            continue;
        }
        DsExpr *binary = parser_new_expr(DS_EXPR_BINARY, span);
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
        parser_expr_vec_push(args, parse_expr(p));
        if (!parser_advance_if(p, DS_TOK_COMMA)) break;
        if (parser_at(p, DS_TOK_RPAREN)) {
            ds_diag_error(p->diag, parser_peek(p)->span, "expected function call argument after `,`");
            break;
        }
    }
}
