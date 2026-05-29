#include "ds.h"

#include <assert.h>
#include <string.h>

static DsStr view(const char *text) {
    DsStr out = {(char *)text, strlen(text)};
    return out;
}

static void expect_key(DsStr key, const char *expected) {
    assert(key.len == strlen(expected));
    assert(memcmp(key.data ? key.data : "", expected, key.len) == 0);
}

static void test_empty_map_sorted_keys(void) {
    DsMap map;
    ds_map_init(&map);

    DsStr *keys = (DsStr *)1;
    size_t len = 99;
    assert(ds_map_sorted_keys(&map, &keys, &len));
    assert(len == 0);
    assert(keys == NULL);

    ds_map_sorted_keys_free(keys, len);
    ds_map_free(&map);
}

static void test_sorted_keys_are_bytewise(void) {
    DsMap map;
    ds_map_init(&map);
    assert(ds_map_set(&map, view("2"), ds_value_int(2)));
    assert(ds_map_set(&map, view("10"), ds_value_int(10)));
    assert(ds_map_set(&map, view("1"), ds_value_int(1)));
    assert(ds_map_set(&map, view("b"), ds_value_int(4)));
    assert(ds_map_set(&map, view("A"), ds_value_int(5)));

    DsStr *keys = NULL;
    size_t len = 0;
    assert(ds_map_sorted_keys(&map, &keys, &len));
    assert(len == 5);
    expect_key(keys[0], "1");
    expect_key(keys[1], "10");
    expect_key(keys[2], "2");
    expect_key(keys[3], "A");
    expect_key(keys[4], "b");

    ds_map_sorted_keys_free(keys, len);
    ds_map_free(&map);
}

int main(void) {
    test_empty_map_sorted_keys();
    test_sorted_keys_are_bytewise();
    return 0;
}