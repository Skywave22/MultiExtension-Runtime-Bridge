/* http_probe.c — fetch one URL with the built-in HTTP client and print what
 * came back. Handy when an extension misbehaves and you need to know whether
 * the daemon or the site is at fault:
 *
 *     cc -std=c11 -D_GNU_SOURCE -Icore/include tests/manual/http_probe.c \
 *        build/obj/*.o -lpthread -lm -lrt -o /tmp/http_probe   # (main.o clash)
 *     /tmp/http_probe 'http://127.0.0.1:8080/?q=solo'
 */
#include "bridge/http.h"
#include "bridge/util.h"

#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <url> [post-body]\n", argv[0]);
        return 2;
    }

    xb_http_req req;
    memset(&req, 0, sizeof req);
    req.url = argv[1];
    req.method = (argc > 2) ? "POST" : "GET";
    req.body = (argc > 2) ? argv[2] : NULL;
    req.timeout_ms = 8000;
    req.follow_redirects = true;
    req.max_redirects = 5;
    req.max_bytes = 8u * 1024 * 1024;

    xb_http_res res;
    memset(&res, 0, sizeof res);
    xb_cookies *jar = xb_cookies_new();

    int rc = xb_http_do(&req, jar, &res);
    printf("rc=%d status=%d bytes=%zu content_type=%s\n",
           rc, res.status, res.body_len, res.content_type);
    if (rc != 0) printf("error: %s\n", res.error);
    if (res.final_url[0]) printf("final_url: %s\n", res.final_url);
    if (res.body) printf("---- body ----\n%.400s\n", res.body);

    xb_http_res_free(&res);
    xb_cookies_free(jar);
    return rc == 0 ? 0 : 1;
}
