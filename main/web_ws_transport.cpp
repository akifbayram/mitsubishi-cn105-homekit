#include "web_ws_transport.h"
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <lwip/sockets.h>

// A scheduling caller may wait for UDP control-mailbox capacity, so it must
// NEVER be main. With this setting successful admission cannot silently lose
// the sole outstanding callback and strand its owned payload forever.
#if !CONFIG_HTTPD_QUEUE_WORK_BLOCKING
#error "WebWsTransport requires CONFIG_HTTPD_QUEUE_WORK_BLOCKING=y"
#endif

bool WebWsTransport::start(httpd_handle_t server) {
    if (_mutex || _worker || !server) return false;
    _mutex = xSemaphoreCreateMutex();
    _ready = xSemaphoreCreateBinary();
    _sent = xSemaphoreCreateBinary();
    _exited = xSemaphoreCreateBinary();
    if (!_mutex || !_ready || !_sent || !_exited) {
        stop();
        release();
        return false;
    }
    _server = server;
    _stopping = false;
    if (xTaskCreate(run, "ws_publish", 3072, this, tskIDLE_PRIORITY + 1,
                    &_worker) != pdPASS) {
        _worker = nullptr;
        stop();
        release();
        return false;
    }
    return true;
}

void WebWsTransport::stop() {
    if (_worker) {
        xSemaphoreTake(_mutex, portMAX_DELAY);
        _stopping = true;
        xSemaphoreGive(_mutex);
        xSemaphoreGive(_ready);
        xSemaphoreTake(_exited, portMAX_DELAY);
        _worker = nullptr;
    }
    if (_mutex) xSemaphoreTake(_mutex, portMAX_DELAY);
    while (_count) discard(0);
    for (auto &client : _clients) client = {};
    if (_mutex) xSemaphoreGive(_mutex);
}

void WebWsTransport::release() {
    _server = nullptr;
    for (auto *semaphore : {&_mutex, &_ready, &_sent, &_exited}) {
        if (*semaphore) vSemaphoreDelete(*semaphore);
        *semaphore = nullptr;
    }
}

void WebWsTransport::connected(int fd) {
    if (!_mutex) return;
    xSemaphoreTake(_mutex, portMAX_DELAY);
    if (_stopping) {
        xSemaphoreGive(_mutex);
        return;
    }
    for (auto &client : _clients) {
        if (client.fd == fd || client.fd < 0) {
            if (++_generation == 0) ++_generation;
            client = {fd, _generation};
            break;
        }
    }
    xSemaphoreGive(_mutex);
}

void WebWsTransport::disconnected(int fd) {
    if (!_mutex) return;
    xSemaphoreTake(_mutex, portMAX_DELAY);
    for (auto &client : _clients) if (client.fd == fd) client = {};
    xSemaphoreGive(_mutex);
}

bool WebWsTransport::hasClients() {
    if (!_mutex || xSemaphoreTake(_mutex, 0) != pdTRUE) return false;
    bool found = false;
    for (const auto &client : _clients) found |= client.fd >= 0;
    xSemaphoreGive(_mutex);
    return found;
}

void WebWsTransport::discard(size_t index) {
    _bytes -= _pending[index].bytes;
    free(_pending[index].text);
    for (size_t i = index + 1; i < _count; ++i) _pending[i - 1] = _pending[i];
    _pending[--_count] = {};
}

bool WebWsTransport::publish(const char *text, Kind kind) {
    if (!text || !_mutex) return false;
    const size_t length = strnlen(text, MAX_FRAME);
    if (!length || length == MAX_FRAME) return false;
    if (xSemaphoreTake(_mutex, 0) != pdTRUE) return false;
    bool haveClient = false;
    for (const auto &client : _clients) haveClient |= client.fd >= 0;
    if (_stopping || !haveClient) {
        xSemaphoreGive(_mutex);
        return false;
    }
    // State/discovery are snapshots: replace an unsent older snapshot. Logs
    // are best effort, and can never occupy the space needed by fresh state.
    if (kind != Kind::Log) {
        for (size_t i = 0; i < _count; ) {
            if (_pending[i].kind == kind) discard(i); else ++i;
        }
    }
    const size_t bytes = length + 1;
    while (kind != Kind::Log && (_count == MAX_PENDING || _bytes + bytes > MAX_BYTES)) {
        size_t i = 0;
        while (i < _count && _pending[i].kind != Kind::Log) ++i;
        if (i == _count && kind == Kind::Discovery) {
            // Discovery completion is one-shot; state is periodic. Make room
            // for the final scan snapshot even if a large state is in flight.
            i = 0;
            while (i < _count && _pending[i].kind != Kind::State) ++i;
        }
        if (i == _count) break;
        discard(i);
    }
    if (_count == MAX_PENDING || _bytes + bytes > MAX_BYTES) {
        xSemaphoreGive(_mutex);
        return false;
    }
    char *copy = static_cast<char *>(malloc(bytes));
    if (!copy) {
        xSemaphoreGive(_mutex);
        return false;
    }
    memcpy(copy, text, bytes);
    Message &message = _pending[_count++];
    message.text = copy;
    message.bytes = bytes;
    message.kind = kind;
    memcpy(message.clients, _clients, sizeof(_clients));
    _bytes += bytes;
    xSemaphoreGive(_mutex);
    xSemaphoreGive(_ready);
    return true;
}

