#include "setpoint_range.h"
#include "mode_caps.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

using namespace SetpointRange;
static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::fprintf(stderr, "%s:%d: %s\n", __func__, __LINE__, #cond); \
    ++failures; return; } } while (0)

static Preferences automatic() {
    return {{{Selection::Automatic, {}}, {Selection::Automatic, {}},
             {Selection::Automatic, {}}}};
}
static void bounds(Range actual, int min2, int max2) {
    if (actual.min2 != min2 || actual.max2 != max2) {
        std::fprintf(stderr, "bounds: expected %d..%d, got %d..%d\n",
                     min2, max2, actual.min2, actual.max2);
        ++failures;
    }
}

// Catches intersecting manual limits with advisory discovery.
static void manual_replaces_detection() {
    auto p = automatic();
    p.mode[0] = {Selection::Manual, {20, 62}};
    Detection d{{{32, 56}, {36, 58}, {34, 60}}, 7, 7};
    auto s = resolve(p, d, Encoding::Enhanced, Encoding::Unknown, MODE_CAP_ALL);
    bounds(s.mode[0].range, 20, 62);
    CHECK(s.mode[0].source == Source::Manual);
    CHECK(s.mode[0].availability == Availability::Ready);
    CHECK(s.mode[0].step2 == 1);
    p.mode[0].manual = {41, 42};
    s = resolve(p, d, Encoding::Enhanced, Encoding::Unknown, MODE_CAP_ALL);
    bounds(s.mode[0].range, 41, 42);
}

// Catches migrating untouched modes when just one mode opts in.
static void per_mode_compatibility() {
    auto p = automatic();
    p.mode[0].selection = Selection::Compatibility;
    Detection d{{{40, 50}, {36, 58}, {}}, 3, 1};
    auto s = resolve(p, d, Encoding::Legacy, Encoding::Unknown, MODE_CAP_ALL);
    bounds(s.mode[0].range, 32, 61);
    CHECK(s.mode[0].source == Source::Compatibility);
    CHECK(s.mode[0].step2 == 1);
    bounds(s.mode[1].range, 36, 58);
    CHECK(s.mode[1].source == Source::Cached);
    CHECK(s.mode[1].step2 == 2);
    bounds(s.mode[2].range, 32, 60);
    CHECK(s.mode[2].source == Source::Fallback);
    bounds(s.advertised, 32, 61);
    CHECK(s.advertisedStep2 == 1);
    CHECK(s.ftabId == 1);
    CHECK(s.generation == 0);
    CHECK(s.encoding == Encoding::Legacy);
}

// Catches rounding the legacy envelope outward or retaining invalid detection.
static void legacy_grid() {
    auto p = automatic();
    Detection d{{{21, 61}, {37, 59}, {40, 43}}, 7, 7};
    auto s = resolve(p, d, Encoding::Legacy, Encoding::Enhanced, MODE_CAP_ALL);
    bounds(s.mode[0].range, 32, 60);
    bounds(s.mode[1].range, 38, 58);
    bounds(s.mode[2].range, 32, 60);
    CHECK(s.mode[0].source == Source::Fresh);
    CHECK(s.mode[2].source == Source::Fallback);
    CHECK(s.mode[0].step2 == 2);
    CHECK(validateManual(Mode::Heat, {32, 62}, Encoding::Legacy) == Result::Ok);
    CHECK(validateManual(Mode::Heat, {33, 60}, Encoding::Legacy) == Result::Invalid);
    CHECK(validateManual(Mode::Heat, {32, 61}, Encoding::Legacy) == Result::Invalid);
    CHECK(validateManual(Mode::Auto, {40, 44}, Encoding::Legacy) == Result::Ok);
    d.mode[0] = {80, 90};
    d.mode[1] = {40, 40};
    s = resolve(p, d, Encoding::Enhanced, Encoding::Unknown, MODE_CAP_ALL);
    bounds(s.mode[0].range, 32, 61);
    bounds(s.mode[1].range, 32, 61);
    CHECK(s.mode[0].source == Source::Fallback);
    CHECK(s.mode[1].source == Source::Fallback);
}

