#include "ds.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

char *ds_str_dup_range(const char *data, size_t len) {
    char *out = (char *)malloc(len + 1);
    if (!out) {
        fprintf(stderr, "fatal: out of memory\n");
        exit(2);
    }
    memcpy(out, data, len);
    out[len] = '\0';
    return out;
}

void *ds_xcalloc(size_t count, size_t size) {
    void *ptr = calloc(count, size);
    if (!ptr) {
        fprintf(stderr, "fatal: out of memory\n");
        exit(2);
    }
    return ptr;
}

void *ds_xrealloc(void *ptr, size_t size) {
    void *out = realloc(ptr, size);
    if (!out) {
        fprintf(stderr, "fatal: out of memory\n");
        exit(2);
    }
    return out;
}

bool ds_source_read(const char *path, DsSource *out, DsDiag *diag) {
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        DsSpan span = {{0, 1, 1}, {0, 1, 1}};
        ds_diag_error(diag, span, "failed to open source file `%s`: %s", path, strerror(errno));
        return false;
    }

    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return false;
    }
    long size = ftell(fp);
    if (size < 0) {
        fclose(fp);
        return false;
    }
    rewind(fp);

    char *data = (char *)malloc((size_t)size + 1);
    if (!data) {
        fclose(fp);
        fprintf(stderr, "fatal: out of memory\n");
        exit(2);
    }

    size_t got = fread(data, 1, (size_t)size, fp);
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
