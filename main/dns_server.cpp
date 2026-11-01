#include "dns_server.h"
#include "logging.h"

#include <string.h>
#include <lwip/sockets.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static const char *TAG = "dns";

// ── State ────────────────────────────────────────────────────────────────────
static volatile bool s_running   = false;
static TaskHandle_t  s_taskHandle = nullptr;
static int           s_sock      = -1;
static uint32_t      s_redirectIP = 0;  // network byte order

// ── DNS constants ────────────────────────────────────────────────────────────
static constexpr int    DNS_PORT        = 53;
static constexpr size_t DNS_HEADER_SIZE = 12;
static constexpr size_t DNS_MAX_PACKET  = 512;

// DNS header flags for a standard response
static constexpr uint16_t DNS_QR_RESPONSE  = 0x8000;
static constexpr uint16_t DNS_OPCODE_QUERY = 0x0000;
static constexpr uint16_t DNS_AA_FLAG      = 0x0400;  // Authoritative Answer
static constexpr uint16_t DNS_RCODE_OK     = 0x0000;

// ── Helpers ──────────────────────────────────────────────────────────────────

/// Build a minimal DNS response that returns `redirect_ip` as the answer.
/// Returns the response length, or 0 on error.
static size_t build_response(const uint8_t *query, size_t queryLen,
                             uint8_t *resp, size_t respCap, uint32_t ip)
{
    if (queryLen < DNS_HEADER_SIZE || queryLen > DNS_MAX_PACKET) return 0;
    // One ordinary question, with at most one EDNS OPT additional record.
    // Never echo unparsed answer/authority sections or interpret a response
    // as another query. Compression in this first name has no prior name to
    // reference, so reject pointers (including loops) without following them.
    if ((query[2] & 0xF8) || query[4] || query[5] != 1 ||
        query[6] || query[7] || query[8] || query[9] ||
        query[10] || query[11] > 1) return 0;
    size_t end = DNS_HEADER_SIZE;
    while (true) {
        if (end >= queryLen) return 0;
        const uint8_t len = query[end++];
        if (len > 63 || len > queryLen - end) return 0;
        end += len;
        if (end - DNS_HEADER_SIZE > 255) return 0;
        if (!len) break;
    }
    if (queryLen - end < 4) return 0;
    const uint16_t type = ((uint16_t)query[end] << 8) | query[end + 1];
    const uint16_t cls = ((uint16_t)query[end + 2] << 8) | query[end + 3];
    end += 4;
    const size_t questionEnd = end;
    const bool edns = query[11] != 0;
    uint8_t ednsFlags = 0;
    if (edns) {
        // OPT has a root owner, TYPE 41, zero extended RCODE and version 0.
        if (queryLen - end < 11 || query[end] || query[end + 1] ||
            query[end + 2] != 41 || query[end + 5] || query[end + 6]) return 0;
        const size_t dataLen = ((size_t)query[end + 9] << 8) | query[end + 10];
        ednsFlags = query[end + 7] & 0x80;  // DNSSEC OK, no reserved flags
        end += 11;
        if (dataLen != queryLen - end) return 0;
        // Unknown EDNS options are ignored, but their framing must be valid.
        while (end < queryLen) {
            if (queryLen - end < 4) return 0;
            const size_t len = ((size_t)query[end + 2] << 8) | query[end + 3];
            end += 4;
            if (len > queryLen - end) return 0;
            end += len;
        }
    }
    if (end != queryLen) return 0;

    const bool answer = cls == 1 && (type == 1 || type == 255);
    const size_t responseLen = questionEnd + (answer ? 16 : 0) + (edns ? 11 : 0);
    if (responseLen > respCap) return 0;
    memcpy(resp, query, questionEnd);

    // Set response flags: QR=1, AA=1, RCODE=0
    uint16_t flags = DNS_QR_RESPONSE | DNS_OPCODE_QUERY | DNS_AA_FLAG | DNS_RCODE_OK |
                     ((uint16_t)(query[2] & 1) << 8);  // echo recursion desired
    resp[2] = (uint8_t)(flags >> 8);
    resp[3] = (uint8_t)(flags & 0xFF);

    // Explicit section counts: the OPT record belongs after the answer.
    resp[6] = 0;
    resp[7] = answer ? 1 : 0;
    resp[8] = resp[9] = resp[10] = 0;
    resp[11] = edns ? 1 : 0;

    // Append answer resource record after the question section
    size_t offset = questionEnd;

    if (answer) {
        // NAME — pointer to QNAME in the question section (offset 12)
        resp[offset++] = 0xC0;
        resp[offset++] = 0x0C;

        // TYPE = A (1)
        resp[offset++] = 0x00;
        resp[offset++] = 0x01;

        // CLASS = IN (1)
        resp[offset++] = 0x00;
        resp[offset++] = 0x01;

        // TTL = 60 seconds
        resp[offset++] = 0x00;
        resp[offset++] = 0x00;
        resp[offset++] = 0x00;
        resp[offset++] = 0x3C;

        // RDLENGTH = 4
        resp[offset++] = 0x00;
        resp[offset++] = 0x04;

        // RDATA = IP address (already in network byte order)
        memcpy(&resp[offset], &ip, 4);
        offset += 4;
    }

    if (edns) {
        // Advertise only the packet size this server actually receives.
        const uint8_t opt[] = {0, 0, 41,
            (uint8_t)(DNS_MAX_PACKET >> 8), (uint8_t)DNS_MAX_PACKET,
            0, 0, ednsFlags, 0, 0, 0};
        memcpy(resp + offset, opt, sizeof(opt));
        offset += sizeof(opt);
    }

    return offset;
}