// Catches accepting bounds outside firmware limits or collapsing Auto width.
static void invalid_manual() {
    CHECK(validateManual(Mode::Heat, {20, 62}, Encoding::Enhanced) == Result::Ok);
    CHECK(validateManual(Mode::Heat, {20, 62}, Encoding::Unknown) == Result::Ok);
    CHECK(validateManual(Mode::Heat, {20, 62}, Encoding::Legacy) == Result::Invalid);
    CHECK(validateManual(Mode::Auto, {40, 43}, Encoding::Enhanced) == Result::Invalid);
    CHECK(validateManual(Mode::Auto, {40, 44}, Encoding::Enhanced) == Result::Ok);
    CHECK(validateManual(Mode::Heat, {41, 42}, Encoding::Enhanced) == Result::Ok);
    CHECK(validateManual(Mode::CoolDry, {19, 62}, Encoding::Enhanced) == Result::Invalid);
    CHECK(validateManual(Mode::CoolDry, {20, 63}, Encoding::Enhanced) == Result::Invalid);
    CHECK(validateManual(Mode::Heat, {42, 42}, Encoding::Enhanced) == Result::Invalid);
    CHECK(validateManual(Mode::Heat, {44, 40}, Encoding::Enhanced) == Result::Invalid);
}

// Catches destroying saved preferences on reconnect or admitting speculative SETs.
static void unknown_or_incompatible_encoding() {
    auto p = automatic();
    p.mode[0] = {Selection::Manual, {20, 28}};
    auto s = resolve(p, {}, Encoding::Unknown, Encoding::Enhanced, MODE_CAP_ALL);
    bounds(s.mode[0].range, 20, 28);
    CHECK(s.mode[0].availability == Availability::AwaitingBaseline);
    CHECK(validateTarget(12, s.mode[0]) == Result::AwaitingBaseline);
    s = resolve(p, {}, Encoding::Legacy, Encoding::Enhanced, MODE_CAP_ALL);
    bounds(s.mode[0].range, 20, 28);
    CHECK(s.mode[0].availability == Availability::Incompatible);
    CHECK(validateTarget(12, s.mode[0]) == Result::Incompatible);
    CHECK(s.mode[0].source == Source::Manual);
    CHECK(p.mode[0].manual.min2 == 20 && p.mode[0].manual.max2 == 28);
    s = resolve(p, {}, Encoding::Enhanced, Encoding::Legacy, MODE_CAP_ALL);
    CHECK(s.mode[0].availability == Availability::Ready);
    Detection cached{{{}, {21, 61}, {}}, 2, 0};
    s = resolve(p, cached, Encoding::Unknown, Encoding::Legacy, MODE_CAP_ALL);
    bounds(s.mode[1].range, 32, 60);
    CHECK(s.mode[1].source == Source::Cached);
}

