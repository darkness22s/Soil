#ifndef SOIL_JSON_H
#define SOIL_JSON_H

#include <stddef.h>

typedef enum {
    JSON_NULL, JSON_BOOL, JSON_NUM, JSON_STR, JSON_ARR, JSON_OBJ
} jtype;

typedef struct jval jval;

struct jval {
    jtype type;
    int bval;
    double num;
    char *str;
    size_t n;
    jval **items;
    char **keys;
    jval **vals;
};

jval *json_parse(const char *s, size_t len, char *err, size_t errlen);
void json_free(jval *v);

jval *json_obj_get(const jval *obj, const char *key);
const char *json_str_get(const jval *obj, const char *key);
int json_int_get(const jval *obj, const char *key, int def);
int json_bool_get(const jval *obj, const char *key, int def);

typedef struct {
    char *p;
    size_t len;
    size_t cap;
} jbuf;

void jbuf_init(jbuf *b);
void jbuf_free(jbuf *b);
void jbuf_putc(jbuf *b, char c);
void jbuf_puts(jbuf *b, const char *s);
void jbuf_putn(jbuf *b, const char *s, size_t n);
void jbuf_printf(jbuf *b, const char *fmt, ...);

void json_write(jbuf *b, const jval *v);
void json_escape(jbuf *b, const char *s);

#endif
