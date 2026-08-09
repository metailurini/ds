#include "ds.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static DsStr view(const char *text) {
    DsStr out = {(char *)text, strlen(text)};
    return out;
}

static void expect_value_string(const DsValue *value, const char *expected) {
    assert(value->kind == DS_VALUE_STRING);
    assert(value->as.string.len == strlen(expected));
    assert(memcmp(value->as.string.data ? value->as.string.data : "", expected, value->as.string.len) == 0);
}

static void test_string_take_and_copy_are_independent(void) {
    DsString source;
    assert(ds_string_from_cstr(&source, "owned"));
    char *original = source.data;
    DsValue taken = ds_value_string_take(&source);
    assert(source.data == NULL);
    assert(source.len == 0);
    assert(taken.as.string.data == original);

    DsValue copy = ds_value_copy(&taken);
    expect_value_string(&taken, "owned");
    expect_value_string(&copy, "owned");
    assert(copy.as.string.data != taken.as.string.data);

    ds_value_free(&taken);
    expect_value_string(&copy, "owned");
    ds_value_free(&copy);
}

static void test_array_clear_reuse_borrowed_items(void) {
    DsArray array;
    ds_array_init(&array);
    int first = 1;
    int second = 2;
    DS_VEC_PUSH(&array, &first, 8);
    DS_VEC_PUSH(&array, &second, 8);
    assert(array.len == 2);
    size_t cap = array.cap;
    ds_array_clear(&array);
    assert(array.len == 0);
    assert(array.cap == cap);
    assert(array.items != NULL);
    DS_VEC_PUSH(&array, &second, 8);
    assert(array.len == 1);
    assert(*(int *)array.items[0] == 2);
    ds_array_free(&array);
    assert(array.items == NULL && array.len == 0 && array.cap == 0);
}

static void test_map_key_copy_update_clear_and_reuse(void) {
    DsMap map;
    ds_map_init(&map);

    char mutable_key[] = "alpha";
    DsString value;
    assert(ds_string_from_cstr(&value, "first"));
    assert(ds_map_set(&map, view(mutable_key), ds_value_string_take(&value)));
    mutable_key[0] = 'X';
    DsValue *found = ds_map_get(&map, view("alpha"));
    assert(found != NULL);
    expect_value_string(found, "first");

    assert(ds_string_from_cstr(&value, "second"));
    assert(ds_map_set(&map, view("alpha"), ds_value_string_take(&value)));
    found = ds_map_get(&map, view("alpha"));
    assert(found != NULL);
    expect_value_string(found, "second");
    assert(ds_map_len(&map) == 1);

    assert(ds_map_set(&map, view(""), ds_value_int(0)));
    assert(ds_map_set(&map, view("common_prefix_a"), ds_value_int(1)));
    assert(ds_map_set(&map, view("common_prefix_b"), ds_value_int(2)));
    char long_key[256];
    memset(long_key, 'k', sizeof(long_key) - 1);
    long_key[sizeof(long_key) - 1] = '\0';
    assert(ds_map_set(&map, view(long_key), ds_value_bool(true)));
    assert(ds_map_get(&map, view(long_key))->kind == DS_VALUE_BOOL);

    for (int i = 0; i < 128; i++) {
        char key[32];
        snprintf(key, sizeof(key), "collision_%03d", i);
        assert(ds_map_set(&map, view(key), ds_value_int(i)));
    }
    assert(ds_map_get(&map, view("collision_127"))->as.integer == 127);

    ds_map_clear(&map);
    assert(ds_map_len(&map) == 0);
    assert(map.impl != NULL);
    assert(ds_map_get(&map, view("alpha")) == NULL);
    assert(ds_map_set(&map, view("reuse"), ds_value_int(42)));
    assert(ds_map_get(&map, view("reuse"))->as.integer == 42);
    ds_map_free(&map);
    assert(map.impl == NULL);
}

int main(void) {
    test_string_take_and_copy_are_independent();
    test_array_clear_reuse_borrowed_items();
    test_map_key_copy_update_clear_and_reuse();
    return 0;
}
