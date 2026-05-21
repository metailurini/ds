#define _POSIX_C_SOURCE 200809L

#include "vm_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static void print_trace_escaped(FILE *out, const char *data) {
    fputc('"', out);
    for (const char *p = data ? data : ""; *p; p++) {
        if (*p == '\\' || *p == '"') fputc('\\', out);
        if (*p == '\n') fputs("\\n", out);
        else if (*p == '\t') fputs("\\t", out);
        else fputc(*p, out);
    }
    fputc('"', out);
}

static bool ascii_transform_string(const DsString *in, DsString *out, const char *spec) {
    ds_string_init(out);
    size_t a = 0, b = in->len;
    if (strcmp(spec, "trim") == 0) {
        while (a < b && (in->data[a] == ' ' || in->data[a] == '\t' || in->data[a] == '\n' || in->data[a] == '\r' || in->data[a] == '\v' || in->data[a] == '\f')) a++;
        while (b > a && (in->data[b - 1] == ' ' || in->data[b - 1] == '\t' || in->data[b - 1] == '\n' || in->data[b - 1] == '\r' || in->data[b - 1] == '\v' || in->data[b - 1] == '\f')) b--;
    }
    ds_string_append_range(out, in->data ? in->data + a : "", b - a);
    if (strcmp(spec, "upper") == 0 || strcmp(spec, "lower") == 0) {
        for (size_t i = 0; i < out->len; i++) {
            if (strcmp(spec, "upper") == 0 && out->data[i] >= 'a' && out->data[i] <= 'z') out->data[i] = (char)(out->data[i] - 'a' + 'A');
            if (strcmp(spec, "lower") == 0 && out->data[i] >= 'A' && out->data[i] <= 'Z') out->data[i] = (char)(out->data[i] - 'A' + 'a');
        }
    }
    return true;
}

static bool parse_positive_number(const char *s, size_t len, size_t *idx, int *out) {
    int v = 0;
    size_t start = *idx;
    while (*idx < len && s[*idx] >= '0' && s[*idx] <= '9') {
        v = v * 10 + (s[*idx] - '0');
        if (v > 1024) return false;
        (*idx)++;
    }
    if (*idx == start || v <= 0) return false;
    *out = v;
    return true;
}

static bool append_padded(DsString *out, const char *data, size_t len, int width, char align) {
    if (width <= (int)len) return ds_string_append_range(out, data, len);
    int pad = width - (int)len;
    int left = 0, right = 0;
    if (align == '<') right = pad;
    else if (align == '>') left = pad;
    else { left = pad / 2; right = pad - left; }
    for (int i = 0; i < left; i++) ds_string_append_char(out, ' ');
    ds_string_append_range(out, data, len);
    for (int i = 0; i < right; i++) ds_string_append_char(out, ' ');
    return true;
}

static bool append_formatted_value(Vm *vm, DsValue *value, const char *spec, size_t spec_len, DsString *out, DsSpan span) {
    if (spec_len == 0) {
        DsString rendered; ds_value_to_string(value, &rendered);
        ds_string_append_range(out, rendered.data ? rendered.data : "", rendered.len);
        ds_string_free(&rendered);
        return true;
    }
    if ((spec_len == 5 && memcmp(spec, "upper", 5) == 0) || (spec_len == 5 && memcmp(spec, "lower", 5) == 0) || (spec_len == 4 && memcmp(spec, "trim", 4) == 0)) {
        if (value->kind != DS_VALUE_STRING) { ds_diag_error(vm->diag, span, "string format specifier requires a string value"); return false; }
        DsString rendered;
        ascii_transform_string(&value->as.string, &rendered, spec_len == 5 && spec[0] == 'u' ? "upper" : (spec_len == 5 ? "lower" : "trim"));
        ds_string_append_range(out, rendered.data ? rendered.data : "", rendered.len);
        ds_string_free(&rendered);
        return true;
    }
    if (spec[0] == '<' || spec[0] == '>' || spec[0] == '^') {
        if (value->kind != DS_VALUE_STRING) { ds_diag_error(vm->diag, span, "width format specifier requires a string value"); return false; }
        size_t idx = 1; int width = 0;
        if (!parse_positive_number(spec, spec_len, &idx, &width) || idx != spec_len) { ds_diag_error(vm->diag, span, "unsupported interpolation format specifier `%.*s`", (int)spec_len, spec); return false; }
        return append_padded(out, value->as.string.data ? value->as.string.data : "", value->as.string.len, width, spec[0]);
    }
    if (value->kind != DS_VALUE_INT) { ds_diag_error(vm->diag, span, "numeric format specifier requires an int value"); return false; }
    size_t idx = 0; bool zero = false; int width = 0, prec = -1;
    if (idx < spec_len && spec[idx] == '0') { zero = true; idx++; }
    if (idx < spec_len && spec[idx] >= '0' && spec[idx] <= '9') {
        if (!parse_positive_number(spec, spec_len, &idx, &width)) { ds_diag_error(vm->diag, span, "unsupported interpolation format specifier `%.*s`", (int)spec_len, spec); return false; }
    }
    if (idx < spec_len && spec[idx] == '.') {
        idx++;
        if (!parse_positive_number(spec, spec_len, &idx, &prec)) { ds_diag_error(vm->diag, span, "unsupported interpolation format specifier `%.*s`", (int)spec_len, spec); return false; }
    }
    if (idx >= spec_len) return false;
    char conv = spec[idx++];
    if (idx != spec_len || !(conv == 'd' || conv == 'f')) { ds_diag_error(vm->diag, span, "unsupported interpolation format specifier `%.*s`", (int)spec_len, spec); return false; }
    char buf[64];
    if (conv == 'd') {
        snprintf(buf, sizeof(buf), "%lld", (long long)value->as.integer);
        size_t len = strlen(buf);
        if (width <= (int)len) return ds_string_append_cstr(out, buf);
        int pad = width - (int)len;
        if (zero) {
            if (buf[0] == '-') {
                ds_string_append_char(out, '-');
                for (int i = 0; i < pad; i++) ds_string_append_char(out, '0');
                return ds_string_append_cstr(out, buf + 1);
            }
            for (int i = 0; i < pad; i++) ds_string_append_char(out, '0');
            return ds_string_append_cstr(out, buf);
        }
        for (int i = 0; i < pad; i++) ds_string_append_char(out, ' ');
        return ds_string_append_cstr(out, buf);
    }
    if (prec < 0) prec = 6;
    char ibuf[64]; snprintf(ibuf, sizeof(ibuf), "%lld", (long long)value->as.integer);
    DsString tmp; ds_string_init(&tmp); ds_string_append_cstr(&tmp, ibuf); ds_string_append_char(&tmp, '.'); for (int i = 0; i < prec; i++) ds_string_append_char(&tmp, '0');
    if (width > (int)tmp.len) append_padded(out, tmp.data, tmp.len, width, '>'); else ds_string_append_range(out, tmp.data, tmp.len);
    ds_string_free(&tmp);
    return true;
}


