#include "json_utils.h"
#include <cassert>
#include <cstring>
#include <cstdio>

static int checks = 0;

// jsonGetString must parse what JSON.stringify() produces — every browser
// client sends values through it, so escaped quotes/backslashes are normal
// input (WiFi passwords, imported device names), not an edge case.
static void expectStr(const char *json, const char *key, const char *want) {
    char buf[64];
    memset(buf, 0xAA, sizeof(buf));
    bool ok = jsonGetString(json, key, buf, sizeof(buf));
    if (!ok || strcmp(buf, want) != 0) {
        fprintf(stderr, "FAIL key=%s json=%s\n  want [%s]\n  got  [%s] ok=%d\n",
                key, json, want, ok ? buf : "(not found)", (int)ok);
        assert(false);
    }
    checks++;
}

static void expectMissing(const char *json, const char *key) {
    char buf[64];
    assert(!jsonGetString(json, key, buf, sizeof(buf)));
    checks++;
}

int main() {
    // Plain values still work
    expectStr("{\"ssid\":\"HomeNet\"}", "ssid", "HomeNet");
    expectStr("{\"a\":\"x\",\"b\":\"y\"}", "b", "y");
    expectStr("{\"empty\":\"\"}", "empty", "");

    // Escaped quote inside the value — the original bug: a WiFi password
    // like pass"word arrives as "pass\"word" and was truncated at the \.
    expectStr("{\"password\":\"pass\\\"word\"}", "password", "pass\"word");
    // Escaped backslash
    expectStr("{\"password\":\"back\\\\slash\"}", "password", "back\\slash");
    // Trailing escaped backslash right before the closing quote
    expectStr("{\"v\":\"ends\\\\\"}", "v", "ends\\");
    // Escaped quote as the entire value
    expectStr("{\"v\":\"\\\"\"}", "v", "\"");
    // Solidus escape (legal JSON, some encoders emit it)
    expectStr("{\"v\":\"a\\/b\"}", "v", "a/b");
    // Whitespace escapes
    expectStr("{\"v\":\"a\\nb\\tc\\rd\\be\\ff\"}", "v", "a\nb\tc\rd\be\ff");

    // \uXXXX BMP escape → UTF-8 (JSON.stringify emits these for controls;
    // other encoders may escape any char)
    expectStr("{\"v\":\"a\\u0041b\"}", "v", "aAb");         // 'A'
    expectStr("{\"v\":\"\\u00e9\"}", "v", "\xc3\xa9");      // é (2-byte UTF-8)
    expectStr("{\"v\":\"\\u20ac\"}", "v", "\xe2\x82\xac");  // € (3-byte UTF-8)
    // Surrogate pair → 4-byte UTF-8 (U+1F600)
    expectStr("{\"v\":\"\\ud83d\\ude00\"}", "v", "\xf0\x9f\x98\x80");
    // Lone surrogate degrades to '?' instead of emitting invalid UTF-8
    expectStr("{\"v\":\"a\\ud800b\"}", "v", "a?b");
    // Malformed \u (short hex) degrades to '?' without eating the tail
    expectStr("{\"v\":\"a\\u12\"}", "v", "a?");

    // Unknown escape: keep the escaped char (lenient, never truncate)
    expectStr("{\"v\":\"a\\qb\"}", "v", "aqb");

    // A value containing an escaped quote must not terminate early and
    // must not leak the next key's content
    expectStr("{\"name\":\"say \\\"hi\\\"\",\"other\":\"z\"}", "name", "say \"hi\"");
    expectStr("{\"name\":\"say \\\"hi\\\"\",\"other\":\"z\"}", "other", "z");

    // Truncation at bufLen-1 still NUL-terminates, and never splits an escape
    {
        char small[6];
        assert(jsonGetString("{\"v\":\"abcdefgh\"}", "v", small, sizeof(small)));
        assert(strcmp(small, "abcde") == 0);
        checks++;
        // Escape landing exactly at the boundary: drop it whole, don't split
        assert(jsonGetString("{\"v\":\"abcd\\\"xyz\"}", "v", small, sizeof(small)));
        assert(strcmp(small, "abcd") == 0 || strcmp(small, "abcd\"") == 0);
        checks++;
    }

    // Missing key / unterminated value
    expectMissing("{\"a\":\"x\"}", "b");
    expectMissing("{\"a\":\"unterminated", "a");
    // Unterminated value that ends in a backslash must not read past the end
    expectMissing("{\"a\":\"trailing\\", "a");

    printf("json_utils: all %d checks passed\n", checks);
    return 0;
}
