#include "ds_common.h"

#include <stdarg.h>

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

static void ds_diag_print_source_line(FILE *out, const DsSource *source, int wanted_line, int column) {
    if (!out || !source || !source->data || wanted_line <= 0) return;

    const char *start = source->data;
    int line = 1;
    bool found = wanted_line == 1;
    for (size_t i = 0; i < source->len; i++) {
        if (line == wanted_line) {
            start = source->data + i;
            found = true;
            break;
        }
        if (source->data[i] == '\n') {
            line++;
            if (line == wanted_line) {
                start = source->data + i + 1;
                found = true;
                break;
            }
        }
    }
    if (!found) return;

    const char *end = start;
    while (*end && *end != '\n' && *end != '\r') end++;
    fprintf(out, "\n  %.*s\n  ", (int)(end - start), start);
    for (int i = 1; i < column; i++) fputc(' ', out);
    fputs("^\n", out);
}

static void ds_diag_vreport(FILE *out, const DsSource *source, DsSpan span, const char *severity, const char *fmt, va_list args) {
    if (!out) out = stderr;
    if (!severity) severity = "diagnostic";

    char location[1024];
    ds_diag_format_location(source, span, location, sizeof(location));
    fprintf(out, "%s: %s: ", location, severity);
    vfprintf(out, fmt, args);
    fputc('\n', out);
    ds_diag_print_source_line(out, span.source ? span.source : source, span.start.line, span.start.column);
}

void ds_diag_report(FILE *out, const DsSource *source, DsSpan span, const char *severity, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    ds_diag_vreport(out, source, span, severity, fmt, args);
    va_end(args);
}

void ds_diag_error(DsDiag *diag, DsSpan span, const char *fmt, ...) {
    diag->has_error = true;

    va_list args;
    va_start(args, fmt);
    ds_diag_vreport(stderr, diag->source, span, "error", fmt, args);
    va_end(args);
}