typedef struct { Vm *vm; const char *s; size_t n, i; DsSpan span; } VmInterpParser;
static void vip_ws(VmInterpParser *p){ while(p->i<p->n&&(p->s[p->i]==' '||p->s[p->i]=='\t')) p->i++; }
static bool vip_add(int64_t a,int64_t b,int64_t*o){ if((b>0&&a>INT64_MAX-b)||(b<0&&a<INT64_MIN-b)) return false; *o=a+b; return true; }
static bool vip_sub(int64_t a,int64_t b,int64_t*o){ if((b<0&&a>INT64_MAX+b)||(b>0&&a<INT64_MIN+b)) return false; *o=a-b; return true; }
static bool vip_mul(int64_t a,int64_t b,int64_t*o){ if(a==0||b==0){*o=0;return true;} if((a==-1&&b==INT64_MIN)||(b==-1&&a==INT64_MIN)) return false; if(a>0){ if(b>0){ if(a>INT64_MAX/b)return false; } else if(b<INT64_MIN/a)return false; } else { if(b>0){ if(a<INT64_MIN/b)return false; } else if(a<INT64_MAX/b)return false; } *o=a*b; return true; }
static bool vip_expr(VmInterpParser*,int64_t*);
static bool vip_primary(VmInterpParser*p,int64_t*o){ vip_ws(p); if(p->i>=p->n)return false; char c=p->s[p->i]; if(c=='('){ p->i++; if(!vip_expr(p,o))return false; vip_ws(p); if(p->i>=p->n||p->s[p->i]!=')')return false; p->i++; return true; } if(c=='-'){ p->i++; int64_t v=0; if(!vip_primary(p,&v))return false; if(v==INT64_MIN){ ds_diag_error(p->vm->diag,p->span,"integer overflow in unary `-`"); return false; } *o=-v; return true; } if(c>='0'&&c<='9'){ size_t st=p->i++; while(p->i<p->n&&p->s[p->i]>='0'&&p->s[p->i]<='9')p->i++; char*tmp=ds_str_dup_range(p->s+st,p->i-st); errno=0; char*end=NULL; long long v=strtoll(tmp,&end,10); bool ok=errno==0&&end&&*end=='\0'; free(tmp); if(!ok){ ds_diag_error(p->vm->diag,p->span,"integer literal is outside the supported int range"); return false; } *o=(int64_t)v; return true; } if((c>='A'&&c<='Z')||(c>='a'&&c<='z')||c=='_'){ size_t st=p->i++; while(p->i<p->n&&((p->s[p->i]>='A'&&p->s[p->i]<='Z')||(p->s[p->i]>='a'&&p->s[p->i]<='z')||(p->s[p->i]>='0'&&p->s[p->i]<='9')||p->s[p->i]=='_'))p->i++; char*name=ds_str_dup_range(p->s+st,p->i-st); DsValue v; if(!lookup_var(p->vm,name,&v,p->span)){ free(name); return false; } free(name); if(v.kind!=DS_VALUE_INT){ ds_value_free(&v); ds_diag_error(p->vm->diag,p->span,"arithmetic interpolation operands must be integers in v0.21.0"); return false; } *o=v.as.integer; ds_value_free(&v); return true; } return false; }
static bool vip_power(VmInterpParser*p,int64_t*o){ int64_t l=0; if(!vip_primary(p,&l))return false; vip_ws(p); if(p->i+1<p->n&&p->s[p->i]=='*'&&p->s[p->i+1]=='*'){ p->i+=2; int64_t e=0; if(!vip_power(p,&e))return false; if(e<0){ ds_diag_error(p->vm->diag,p->span,"negative exponents are not supported"); return false; } int64_t r=1,f=l; while(e>0){ if(e&1){ if(!vip_mul(r,f,&r)){ ds_diag_error(p->vm->diag,p->span,"integer overflow in operator `**`"); return false; }} e>>=1; if(e>0&&!vip_mul(f,f,&f)){ ds_diag_error(p->vm->diag,p->span,"integer overflow in operator `**`"); return false; }} *o=r; return true;} *o=l; return true; }
static bool vip_muldiv(VmInterpParser*p,int64_t*o){ int64_t a=0; if(!vip_power(p,&a))return false; for(;;){ vip_ws(p); if(p->i>=p->n)break; char op=p->s[p->i]; if(!(op=='*'||op=='/'||op=='%'))break; if(op=='*'&&p->i+1<p->n&&p->s[p->i+1]=='*')break; p->i++; int64_t b=0; if(!vip_power(p,&b))return false; if((op=='/'||op=='%')&&b==0){ ds_diag_error(p->vm->diag,p->span,"division or modulo by zero"); return false; } if((op=='/'||op=='%')&&a==INT64_MIN&&b==-1){ ds_diag_error(p->vm->diag,p->span,"integer overflow in operator `%c`",op); return false; } if(op=='*'){ if(!vip_mul(a,b,&a)){ ds_diag_error(p->vm->diag,p->span,"integer overflow in operator `*`"); return false; }} else if(op=='/') a/=b; else a%=b; } *o=a; return true; }
static bool vip_expr(VmInterpParser*p,int64_t*o){ int64_t a=0; if(!vip_muldiv(p,&a))return false; for(;;){ vip_ws(p); if(p->i>=p->n)break; char op=p->s[p->i]; if(!(op=='+'||op=='-'))break; p->i++; int64_t b=0; if(!vip_muldiv(p,&b))return false; if(op=='+'){ if(!vip_add(a,b,&a)){ ds_diag_error(p->vm->diag,p->span,"integer overflow in operator `+`"); return false; }} else if(!vip_sub(a,b,&a)){ ds_diag_error(p->vm->diag,p->span,"integer overflow in operator `-`"); return false; }} *o=a; return true; }
static bool append_arithmetic_interpolation(Vm*vm,const char*data,size_t len,DsString*out,DsSpan span){ VmInterpParser p={.vm=vm,.s=data,.n=len,.span=span}; int64_t r=0; if(!vip_expr(&p,&r))return false; vip_ws(&p); if(p.i!=p.n)return false; char buf[64]; snprintf(buf,sizeof(buf),"%lld",(long long)r); return ds_string_append_cstr(out,buf); }

