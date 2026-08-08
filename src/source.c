#include "ds_common.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

void ds_fatal_oom(void) {
    fprintf(stderr, "fatal: out of memory\n");
    exit(2);
}

void *ds_xmalloc(size_t size) {
    void *ptr = malloc(size);
    if (!ptr) ds_fatal_oom();
    return ptr;
}

char *ds_str_dup_range(const char *data, size_t len) {
    char *out = (char *)ds_xmalloc(len + 1);
    memcpy(out, data, len);
    out[len] = '\0';
    return out;
}

void *ds_xcalloc(size_t count, size_t size) {
    void *ptr = calloc(count, size);
    if (!ptr) ds_fatal_oom();
    return ptr;
}

void *ds_xrealloc(void *ptr, size_t size) {
    void *out = realloc(ptr, size);
    if (!out) ds_fatal_oom();
    return out;
}

bool ds_source_read(const char *path, DsSource *out, DsDiag *diag) {
    out->path = path;
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        DsSpan span = {{0, 1, 1}, {0, 1, 1}, out};
        ds_diag_error(diag, span, "failed to open source file `%s`: %s", path, strerror(errno));
        return false;
    }

    if (fseek(fp, 0, SEEK_END) != 0) {
        DsSpan span = {{0, 1, 1}, {0, 1, 1}, out};
        ds_diag_error(diag, span, "failed to read source file `%s`: %s", path, strerror(errno));
        fclose(fp);
        return false;
    }
    long size = ftell(fp);
    if (size < 0) {
        DsSpan span = {{0, 1, 1}, {0, 1, 1}, out};
        ds_diag_error(diag, span, "failed to read source file `%s`: %s", path, strerror(errno));
        fclose(fp);
        return false;
    }
    rewind(fp);

    char *data = (char *)ds_xmalloc((size_t)size + 1);

    size_t got = fread(data, 1, (size_t)size, fp);
    if (got < (size_t)size && ferror(fp)) {
        DsSpan span = {{0, 1, 1}, {0, 1, 1}, out};
        ds_diag_error(diag, span, "failed to read source file `%s`: %s", path, strerror(errno));
        free(data);
        fclose(fp);
        return false;
    }
    fclose(fp);
    data[got] = '\0';

    out->path = path;
    out->data = data;
    out->len = got;
    return true;
}

void ds_source_free(DsSource *source) {
    free(source->data);
    source->data = NULL;
    source->len = 0;
}
