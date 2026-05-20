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
    if (cmd->stages.len == 0) {
        cmd->stages.len = 1;
        cmd->stages.cap = 1;
        cmd->stages.items = ds_xcalloc(1, sizeof(DsCommandStage));
        ds_command_stage_init(&cmd->stages.items[0]);
        cmd->stages.items[0].span = cmd->span;
    }
    DsWordVec *words = &cmd->stages.items[0].words;
    size_t idx = words->len++;
    words->items = ds_xrealloc(words->items, words->len * sizeof(DsWord));
    words->items[idx] = w;
    words->cap = words->len;
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
    assert(clone.stages.len == 1);
    assert(clone.stages.items[0].words.len == 2);
    assert(clone.stages.items[0].words.items[0].text.len == 6);
    assert(memcmp(clone.stages.items[0].words.items[0].text.data, "printf", 6) == 0);
    assert(clone.stages.items[0].words.items[1].span.start.column == 8);
    assert(clone.redirect.kind == DS_REDIRECT_ALL_APPEND);
    assert(clone.redirect.target.len == 7);
    assert(memcmp(clone.redirect.target.data, "log.txt", 7) == 0);
    assert(clone.redirect.op_span.start.column == 14);
    assert(clone.redirect.target_span.start.column == 18);

    ds_command_free(&original);
    assert(clone.stages.items[0].words.len == 2);
    assert(memcmp(clone.stages.items[0].words.items[1].text.data, "hello", 5) == 0);
    ds_command_free(&clone);
    assert(clone.stages.len == 0);
    assert(clone.redirect.kind == DS_REDIRECT_NONE);

    ds_command_init(&original, DS_COMMAND_PLAIN, span_at(2, 3));
    assert(original.kind == DS_COMMAND_PLAIN);
    assert(original.stages.len == 0);
    ds_command_free(&original);
    return 0;
}
