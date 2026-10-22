#include "json.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define JSON_MAX_DEPTH 100

typedef struct {
    const char *s;
    size_t len;
    size_t pos;
    char *err;
    size_t errlen;
    int failed;
} jparser;

static void jerr(jparser *p, const char *msg)
{
    if (!p->failed) {
        p->failed = 1;
        if (p->err && p->errlen)
            snprintf(p->err, p->errlen, "%s at byte %zu", msg, p->pos);
    }
}

static void skip_ws(jparser *p)
{
    while (p->pos < p->len) {
        char c = p->s[p->pos];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
            p->pos++;
        else
            break;
    }
}

static jval *alloc_val(jtype t)
{
    jval *v = calloc(1, sizeof(*v));
    if (v)
        v->type = t;
    return v;
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void put_utf8(char **out, size_t *cap, size_t *len, unsigned cp)
{
    char tmp[4];
    size_t n;
    if (cp < 0x80) {
        tmp[0] = (char)cp;
        n = 1;
    } else if (cp < 0x800) {
        tmp[0] = (char)(0xC0 | (cp >> 6));
        tmp[1] = (char)(0x80 | (cp & 0x3F));
        n = 2;
    } else if (cp < 0x10000) {
        tmp[0] = (char)(0xE0 | (cp >> 12));
        tmp[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        tmp[2] = (char)(0x80 | (cp & 0x3F));
        n = 3;
    } else {
        tmp[0] = (char)(0xF0 | (cp >> 18));
        tmp[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        tmp[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        tmp[3] = (char)(0x80 | (cp & 0x3F));
        n = 4;
    }
    if (*len + n + 1 > *cap) {
        *cap = (*cap + n + 32) * 2;
        *out = realloc(*out, *cap);
    }
    memcpy(*out + *len, tmp, n);
    *len += n;
    (*out)[*len] = 0;
}

static char *parse_string(jparser *p)
{
    if (p->pos >= p->len || p->s[p->pos] != '"') {
        jerr(p, "expected string");
        return NULL;
    }
    p->pos++;
    size_t cap = 32, len = 0;
    char *out = malloc(cap);
    if (!out) {
        jerr(p, "oom");
        return NULL;
    }
    out[0] = 0;
    while (p->pos < p->len) {
        unsigned char c = (unsigned char)p->s[p->pos++];
        if (c == '"') {
            return out;
        }
        if (c == '\\') {
            if (p->pos >= p->len) break;
            char e = p->s[p->pos++];
            char ch = 0;
            switch (e) {
            case '"': ch = '"'; break;
            case '\\': ch = '\\'; break;
            case '/': ch = '/'; break;
            case 'b': ch = '\b'; break;
            case 'f': ch = '\f'; break;
            case 'n': ch = '\n'; break;
            case 'r': ch = '\r'; break;
            case 't': ch = '\t'; break;
            case 'u': {
                if (p->pos + 4 > p->len) {
                    jerr(p, "truncated \\u escape");
                    free(out);
                    return NULL;
                }
                unsigned cp = 0;
                for (int i = 0; i < 4; i++) {
                    int h = hexval(p->s[p->pos + i]);
                    if (h < 0) {
                        jerr(p, "bad \\u escape");
                        free(out);
                        return NULL;
                    }
                    cp = (cp << 4) | (unsigned)h;
                }
                p->pos += 4;
                unsigned low = cp;
                if (cp >= 0xD800 && cp <= 0xDBFF && p->pos + 6 <= p->len &&
                    p->s[p->pos] == '\\' && p->s[p->pos + 1] == 'u') {
                    unsigned lo = 0;
                    int ok = 1;
                    for (int i = 0; i < 4; i++) {
                        int h = hexval(p->s[p->pos + 2 + i]);
                        if (h < 0) { ok = 0; break; }
                        lo = (lo << 4) | (unsigned)h;
                    }
                    if (ok && lo >= 0xDC00 && lo <= 0xDFFF) {
                        low = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        p->pos += 6;
                    }
                }
                put_utf8(&out, &cap, &len, low);
                continue;
            }
            default:
                jerr(p, "bad escape");
                free(out);
                return NULL;
            }
            if (len + 2 > cap) {
                cap *= 2;
                out = realloc(out, cap);
            }
            out[len++] = ch;
            out[len] = 0;
            continue;
        }
        if (c < 0x20) {
            jerr(p, "control character in string");
            free(out);
            return NULL;
        }
        if (len + 2 > cap) {
            cap *= 2;
            out = realloc(out, cap);
        }
        out[len++] = (char)c;
        out[len] = 0;
    }
    jerr(p, "unterminated string");
    free(out);
    return NULL;
}

static jval *parse_value(jparser *p, int depth);

static jval *parse_array(jparser *p, int depth)
{
    if (depth > JSON_MAX_DEPTH) {
        jerr(p, "nesting too deep");
        return NULL;
    }
    p->pos++;
    jval *v = alloc_val(JSON_ARR);
    if (!v) { jerr(p, "oom"); return NULL; }
    size_t cap = 0;
    skip_ws(p);
    if (p->pos < p->len && p->s[p->pos] == ']') {
        p->pos++;
        return v;
    }
    while (p->pos < p->len) {
        jval *item = parse_value(p, depth + 1);
        if (!item) { json_free(v); return NULL; }
        if (v->n == cap) {
            cap = cap ? cap * 2 : 4;
            v->items = realloc(v->items, cap * sizeof(*v->items));
        }
        v->items[v->n++] = item;
        skip_ws(p);
        if (p->pos < p->len && p->s[p->pos] == ',') {
            p->pos++;
            skip_ws(p);
            continue;
        }
        if (p->pos < p->len && p->s[p->pos] == ']') {
            p->pos++;
            return v;
        }
        jerr(p, "expected ',' or ']'");
        json_free(v);
        return NULL;
    }
    jerr(p, "unterminated array");
    json_free(v);
    return NULL;
}

static jval *parse_object(jparser *p, int depth)
{
    if (depth > JSON_MAX_DEPTH) {
        jerr(p, "nesting too deep");
        return NULL;
    }
    p->pos++;
    jval *v = alloc_val(JSON_OBJ);
    if (!v) { jerr(p, "oom"); return NULL; }
    size_t cap = 0;
    skip_ws(p);
    if (p->pos < p->len && p->s[p->pos] == '}') {
        p->pos++;
        return v;
    }
    while (p->pos < p->len) {
        char *key = parse_string(p);
        if (!key) { json_free(v); return NULL; }
        skip_ws(p);
        if (p->pos >= p->len || p->s[p->pos] != ':') {
            jerr(p, "expected ':'");
            free(key);
            json_free(v);
            return NULL;
        }
        p->pos++;
        jval *item = parse_value(p, depth + 1);
        if (!item) { free(key); json_free(v); return NULL; }
        if (v->n == cap) {
            cap = cap ? cap * 2 : 4;
            v->keys = realloc(v->keys, cap * sizeof(*v->keys));
            v->vals = realloc(v->vals, cap * sizeof(*v->vals));
        }
        v->keys[v->n] = key;
        v->vals[v->n] = item;
        v->n++;
        skip_ws(p);
        if (p->pos < p->len && p->s[p->pos] == ',') {
            p->pos++;
            skip_ws(p);
            continue;
        }
        if (p->pos < p->len && p->s[p->pos] == '}') {
            p->pos++;
            return v;
        }
        jerr(p, "expected ',' or '}'");
        json_free(v);
        return NULL;
    }
    jerr(p, "unterminated object");
    json_free(v);
    return NULL;
}

static jval *parse_number(jparser *p)
{
    size_t start = p->pos;
    if (p->pos < p->len && (p->s[p->pos] == '-' || p->s[p->pos] == '+'))
        p->pos++;
    while (p->pos < p->len) {
        char c = p->s[p->pos];
        if ((c >= '0' && c <= '9') || c == '.' || c == 'e' || c == 'E' ||
            c == '-' || c == '+')
            p->pos++;
        else
            break;
    }
    char tmp[64];
    size_t n = p->pos - start;
    if (n == 0 || n >= sizeof(tmp)) {
        jerr(p, "bad number");
        return NULL;
    }
    memcpy(tmp, p->s + start, n);
    tmp[n] = 0;
    char *end = NULL;
    double d = strtod(tmp, &end);
    if (!end || *end) {
        jerr(p, "bad number");
        return NULL;
    }
    jval *v = alloc_val(JSON_NUM);
    if (!v) { jerr(p, "oom"); return NULL; }
    v->num = d;
    return v;
}

static jval *parse_value(jparser *p, int depth)
{
    skip_ws(p);
    if (p->pos >= p->len) {
        jerr(p, "unexpected end of input");
        return NULL;
    }
    char c = p->s[p->pos];
    if (c == '{') return parse_object(p, depth);
    if (c == '[') return parse_array(p, depth);
    if (c == '"') {
        char *s = parse_string(p);
        if (!s) return NULL;
        jval *v = alloc_val(JSON_STR);
        if (!v) { free(s); jerr(p, "oom"); return NULL; }
        v->str = s;
        return v;
    }
    if (c == 't' && p->pos + 4 <= p->len && !memcmp(p->s + p->pos, "true", 4)) {
        p->pos += 4;
        jval *v = alloc_val(JSON_BOOL);
        v->bval = 1;
        return v;
    }
    if (c == 'f' && p->pos + 5 <= p->len && !memcmp(p->s + p->pos, "false", 5)) {
        p->pos += 5;
        jval *v = alloc_val(JSON_BOOL);
        v->bval = 0;
        return v;
    }
    if (c == 'n' && p->pos + 4 <= p->len && !memcmp(p->s + p->pos, "null", 4)) {
        p->pos += 4;
        return alloc_val(JSON_NULL);
    }
    if (c == '-' || c == '+' || (c >= '0' && c <= '9'))
        return parse_number(p);
    jerr(p, "unexpected character");
    return NULL;
}

jval *json_parse(const char *s, size_t len, char *err, size_t errlen)
{
    jparser p = { s, len, 0, err, errlen, 0 };
    if (err && errlen) err[0] = 0;
    jval *v = parse_value(&p, 0);
    if (!v)
        return NULL;
    skip_ws(&p);
    if (p.pos != p.len) {
        jerr(&p, "trailing data");
        json_free(v);
        return NULL;
    }
    return v;
}

void json_free(jval *v)
{
    if (!v)
        return;
    if (v->type == JSON_STR)
        free(v->str);
    if (v->type == JSON_ARR) {
        for (size_t i = 0; i < v->n; i++)
            json_free(v->items[i]);
        free(v->items);
    }
    if (v->type == JSON_OBJ) {
        for (size_t i = 0; i < v->n; i++) {
            free(v->keys[i]);
            json_free(v->vals[i]);
        }
        free(v->keys);
        free(v->vals);
    }
    free(v);
}

jval *json_obj_get(const jval *obj, const char *key)
{
    if (!obj || obj->type != JSON_OBJ)
        return NULL;
    for (size_t i = 0; i < obj->n; i++)
        if (!strcmp(obj->keys[i], key))
            return obj->vals[i];
    return NULL;
}

const char *json_str_get(const jval *obj, const char *key)
{
    jval *v = json_obj_get(obj, key);
    return (v && v->type == JSON_STR) ? v->str : NULL;
}

int json_int_get(const jval *obj, const char *key, int def)
{
    jval *v = json_obj_get(obj, key);
    if (!v || v->type != JSON_NUM)
        return def;
    return (int)v->num;
}

int json_bool_get(const jval *obj, const char *key, int def)
{
    jval *v = json_obj_get(obj, key);
    if (!v || v->type != JSON_BOOL)
        return def;
    return v->bval;
}

static void buf_reserve(jbuf *b, size_t extra)
{
    if (b->len + extra + 1 <= b->cap)
        return;
    size_t cap = b->cap ? b->cap : 128;
    while (cap < b->len + extra + 1)
        cap *= 2;
    b->p = realloc(b->p, cap);
    b->cap = cap;
}

void jbuf_init(jbuf *b)
{
    memset(b, 0, sizeof(*b));
    buf_reserve(b, 64);
    b->p[0] = 0;
}

void jbuf_free(jbuf *b)
{
    free(b->p);
    memset(b, 0, sizeof(*b));
}

void jbuf_putc(jbuf *b, char c)
{
    buf_reserve(b, 1);
    b->p[b->len++] = c;
    b->p[b->len] = 0;
}

void jbuf_putn(jbuf *b, const char *s, size_t n)
{
    buf_reserve(b, n);
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = 0;
}

void jbuf_puts(jbuf *b, const char *s)
{
    jbuf_putn(b, s, strlen(s));
}

void jbuf_printf(jbuf *b, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char tmp[256];
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    if ((size_t)n < sizeof(tmp)) {
        jbuf_putn(b, tmp, (size_t)n);
        return;
    }
    char *big = malloc((size_t)n + 1);
    if (!big)
        return;
    va_start(ap, fmt);
    vsnprintf(big, (size_t)n + 1, fmt, ap);
    va_end(ap);
    jbuf_putn(b, big, (size_t)n);
    free(big);
}

void json_escape(jbuf *b, const char *s)
{
    jbuf_putc(b, '"');
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        switch (*p) {
        case '"': jbuf_puts(b, "\\\""); break;
        case '\\': jbuf_puts(b, "\\\\"); break;
        case '\b': jbuf_puts(b, "\\b"); break;
        case '\f': jbuf_puts(b, "\\f"); break;
        case '\n': jbuf_puts(b, "\\n"); break;
        case '\r': jbuf_puts(b, "\\r"); break;
        case '\t': jbuf_puts(b, "\\t"); break;
        default:
            if (*p < 0x20)
                jbuf_printf(b, "\\u%04x", *p);
            else
                jbuf_putc(b, (char)*p);
        }
    }
    jbuf_putc(b, '"');
}

void json_write(jbuf *b, const jval *v)
{
    if (!v) {
        jbuf_puts(b, "null");
        return;
    }
    switch (v->type) {
    case JSON_NULL:
        jbuf_puts(b, "null");
        break;
    case JSON_BOOL:
        jbuf_puts(b, v->bval ? "true" : "false");
        break;
    case JSON_NUM: {
        double d = v->num;
        if (d == (double)(long long)d && d > -9e15 && d < 9e15)
            jbuf_printf(b, "%lld", (long long)d);
        else
            jbuf_printf(b, "%.17g", d);
        break;
    }
    case JSON_STR:
        json_escape(b, v->str);
        break;
    case JSON_ARR:
        jbuf_putc(b, '[');
        for (size_t i = 0; i < v->n; i++) {
            if (i) jbuf_putc(b, ',');
            json_write(b, v->items[i]);
        }
        jbuf_putc(b, ']');
        break;
    case JSON_OBJ:
        jbuf_putc(b, '{');
        for (size_t i = 0; i < v->n; i++) {
            if (i) jbuf_putc(b, ',');
            json_escape(b, v->keys[i]);
            jbuf_putc(b, ':');
            json_write(b, v->vals[i]);
        }
        jbuf_putc(b, '}');
        break;
    }
}