bool interpolate_string(Vm *vm, const DsString *input, DsString *out, DsSpan span) {
    ds_string_init(out);
    for (size_t i = 0; i < input->len; i++) {
        char c = input->data[i];
        if (c == '{') {
            size_t start = i + 1;
            size_t j = start;
            if (j < input->len && ((input->data[j] >= 'A' && input->data[j] <= 'Z') || (input->data[j] >= 'a' && input->data[j] <= 'z') || input->data[j] == '_')) {
                j++;
                while (j < input->len && ((input->data[j] >= 'A' && input->data[j] <= 'Z') || (input->data[j] >= 'a' && input->data[j] <= 'z') || (input->data[j] >= '0' && input->data[j] <= '9') || input->data[j] == '_')) j++;
                if (j < input->len && (input->data[j] == '}' || input->data[j] == '.' || input->data[j] == ':')) {
                    char *name = ds_str_dup_range(input->data + start, j - start);
                    DsValue value;
                    if (!lookup_var(vm, name, &value, span)) { free(name); ds_string_free(out); return false; }
                    if (input->data[j] == '.') {
                        size_t field_start = ++j;
                        if (j < input->len && ((input->data[j] >= 'A' && input->data[j] <= 'Z') || (input->data[j] >= 'a' && input->data[j] <= 'z') || input->data[j] == '_')) {
                            j++;
                            while (j < input->len && ((input->data[j] >= 'A' && input->data[j] <= 'Z') || (input->data[j] >= 'a' && input->data[j] <= 'z') || (input->data[j] >= '0' && input->data[j] <= '9') || input->data[j] == '_')) j++;
                        }
                        char *field = ds_str_dup_range(input->data + field_start, j - field_start);
                        DsValue field_value = ds_value_null();
                        bool ok = command_result_field(vm, &value, field, span, &field_value);
                        free(field); ds_value_free(&value);
                        if (!ok) { free(name); ds_string_free(out); return false; }
                        value = field_value;
                    }
                    const char *spec = NULL; size_t spec_len = 0;
                    if (j < input->len && input->data[j] == ':') {
                        size_t spec_start = ++j;
                        while (j < input->len && input->data[j] != '}') j++;
                        spec = input->data + spec_start; spec_len = j - spec_start;
                    }
                    if (j >= input->len || input->data[j] != '}') {
                        ds_diag_error(vm->diag, span, "unsupported string interpolation; expected `{name}` or `{name.field}`");
                        ds_value_free(&value); free(name); ds_string_free(out); return false;
                    }
                    bool ok = append_formatted_value(vm, &value, spec, spec_len, out, span);
                    ds_value_free(&value); free(name);
                    if (!ok) { ds_string_free(out); return false; }
                    i = j; continue;
                }
            }
            size_t arith_end = start;
            while (arith_end < input->len && input->data[arith_end] != '}') arith_end++;
            if (arith_end < input->len && append_arithmetic_interpolation(vm, input->data + start, arith_end - start, out, span)) {
                i = arith_end;
                continue;
            }
            ds_diag_error(vm->diag, span, "unsupported string interpolation; expected `{name}`, `{name.field}`, or arithmetic");
            ds_string_free(out); return false;
        }
        ds_string_append_char(out, c);
    }
    return true;
}

