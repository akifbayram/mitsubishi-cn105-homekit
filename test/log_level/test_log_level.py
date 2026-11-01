#!/usr/bin/env python3
"""Compile the production log-level setter against ESP-IDF's tag semantics.

esp_log_level_set("*") sets the default and discards every per-tag entry, so
the order of calls decides the effective level of each tag. The stub models
exactly that; the assertions are on effective levels, not on call counts."""
from pathlib import Path
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
MAIN = HERE.parent.parent / "main"
src = (MAIN / "logging.cpp").read_text()


def block(start_sig):
    start = src.index(start_sig)
    opening = src.index("{", start)
    depth, end = 1, opening + 1
    while depth:
        depth += {"{": 1, "}": -1}.get(src[end], 0)
        end += 1
    return src[start:end]


# File-scope tables the setter reads live between the level conversion and
# the setter itself; keep that span so the production list is what is tested.
production = src[src.index("esp_log_level_t log_level_to_esp("):src.index("void logging_set_level(")]
production += block("void logging_set_level(")

HOST = r'''
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
enum esp_log_level_t { ESP_LOG_NONE, ESP_LOG_ERROR, ESP_LOG_WARN, ESP_LOG_INFO, ESP_LOG_DEBUG, ESP_LOG_VERBOSE };
static esp_log_level_t g_default = ESP_LOG_INFO;
static std::map<std::string, esp_log_level_t> g_tags;
static void esp_log_level_set(const char* tag, esp_log_level_t level) {
    if (!strcmp(tag, "*")) { g_default = level; g_tags.clear(); return; }
    g_tags[tag] = level;
}
static esp_log_level_t effective(const char* tag) {
    auto it = g_tags.find(tag); return it == g_tags.end() ? g_default : it->second;
}
enum LogLevel : uint8_t { LOG_LEVEL_ERROR = 0, LOG_LEVEL_WARN = 1, LOG_LEVEL_INFO = 2, LOG_LEVEL_DEBUG = 3 };
LogLevel currentLogLevel = LOG_LEVEL_INFO;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "line %d: %s\n", __LINE__, #c); return 1; } } while (0)
'''
TEST = r'''
int main() {
    // DEBUG applies to the firmware's own tags but never to the transport that
    // carries the log stream: HTTPD logs several lines per WebSocket frame.
    logging_set_level(LOG_LEVEL_DEBUG);
    CHECK(currentLogLevel == LOG_LEVEL_DEBUG);
    CHECK(effective("cn105") == ESP_LOG_DEBUG);
    CHECK(effective("ble") == ESP_LOG_DEBUG);
    for (const char* tag : {"httpd", "httpd_txrx", "httpd_ws", "httpd_uri", "httpd_sess", "httpd_parse", "esp_netif_lwip", "nvs"})
        CHECK(effective(tag) == ESP_LOG_INFO);
    // Lower levels still bind the transport tags: INFO means INFO, WARN means WARN.
    logging_set_level(LOG_LEVEL_INFO);
    CHECK(effective("httpd") == ESP_LOG_INFO && effective("cn105") == ESP_LOG_INFO);
    logging_set_level(LOG_LEVEL_WARN);
    CHECK(effective("httpd") == ESP_LOG_WARN && effective("cn105") == ESP_LOG_WARN);
    logging_set_level(LOG_LEVEL_ERROR);
    CHECK(effective("httpd_txrx") == ESP_LOG_ERROR);
    puts("debug level leaves the log transport at INFO; lower levels apply everywhere");
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix="cn105-log-level-") as directory:
    path = Path(directory) / "log_level.cpp"
    path.write_text(HOST + production + TEST)
    exe = Path(directory) / "log_level"
    subprocess.run(["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror", str(path), "-o", str(exe)], check=True)
    result = subprocess.run([str(exe)])
    print(f"log_level/transport_tags: {'PASS' if result.returncode == 0 else 'FAIL'}")
    raise SystemExit(result.returncode)
