#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>
#define BLE_ENABLE 1
#define REQUIRE(test) do {if (!(test)) {std::fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#test); std::exit(1);}} while (0)
uint32_t uptime_ms() {return 1500;}
size_t logging_drain(char *,size_t) {return 0;}
namespace BleSensor {
bool complete=true, updated=false, discovering=false;
int starts=0;
bool isDiscovering() {return discovering;}
void startDiscovery() {++starts; complete=false; updated=false; discovering=true;}
bool pollDiscoveryComplete() {bool result=complete; complete=false; return result;}
bool pollDiscoveryUpdate() {bool result=updated; updated=false; return result;}
}
class WebUI {
public:
    void loop();
    void pushState() {}
    void broadcastLog(const char *,size_t) {}
    bool pushDiscoveryResults(bool done) {
        attempts.push_back(done);
        return admit;
    }
    void *_server=reinterpret_cast<void *>(1);
    uint32_t _lastStatePush=1000;
    std::atomic<bool> _discoveryRequested{false};
    std::atomic<bool> _discoveryResync{false};
    bool _discoveryPushPending=false, _discoveryDonePending=false;
    bool admit=false;
    std::vector<bool> attempts;
};
#include "loop.inc"
int main() {
    WebUI ui;
    ui.loop(); // completion consumed, mailbox admission refused
    ui.admit=true;
    ui.loop(); // must retry the one-shot result
    REQUIRE(ui.attempts.size()==2);
    REQUIRE(ui.attempts[0] && ui.attempts[1]);
    ui.loop();
    REQUIRE(ui.attempts.size()==2);
    ui.admit=false;
    BleSensor::complete=true;
    ui.loop(); // old completion pending
    ui._discoveryRequested.store(true);
    ui.admit=true;
    ui.loop();
    REQUIRE(BleSensor::starts==1);
    REQUIRE(!ui.attempts.back()); // new scan must not inherit old done=true
    BleSensor::complete=true;
    ui.loop();
    REQUIRE(ui.attempts.back());
    // A browser that connects mid-scan (a reload during a long session) gets
    // the list found so far, without restarting the session.
    BleSensor::discovering=false;
    ui._discoveryRequested.store(true);
    ui.loop();                     // a new session opens
    REQUIRE(BleSensor::starts==2);
    const size_t before=ui.attempts.size();
    ui._discoveryResync.store(true);
    ui.loop();
    REQUIRE(BleSensor::starts==2);
    REQUIRE(ui.attempts.size()==before+1);
    REQUIRE(!ui.attempts.back());
    // A connect with no scan open sends nothing.
    BleSensor::discovering=false;
    ui._discoveryResync.store(true);
    ui.loop();
    REQUIRE(ui.attempts.size()==before+1);
    std::puts("PASS: production loop retries rejected scan completion and resets it on a new scan; late joiners get the current list");
}