// ── Task ─────────────────────────────────────────────────────────────────────

static void dns_task(void *arg) {
    uint8_t  buf[DNS_MAX_PACKET];
    uint8_t  resp[DNS_MAX_PACKET + 16];

    s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_sock < 0) {
        LOG_ERROR("Failed to create socket");
        s_running = false;
        s_taskHandle = nullptr;
        vTaskDelete(nullptr);
        return;
    }

    // Set receive timeout so we can check the stop flag periodically
    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    setsockopt(s_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in addr = {};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(DNS_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(s_sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        LOG_ERROR("Failed to bind port %d", DNS_PORT);
        close(s_sock);
        s_sock = -1;
        s_running = false;
        s_taskHandle = nullptr;
        vTaskDelete(nullptr);
        return;
    }

    LOG_INFO("Captive portal DNS started on port %d", DNS_PORT);

    while (s_running) {
        struct sockaddr_in client = {};
        socklen_t clientLen = sizeof(client);

        int n = recvfrom(s_sock, buf, sizeof(buf), 0,
                         (struct sockaddr *)&client, &clientLen);
        if (n <= 0) {
            continue;  // timeout or error — just re-check stop flag
        }

        size_t respLen = build_response(buf, (size_t)n, resp, sizeof(resp), s_redirectIP);
        if (respLen > 0) {
            sendto(s_sock, resp, respLen, 0,
                   (struct sockaddr *)&client, clientLen);
        }
    }

    close(s_sock);
    s_sock = -1;
    LOG_INFO("Captive portal DNS stopped");

    s_taskHandle = nullptr;
    vTaskDelete(nullptr);
}

// ── Public API ───────────────────────────────────────────────────────────────

void dns_captive_start(uint32_t redirect_ip) {
    if (s_running) return;

    s_redirectIP = redirect_ip;  // already in network byte order
    s_running = true;

    xTaskCreate(dns_task, "dns_captive", 4096, nullptr, 2, &s_taskHandle);
}

void dns_captive_stop() {
    if (!s_running) return;

    s_running = false;

    // Close the socket to unblock recvfrom() immediately
    if (s_sock >= 0) {
        close(s_sock);
        s_sock = -1;
    }

    // Wait for task to finish (up to 2 seconds)
    for (int i = 0; i < 20 && s_taskHandle != nullptr; i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