// Catches rounding before validation and replacing compatibility's old truncation.
static void target_quantization() {
    const ResolvedMode legacy{{32, 62}, 2, Source::Manual, Availability::Ready};
    CHECK(validateTarget(16.2f, legacy) == Result::Ok);
    CHECK(validateTarget(15.99f, legacy) == Result::OutsideRange);
    CHECK(validateTarget(31.01f, legacy) == Result::OutsideRange);
    CHECK(validateTarget(NAN, legacy) == Result::Invalid);
    CHECK(validateTarget(INFINITY, legacy) == Result::Invalid);
    CHECK(normalizeTarget(20.6f, legacy, Encoding::Legacy) == 21.0f);
    CHECK(normalizeTarget(16.1f, legacy, Encoding::Legacy) == 16.0f);
    CHECK(normalizeTarget(-100, legacy, Encoding::Legacy) == 16.0f);
    CHECK(normalizeTarget(100, legacy, Encoding::Legacy) == 31.0f);
    CHECK(normalizeTarget(std::numeric_limits<float>::max(), legacy, Encoding::Legacy) == 31.0f);
    CHECK(std::isnan(normalizeTarget(NAN, legacy, Encoding::Legacy)));
    CHECK(std::isnan(normalizeTarget(INFINITY, legacy, Encoding::Legacy)));
    const ResolvedMode compatibility{{32, 61}, 1, Source::Compatibility, Availability::Ready};
    CHECK(normalizeTarget(20.6f, compatibility, Encoding::Legacy) == 20.0f);
    CHECK(normalizeTarget(20.75f, compatibility, Encoding::Legacy) == 21.0f);
    CHECK(normalizeTarget(30.5f, compatibility, Encoding::Legacy) == 30.0f);
    CHECK(normalizeTarget(20.6f, compatibility, Encoding::Enhanced) == 20.5f);
    const ResolvedMode half{{41, 43}, 1, Source::Manual, Availability::Ready};
    CHECK(normalizeTarget(20.75f, half, Encoding::Enhanced) == 21.0f);
    CHECK(normalizeTarget(20.4f, half, Encoding::Enhanced) == 20.5f);
}

// Catches projecting outside bounds, losing the minimum gap, or moving midpoint ties up.
static void auto_projection() {
    const ResolvedMode auto10to31{{20, 62}, 1, Source::Manual, Availability::Ready};
    struct Example { Band input, want; };
    const Example enhanced[] = {
        {{60, 62}, {58, 62}}, {{0, 22}, {20, 24}},
        {{40, 42}, {39, 43}}, {{40, 41}, {38, 42}},
        {{40, 44}, {40, 44}}, {{0, 80}, {20, 62}},
        {{45, 41}, {41, 45}}, {{62, 62}, {58, 62}},
        {{20, 20}, {20, 24}},
    };
    for (const auto& example : enhanced) {
        const auto band = projectBand(example.input, auto10to31);
        CHECK(band.heat2 == example.want.heat2);
        CHECK(band.cool2 == example.want.cool2);
    }
    const ResolvedMode legacy{{32, 62}, 2, Source::Manual, Availability::Ready};
    const Example whole[] = {
        {{41, 43}, {40, 44}}, {{40, 42}, {38, 42}},
        {{61, 62}, {58, 62}}, {{32, 33}, {32, 36}},
        {{41, 48}, {40, 48}}, {{40, 47}, {40, 46}},
    };
    for (const auto& example : whole) {
        const auto band = projectBand(example.input, legacy);
        CHECK(band.heat2 == example.want.heat2);
        CHECK(band.cool2 == example.want.cool2);
    }
    const ResolvedMode halfEndpoints{{41, 45}, 1, Source::Manual, Availability::Ready};
    const auto narrow = projectBand({0, 80}, halfEndpoints);
    CHECK(narrow.heat2 == 41 && narrow.cool2 == 45);
}

// Catches disabled profiles expanding metadata or using native F-table outside its domain.
static void enabled_profile_presentation() {
    auto p = automatic();
    p.mode[0] = {Selection::Manual, {41, 42}};
    p.mode[1] = {Selection::Manual, {20, 62}};
    p.mode[2] = {Selection::Manual, {40, 44}};
    auto s = resolve(p, {}, Encoding::Enhanced, Encoding::Unknown, MODE_CAP_HEAT);
    bounds(s.advertised, 41, 42);
    CHECK(s.advertisedStep2 == 1);
    CHECK(s.ftabId == 1);
    s = resolve(p, {}, Encoding::Enhanced, Encoding::Unknown, MODE_CAP_DRY);
    bounds(s.advertised, 20, 62);
    CHECK(s.ftabId == 0);
    s = resolve(p, {}, Encoding::Enhanced, Encoding::Unknown, MODE_CAP_ALL);
    bounds(s.advertised, 20, 62);
    CHECK(s.ftabId == 0);
    p.mode[1].manual = {42, 62};
    s = resolve(p, {}, Encoding::Enhanced, Encoding::Unknown, MODE_CAP_COOL);
    CHECK(s.ftabId == 0);
    p.mode[1].manual = {20, 60};
    s = resolve(p, {}, Encoding::Enhanced, Encoding::Unknown, MODE_CAP_COOL);
    CHECK(s.ftabId == 0);
    s = resolve(p, {}, Encoding::Legacy, Encoding::Enhanced, MODE_CAP_FAN);
    bounds(s.advertised, 32, 61);
    CHECK(s.advertisedStep2 == 1);
    CHECK(s.ftabId == 1);
}