static bool word_to_arg(Vm *vm, DsStr word, DsSpan span, char **out) {
    if (word.len >= 2 && word.data[0] == '"' && word.data[word.len - 1] == '"') {
        DsString decoded;
        if (!decode_string_text(word, &decoded)) return false;
        DsString rendered;
        bool ok = interpolate_string(vm, &decoded, &rendered, span);
        ds_string_free(&decoded);
        if (!ok) return false;
        *out = ds_str_dup_range(rendered.data ? rendered.data : "", rendered.len);
        ds_string_free(&rendered);
        return true;
    }
    if (word.len >= 2 && word.data[0] == '$') {
        char *name = ds_str_dup_range(word.data + 1, word.len - 1);
        DsValue value;
        if (!lookup_var(vm, name, &value, span)) {
            free(name);
            return false;
        }
        DsString rendered;
        ds_value_to_string(&value, &rendered);
        *out = ds_str_dup_range(rendered.data ? rendered.data : "", rendered.len);
        ds_string_free(&rendered);
        ds_value_free(&value);
        free(name);
        return true;
    }
    for (size_t i = 1; i + 1 < word.len; i++) {
        if (word.data[i] == '.') {
            char *name = ds_str_dup_range(word.data, i);
            char *field = ds_str_dup_range(word.data + i + 1, word.len - i - 1);
            DsValue value;
            if (!lookup_var(vm, name, &value, span)) { free(name); free(field); return false; }
            DsValue field_value = ds_value_null();
            bool ok = command_result_field(vm, &value, field, span, &field_value);
            if (!ok) {
                ds_value_free(&value);
                free(name); free(field);
                return false;
            }
            DsString rendered;
            ds_value_to_string(&field_value, &rendered);
            *out = ds_str_dup_range(rendered.data ? rendered.data : "", rendered.len);
            ds_string_free(&rendered);
            ds_value_free(&field_value);
            ds_value_free(&value);
            free(name); free(field);
            return !vm->diag->has_error;
        }
    }
    *out = ds_str_dup_range(word.data, word.len);
    return true;
}

static bool render_redirect_target(Vm *vm, const DsRedirect *redirect, char **out) {
    DsString decoded;
    if (!decode_string_text(redirect->target, &decoded)) {
        ds_diag_error(vm->diag, redirect->target_span, "invalid redirection target");
        return false;
    }
    DsString rendered;
    bool ok = interpolate_string(vm, &decoded, &rendered, redirect->target_span);
    ds_string_free(&decoded);
    if (!ok) return false;
    *out = ds_str_dup_range(rendered.data ? rendered.data : "", rendered.len);
    ds_string_free(&rendered);
    return true;
}

typedef struct {
    char **items;
    size_t len;
} VmArgv;

typedef struct {
    DsString stdout_text;
    DsString stderr_text;
    int code;
} VmProcessResult;

typedef struct {
    VmArgv argv;
    DsRedirect redirect;
    DsSpan span;
    bool capture;
    int exec_error_fd;
} VmProcessSpec;

static void argv_free(VmArgv *argv) {
    for (size_t i = 0; i < argv->len; i++) free(argv->items[i]);
    free(argv->items);
    argv->items = NULL;
    argv->len = 0;
}

static bool argv_build_range(Vm *vm, Instr *ins, size_t first_word, size_t word_count, VmArgv *argv) {
    argv->items = NULL;
    argv->len = 0;
    if (word_count == 0) return false;
    argv->items = (char **)ds_xcalloc(word_count + 1, sizeof(char *));
    argv->len = word_count;
    for (size_t i = 0; i < word_count; i++) {
        if (!word_to_arg(vm, ins->words[first_word + i], ins->span, &argv->items[i])) {
            argv->len = i;
            argv_free(argv);
            return false;
        }
    }
    return true;
}

static bool argv_build(Vm *vm, Instr *ins, VmArgv *argv) {
    return argv_build_range(vm, ins, 0, ins->word_count, argv);
}

static int process_status_code(int status) {
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return 1;
}

static void process_result_init(VmProcessResult *result) {
    ds_string_init(&result->stdout_text);
    ds_string_init(&result->stderr_text);
    result->code = 0;
}

static void process_result_free(VmProcessResult *result) {
    ds_string_free(&result->stdout_text);
    ds_string_free(&result->stderr_text);
    result->code = 0;
}

static bool read_file_into_string(FILE *fp, DsString *out) {
    ds_string_init(out);
    fflush(fp);
    if (fseek(fp, 0, SEEK_SET) != 0) return false;
    char buf[4096];
    size_t n = 0;
    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) ds_string_append_range(out, buf, n);
    return ferror(fp) == 0;
}

static bool open_redirect_target(Vm *vm, const DsRedirect *redirect, int *out_fd) {
    *out_fd = -1;
    char *redirect_path = NULL;
    if (!render_redirect_target(vm, redirect, &redirect_path)) return false;

    int flags = O_CREAT | O_WRONLY;
    if (redirect->kind == DS_REDIRECT_OUT_APPEND || redirect->kind == DS_REDIRECT_ERR_APPEND || redirect->kind == DS_REDIRECT_ALL_APPEND) flags |= O_APPEND;
    else flags |= O_TRUNC;

    int fd = open(redirect_path, flags, 0666);
    if (fd < 0) {
        ds_diag_error(vm->diag, redirect->target_span, "failed to open redirection target `%s`: %s", redirect_path, strerror(errno));
        free(redirect_path);
        return false;
    }
    free(redirect_path);
    *out_fd = fd;
    return true;
}

static bool process_spec_from_instr(Vm *vm, Instr *ins, bool capture, VmProcessSpec *spec) {
    memset(spec, 0, sizeof(*spec));
    spec->span = ins->span;
    spec->redirect = ins->redirect;
    spec->capture = capture;
    return argv_build(vm, ins, &spec->argv);
}