bool WebWsTransport::takeNext() {
    xSemaphoreTake(_mutex, portMAX_DELAY);
    const bool available = !_stopping && _count;
    if (available) {
        _inFlight = _pending[0];
        for (size_t i = 1; i < _count; ++i) _pending[i - 1] = _pending[i];
        _pending[--_count] = {};
        // Keep these bytes charged until the httpd callback finishes.
    }
    xSemaphoreGive(_mutex);
    return available;
}

void WebWsTransport::run(void *arg) {
    auto &self = *static_cast<WebWsTransport *>(arg);
    for (;;) {
        xSemaphoreTake(self._ready, portMAX_DELAY);
        while (self.takeNext()) {
            if (httpd_queue_work(self._server, deliver, &self) == ESP_OK)
                xSemaphoreTake(self._sent, portMAX_DELAY);
            xSemaphoreTake(self._mutex, portMAX_DELAY);
            self._bytes -= self._inFlight.bytes;
            free(self._inFlight.text);
            self._inFlight = {};
            xSemaphoreGive(self._mutex);
        }
        xSemaphoreTake(self._mutex, portMAX_DELAY);
        bool stopping = self._stopping;
        xSemaphoreGive(self._mutex);
        if (stopping) break;
    }
    xSemaphoreGive(self._exited);
    vTaskDelete(nullptr);
}

void WebWsTransport::deliver(void *arg) {
    auto &self = *static_cast<WebWsTransport *>(arg);
    for (const auto &client : self._inFlight.clients)
        if (client.fd >= 0) self.sendTo(client, self._inFlight.text);
    xSemaphoreGive(self._sent);
}

void WebWsTransport::sendText(int fd, const char *text) {
    if (!_mutex || !text) return;
    Client target;
    xSemaphoreTake(_mutex, portMAX_DELAY);
    for (const auto &client : _clients) if (client.fd == fd) target = client;
    xSemaphoreGive(_mutex);
    if (target.fd >= 0) sendTo(target, text);
}

void WebWsTransport::sendTo(Client target, const char *text) {
    bool current = false;
    xSemaphoreTake(_mutex, portMAX_DELAY);
    for (const auto &client : _clients)
        current |= client.fd == target.fd && client.generation == target.generation;
    xSemaphoreGive(_mutex);
    if (!current || httpd_ws_get_fd_info(_server, target.fd) != HTTPD_WS_CLIENT_WEBSOCKET)
        return;
    httpd_ws_frame_t frame = {};
    frame.type = HTTPD_WS_TYPE_TEXT;
    frame.payload = reinterpret_cast<uint8_t *>(const_cast<char *>(text));
    frame.len = strlen(text);
    if (httpd_ws_send_frame_async(_server, target.fd, &frame) == ESP_OK) {
        httpd_sess_update_lru_counter(_server, target.fd);
    } else {
        // Do not queue close work from a work callback: the HTTPD control
        // semaphore can already be full. shutdown makes select reap it; mark
        // it unavailable NOW, so queued broadcasts never retry a broken frame.
        disconnected(target.fd);
        shutdown(target.fd, SHUT_RDWR);
    }
}

int WebWsTransport::sendComplete(httpd_handle_t, int fd, const char *data,
                                 size_t length, int flags) {
    // IDF's WS encoder accepts positive SHORT writes as complete. A timeout
    // after partial progress would therefore corrupt the next frame. Treat it
    // as failure and close the connection instead. SO_SNDTIMEO bounds each
    // call to 250 ms; a frame has at most a header call and a payload call.
    const int sent = send(fd, data, length, flags);
    return sent == static_cast<int>(length) ? sent : -1;
}
