#ifndef LCBD_HTTPUTIL_H
#define LCBD_HTTPUTIL_H

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

typedef struct {
    char method[8];
    char path[128];
    char query[256];
    char auth[256];
    long content_length; // -1 if absent
    size_t header_len;   // bytes up to and including the empty line
} HttpRequest;

bool http_parse_head(const char *buf, size_t len, HttpRequest *req);
bool http_form_value(const char *form, const char *key, char *value, size_t size);
bool http_basic_auth_ok(const char *header, const char *user, const char *password);

typedef struct {
    char *data;
    size_t len, size;
    bool failed; // out of memory, the text is incomplete
} Buf;

void buf_free(Buf *out);
void buf_printf(Buf *out, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void buf_put(Buf *out, const char *data, size_t len);
void buf_json_str(Buf *out, const char *text);
void buf_json_num(Buf *out, double value);

static inline void buf_puts(Buf *out, const char *text) { buf_put(out, text, strlen(text)); }

#endif
