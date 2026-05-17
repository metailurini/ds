#include "ds.h"

#include <assert.h>
#include <string.h>

static DsStr view(const char *s) {
    DsStr out = {(char *)s, strlen(s)};
    return out;
}

int main(void) {
    const DsCommandResultField *field;
    field = ds_command_result_field_lookup(view("stdout"));
    assert(field && field->kind == DS_COMMAND_RESULT_FIELD_STRING);
    assert(strcmp(ds_command_result_field_kind_name(field->kind), "string") == 0);
    field = ds_command_result_field_lookup(view("stderr"));
    assert(field && field->kind == DS_COMMAND_RESULT_FIELD_STRING);
    field = ds_command_result_field_lookup(view("code"));
    assert(field && field->kind == DS_COMMAND_RESULT_FIELD_INT);
    assert(strcmp(ds_command_result_field_kind_name(field->kind), "int") == 0);
    field = ds_command_result_field_lookup(view("ok"));
    assert(field && field->kind == DS_COMMAND_RESULT_FIELD_BOOL);
    assert(strcmp(ds_command_result_field_kind_name(field->kind), "bool") == 0);
    field = ds_command_result_field_lookup(view("failed"));
    assert(field && field->kind == DS_COMMAND_RESULT_FIELD_BOOL);

    assert(ds_command_result_field_lookup(view("missing")) == NULL);
    assert(ds_command_result_field_lookup(view("status")) == NULL);
    assert(ds_command_result_field_lookup(view("exit_code")) == NULL);
    assert(ds_command_result_field_lookup(view("out")) == NULL);
    assert(ds_command_result_field_lookup(view("err")) == NULL);
    assert(ds_command_result_field_lookup(view("stdout.trim")) == NULL);
    return 0;
}
