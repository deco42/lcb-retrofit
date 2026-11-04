#include "httputil.h"

#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define LINE_END "\r\n"
#define HEAD_END "\r\n\r\n" // the empty line after the header lines
#define BASIC_PREFIX "Basic "
#define BASE64_DIGIT_BITS 6
#define BUF_INITIAL_SIZE 4096

static const char BASE64_DIGITS[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static bool header_name_is(const char *name, size_t name_len, const char *expected)
{
    return name_len == strlen(expected) && strncasecmp(name, expected, strlen(expected)) == 0;
}

bool http_parse_head(const char *buf, size_t len, HttpRequest *req)
{
    memset(req, 0, sizeof(*req));
    req->content_length = -1;

    const char *end = memmem(buf, len, HEAD_END, strlen(HEAD_END));
    if (!end) {
        return false;
    }

    end += strlen(HEAD_END);
    req->header_len = (size_t)(end - buf);
    const char *method_end = memchr(buf, ' ', req->header_len);
    const char *target_end = method_end ? memchr(method_end + 1, ' ', (size_t)(end - method_end - 1)) : NULL;
    if (!method_end || !target_end) {
        return false;
    }

    const char *target = method_end + 1, *query = memchr(target, '?', (size_t)(target_end - target));
    const char *path_end = query ? query : target_end;
    snprintf(req->method, sizeof(req->method), "%.*s", (int)(method_end - buf), buf);
    snprintf(req->path, sizeof(req->path), "%.*s", (int)(path_end - target), target);
    if (query) {
        snprintf(req->query, sizeof(req->query), "%.*s", (int)(target_end - query - 1), query + 1);
    }

    // The head ends with a line end, so every line in it has one
    const char *line_end = memmem(buf, req->header_len, LINE_END, strlen(LINE_END));
    for (const char *name = line_end + strlen(LINE_END); name < end; name = line_end + strlen(LINE_END)) {
        line_end = memmem(name, (size_t)(end - name), LINE_END, strlen(LINE_END));
        const char *colon = memchr(name, ':', (size_t)(line_end - name));
        if (!colon) {
            continue;
        }

        const char *value = colon + 1;
        while (value < line_end && *value == ' ') {
            value++;
        }

        char text[sizeof(req->auth)];
        snprintf(text, sizeof(text), "%.*s", (int)(line_end - value), value);
        size_t name_len = (size_t)(colon - name);
        if (header_name_is(name, name_len, "Content-Length")) {
            req->content_length = strtol(text, NULL, 10);
        } else if (header_name_is(name, name_len, "Authorization")) {
            snprintf(req->auth, sizeof(req->auth), "%s", text);
        }
    }

    return req->method[0] && req->path[0] == '/';
}

static int hex_value(int digit)
{
    if (digit >= '0' && digit <= '9') {
        return digit - '0';
    }

    digit = tolower(digit);

    return digit >= 'a' && digit <= 'f' ? digit - 'a' + 10 : -1;
}

static const char *next_field(const char *field)
{
    const char *separator = strchr(field, '&');

    return separator ? separator + 1 : NULL;
}

bool http_form_value(const char *form, const char *key, char *value, size_t size)
{
    size_t key_len = strlen(key);
    for (const char *field = form; field && *field; field = next_field(field)) {
        if (strncmp(field, key, key_len) != 0 || field[key_len] != '=') {
            continue;
        }

        size_t len = 0;
        for (const char *src = field + key_len + 1; *src && *src != '&' && len + 1 < size; src++) {
            if (*src == '+') {
                value[len++] = ' ';
            } else if (*src == '%' && hex_value(src[1]) >= 0 && hex_value(src[2]) >= 0) {
                value[len++] = (char)(hex_value(src[1]) * 16 + hex_value(src[2]));
                src += 2;
            } else {
                value[len++] = *src;
            }
        }

        value[len] = '\0';
        return true;
    }

    return false;
}

bool http_basic_auth_ok(const char *header, const char *user, const char *password)
{
    if (strncasecmp(header, BASIC_PREFIX, strlen(BASIC_PREFIX)) != 0) {
        return false;
    }

    char decoded[256];
    size_t len = 0;
    unsigned bits = 0;
    int n_bits = 0;
    for (const char *src = header + strlen(BASIC_PREFIX); *src && *src != '='; src++) {
        const char *digit = strchr(BASE64_DIGITS, *src);
        if (!digit) {
            return false;
        }

        bits = bits << BASE64_DIGIT_BITS | (unsigned)(digit - BASE64_DIGITS);
        n_bits += BASE64_DIGIT_BITS;
        if (n_bits >= 8) {
            n_bits -= 8;
            if (len + 1 >= sizeof(decoded)) {
                return false;
            }

            decoded[len++] = (char)((bits >> n_bits) & 0xFF);
        }
    }

    size_t user_len = strlen(user), password_len = strlen(password);

    return len == user_len + 1 + password_len && memcmp(decoded, user, user_len) == 0 && decoded[user_len] == ':' &&
           memcmp(decoded + user_len + 1, password, password_len) == 0;
}

static void reserve(Buf *out, size_t extra)
{
    if (out->failed || out->len + extra + 1 <= out->size) {
        return;
    }

    size_t size = out->size ? out->size : BUF_INITIAL_SIZE;
    while (size < out->len + extra + 1) {
        size *= 2;
    }

    char *data = realloc(out->data, size);
    if (!data) {
        out->failed = true;
        return;
    }

    out->data = data;
    out->size = size;
}

void buf_free(Buf *out)
{
    free(out->data);
    memset(out, 0, sizeof(*out));
}

void buf_put(Buf *out, const char *data, size_t len)
{
    reserve(out, len);
    if (out->failed) {
        return;
    }

    memcpy(out->data + out->len, data, len);
    out->len += len;
    out->data[out->len] = '\0';
}

void buf_printf(Buf *out, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    int len = vsnprintf(NULL, 0, fmt, args);
    va_end(args);
    if (len < 0) {
        return;
    }

    reserve(out, (size_t)len);
    if (out->failed) {
        return;
    }

    va_start(args, fmt);
    vsnprintf(out->data + out->len, (size_t)len + 1, fmt, args);
    va_end(args);
    out->len += (size_t)len;
}

static size_t utf8_sequence_len(const unsigned char *text)
{
    size_t len = text[0] >= 0xF5 ? 0 : text[0] >= 0xF0 ? 4 : text[0] >= 0xE0 ? 3 : text[0] >= 0xC2 ? 2 : 0;
    for (size_t i = 1; i < len; i++) {
        if ((text[i] & 0xC0) != 0x80) {
            return 0;
        }
    }

    return len;
}

void buf_json_str(Buf *out, const char *text)
{
    buf_puts(out, "\"");
    while (*text) {
        unsigned char byte = (unsigned char)*text;
        size_t utf8_len = utf8_sequence_len((const unsigned char *)text);
        if (utf8_len > 0) {
            buf_put(out, text, utf8_len);
            text += utf8_len;
            continue;
        }

        if (byte == '"' || byte == '\\') {
            buf_printf(out, "\\%c", byte);
        } else if (byte < ' ' || byte > '~') {
            buf_printf(out, "\\u%04x", byte);
        } else {
            buf_put(out, (const char *)&byte, 1);
        }

        text++;
    }

    buf_puts(out, "\"");
}

void buf_json_num(Buf *out, double value)
{
    if (isfinite(value)) {
        buf_printf(out, "%.9g", value); // 9 digits are enough for every float
    } else {
        buf_puts(out, "null");
    }
}