static bool process_spec_from_stage(Vm *vm, Instr *ins, size_t stage_index, bool capture, VmProcessSpec *spec) {
    memset(spec, 0, sizeof(*spec));
    spec->span = ins->span;
    ds_redirect_init(&spec->redirect);
    spec->capture = capture;
    size_t first = 0;
    for (size_t i = 0; i < stage_index; i++) first += ins->stage_word_counts[i];
    return argv_build_range(vm, ins, first, ins->stage_word_counts[stage_index], &spec->argv);
}

static bool parse_exit_code_arg(const char *text, int *out) {
    if (!text || !*text) return false;
    char *end = NULL;
    errno = 0;
    long value = strtol(text, &end, 10);
    if (errno != 0 || !end || *end != '\0' || value < 0 || value > 255) return false;
    *out = (int)value;
    return true;
}

static void append_test_helper_message(DsString *out, const VmProcessSpec *spec, size_t first_arg) {
    ds_string_init(out);
    for (size_t i = first_arg; i < spec->argv.len; i++) {
        if (i > first_arg) ds_string_append_char(out, ' ');
        ds_string_append_cstr(out, spec->argv.items[i]);
    }
}

static bool run_test_helper_command(Vm *vm, const VmProcessSpec *spec, int *out_code) {
    *out_code = 0;
    if (!vm->options.test_mode || spec->capture || spec->argv.len == 0) return false;
    const char *name = spec->argv.items[0];
    if (strcmp(name, "fail") != 0 && strcmp(name, "exit") != 0) return false;

    const char *test_name = vm->options.test_name.data ? vm->options.test_name.data : "<test>";
    int test_name_len = (int)vm->options.test_name.len;
    if (test_name_len <= 0) test_name_len = (int)strlen(test_name);

    if (spec->redirect.kind != DS_REDIRECT_NONE) {
        ds_diag_error(vm->diag, spec->span, "test `%.*s`: `%s` does not support redirection", test_name_len, test_name, name);
        *out_code = 1;
        return true;
    }

    if (strcmp(name, "fail") == 0) {
        DsString message;
        append_test_helper_message(&message, spec, 1);
        if (message.len > 0) {
            ds_diag_error(vm->diag, spec->span, "test `%.*s`: fail: %.*s", test_name_len, test_name, (int)message.len, message.data);
        } else {
            ds_diag_error(vm->diag, spec->span, "test `%.*s`: fail", test_name_len, test_name);
        }
        ds_string_free(&message);
        *out_code = 1;
        return true;
    }

    if (spec->argv.len != 2) {
        ds_diag_error(vm->diag, spec->span, "test `%.*s`: `exit` expects exactly one integer code", test_name_len, test_name);
        *out_code = 1;
        return true;
    }
    int code = 0;
    if (!parse_exit_code_arg(spec->argv.items[1], &code)) {
        ds_diag_error(vm->diag, spec->span, "test `%.*s`: `exit` code must be an integer from 0 to 255", test_name_len, test_name);
        *out_code = 1;
        return true;
    }
    vm->test_done = true;
    if (code != 0) {
        ds_diag_error(vm->diag, spec->span, "test `%.*s`: exit %d", test_name_len, test_name, code);
    }
    *out_code = code;
    return true;
}

static void process_spec_free(VmProcessSpec *spec) {
    argv_free(&spec->argv);
}

static void trace_command_spec(Vm *vm, const VmProcessSpec *spec) {
    if (!vm->options.trace_cmd || spec->argv.len == 0) return;
    fprintf(stderr, "trace: cmd %s:%d:%d:", span_path(vm->source, spec->span), spec->span.start.line, spec->span.start.column);
    for (size_t i = 0; i < spec->argv.len; i++) {
        fputc(' ', stderr);
        print_trace_escaped(stderr, spec->argv.items[i]);
    }
    if (spec->redirect.kind != DS_REDIRECT_NONE) {
        char *redirect_path = NULL;
        const char *op = NULL;
        switch (spec->redirect.kind) {
            case DS_REDIRECT_OUT: op = ">"; break;
            case DS_REDIRECT_OUT_APPEND: op = ">>"; break;
            case DS_REDIRECT_ERR: op = "2>"; break;
            case DS_REDIRECT_ERR_APPEND: op = "2>>"; break;
            case DS_REDIRECT_ALL: op = "&>"; break;
            case DS_REDIRECT_ALL_APPEND: op = "&>>"; break;
            case DS_REDIRECT_NONE: break;
        }
        if (op && render_redirect_target(vm, &spec->redirect, &redirect_path)) {
            fputc(' ', stderr);
            fputs(op, stderr);
            fputc(' ', stderr);
            print_trace_escaped(stderr, redirect_path);
            free(redirect_path);
        } else {
            fputs(" <redirect>", stderr);
        }
    }
    fputc('\n', stderr);
}

static bool fd_set_cloexec(int fd) {
    int flags = fcntl(fd, F_GETFD);
    if (flags < 0) return false;
    return fcntl(fd, F_SETFD, flags | FD_CLOEXEC) == 0;
}

static bool process_exec_error_pipe(Vm *vm, const VmProcessSpec *spec, int pipe_fds[2]) {
    pipe_fds[0] = -1;
    pipe_fds[1] = -1;
    if (pipe(pipe_fds) != 0) {
        ds_diag_error(vm->diag, spec->span, "failed to prepare command `%s`: %s", spec->argv.items[0], strerror(errno));
        return false;
    }
    if (!fd_set_cloexec(pipe_fds[1])) {
        ds_diag_error(vm->diag, spec->span, "failed to prepare command `%s`: %s", spec->argv.items[0], strerror(errno));
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        pipe_fds[0] = -1;
        pipe_fds[1] = -1;
        return false;
    }
    return true;
}

