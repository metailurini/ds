#include "ds.h"
#include "ds_command_facts.h"

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

static void append_word_to_stage(DsCommand *cmd, size_t stage_index, DsWord w) {
    assert(stage_index < cmd->stages.len);
    DsWordVec *words = &cmd->stages.items[stage_index].words;
    size_t idx = words->len++;
    words->items = ds_xrealloc(words->items, words->len * sizeof(DsWord));
    words->items[idx] = w;
    words->cap = words->len;
}

static void append_word(DsCommand *cmd, DsWord w) {
    if (cmd->stages.len == 0) {
        cmd->stages.len = 1;
        cmd->stages.cap = 1;
        cmd->stages.items = ds_xcalloc(1, sizeof(DsCommandStage));
        ds_command_stage_init(&cmd->stages.items[0]);
        cmd->stages.items[0].span = cmd->span;
    }
    append_word_to_stage(cmd, 0, w);
}

static void append_empty_stage(DsCommand *cmd, int column) {
    size_t idx = cmd->stages.len++;
    cmd->stages.items = ds_xrealloc(cmd->stages.items, cmd->stages.len * sizeof(DsCommandStage));
    cmd->stages.cap = cmd->stages.len;
    ds_command_stage_init(&cmd->stages.items[idx]);
    cmd->stages.items[idx].span = span_at(1, column);
}

static void assert_clone_result_contracts(void) {
    bool (*word_vec_clone)(DsWordVec *, const DsWordVec *) = ds_word_vec_clone;
    bool (*stage_clone)(DsCommandStage *, const DsCommandStage *) = ds_command_stage_clone;
    bool (*stage_vec_clone)(DsCommandStageVec *, const DsCommandStageVec *) = ds_command_stage_vec_clone;
    bool (*redirect_clone)(DsRedirect *, const DsRedirect *) = ds_redirect_clone;
    bool (*command_clone)(DsCommand *, const DsCommand *) = ds_command_clone;
    assert(word_vec_clone && stage_clone && stage_vec_clone && redirect_clone && command_clone);
}

int main(void) {
    assert_clone_result_contracts();
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
    assert(ds_command_stage_count(&original) == 0);
    assert(!ds_command_is_pipeline(&original));
    append_word(&original, word("printf", 3));
    assert(ds_command_stage_count(&original) == 1);
    assert(!ds_command_is_pipeline(&original));
    append_empty_stage(&original, 12);
    append_word_to_stage(&original, 1, word("cat", 12));
    assert(ds_command_stage_count(&original) == 2);
    assert(ds_command_is_pipeline(&original));
    ds_command_free(&original);

    int ok_codes[] = {0, 0, 0};
    int left_fail_codes[] = {7, 0, 0};
    int right_fail_codes[] = {7, 0, 3};
    assert(ds_command_pipeline_status(ok_codes, 3) == 0);
    assert(ds_command_pipeline_status(left_fail_codes, 3) == 7);
    assert(ds_command_pipeline_status(right_fail_codes, 3) == 3);
    assert(ds_command_pipeline_status(NULL, 3) == 0);
    return 0;
}