// Catches absent masks, collapsed intervals, or stale encoding bypassing the baseline gate.
static void detection_and_unknown_presentation() {
    auto p = automatic();
    Detection d{{{40, 41}, {20, 62}, {40, 43}}, 7, 7};
    auto s = resolve(p, d, Encoding::Legacy, Encoding::Unknown, MODE_CAP_ALL);
    bounds(s.mode[0].range, 32, 60);
    CHECK(s.mode[0].source == Source::Fallback);
    d.validMask = 0;
    s = resolve(p, d, Encoding::Enhanced, Encoding::Unknown, MODE_CAP_ALL);
    bounds(s.mode[0].range, 32, 61);
    CHECK(s.mode[0].source == Source::Fallback);
    s = resolve(p, {}, Encoding::Unknown, Encoding::Unknown, MODE_CAP_ALL);
    bounds(s.advertised, 32, 61);
    CHECK(s.mode[0].availability == Availability::AwaitingBaseline);
    s = resolve(p, {}, Encoding::Unknown, Encoding::Legacy, MODE_CAP_ALL);
    bounds(s.advertised, 32, 60);
    CHECK(s.mode[0].step2 == 2);
    CHECK(s.mode[0].availability == Availability::AwaitingBaseline);
    p.mode[0].selection = Selection::Compatibility;
    s = resolve(p, {}, Encoding::Unknown, Encoding::Legacy, MODE_CAP_HEAT);
    CHECK(s.mode[0].availability == Availability::Ready);
    CHECK(validateTarget(30.5f, s.mode[0]) == Result::Ok);
    CHECK(normalizeTarget(20.6f, s.mode[0], Encoding::Unknown) == 20.0f);
    p.mode[0] = {Selection::Manual, {41, 45}};
    s = resolve(p, {}, Encoding::Legacy, Encoding::Enhanced, MODE_CAP_HEAT);
    bounds(s.mode[0].range, 41, 45);
    CHECK(s.mode[0].availability == Availability::Incompatible);
    CHECK(s.advertisedStep2 == 1);
    s = resolve(p, {}, Encoding::Enhanced, Encoding::Legacy, MODE_CAP_HEAT);
    CHECK(s.mode[0].availability == Availability::Ready);
}

int main(int argc, char** argv) {
    struct Case { const char* name; void (*run)(); };
    const Case cases[] = {
        {"manual_replaces_detection", manual_replaces_detection},
        {"per_mode_compatibility", per_mode_compatibility},
        {"legacy_grid", legacy_grid}, {"invalid_manual", invalid_manual},
        {"unknown_or_incompatible_encoding", unknown_or_incompatible_encoding},
        {"target_quantization", target_quantization},
        {"auto_projection", auto_projection},
        {"enabled_profile_presentation", enabled_profile_presentation},
        {"detection_and_unknown_presentation", detection_and_unknown_presentation},
    };
    for (const auto& test : cases) {
        if (argc > 1 && std::strcmp(argv[1], test.name)) continue;
        int before = failures;
        test.run();
        std::printf("%s: %s\n", test.name, before == failures ? "PASS" : "FAIL");
    }
    return failures ? 1 : 0;
}
