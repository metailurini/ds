#include "ds.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static DsStr str_view(const char *s) {
    DsStr out = {(char *)s, strlen(s)};
    return out;
}

static void expect_string(DsString *s, const char *expected) {
    assert(s->len == strlen(expected));
    assert(s->data != NULL || s->len == 0);
    assert(memcmp(s->data ? s->data : "", expected, s->len) == 0);
    if (s->data) assert(s->data[s->len] == '\0');
}

static void test_string(void) {
    DsString s;
    ds_string_init(&s);
    assert(s.data == NULL);
    assert(s.len == 0);
    assert(s.cap == 0);

    assert(ds_string_append_cstr(&s, "hello"));
    assert(ds_string_append_char(&s, ' '));
    assert(ds_string_append_range(&s, "world", 5));
    expect_string(&s, "hello world");

    assert(ds_string_append_range(&s, "", 0));
    expect_string(&s, "hello world");

    for (int i = 0; i < 128; i++) assert(ds_string_append_char(&s, 'x'));
    assert(s.len == strlen("hello world") + 128);
    assert(s.data[s.len] == '\0');
    ds_string_free(&s);
    assert(s.data == NULL);
    assert(s.len == 0);
    assert(s.cap == 0);

    assert(ds_string_from_cstr(&s, "$HOME $(echo bad) `bad` {name} \\ \""));
    expect_string(&s, "$HOME $(echo bad) `bad` {name} \\ \"");
    ds_string_free(&s);

    const char bytes[] = {'a', '\0', 'b'};
    assert(ds_string_from_range(&s, bytes, sizeof(bytes)));
    assert(s.len == 3);
    assert(memcmp(s.data, bytes, 3) == 0);
    assert(s.data[3] == '\0');
    ds_string_free(&s);

    assert(ds_string_from_range(&s, NULL, 0));
    assert(s.len == 0);
    assert(s.data != NULL);
    assert(s.data[0] == '\0');
    ds_string_free(&s);
}

static void test_values(void) {
    DsValue n = ds_value_null();
    DsValue t = ds_value_bool(true);
    DsValue f = ds_value_bool(false);
    DsValue z = ds_value_int(0);
    DsValue one = ds_value_int(1);
    DsValue big = ds_value_int(INT64_C(1234567890123));

    bool truth = true;
    assert(ds_value_truthy(&n, &truth) && truth == false);
    assert(ds_value_truthy(&t, &truth) && truth == true);
    assert(ds_value_truthy(&f, &truth) && truth == false);
    assert(ds_value_truthy(&z, &truth) && truth == false);
    assert(ds_value_truthy(&one, &truth) && truth == true);
    assert(ds_value_truthy(&big, &truth) && truth == true);

    DsString out;
    assert(ds_value_to_string(&n, &out));
    expect_string(&out, "null");
    ds_string_free(&out);
    assert(ds_value_to_string(&t, &out));
    expect_string(&out, "true");
    ds_string_free(&out);
    assert(ds_value_to_string(&big, &out));
    expect_string(&out, "1234567890123");
    ds_string_free(&out);

    DsString source;
    assert(ds_string_from_cstr(&source, "owned string"));
    DsValue sv = ds_value_string_take(&source);
    assert(source.data == NULL && source.len == 0);
    assert(ds_value_truthy(&sv, &truth) && truth == true);
    assert(ds_value_to_string(&sv, &out));
    expect_string(&out, "owned string");
    ds_string_free(&out);

    DsValue copy = ds_value_copy(&sv);
    assert(copy.kind == DS_VALUE_STRING);
    assert(copy.as.string.data != sv.as.string.data);
    assert(ds_value_compare(&sv, &copy) == 0);

    DsValue a = ds_value_int(10);
    DsValue b = ds_value_int(2);
    assert(ds_value_compare(&a, &b) > 0); /* integers compare numerically. */
    assert(ds_value_compare(&t, &f) > 0);

    ds_value_free(&copy);
    ds_value_free(&sv);
    ds_value_free(&n);
    ds_value_free(&t);
    ds_value_free(&f);
    ds_value_free(&z);
    ds_value_free(&one);
    ds_value_free(&big);
    ds_value_free(&a);
    ds_value_free(&b);
}

static void test_array(void) {
    DsArray array;
    ds_array_init(&array);
    assert(array.items == NULL && array.len == 0 && array.cap == 0);
    int values[64];
    for (int i = 0; i < 64; i++) {
        values[i] = i;
        assert(ds_array_push(&array, &values[i]));
        assert(array.len == (size_t)i + 1);
    }
    for (int i = 0; i < 64; i++) assert(*(int *)array.items[i] == i);
    ds_array_free(&array);
    assert(array.items == NULL && array.len == 0 && array.cap == 0);
}

static void test_map(void) {
    DsMap map;
    ds_map_init(&map);
    assert(ds_map_len(&map) == 0);
    assert(ds_map_get(&map, str_view("missing")) == NULL);

    assert(ds_map_set(&map, str_view("key"), ds_value_int(1)));
    assert(ds_map_set(&map, str_view("key_1"), ds_value_bool(true)));
    assert(ds_map_set(&map, str_view("key_10"), ds_value_int(10)));
    assert(ds_map_len(&map) == 3);

    DsValue *found = ds_map_get(&map, str_view("key"));
    assert(found != NULL && found->kind == DS_VALUE_INT && found->as.integer == 1);
    assert(ds_map_set(&map, str_view("key"), ds_value_int(2)));
    found = ds_map_get(&map, str_view("key"));
    assert(found != NULL && found->kind == DS_VALUE_INT && found->as.integer == 2);
    assert(ds_map_len(&map) == 3);

    for (int i = 0; i < 80; i++) {
        char key[32];
        snprintf(key, sizeof(key), "many_%02d", i);
        assert(ds_map_set(&map, str_view(key), ds_value_int(i)));
    }
    found = ds_map_get(&map, str_view("many_79"));
    assert(found != NULL && found->kind == DS_VALUE_INT && found->as.integer == 79);
    assert(ds_map_get(&map, str_view("many_80")) == NULL);
    ds_map_free(&map);
    assert(map.impl == NULL);
}

int main(void) {
    test_string();
    test_values();
    test_array();
    test_map();
    return 0;
}
