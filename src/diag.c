#include "ds.h"

#include <stdarg.h>
#include <string.h>

void ds_diag_init(DsDiag *diag, const DsSource *source) {
    diag->source = source;
    diag->has_error = false;
}

void ds_diag_format_location(const DsSource *source, DsSpan span, char *buf, size_t buf_len) {
    if (span.source) source = span.source;
    const char *path = source && source->path ? source->path : "<source>";
    if (!buf || buf_len == 0) return;
    snprintf(buf, buf_len, "%s:%d:%d", path, span.start.line, span.start.column);
}

static void print_source_line(const DsSource *source, int wanted_line, int column) {
    if (!source || !source->data || wanted_line <= 0) return;

    const char *start = source->data;
    int line = 1;
    for (size_t i = 0; i < source->len; i++) {
        if (line == wanted_line) {
            start = source->data + i;
            break;
        }
        if (source->data[i] == '\n') line++;
    }
    if (line != wanted_line && wanted_line != 1) return;

    const char *end = start;
    while (*end && *end != '\n' && *end != '\r') end++;
    fprintf(stderr, "\n  %.*s\n  ", (int)(end - start), start);
    for (int i = 1; i < column; i++) fputc(' ', stderr);
    fprintf(stderr, "^\n");
}

void ds_diag_error(DsDiag *diag, DsSpan span, const char *fmt, ...) {
    diag->has_error = true;
    char location[1024];
    ds_diag_format_location(diag->source, span, location, sizeof(location));
    fprintf(stderr, "%s: error: ", location);

    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fputc('\n', stderr);
    print_source_line(span.source ? span.source : diag->source, span.start.line, span.start.column);
}
