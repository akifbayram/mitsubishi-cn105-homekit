#pragma once

#include <cstring>
#include <cstdlib>
#include <cstdio>

// ════════════════════════════════════════════════════════════════════════════
// Lightweight JSON parsing helpers (strstr-based, no external library)
// ════════════════════════════════════════════════════════════════════════════

// Extract a string value for a given key from JSON.
// Returns true if found; copies value into buf (up to bufLen-1 chars).
inline bool jsonGetString(const char *json, const char *key, char *buf, size_t bufLen) {
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\":\"", key);
    const char *p = strstr(json, pattern);
    if (!p) return false;
    p += strlen(pattern);
    const char *end = strchr(p, '"');
    if (!end) return false;
    size_t len = end - p;
    if (len >= bufLen) len = bufLen - 1;
    memcpy(buf, p, len);
    buf[len] = '\0';
    return true;
}

// Extract a numeric (float) value for a given key from JSON.
inline bool jsonGetFloat(const char *json, const char *key, float *out) {
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\":", key);
    const char *p = strstr(json, pattern);
    if (!p) return false;
    p += strlen(pattern);
    while (*p == ' ') p++;
    char *endp;
    float val = strtof(p, &endp);
    if (endp == p) return false;
    *out = val;
    return true;
}

// Extract an integer value for a given key from JSON.
inline bool jsonGetInt(const char *json, const char *key, int *out) {
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\":", key);
    const char *p = strstr(json, pattern);
    if (!p) return false;
    p += strlen(pattern);
    while (*p == ' ') p++;
    char *endp;
    long val = strtol(p, &endp, 10);
    if (endp == p) return false;
    *out = (int)val;
    return true;
}

// Extract a boolean value for a given key from JSON.
inline bool jsonGetBool(const char *json, const char *key, bool *out) {
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\":", key);
    const char *p = strstr(json, pattern);
    if (!p) return false;
    p += strlen(pattern);
    while (*p == ' ') p++;
    if (strncmp(p, "true", 4) == 0) {
        *out = true;
        return true;
    }
    if (strncmp(p, "false", 5) == 0) {
        *out = false;
        return true;
    }
    return false;
}

// Escape a string for safe embedding in a JSON string literal.
inline size_t jsonEscape(const char *src, char *dst, size_t dstLen) {
    size_t j = 0;
    for (size_t i = 0; src[i] && j < dstLen - 2; i++) {
        if (src[i] == '"' || src[i] == '\\') {
            if (j < dstLen - 3) { dst[j++] = '\\'; dst[j++] = src[i]; }
        } else if (src[i] == '\n') {
            if (j < dstLen - 3) { dst[j++] = '\\'; dst[j++] = 'n'; }
        } else if (src[i] == '\r') {
            // skip carriage returns
        } else {
            dst[j++] = src[i];
        }
    }
    dst[j] = '\0';
    return j;
}
