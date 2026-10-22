#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sha256.h"
#include "json.h"
#include "sol.h"

static int failures;

#define CHECK(cond, msg) do { \
    if (cond) { printf("ok   %s\n", msg); } \
    else { printf("FAIL %s\n", msg); failures++; } \
} while (0)

static void hexstr(const unsigned char *d, char *out)
{
    for (int i = 0; i < 32; i++)
        sprintf(out + i * 2, "%02x", d[i]);
}

static void test_sha256(void)
{
    char hex[65];
    unsigned char d[32];
    sha256_ctx ctx;

    sha256_init(&ctx);
    sha256_final(&ctx, d);
    hexstr(d, hex);
    CHECK(!strcmp(hex, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"),
          "sha256 empty string");

    sha256_init(&ctx);
    sha256_update(&ctx, "abc", 3);
    sha256_final(&ctx, d);
    hexstr(d, hex);
    CHECK(!strcmp(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"),
          "sha256(\"abc\")");

    sha256_init(&ctx);
    sha256_update(&ctx, "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56);
    sha256_final(&ctx, d);
    hexstr(d, hex);
    CHECK(!strcmp(hex, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"),
          "sha256 two-block vector");
}

static void test_json(void)
{
    const char *src =
        "{\"id\":\"hello.soil\",\"n\":42,\"ok\":true,\"neg\":-1.5,"
        "\"arr\":[\"a\",\"b\\n\",null,false],"
        "\"esc\":\"q\\\"\\\\\\u00e9\\ud83d\\ude00\"}";
    char err[128];
    jval *v = json_parse(src, strlen(src), err, sizeof(err));
    CHECK(v != NULL, "json parses manifest-like object");
    if (!v)
        return;
    CHECK(!strcmp(json_str_get(v, "id"), "hello.soil"), "json string field");
    CHECK(json_int_get(v, "n", 0) == 42, "json int field");
    CHECK(json_bool_get(v, "ok", 0) == 1, "json bool field");
    CHECK(json_obj_get(v, "arr") && json_obj_get(v, "arr")->type == JSON_ARR &&
          json_obj_get(v, "arr")->n == 4, "json array length");
    const char *esc = json_str_get(v, "esc");
    CHECK(esc && !strcmp(esc, "q\"\\\xc3\xa9\xf0\x9f\x98\x80"), "json unicode + escapes");

    jbuf b;
    jbuf_init(&b);
    json_write(&b, v);
    jval *v2 = json_parse(b.p, b.len, err, sizeof(err));
    CHECK(v2 != NULL, "json write output reparses");
    if (v2) {
        CHECK(!strcmp(json_str_get(v2, "id"), "hello.soil"), "json roundtrip field");
        json_free(v2);
    }
    jbuf_free(&b);
    json_free(v);

    const char *bad = "{\"a\":";
    CHECK(json_parse(bad, strlen(bad), err, sizeof(err)) == NULL, "json rejects truncated input");
}

static void test_sol(const char *pkg)
{
    char err[128] = "";
    sol_header h;
    int rc = sol_read_header(pkg, &h, err, sizeof(err));
    CHECK(rc == 0, "sol header parses");
    if (rc != 0) {
        printf("     error: %s\n", err);
        return;
    }
    CHECK(h.version == SOL_FORMAT_VERSION, "sol version = 1");
    CHECK(h.header_len == SOL_HDR_SIZE + h.manifest_len, "sol header_len consistent");
    CHECK(h.payload_size > 0, "sol payload non-empty");
    CHECK(h.signature[0] != 0, "sol signature present");

    char *man = NULL;
    size_t mlen = 0;
    CHECK(sol_read_manifest(pkg, &h, &man, &mlen) == 0, "sol manifest read");
    if (man) {
        char jerr[128];
        jval *m = json_parse(man, mlen, jerr, sizeof(jerr));
        CHECK(m != NULL, "sol manifest is valid json");
        if (m) {
            CHECK(json_str_get(m, "id") != NULL, "sol manifest has id");
            json_free(m);
        }
        free(man);
    }

    unsigned char digest[32];
    CHECK(sol_compute_digest(pkg, &h, digest) == 0, "sol digest computes");
    CHECK(!memcmp(digest, h.digest, 32), "sol digest matches header");

    char signer[128] = "";
    int sig = sol_verify_signature(pkg, &h, signer, sizeof(signer));
    CHECK(sig == SOL_SIG_TRUSTED, "sol signature verifies via openssl");
    if (sig == SOL_SIG_TRUSTED)
        printf("     signer: %s\n", signer);
}

int main(int argc, char **argv)
{
    printf("== soil core tests ==\n");
    test_sha256();
    test_json();
    if (argc > 1) {
        test_sol(argv[1]);
    } else {
        printf("skip sol tests (no package path)\n");
    }
    printf(failures ? "%d FAILURES\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
