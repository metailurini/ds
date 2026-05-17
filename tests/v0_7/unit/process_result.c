#include "ds.h"

#include <assert.h>
#include <string.h>

static void expect_string(const DsValue *value, const char *expected) {
    assert(value->kind == DS_VALUE_STRING);
    assert(value->as.string.len == strlen(expected));
    assert(memcmp(value->as.string.data ? value->as.string.data : "", expected, value->as.string.len) == 0);
}

static void test_command_result_take_copy_and_free(void) {
    DsString out;
    DsString err;
    assert(ds_string_from_cstr(&out, "stdout"));
    assert(ds_string_from_cstr(&err, "stderr"));
    char *out_ptr = out.data;
    char *err_ptr = err.data;
    DsValue value = ds_value_command_result_take(&out, &err, 7);
    assert(out.data == NULL && out.len == 0);
    assert(err.data == NULL && err.len == 0);
    assert(value.kind == DS_VALUE_COMMAND_RESULT);
    assert(value.as.command_result.stdout_text.data == out_ptr);
    assert(value.as.command_result.stderr_text.data == err_ptr);
    assert(value.as.command_result.code == 7);

    DsValue copy = ds_value_copy(&value);
    assert(copy.kind == DS_VALUE_COMMAND_RESULT);
    assert(copy.as.command_result.code == 7);
    assert(copy.as.command_result.stdout_text.data != value.as.command_result.stdout_text.data);
    assert(copy.as.command_result.stderr_text.data != value.as.command_result.stderr_text.data);
    assert(copy.as.command_result.stdout_text.len == strlen("stdout"));
    assert(copy.as.command_result.stderr_text.len == strlen("stderr"));

    ds_value_free(&value);
    assert(copy.as.command_result.stdout_text.len == strlen("stdout"));
    assert(copy.as.command_result.stderr_text.len == strlen("stderr"));
    ds_value_free(&copy);
}

static void test_command_result_to_string_and_truthiness(void) {
    DsString out;
    DsString err;
    ds_string_init(&out);
    ds_string_init(&err);
    DsValue value = ds_value_command_result_take(&out, &err, 0);

    DsString rendered;
    assert(ds_value_to_string(&value, &rendered));
    DsValue rendered_value = ds_value_string_take(&rendered);
    expect_string(&rendered_value, "[command result]");
    ds_value_free(&rendered_value);

    bool truthy = false;
    assert(ds_value_truthy(&value, &truthy));
    assert(truthy == true);
    ds_value_free(&value);
}

static void test_large_command_result_copy(void) {
    DsString out;
    DsString err;
    ds_string_init(&out);
    ds_string_init(&err);
    for (int i = 0; i < 1024; i++) {
        assert(ds_string_append_cstr(&out, "0123456789"));
        assert(ds_string_append_cstr(&err, "abcdefghij"));
    }
    DsValue value = ds_value_command_result_take(&out, &err, 2);
    DsValue copy = ds_value_copy(&value);
    assert(copy.as.command_result.stdout_text.len == 10240);
    assert(copy.as.command_result.stderr_text.len == 10240);
    assert(copy.as.command_result.code == 2);
    ds_value_free(&value);
    ds_value_free(&copy);
}

int main(void) {
    test_command_result_take_copy_and_free();
    test_command_result_to_string_and_truthiness();
    test_large_command_result_copy();
    return 0;
}
