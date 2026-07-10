#pragma once

#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <cstdarg>

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

// Safely append printf-formatted text to a fixed buffer using a running offset.
//
// Replaces the unsafe idiom `n += snprintf(buf + n, sizeof(buf) - n, ...)`, which
// corrupts memory once `n` exceeds the buffer: `sizeof(buf) - n` is unsigned, so it
// underflows to a huge size and `buf + n` already points past the end, letting the
// next snprintf write out of bounds. This helper stops writing once `*pos` reaches
// `cap`, while still advancing `*pos` past `cap` so the caller can detect truncation
// afterwards via `*pos >= (int)cap`.
inline void jsonAppend(char *buf, size_t cap, int *pos, const char *fmt, ...) {
    if (*pos < 0 || (size_t)*pos >= cap) return;   // full/invalid — never underflow cap - *pos

    va_list args;
    va_start(args, fmt);
    int w = vsnprintf(buf + *pos, cap - (size_t)*pos, fmt, args);
    va_end(args);

    // w < 0: encoding error — mark truncated. Otherwise advance; w is the length that
    // *would* have been written, so *pos may exceed cap (vsnprintf still NUL-terminated
    // within cap), making truncation detectable on the next call and at the end.
    *pos = (w < 0) ? (int)cap : *pos + w;
}

// Length of the UTF-8 sequence starting at src (which must not be NUL), or 0 if
// the bytes do not form a valid sequence. Rejects overlongs, surrogates, and
// out-of-range leads — RFC 6455 makes browsers kill the whole WebSocket on any
// invalid UTF-8 in a text frame.
inline size_t utf8SeqLen(const unsigned char *s) {
    unsigned char b = s[0];
    if (b < 0x80) return 1;
    size_t n; unsigned char lo = 0x80, hi = 0xBF;
    if (b >= 0xC2 && b <= 0xDF)      { n = 2; }
    else if (b == 0xE0)              { n = 3; lo = 0xA0; }
    else if (b >= 0xE1 && b <= 0xEC) { n = 3; }
    else if (b == 0xED)              { n = 3; hi = 0x9F; }   // exclude surrogates
    else if (b >= 0xEE && b <= 0xEF) { n = 3; }
    else if (b == 0xF0)              { n = 4; lo = 0x90; }
    else if (b >= 0xF1 && b <= 0xF3) { n = 4; }
    else if (b == 0xF4)              { n = 4; hi = 0x8F; }   // cap at U+10FFFF
    else return 0;
    if (s[1] < lo || s[1] > hi) return 0;
    for (size_t k = 2; k < n; k++)
        if (s[k] < 0x80 || s[k] > 0xBF) return 0;
    return n;
}

// Escape a string for safe embedding in a JSON string literal. Source bytes can
// be hostile (e.g. BLE-advertised device names): control chars are \u-escaped
// and invalid UTF-8 is replaced with '?' so neither JSON.parse nor the
// WebSocket UTF-8 check can be broken by a nearby advertiser.
inline size_t jsonEscape(const char *src, char *dst, size_t dstLen) {
    size_t j = 0;
    for (size_t i = 0; src[i] && j < dstLen - 2; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c == '"' || c == '\\') {
            if (j < dstLen - 3) { dst[j++] = '\\'; dst[j++] = (char)c; }
        } else if (c == '\n') {
            if (j < dstLen - 3) { dst[j++] = '\\'; dst[j++] = 'n'; }
        } else if (c == '\r') {
            // skip carriage returns
        } else if (c < 0x20) {
            if (j + 6 < dstLen - 1) {
                static const char hex[] = "0123456789abcdef";
                dst[j++] = '\\'; dst[j++] = 'u'; dst[j++] = '0'; dst[j++] = '0';
                dst[j++] = hex[c >> 4]; dst[j++] = hex[c & 0x0F];
            }
        } else if (c >= 0x80) {
            size_t n = utf8SeqLen((const unsigned char *)src + i);
            if (n >= 2) {
                // valid sequence: copy whole if it fits, drop whole if not —
                // never split it (a partial sequence is invalid UTF-8 again)
                if (j + n < dstLen - 1)
                    for (size_t k = 0; k < n; k++) dst[j++] = src[i + k];
                i += n - 1;
            } else {
                dst[j++] = '?';
                // swallow the invalid sequence's continuation bytes too
                while ((unsigned char)src[i + 1] >= 0x80 && (unsigned char)src[i + 1] <= 0xBF) i++;
            }
        } else {
            dst[j++] = (char)c;
        }
    }
    dst[j] = '\0';
    return j;
}
