#include <math.h>
#include <string.h>

#include "check.h"
#include "httputil.h"

int main(void)
{
    HttpRequest req;
    const char *head = "POST /api/cmd?x=1 HTTP/1.1\r\nHost: a\r\ncontent-length: 12\r\n"
                       "Authorization: Basic c2VydmljZTpzZWNyZXQ=\r\n\r\nop=on&n=7";
    CHECK(http_parse_head(head, strlen(head), &req));
    CHECK(strcmp(req.method, "POST") == 0 && strcmp(req.path, "/api/cmd") == 0 && strcmp(req.query, "x=1") == 0);
    CHECK(req.content_length == 12 && req.header_len == strlen(head) - strlen("op=on&n=7"));
    CHECK(http_basic_auth_ok(req.auth, "service", "secret"));
    CHECK(!http_basic_auth_ok(req.auth, "service", "secret2"));
    CHECK(!http_basic_auth_ok("Basic c2VydmljZTpzZWNyZXQx", "service", "secret"));
    CHECK(!http_basic_auth_ok("", "service", "secret"));
    CHECK(!http_basic_auth_ok("Basic c2VydmljZTpz-ZWNyZXQ=", "service", "secret"));
    const char *incomplete = "GET / HTTP/1.1\r\nHost: a\r\n";
    CHECK(!http_parse_head(incomplete, strlen(incomplete), &req));

    // A NUL byte inside a header line ends nothing, the lines after it are still read
    const char nul_head[] = "GET / HTTP/1.1\r\nX: a\0b\r\nContent-Length: 5\r\n\r\n";
    CHECK(http_parse_head(nul_head, sizeof(nul_head) - 1, &req) && req.content_length == 5);

    // The password is compared in full, the header holds only 247 of its 250 characters
    char long_password[251], long_auth[400] = "Basic c2VydmljZTph"; // "service:a"
    memset(long_password, 'a', 250);
    long_password[250] = '\0';
    for (int i = 0; i < 82; i++) {
        strcat(long_auth, "YWFh"); // "aaa"
    }

    CHECK(!http_basic_auth_ok(long_auth, "service", long_password));
    long_password[247] = '\0';
    CHECK(http_basic_auth_ok(long_auth, "service", long_password));

    char value[64];
    const char *form = "op=on&n=7&text=a%2Cb+c%0A";
    CHECK(http_form_value(form, "n", value, sizeof(value)) && strcmp(value, "7") == 0);
    CHECK(http_form_value(form, "text", value, sizeof(value)) && strcmp(value, "a,b c\n") == 0);
    CHECK(!http_form_value("op=on&nn=7", "n", value, sizeof(value)));
    CHECK(http_form_value("op=&n=7", "op", value, sizeof(value)) && value[0] == '\0');

    Buf out = {0};
    buf_json_str(&out, "a\"b\\c\n\x01\xC3\xA4\xC3");
    buf_puts(&out, " ");
    buf_json_num(&out, 1.5);
    buf_puts(&out, " ");
    buf_json_num(&out, NAN);
    CHECK(!out.failed && strcmp(out.data, "\"a\\\"b\\\\c\\u000a\\u0001\xC3\xA4\\u00c3\" 1.5 null") == 0);
    buf_free(&out);

    return check_report("httputil");
}
