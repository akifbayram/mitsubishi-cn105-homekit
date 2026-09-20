#pragma once

#include <cstddef>
#include <cstdint>
#include <esp_http_server.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

// Main only copies complete broadcasts into a bounded mailbox. A small task
// retries an idempotent HTTPD drain callback; only httpd touches sessions/sockets,
// including SDK-generated PONG/CLOSE and command replies before a reboot.
class WebWsTransport {
public:
    enum class Kind { State, Discovery, Log };
    static constexpr size_t MAX_FRAME = 4112; // includes pushState's closing reserve
    static constexpr size_t MAX_BYTES = 8192; // queued AND in-flight payloads
    static constexpr size_t MAX_PENDING = 8;
    static constexpr size_t MAX_CLIENTS = 7;

    bool start(httpd_handle_t server);
    // Startup cleanup / host tests only. A successful production transport
    // lives until reboot: do not add a runtime httpd_stop flow without handling
    // IDF's lossy UDP shutdown message. stop() joins only our scheduler;
    // release() requires that HTTPD has exited and ALL callbacks have finished.
    void stop();
    void release();
    bool hasClients();
    bool publish(const char *text, Kind kind);

    // These three methods run on httpd only. No fd is retained without its
    // generation; closing/reusing a socket invalidates already queued work.
    void connected(int fd);
    void disconnected(int fd);
    void sendText(int fd, const char *text);
    static int sendComplete(httpd_handle_t server, int fd, const char *data,
                            size_t length, int flags);

private:
    struct Client { int fd = -1; uint32_t generation = 0; };
    struct Message {
        char *text = nullptr;
        size_t bytes = 0;
        Kind kind = Kind::Log;
        Client clients[MAX_CLIENTS];
    };
    httpd_handle_t _server = nullptr;
    SemaphoreHandle_t _mutex = nullptr, _ready = nullptr;
    SemaphoreHandle_t _sent = nullptr, _exited = nullptr;
    TaskHandle_t _worker = nullptr;
    bool _stopping = false;
    Client _clients[MAX_CLIENTS];
    uint32_t _generation = 0;
    Message _pending[MAX_PENDING];
    size_t _count = 0, _bytes = 0;

    static void run(void *arg);
    static void deliver(void *arg);
    bool takeNext(Message &message);
    bool hasPending();
    void discard(size_t index);
    void sendTo(Client client, const char *text);
};
