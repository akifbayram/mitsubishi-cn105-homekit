#include <atomic>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <future>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "web_ws_transport.h"
using namespace std::chrono_literals;
#define REQUIRE(test) do {if (!(test)) {std::fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#test); std::abort();}} while (0)
static std::mutex gateMutex, heapMutex;
static std::condition_variable gateCv;
static bool releaseSend = false;
static std::atomic<int> sendCalls{0}, shutdownCalls{0}, callbacks{0};
static int blockFd = 42, failFd = -1;
static bool partialSend = false;
static std::vector<std::pair<int,std::string>> deliveries;
static std::map<void *, size_t> payloadSizes;
static size_t payloadBytes = 0, maxPayloadBytes = 0;
static bool failAllocation = false, failQueue = false, failTask = false;
static std::mutex workMutex;
static std::condition_variable workCv;
static std::deque<std::function<void()>> work;
static bool stoppingHttpd = false;
static std::thread::id httpdId;
static void *trackedMalloc(size_t bytes) {
    if (failAllocation) return nullptr;
    void *p = std::malloc(bytes);
    if (p) {std::lock_guard<std::mutex> lock(heapMutex); payloadSizes[p]=bytes; payloadBytes+=bytes; maxPayloadBytes=std::max(maxPayloadBytes,payloadBytes);}
    return p;
}
static void trackedFree(void *p) {
    if (!p) return;
    {std::lock_guard<std::mutex> lock(heapMutex); REQUIRE(payloadSizes.count(p)); payloadBytes-=payloadSizes[p]; payloadSizes.erase(p);}
    std::free(p);
}
// Compile the actual firmware implementation, tracking its owned payloads.
#define malloc trackedMalloc
#define free trackedFree
#include "web_ws_transport.cpp"
#undef free
#undef malloc
SemaphoreHandle_t xSemaphoreCreateMutex() {return new TestSemaphore(true);}
SemaphoreHandle_t xSemaphoreCreateBinary() {return new TestSemaphore(false);}
int xSemaphoreTake(SemaphoreHandle_t s, uint32_t wait) {
    std::unique_lock<std::mutex> lock(s->mutex);
    if (wait==0 && !s->available) return 0;
    if (wait==portMAX_DELAY) s->cv.wait(lock,[&]{return s->available;});
    else if (!s->cv.wait_for(lock,std::chrono::milliseconds(wait),[&]{return s->available;})) return 0;
    s->available=false; return pdTRUE;
}
void xSemaphoreGive(SemaphoreHandle_t s) {std::lock_guard<std::mutex> lock(s->mutex); s->available=true; s->cv.notify_one();}
void vSemaphoreDelete(SemaphoreHandle_t s) {delete s;}
int xTaskCreate(void (*entry)(void *), const char *, uint32_t, void *arg, int, TaskHandle_t *out) {
    if (failTask) return 0;
    *out=reinterpret_cast<void *>(1); std::thread([=]{entry(arg);}).detach(); return pdPASS;
}
void vTaskDelete(TaskHandle_t) {}
int httpd_queue_work(httpd_handle_t, void (*callback)(void *), void *arg) {
    if (failQueue) return ESP_FAIL;
    {std::lock_guard<std::mutex> lock(workMutex); work.push_back([=]{++callbacks; callback(arg);});}
    workCv.notify_one(); return ESP_OK;
}
int httpd_ws_get_fd_info(httpd_handle_t, int) {REQUIRE(std::this_thread::get_id()==httpdId); return HTTPD_WS_CLIENT_WEBSOCKET;}
int httpd_sess_update_lru_counter(httpd_handle_t, int) {REQUIRE(std::this_thread::get_id()==httpdId); return ESP_OK;}
int shutdown(int, int) {REQUIRE(std::this_thread::get_id()==httpdId); ++shutdownCalls; return 0;}
int send(int, const void *, size_t length, int) {return partialSend ? static_cast<int>(length)-1 : static_cast<int>(length);}
int httpd_ws_send_frame_async(httpd_handle_t, int fd, httpd_ws_frame_t *frame) {
    REQUIRE(std::this_thread::get_id()==httpdId);
    ++sendCalls;
    std::unique_lock<std::mutex> lock(gateMutex);
    gateCv.notify_all();
    if (fd==blockFd) gateCv.wait(lock,[]{return releaseSend;});
    if (fd==failFd) return ESP_FAIL;
    deliveries.emplace_back(fd,std::string(reinterpret_cast<char *>(frame->payload),frame->len));
    return ESP_OK;
}
class WebUI {
public:
    void sendWsText(int fd, const char *text);
    void broadcastWs(const char *text, WebWsTransport::Kind kind = WebWsTransport::Kind::Log);
    WebWsTransport _ws;
};
#include "boundary.inc"
static void onHttpd(std::function<void()> fn) {
    auto done=std::make_shared<std::promise<void>>();
    auto future=done->get_future();
    {std::lock_guard<std::mutex> lock(workMutex); work.push_back([=]{fn(); done->set_value();});}
    workCv.notify_one();
    REQUIRE(future.wait_for(2s)==std::future_status::ready);
}
static void waitFor(std::function<bool()> predicate) {
    const auto deadline=std::chrono::steady_clock::now()+2s;
    while (!predicate() && std::chrono::steady_clock::now()<deadline) std::this_thread::sleep_for(1ms);
    REQUIRE(predicate());
}
int main() {
    std::thread httpd([]{
        httpdId=std::this_thread::get_id();
        for (;;) {
            std::function<void()> fn;
            {std::unique_lock<std::mutex> lock(workMutex); workCv.wait(lock,[]{return stoppingHttpd || !work.empty();}); if (work.empty() && stoppingHttpd) break; fn=std::move(work.front()); work.pop_front();}
            fn();
        }
    });
    WebUI ui;
    REQUIRE(ui._ws.start(reinterpret_cast<void *>(1)));
    onHttpd([&]{ui._ws.connected(42);});
    std::string state(4000,'S');
    auto mainLoop=std::async(std::launch::async,[&]{ui.broadcastWs(state.c_str(),WebWsTransport::Kind::State);});
    REQUIRE(mainLoop.wait_for(100ms)==std::future_status::ready);
    mainLoop.get();
    waitFor([]{return sendCalls.load()==1;}); // prove the real send is stalled
    auto ticks=std::async(std::launch::async,[&]{
        for (int i=0;i<1000;++i) {
            ui.broadcastWs(state.c_str(),WebWsTransport::Kind::State);
            ui.broadcastWs("log line");
        }
    });
    REQUIRE(ticks.wait_for(500ms)==std::future_status::ready);
    ticks.get();
    REQUIRE(callbacks==1); // only ONE callback queued or executing
    std::string discovery(2600,'D');
    REQUIRE(ui._ws.publish(discovery.c_str(),WebWsTransport::Kind::Discovery));
    {std::lock_guard<std::mutex> lock(heapMutex); REQUIRE(maxPayloadBytes<=WebWsTransport::MAX_BYTES);}
    {std::lock_guard<std::mutex> lock(gateMutex); releaseSend=true;}
    gateCv.notify_all();
    waitFor([]{std::lock_guard<std::mutex> lock(heapMutex); return payloadBytes==0;});
    {std::lock_guard<std::mutex> lock(gateMutex);
     REQUIRE(std::any_of(deliveries.begin(),deliveries.end(),[&](const auto &delivery){return delivery.second==discovery;}));}
    std::puts("PASS: production WebUI caller keeps ticking while HTTPD send is blocked; memory bounded; one-shot discovery survives saturation");

    // Queue while HTTPD is held, then replace fd 42 BEFORE queued delivery.
    std::promise<void> held, resume;
    auto go=resume.get_future();
    {std::lock_guard<std::mutex> lock(workMutex); work.push_back([&]{held.set_value(); go.wait(); ui._ws.disconnected(42); ui._ws.connected(42);});}
    workCv.notify_one(); held.get_future().wait();
    const int before=sendCalls;
    REQUIRE(ui._ws.publish("stale old connection",WebWsTransport::Kind::Log));
    resume.set_value();
    waitFor([]{std::lock_guard<std::mutex> lock(heapMutex); return payloadBytes==0;});
    REQUIRE(sendCalls==before);
    std::puts("PASS: reused fd never receives a prior connection's queued frame");

    failFd=42;
    REQUIRE(ui._ws.publish("failed frame",WebWsTransport::Kind::Log));
    waitFor([]{return shutdownCalls.load()==1;});
    waitFor([]{std::lock_guard<std::mutex> lock(heapMutex); return payloadBytes==0;});
    REQUIRE(!ui._ws.publish("no retry",WebWsTransport::Kind::State));
    REQUIRE(shutdownCalls==1);
    onHttpd([&]{ui._ws.connected(43); ui.sendWsText(43,"reply before reboot");});
    {std::lock_guard<std::mutex> lock(gateMutex); REQUIRE(deliveries.back().second=="reply before reboot");}
    std::puts("PASS: failed client retired; command replies finish on handler before reboot");

    failAllocation=true;
    REQUIRE(!ui._ws.publish("allocation failure",WebWsTransport::Kind::State));
    failAllocation=false;
    failQueue=true;
    REQUIRE(ui._ws.publish("queue failure",WebWsTransport::Kind::State));
    waitFor([]{std::lock_guard<std::mutex> lock(heapMutex); return payloadBytes==0;});
    failQueue=false;
    partialSend=true;
    REQUIRE(WebWsTransport::sendComplete(nullptr,43,"partial",7,0)==-1);
    partialSend=false;
    REQUIRE(WebWsTransport::sendComplete(nullptr,43,"complete",8,0)==8);
    // Shutdown must retain callback-owned storage until its socket send ends.
    blockFd=43;
    {std::lock_guard<std::mutex> lock(gateMutex); releaseSend=false;}
    const int beforeStop=sendCalls;
    REQUIRE(ui._ws.publish("in flight at stop",WebWsTransport::Kind::State));
    waitFor([&]{return sendCalls.load()==beforeStop+1;});
    auto stopped=std::async(std::launch::async,[&]{ui._ws.stop();});
    REQUIRE(stopped.wait_for(20ms)==std::future_status::timeout);
    {std::lock_guard<std::mutex> lock(gateMutex); releaseSend=true;}
    gateCv.notify_all();
    REQUIRE(stopped.wait_for(2s)==std::future_status::ready);
    stopped.get();
    REQUIRE(!ui._ws.publish("after stop",WebWsTransport::Kind::State));
    onHttpd([&]{ui._ws.disconnected(43);}); // close callback before release
    ui._ws.release();
    failTask=true;
    REQUIRE(!ui._ws.start(reinterpret_cast<void *>(1)));
    failTask=false;
    REQUIRE(ui._ws.start(reinterpret_cast<void *>(1)));
    ui._ws.stop(); ui._ws.release();
    {std::lock_guard<std::mutex> lock(heapMutex); REQUIRE(payloadBytes==0); REQUIRE(payloadSizes.empty());}
    {std::lock_guard<std::mutex> lock(workMutex); stoppingHttpd=true;}
    workCv.notify_one(); httpd.join();
    std::puts("PASS: allocation/queue/task failures, short writes, stop/restart release resources");
}
