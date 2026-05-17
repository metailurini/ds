#include "ds.h"

#include <assert.h>
#include <string.h>

static DsSpan span_at(int line, int column) {
    DsSpan span;
    memset(&span, 0, sizeof(span));
    span.start.line = line;
    span.start.column = column;
    span.end.line = line;
    span.end.column = column + 1;
    return span;
}

static DsWord word(const char *text, int column) {
    DsWord w;
    w.text.data = ds_str_dup_range(text, strlen(text));
    w.text.len = strlen(text);
    w.span = span_at(1, column);
    return w;
}

static void append_word(DsCommand *cmd, DsWord w) {
    size_t idx = cmd->words.len++;
    cmd->words.items = ds_xrealloc(cmd->words.items, cmd->words.len * sizeof(DsWord));
    cmd->words.items[idx] = w;
    cmd->words.cap = cmd->words.len;
}

int main(void) {
    DsCommand original;
    ds_command_init(&original, DS_COMMAND_CAPTURE, span_at(1, 1));
    append_word(&original, word("printf", 1));
    append_word(&original, word("hello", 8));
    original.redirect.kind = DS_REDIRECT_ALL_APPEND;
    original.redirect.target.data = ds_str_dup_range("log.txt", 7);
    original.redirect.target.len = 7;
    original.redirect.op_span = span_at(1, 14);
    original.redirect.target_span = span_at(1, 18);

    DsCommand clone;
    assert(ds_command_clone(&clone, &original));
    assert(clone.kind == DS_COMMAND_CAPTURE);
    assert(clone.words.len == 2);
    assert(clone.words.items[0].text.len == 6);
    assert(memcmp(clone.words.items[0].text.data, "printf", 6) == 0);
    assert(clone.words.items[1].span.start.column == 8);
    assert(clone.redirect.kind == DS_REDIRECT_ALL_APPEND);
    assert(clone.redirect.target.len == 7);
    assert(memcmp(clone.redirect.target.data, "log.txt", 7) == 0);
    assert(clone.redirect.op_span.start.column == 14);
    assert(clone.redirect.target_span.start.column == 18);

    ds_command_free(&original);
    assert(clone.words.len == 2);
    assert(memcmp(clone.words.items[1].text.data, "hello", 5) == 0);
    ds_command_free(&clone);
    assert(clone.words.len == 0);
    assert(clone.redirect.kind == DS_REDIRECT_NONE);

    ds_command_init(&original, DS_COMMAND_PLAIN, span_at(2, 3));
    assert(original.kind == DS_COMMAND_PLAIN);
    assert(original.words.len == 0);
    ds_command_free(&original);
    return 0;
}