static void process_child_exec(const VmProcessSpec *spec, int redirect_fd, FILE *out_fp, FILE *err_fp) {
    if (spec->capture) {
        dup2(fileno(out_fp), STDOUT_FILENO);
        dup2(fileno(err_fp), STDERR_FILENO);
    } else if (spec->redirect.kind != DS_REDIRECT_NONE) {
        if (spec->redirect.kind == DS_REDIRECT_OUT || spec->redirect.kind == DS_REDIRECT_OUT_APPEND) dup2(redirect_fd, STDOUT_FILENO);
        else if (spec->redirect.kind == DS_REDIRECT_ERR || spec->redirect.kind == DS_REDIRECT_ERR_APPEND) dup2(redirect_fd, STDERR_FILENO);
        else { dup2(redirect_fd, STDOUT_FILENO); dup2(redirect_fd, STDERR_FILENO); }
    }
    if (redirect_fd >= 0) close(redirect_fd);
    execvp(spec->argv.items[0], spec->argv.items);
    int exec_errno = errno;
    if (spec->exec_error_fd >= 0) {
        ssize_t ignored = write(spec->exec_error_fd, &exec_errno, sizeof(exec_errno));
        (void)ignored;
        close(spec->exec_error_fd);
    }
    if (spec->capture) fprintf(stderr, "ds: failed to launch command `%s`: %s\n", spec->argv.items[0], strerror(exec_errno));
    _exit(127);
}

static bool process_execute(Vm *vm, VmProcessSpec *spec, VmProcessResult *result) {
    process_result_init(result);
    int redirect_fd = -1;
    FILE *out_fp = NULL;
    FILE *err_fp = NULL;
    int exec_error_pipe[2] = {-1, -1};
    spec->exec_error_fd = -1;

    trace_command_spec(vm, spec);

    if (!spec->capture && spec->redirect.kind != DS_REDIRECT_NONE) {
        if (!open_redirect_target(vm, &spec->redirect, &redirect_fd)) return false;
    }

    if (spec->capture) {
        out_fp = tmpfile();
        err_fp = tmpfile();
        if (!out_fp || !err_fp) {
            ds_diag_error(vm->diag, spec->span, "failed to create command capture temporary files: %s", strerror(errno));
            if (out_fp) fclose(out_fp);
            if (err_fp) fclose(err_fp);
            return false;
        }
    }

    if (!process_exec_error_pipe(vm, spec, exec_error_pipe)) {
        if (redirect_fd >= 0) close(redirect_fd);
        if (out_fp) fclose(out_fp);
        if (err_fp) fclose(err_fp);
        return false;
    }
    spec->exec_error_fd = exec_error_pipe[1];

    pid_t pid = fork();
    if (pid < 0) {
        ds_diag_error(vm->diag, spec->span, "failed to launch command `%s`: %s", spec->argv.items[0], strerror(errno));
        if (redirect_fd >= 0) close(redirect_fd);
        if (out_fp) fclose(out_fp);
        if (err_fp) fclose(err_fp);
        close(exec_error_pipe[0]);
        close(exec_error_pipe[1]);
        spec->exec_error_fd = -1;
        return false;
    }

    if (pid == 0) {
        close(exec_error_pipe[0]);
        process_child_exec(spec, redirect_fd, out_fp, err_fp);
    }

    close(exec_error_pipe[1]);
    spec->exec_error_fd = -1;
    if (redirect_fd >= 0) close(redirect_fd);
    int exec_errno = 0;
    ssize_t exec_error_len = read(exec_error_pipe[0], &exec_errno, sizeof(exec_errno));
    close(exec_error_pipe[0]);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            ds_diag_error(vm->diag, spec->span, "failed waiting for command `%s`: %s", spec->argv.items[0], strerror(errno));
            if (out_fp) fclose(out_fp);
            if (err_fp) fclose(err_fp);
            return false;
        }
    }
    result->code = process_status_code(status);

    if (!spec->capture && exec_error_len == (ssize_t)sizeof(exec_errno)) {
        ds_diag_error(vm->diag, spec->span, "failed to launch command `%s`: %s", spec->argv.items[0], strerror(exec_errno));
        return true;
    }

    if (spec->capture) {
        if (!read_file_into_string(out_fp, &result->stdout_text) || !read_file_into_string(err_fp, &result->stderr_text)) {
            ds_diag_error(vm->diag, spec->span, "failed to read command capture output");
            fclose(out_fp);
            fclose(err_fp);
            return false;
        }
        fclose(out_fp);
        fclose(err_fp);
    }
    return true;
}

static int pipefail_status(const int *codes, size_t len) {
    int code = 0;
    for (size_t i = 0; i < len; i++) if (codes[i] != 0) code = codes[i];
    return code;
}

static void close_pipe_array(int (*pipes)[2], size_t count) {
    for (size_t i = 0; i < count; i++) {
        if (pipes[i][0] >= 0) close(pipes[i][0]);
        if (pipes[i][1] >= 0) close(pipes[i][1]);
        pipes[i][0] = pipes[i][1] = -1;
    }
}

static bool redirect_wants_stdout(DsRedirectKind kind) {
    return kind == DS_REDIRECT_OUT || kind == DS_REDIRECT_OUT_APPEND || kind == DS_REDIRECT_ALL || kind == DS_REDIRECT_ALL_APPEND;
}

static bool redirect_wants_stderr(DsRedirectKind kind) {
    return kind == DS_REDIRECT_ERR || kind == DS_REDIRECT_ERR_APPEND || kind == DS_REDIRECT_ALL || kind == DS_REDIRECT_ALL_APPEND;
}

