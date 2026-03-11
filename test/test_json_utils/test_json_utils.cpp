#include <unity.h>
#include "json_utils.h"

// ── jsonGetString ──────────────────────────────────────────────────────────

void test_jsonGetString_normal() {
    char buf[32];
    TEST_ASSERT_TRUE(jsonGetString("{\"name\":\"hello\"}", "name", buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("hello", buf);
}

void test_jsonGetString_missing_key() {
    char buf[32];
    TEST_ASSERT_FALSE(jsonGetString("{\"name\":\"hello\"}", "other", buf, sizeof(buf)));
}

void test_jsonGetString_empty_value() {
    char buf[32];
    TEST_ASSERT_TRUE(jsonGetString("{\"name\":\"\"}", "name", buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("", buf);
}

void test_jsonGetString_truncation() {
    char buf[4];
    TEST_ASSERT_TRUE(jsonGetString("{\"k\":\"abcdef\"}", "k", buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("abc", buf);
}

// ── jsonGetFloat ──────────────────────────────────────────────────────────

void test_jsonGetFloat_integer() {
    float val;
    TEST_ASSERT_TRUE(jsonGetFloat("{\"temp\":22}", "temp", &val));
    TEST_ASSERT_EQUAL_FLOAT(22.0f, val);
}

void test_jsonGetFloat_decimal() {
    float val;
    TEST_ASSERT_TRUE(jsonGetFloat("{\"temp\":22.5}", "temp", &val));
    TEST_ASSERT_EQUAL_FLOAT(22.5f, val);
}

void test_jsonGetFloat_negative() {
    float val;
    TEST_ASSERT_TRUE(jsonGetFloat("{\"temp\":-3.5}", "temp", &val));
    TEST_ASSERT_EQUAL_FLOAT(-3.5f, val);
}

void test_jsonGetFloat_missing() {
    float val;
    TEST_ASSERT_FALSE(jsonGetFloat("{\"temp\":22}", "other", &val));
}

// ── jsonGetInt ────────────────────────────────────────────────────────────

void test_jsonGetInt_normal() {
    int val;
    TEST_ASSERT_TRUE(jsonGetInt("{\"level\":3}", "level", &val));
    TEST_ASSERT_EQUAL_INT(3, val);
}

void test_jsonGetInt_negative() {
    int val;
    TEST_ASSERT_TRUE(jsonGetInt("{\"level\":-1}", "level", &val));
    TEST_ASSERT_EQUAL_INT(-1, val);
}

void test_jsonGetInt_missing() {
    int val;
    TEST_ASSERT_FALSE(jsonGetInt("{\"level\":3}", "other", &val));
}

// ── jsonGetBool ───────────────────────────────────────────────────────────

void test_jsonGetBool_true() {
    bool val;
    TEST_ASSERT_TRUE(jsonGetBool("{\"on\":true}", "on", &val));
    TEST_ASSERT_TRUE(val);
}

void test_jsonGetBool_false() {
    bool val;
    TEST_ASSERT_TRUE(jsonGetBool("{\"on\":false}", "on", &val));
    TEST_ASSERT_FALSE(val);
}

void test_jsonGetBool_missing() {
    bool val;
    TEST_ASSERT_FALSE(jsonGetBool("{\"on\":true}", "other", &val));
}

void test_jsonGetBool_non_boolean() {
    bool val;
    TEST_ASSERT_FALSE(jsonGetBool("{\"on\":123}", "on", &val));
}

// ── jsonEscape ────────────────────────────────────────────────────────────

void test_jsonEscape_plain() {
    char dst[32];
    size_t len = jsonEscape("hello", dst, sizeof(dst));
    TEST_ASSERT_EQUAL_STRING("hello", dst);
    TEST_ASSERT_EQUAL(5, len);
}

void test_jsonEscape_quotes() {
    char dst[32];
    jsonEscape("say \"hi\"", dst, sizeof(dst));
    TEST_ASSERT_EQUAL_STRING("say \\\"hi\\\"", dst);
}

void test_jsonEscape_backslash() {
    char dst[32];
    jsonEscape("a\\b", dst, sizeof(dst));
    TEST_ASSERT_EQUAL_STRING("a\\\\b", dst);
}

void test_jsonEscape_newline() {
    char dst[32];
    jsonEscape("a\nb", dst, sizeof(dst));
    TEST_ASSERT_EQUAL_STRING("a\\nb", dst);
}

void test_jsonEscape_carriage_return_stripped() {
    char dst[32];
    jsonEscape("a\rb", dst, sizeof(dst));
    TEST_ASSERT_EQUAL_STRING("ab", dst);
}

void test_jsonEscape_empty() {
    char dst[32];
    size_t len = jsonEscape("", dst, sizeof(dst));
    TEST_ASSERT_EQUAL_STRING("", dst);
    TEST_ASSERT_EQUAL(0, len);
}

// ── Runner ────────────────────────────────────────────────────────────────

int main() {
    UNITY_BEGIN();

    RUN_TEST(test_jsonGetString_normal);
    RUN_TEST(test_jsonGetString_missing_key);
    RUN_TEST(test_jsonGetString_empty_value);
    RUN_TEST(test_jsonGetString_truncation);

    RUN_TEST(test_jsonGetFloat_integer);
    RUN_TEST(test_jsonGetFloat_decimal);
    RUN_TEST(test_jsonGetFloat_negative);
    RUN_TEST(test_jsonGetFloat_missing);

    RUN_TEST(test_jsonGetInt_normal);
    RUN_TEST(test_jsonGetInt_negative);
    RUN_TEST(test_jsonGetInt_missing);

    RUN_TEST(test_jsonGetBool_true);
    RUN_TEST(test_jsonGetBool_false);
    RUN_TEST(test_jsonGetBool_missing);
    RUN_TEST(test_jsonGetBool_non_boolean);

    RUN_TEST(test_jsonEscape_plain);
    RUN_TEST(test_jsonEscape_quotes);
    RUN_TEST(test_jsonEscape_backslash);
    RUN_TEST(test_jsonEscape_newline);
    RUN_TEST(test_jsonEscape_carriage_return_stripped);
    RUN_TEST(test_jsonEscape_empty);

    return UNITY_END();
}
