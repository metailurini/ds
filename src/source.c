#include "ds_common.h"

#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

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
    if (data) memcpy(out, data, len);
    else memset(out, 0, len);
    out[len] = '\0';
    return out;
}

void *ds_xcalloc(size_t count, size_t size) {
    void *ptr = calloc(count ? count : 1, size ? size : 1);
    if (!ptr) ds_fatal_oom();
    return ptr;
}

void *ds_xrealloc(void *ptr, size_t size) {
    void *out = realloc(ptr, size);
    if (!out) ds_fatal_oom();
    return out;
}

bool ds_parse_i64_range(const char *data, size_t len, int64_t *out) {
    if (!data || len == 0 || !out) return false;
    char *text = ds_str_dup_range(data, len);
    char *end = NULL;
    errno = 0;
    long long value = strtoll(text, &end, 10);
    bool ok = errno != ERANGE && end == text + len;
    if (ok) *out = (int64_t)value;
    free(text);
    return ok;
}

bool ds_source_read(const char *path, DsSource *out, DsDiag *diag) {
    out->path = path;
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        ds_diag_error(diag, ds_span_zero(out), "failed to open source file `%s`: %s", path, strerror(errno));
        return false;
    }

    bool ok = false;
    char *data = NULL;
    struct stat st;
    if (fstat(fileno(fp), &st) != 0) {
        goto read_error;
    }
    if (S_ISDIR(st.st_mode)) {
        errno = EISDIR;
        goto read_error;
    }
    if (fseek(fp, 0, SEEK_END) != 0) {
        goto read_error;
    }
    long size = ftell(fp);
    if (size < 0) {
        goto read_error;
    }
    rewind(fp);

    data = (char *)ds_xmalloc((size_t)size + 1);

    size_t got = fread(data, 1, (size_t)size, fp);
    if (got < (size_t)size && ferror(fp)) {
        goto read_error;
    }
    data[got] = '\0';

    out->data = data;
    out->len = got;
    data = NULL;
    ok = true;
    goto cleanup;

read_error:
    ds_diag_error(diag, ds_span_zero(out), "failed to read source file `%s`: %s", path, strerror(errno));
cleanup:
    free(data);
    fclose(fp);
    return ok;
}

void ds_source_free(DsSource *source) {
    free(source->data);
    source->data = NULL;
    source->len = 0;
}

bool ds_file_write_atomic(const char *path, const char *data, size_t len) {
    struct stat st;
    mode_t mode = stat(path, &st) == 0 ? st.st_mode & 0777 : 0644;
    size_t path_len = strlen(path);
    size_t tmp_len = path_len + sizeof(".tmp.XXXXXX");
    char *tmp = (char *)ds_xmalloc(tmp_len);
    snprintf(tmp, tmp_len, "%s.tmp.XXXXXX", path);

    int fd = mkstemp(tmp);
    if (fd < 0) {
        fprintf(stderr, "error: failed to write `%s`: %s\n", path, strerror(errno));
        free(tmp);
        return false;
    }
    if (fchmod(fd, mode) != 0) {
        int saved_errno = errno;
        close(fd);
        unlink(tmp);
        fprintf(stderr, "error: failed to write `%s`: %s\n", path, strerror(saved_errno));
        free(tmp);
        return false;
    }

    FILE *out = fdopen(fd, "wb");
    if (!out) {
        int saved_errno = errno;
        close(fd);
        unlink(tmp);
        fprintf(stderr, "error: failed to write `%s`: %s\n", path, strerror(saved_errno));
        free(tmp);
        return false;
    }

    int saved_errno = 0;
    if (len > 0 && fwrite(data, 1, len, out) != len) saved_errno = errno ? errno : EIO;
    if (fclose(out) != 0 && saved_errno == 0) saved_errno = errno;
    if (saved_errno == 0 && rename(tmp, path) != 0) saved_errno = errno;
    if (saved_errno != 0) {
        fprintf(stderr, "error: failed to write `%s`: %s\n", path, strerror(saved_errno));
        unlink(tmp);
    }
    free(tmp);
    return saved_errno == 0;
}