static void pipeline_child_exec(VmProcessSpec *specs, size_t stage_count, size_t idx, int (*pipes)[2], int redirect_fd, const DsRedirect *pipeline_redirect, FILE *out_fp, FILE *err_fp) {
    VmProcessSpec *spec = &specs[idx];
    if (idx > 0) dup2(pipes[idx - 1][0], STDIN_FILENO);
    if (idx + 1 < stage_count) {
        dup2(pipes[idx][1], STDOUT_FILENO);
    } else if (spec->capture) {
        dup2(fileno(out_fp), STDOUT_FILENO);
    } else if (redirect_wants_stdout(pipeline_redirect->kind)) {
        dup2(redirect_fd, STDOUT_FILENO);
    }
    if (spec->capture) {
        dup2(fileno(err_fp), STDERR_FILENO);
    } else if (redirect_wants_stderr(pipeline_redirect->kind)) {
        dup2(redirect_fd, STDERR_FILENO);
    }
    close_pipe_array(pipes, stage_count > 0 ? stage_count - 1 : 0);
    if (redirect_fd >= 0) close(redirect_fd);
    execvp(spec->argv.items[0], spec->argv.items);
    int exec_errno = errno;
    if (spec->exec_error_fd >= 0) {
        ssize_t ignored = write(spec->exec_error_fd, &exec_errno, sizeof(exec_errno));
        (void)ignored;
        close(spec->exec_error_fd);
    }
    if (spec->capture) fprintf(stderr, "ds: failed to launch command `%s`: %s\n", spec->argv.items[0], strerror(exec_errno));
    _exit(127);
}

static bool process_execute_pipeline(Vm *vm, Instr *ins, bool capture, VmProcessResult *result) {
    process_result_init(result);
    size_t n = ins->stage_count ? ins->stage_count : 1;
    VmProcessSpec *specs = (VmProcessSpec *)ds_xcalloc(n, sizeof(VmProcessSpec));
    pid_t *pids = (pid_t *)ds_xcalloc(n, sizeof(pid_t));
    int *codes = (int *)ds_xcalloc(n, sizeof(int));
    int (*pipes)[2] = (int (*)[2])ds_xcalloc(n > 1 ? n - 1 : 1, sizeof(int[2]));
    int redirect_fd = -1;
    FILE *out_fp = NULL;
    FILE *err_fp = NULL;
    int exec_error_pipe[2] = {-1, -1};
    bool ok = true;

    for (size_t i = 0; i + 1 < n; i++) pipes[i][0] = pipes[i][1] = -1;

    for (size_t i = 0; i < n; i++) {
        if (!process_spec_from_stage(vm, ins, i, capture, &specs[i])) { ok = false; goto cleanup; }
        if (i + 1 == n) specs[i].redirect = ins->redirect;
        else ds_redirect_init(&specs[i].redirect);
        trace_command_spec(vm, &specs[i]);
    }

    if (!capture && ins->redirect.kind != DS_REDIRECT_NONE) {
        if (!open_redirect_target(vm, &ins->redirect, &redirect_fd)) { ok = false; goto cleanup; }
    }
    if (capture) {
        out_fp = tmpfile();
        err_fp = tmpfile();
        if (!out_fp || !err_fp) {
            ds_diag_error(vm->diag, ins->span, "failed to create pipeline capture temporary files: %s", strerror(errno));
            ok = false;
            goto cleanup;
        }
    }
    for (size_t i = 0; i + 1 < n; i++) {
        if (pipe(pipes[i]) != 0) {
            ds_diag_error(vm->diag, ins->span, "failed to create pipeline pipe: %s", strerror(errno));
            ok = false;
            goto cleanup;
        }
    }
    if (pipe(exec_error_pipe) != 0 || !fd_set_cloexec(exec_error_pipe[1])) {
        ds_diag_error(vm->diag, ins->span, "failed to prepare pipeline exec error pipe: %s", strerror(errno));
        ok = false;
        goto cleanup;
    }
    for (size_t i = 0; i < n; i++) specs[i].exec_error_fd = exec_error_pipe[1];

    for (size_t i = 0; i < n; i++) {
        pids[i] = fork();
        if (pids[i] < 0) {
            ds_diag_error(vm->diag, ins->span, "failed to launch pipeline stage `%s`: %s", specs[i].argv.len ? specs[i].argv.items[0] : "<stage>", strerror(errno));
            ok = false;
            goto cleanup;
        }
        if (pids[i] == 0) {
            close(exec_error_pipe[0]);
            pipeline_child_exec(specs, n, i, pipes, redirect_fd, &ins->redirect, out_fp, err_fp);
        }
    }

    close(exec_error_pipe[1]); exec_error_pipe[1] = -1;
    close_pipe_array(pipes, n > 1 ? n - 1 : 0);
    if (redirect_fd >= 0) { close(redirect_fd); redirect_fd = -1; }

    int exec_errno = 0;
    ssize_t exec_error_len = read(exec_error_pipe[0], &exec_errno, sizeof(exec_errno));
    close(exec_error_pipe[0]); exec_error_pipe[0] = -1;

    for (size_t i = 0; i < n; i++) {
        int status = 0;
        while (waitpid(pids[i], &status, 0) < 0) {
            if (errno != EINTR) {
                ds_diag_error(vm->diag, ins->span, "failed waiting for pipeline stage `%s`: %s", specs[i].argv.len ? specs[i].argv.items[0] : "<stage>", strerror(errno));
                ok = false;
                goto cleanup;
            }
        }
        codes[i] = process_status_code(status);
    }
    result->code = pipefail_status(codes, n);
    if (!capture && exec_error_len == (ssize_t)sizeof(exec_errno)) {
        ds_diag_error(vm->diag, ins->span, "failed to launch pipeline command: %s", strerror(exec_errno));
    }
    if (capture) {
        if (!read_file_into_string(out_fp, &result->stdout_text) || !read_file_into_string(err_fp, &result->stderr_text)) {
            ds_diag_error(vm->diag, ins->span, "failed to read pipeline capture output");
            ok = false;
            goto cleanup;
        }
    }

cleanup:
    if (exec_error_pipe[0] >= 0) close(exec_error_pipe[0]);
    if (exec_error_pipe[1] >= 0) close(exec_error_pipe[1]);
    close_pipe_array(pipes, n > 1 ? n - 1 : 0);
    if (redirect_fd >= 0) close(redirect_fd);
    if (!ok) {
        for (size_t i = 0; i < n; i++) if (pids[i] > 0) waitpid(pids[i], NULL, 0);
    }
    if (out_fp) fclose(out_fp);
    if (err_fp) fclose(err_fp);
    for (size_t i = 0; i < n; i++) process_spec_free(&specs[i]);
    free(specs); free(pids); free(codes); free(pipes);
    return ok;
}

int run_command(Vm *vm, Instr *ins) {
    if (ins->word_count == 0) return 0;
    if ((ins->stage_count ? ins->stage_count : 1) > 1) {
        VmProcessResult result;
        bool ok = process_execute_pipeline(vm, ins, false, &result);
        int code = ok ? result.code : 1;
        if (ok && code != 0 && !vm->diag->has_error) ds_diag_error(vm->diag, ins->span, "pipeline failed with exit %d", code);
        process_result_free(&result);
        return code;
    }
    VmProcessSpec spec;
    if (!process_spec_from_instr(vm, ins, false, &spec)) return 1;
    int helper_code = 0;
    if (run_test_helper_command(vm, &spec, &helper_code)) {
        process_spec_free(&spec);
        return helper_code;
    }
    VmProcessResult result;
    bool ok = process_execute(vm, &spec, &result);
    int code = ok ? result.code : 1;
    if (ok && code != 0 && !vm->diag->has_error) {
        ds_diag_error(vm->diag, ins->span, "command `%s` failed with exit %d", spec.argv.len > 0 ? spec.argv.items[0] : "<command>", code);
    }
    process_result_free(&result);
    process_spec_free(&spec);
    return code;
}

int run_capture(Vm *vm, Instr *ins, DsValue *out_value) {
    *out_value = ds_value_null();
    if (ins->word_count == 0) return 1;
    if ((ins->stage_count ? ins->stage_count : 1) > 1) {
        VmProcessResult result;
        bool ok = process_execute_pipeline(vm, ins, true, &result);
        if (!ok) { process_result_free(&result); return 1; }
        *out_value = ds_value_command_result_take(&result.stdout_text, &result.stderr_text, result.code);
        return 0;
    }
    VmProcessSpec spec;
    if (!process_spec_from_instr(vm, ins, true, &spec)) return 1;
    VmProcessResult result;
    bool ok = process_execute(vm, &spec, &result);
    process_spec_free(&spec);
    if (!ok) {
        process_result_free(&result);
        return 1;
    }
    *out_value = ds_value_command_result_take(&result.stdout_text, &result.stderr_text, result.code);
    return 0;
}

bool command_result_field(Vm *vm, const DsValue *value, const char *field, DsSpan span, DsValue *out) {
    if (value->kind == DS_VALUE_MAP) {
        DsStr key = {(char *)field, strlen(field)};
        DsValue *found = ds_map_get((DsMap *)&value->as.map, key);
        if (!found) {
            ds_diag_error(vm->diag, span, "missing map key `%s`", field);
            return false;
        }
        *out = ds_value_copy(found);
        return true;
    }
    if (value->kind != DS_VALUE_COMMAND_RESULT) {
        ds_diag_error(vm->diag, span, "field access is only supported on command results and maps in v0.10.0");
        return false;
    }
    DsStr field_view = {(char *)field, strlen(field)};
    const DsCommandResultField *desc = ds_command_result_field_lookup(field_view);
    if (desc && desc->kind == DS_COMMAND_RESULT_FIELD_STRING && strcmp(desc->name, "stdout") == 0) { ds_string_from_range(&out->as.string, value->as.command_result.stdout_text.data ? value->as.command_result.stdout_text.data : "", value->as.command_result.stdout_text.len); out->kind = DS_VALUE_STRING; return true; }
    if (desc && desc->kind == DS_COMMAND_RESULT_FIELD_STRING && strcmp(desc->name, "stderr") == 0) { ds_string_from_range(&out->as.string, value->as.command_result.stderr_text.data ? value->as.command_result.stderr_text.data : "", value->as.command_result.stderr_text.len); out->kind = DS_VALUE_STRING; return true; }
    if (desc && desc->kind == DS_COMMAND_RESULT_FIELD_INT) { *out = ds_value_int(value->as.command_result.code); return true; }
    if (desc && desc->kind == DS_COMMAND_RESULT_FIELD_BOOL && strcmp(desc->name, "ok") == 0) { *out = ds_value_bool(value->as.command_result.code == 0); return true; }
    if (desc && desc->kind == DS_COMMAND_RESULT_FIELD_BOOL && strcmp(desc->name, "failed") == 0) { *out = ds_value_bool(value->as.command_result.code != 0); return true; }
    ds_diag_error(vm->diag, span, "unknown command result field `%s`", field);
    return false;
}
