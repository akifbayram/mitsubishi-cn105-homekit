#include "setpoint_control.h"
#include "cn105_strings.h"

#include <cmath>
#include <algorithm>
#include <mutex>

namespace SetpointControl {
namespace {
using namespace SetpointRange;
std::mutex controlMutex;
CN105Controller* controller = nullptr;
Snapshot published{};
Band effectiveBand{};
CN105State resolvedState{};
uint32_t seenRevision = 0;
uint32_t seenExternal = 0;

int rangeIndex(uint8_t mode) {
    switch (mode) {
        case CN105_MODE_HEAT: return 0;
        case CN105_MODE_COOL: case CN105_MODE_DRY: return 1;
        case CN105_MODE_AUTO: return 2;
        default: return -1;
    }
}
bool samePreference(const Preference& a, const Preference& b) {
    return a.selection == b.selection && (a.selection != Selection::Manual ||
           (a.manual.min2 == b.manual.min2 && a.manual.max2 == b.manual.max2));
}
bool sameSnapshot(const Snapshot& a, const Snapshot& b) {
    if (a.encoding != b.encoding || a.ftabId != b.ftabId ||
        a.advertised.min2 != b.advertised.min2 || a.advertised.max2 != b.advertised.max2 ||
        a.advertisedStep2 != b.advertisedStep2) return false;
    for (int i = 0; i < 3; ++i) {
        const auto& x = a.mode[i]; const auto& y = b.mode[i];
        if (x.range.min2 != y.range.min2 || x.range.max2 != y.range.max2 ||
            x.step2 != y.step2 || x.source != y.source || x.availability != y.availability) return false;
    }
    return true;
}
Detection rangeReference() {
    auto detected = settings.detectedRanges();
    const auto caps = controller->rangeCapabilities().detected;
    detected.freshMask = caps.freshMask;
    for (int i = 0; i < 3; ++i) if (caps.freshMask & (1 << i)) {
        detected.mode[i] = caps.mode[i]; detected.validMask |= 1 << i;
    }
    return detected;
}
Snapshot resolveSettings(const SetpointSettings& cfg, const CN105State& state) {
    return resolve(cfg.preferences, rangeReference(), state.encoding,
                   cfg.lastEncoding, settings.get().modeMask);
}
void refresh() {
    resolvedState = controller->getState();
    auto next = resolveSettings(settings.setpointSettings(), resolvedState);
    next.generation = published.generation + (!sameSnapshot(next, published) ? 1 : 0);
    published = next;
    effectiveBand = projectBand(settings.setpointSettings().band, published.mode[2]);
}
CN105Command merged(CN105Command old, const CN105Command& next) {
    if (next.power && !*next.power) old = {};
    if (next.power) old.power = next.power;
    if (next.mode) { old.mode = next.mode; if (*next.mode == CN105_MODE_FAN) old.targetTemp.reset(); }
    if (next.targetTemp) old.targetTemp = next.targetTemp;
    if (next.fanSpeed) old.fanSpeed = next.fanSpeed;
    if (next.vane) old.vane = next.vane;
    if (next.wideVane) old.wideVane = next.wideVane;
    return old;
}
CN105State withIntent(CN105State state, const CN105Command& intent) {
    if (intent.power) state.power = *intent.power;
    if (intent.mode) state.mode = *intent.mode;
    if (intent.targetTemp) state.targetTemp = *intent.targetTemp;
    return state;
}
Result available(const ResolvedMode& mode) {
    if (mode.availability == Availability::AwaitingBaseline) return Result::AwaitingBaseline;
    if (mode.availability == Availability::Incompatible) return Result::Incompatible;
    return Result::Ok;
}
// Normalize retained intent only. Explicit user targets are checked first by submit.
Result normalizeCommand(CN105Command& cmd, const Snapshot& ranges, const CN105State& state) {
    const uint8_t mode = cmd.mode.value_or(state.mode);
    const int index = rangeIndex(mode);
    if (cmd.mode && !modeToCapBit(mode)) return Result::Invalid;
    if (cmd.mode && !mode_mask_allows(settings.get().modeMask, mode)) return Result::Unavailable;
    if (index < 0) return cmd.targetTemp ? Result::Invalid : Result::Ok;
    if (!cmd.targetTemp && !cmd.mode && !(cmd.power && *cmd.power)) return Result::Ok;
    const auto& range = ranges.mode[index];
    const auto result = available(range);
    if (result != Result::Ok) return result;
    cmd.targetTemp = normalizeTarget(cmd.targetTemp.value_or(state.targetTemp), range, ranges.encoding);
    return std::isfinite(*cmd.targetTemp) ? Result::Ok : Result::Invalid;
}
Status statusFor(const CN105State& state, const CN105State& effective) {
    Status out{Condition::Satisfied, state.targetTemp, effective.targetTemp,
               std::isfinite(effective.targetTemp) && effective.targetTemp * 2 >= published.advertised.min2 &&
               effective.targetTemp * 2 <= published.advertised.max2};
    if (!settings.setpointSettingsAvailable()) { out.condition = Condition::Incompatible; return out; }
    const int index = rangeIndex(state.mode);
    if (index < 0) return out;
    const auto& range = published.mode[index];
    if (range.availability == Availability::Incompatible) { out.condition = Condition::Incompatible; return out; }
    if (range.source != Source::Manual || !state.power) return out;
    if (!state.connected || state.encoding == Encoding::Unknown) { out.condition = Condition::Pending; return out; }
    if (validateTarget(state.targetTemp, range) == Result::Ok) return out;
    out.desiredC = normalizeTarget(state.targetTemp, range, published.encoding);
    out.condition = controller->setpointDelivery(out.desiredC) == SetpointDelivery::Refused ?
                    Condition::Refused : Condition::Pending;
    return out;
}
Status currentStatus() {
    return statusFor(controller->getState(), controller->getEffectiveState());
}
void enforce(bool explicitSave = false) {
    const auto state = controller->getState();
    if (!settings.setpointSettingsAvailable() || !state.connected ||
        state.encoding == Encoding::Unknown || !state.power) return;
    const int index = rangeIndex(state.mode);
    if (index < 0) return;
    const auto& range = published.mode[index];
    if (range.availability != Availability::Ready ||
        (!explicitSave && range.source != Source::Manual) ||
        validateTarget(state.targetTemp, range) == Result::Ok) return;
    const float desired = normalizeTarget(state.targetTemp, range, published.encoding);
    if (controller->setpointDelivery(desired) != SetpointDelivery::None) return;
    // A newer accepted mode/target takes precedence over an older SETTINGS report.
    const auto effective = controller->getEffectiveState();
    if (effective.mode != state.mode || effective.targetTemp != state.targetTemp || !effective.power) return;
    CN105Command correction; correction.targetTemp = desired;
    controller->holdCommands();
    controller->resumeCommands(merged(controller->heldCommand(), correction), resolvedState);
}
}

SettingsView settingsView() {
    std::lock_guard<std::mutex> lock(controlMutex);
    SettingsView view{};
    view.saved = settings.setpointSettings();
    if (!controller) {
        view.status = {Condition::Pending, NAN, NAN, false};
        return view;
    }
    resolvedState = controller->getState();
    view.reference = rangeReference();
    auto next = resolve(view.saved.preferences, view.reference, resolvedState.encoding,
                        view.saved.lastEncoding, settings.get().modeMask);
    next.generation = published.generation + (!sameSnapshot(next, published) ? 1 : 0);
    published = next;
    effectiveBand = projectBand(view.saved.band, published.mode[2]);
    view.ranges = published;
    view.effectiveBand = effectiveBand;
    view.status = statusFor(resolvedState, controller->getEffectiveState());
    return view;
}

void begin(CN105Controller& cn105) {
    std::lock_guard<std::mutex> lock(controlMutex);
    controller = &cn105; published = {}; seenRevision = 0;
    seenExternal = cn105.externalAutoChanges(); refresh();
}
Snapshot snapshot() {
    std::lock_guard<std::mutex> lock(controlMutex);
    if (controller) refresh();
    return published;
}
Status status() {
    std::lock_guard<std::mutex> lock(controlMutex);
    if (!controller) return {Condition::Pending, NAN, NAN, false};
    refresh(); return currentStatus();
}

Result submit(const Command& input) {
    std::lock_guard<std::mutex> lock(controlMutex);
    if (!controller) return Result::Unavailable;
    auto cmd = input.unit;
    const bool bandEdit = input.heatingThreshold || input.coolingThreshold;
    const bool temperatureAction = cmd.targetTemp || cmd.mode || (cmd.power && *cmd.power) || bandEdit;
    if (SettingsStore::isFactoryResetting()) return Result::Unavailable;
    if (temperatureAction && !settings.setpointSettingsAvailable()) return Result::Unavailable;
    refresh();
    const auto state = withIntent(controller->getEffectiveState(), controller->heldCommand());
    const uint8_t mode = cmd.mode.value_or(state.mode);
    const int index = rangeIndex(mode);
    if (cmd.targetTemp) {
        if (index < 0) return Result::Invalid;
        const auto result = validateTarget(*cmd.targetTemp, published.mode[index]);
        if (result != Result::Ok) return result;
    }
    auto next = settings.setpointSettings();
    if (bandEdit) {
        if (settings.autoSingleBoot() || !(settings.get().modeMask & MODE_CAP_AUTO)) return Result::Unavailable;
        const auto& range = published.mode[2];
        for (auto value : {input.heatingThreshold, input.coolingThreshold}) if (value) {
            const auto result = validateTarget(*value, range);
            if (result != Result::Ok) return result;
        }
        Band band{static_cast<int16_t>(std::lround(input.heatingThreshold.value_or(effectiveBand.heat2 / 2.f) * 2)),
                  static_cast<int16_t>(std::lround(input.coolingThreshold.value_or(effectiveBand.cool2 / 2.f) * 2))};
        next.band = projectBand(band, range);
    }
    const auto result = normalizeCommand(cmd, published, state);
    if (result != Result::Ok) return result;
    const bool claim = !settings.autoSingleBoot() && (bandEdit || (cmd.mode && *cmd.mode == CN105_MODE_AUTO));
    if (claim) next.autoOwnerUnit = false;
    controller->holdCommands();
    const auto previous = controller->heldCommand();
    const auto validation = resolvedState;
    auto complete = merged(previous, cmd);
    const auto completeResult = normalizeCommand(complete, published, state);
    if (completeResult != Result::Ok) return completeResult;
    if ((bandEdit || claim) && !settings.saveSetpointSettings(next)) {
        if (settings.setpointSettingsAvailable()) controller->resumeCommands(previous, validation);
        return Result::StorageFailure;
    }
    // A disconnect during persistence leaves accepted intent pending revalidation.
    controller->resumeCommands(complete, validation);
    if (claim) seenExternal = controller->externalAutoChanges();
    return Result::Ok;
}

Result applySettings(const SetpointSettings& input) {
    std::lock_guard<std::mutex> lock(controlMutex);
    if (!controller || SettingsStore::isFactoryResetting()) return Result::Unavailable;
    const auto old = settings.setpointSettings();
    auto next = input;
    // Single keeps the stored band and owner for a switch back to Range,
    // whatever an imported file or a stale client carries.
    const bool single = settings.autoSingleBoot();
    if (single) { next.band = old.band; next.autoOwnerUnit = old.autoOwnerUnit; }
    if (next.lastEncoding > Encoding::Enhanced) return Result::Invalid;
    const auto state = controller->getState();
    // Only confirmed equipment history can authorize an offline edit.
    const auto editEncoding = state.encoding == Encoding::Unknown ? old.lastEncoding : state.encoding;
    bool changed = false;
    for (int i = 0; i < 3; ++i) {
        const auto& p = next.preferences.mode[i];
        const bool edited = !samePreference(p, old.preferences.mode[i]);
        changed |= edited;
        if (p.selection > Selection::Manual) return Result::Invalid;
        if (p.selection == Selection::Manual &&
            validateManual(static_cast<Mode>(i), p.manual, edited ? editEncoding : Encoding::Unknown) != Result::Ok)
            return Result::Invalid;
    }
    auto ranges = resolveSettings(next, state);
    const bool autoEdited = !samePreference(next.preferences.mode[2], old.preferences.mode[2]) ||
                            next.band.heat2 != old.band.heat2 || next.band.cool2 != old.band.cool2;
    if (autoEdited && !single && (ranges.mode[2].availability == Availability::Ready ||
                                  ranges.mode[2].availability == Availability::AwaitingBaseline))
        next.band = projectBand(next.band, ranges.mode[2]);
    controller->holdCommands();
    if (!settings.saveSetpointSettings(next)) {
        // Resume the old intent only if storage still has a trustworthy policy.
        if (settings.setpointSettingsAvailable())
            controller->resumeCommands(controller->heldCommand(), state);
        return Result::StorageFailure;
    }
    refresh();
    auto held = controller->heldCommand();
    const auto effective = withIntent(controller->getEffectiveState(), held);
    const int activeIndex = rangeIndex(effective.mode);
    if (changed && activeIndex >= 0 && effective.power &&
        published.mode[activeIndex].availability == Availability::Ready &&
        validateTarget(effective.targetTemp, published.mode[activeIndex]) != Result::Ok)
        held.targetTemp = effective.targetTemp;
    const auto normalized = normalizeCommand(held, published, controller->getState());
    if (normalized == Result::Ok) controller->resumeCommands(held, resolvedState);
    if (changed) controller->clearSetpointRefusal();
    enforce(true);
    return Result::Ok;
}

namespace {
bool importAutoTargetLocked(float target) {
    if (!controller || !settings.setpointSettingsAvailable()) return false;
    refresh();
    if (currentStatus().condition != Condition::Satisfied || !std::isfinite(target)) return false;
    auto next = settings.setpointSettings();
    next.band = projectBand({static_cast<int16_t>(std::lround(target * 2) - 2),
                             static_cast<int16_t>(std::lround(target * 2) + 2)}, published.mode[2]);
    next.autoOwnerUnit = true;
    return settings.saveSetpointSettings(next);
}
}
bool autoDeviceOwns() {
    std::lock_guard<std::mutex> lock(controlMutex);
    // Suspend steering as soon as a new external observation awaits handover.
    return !settings.autoSingleBoot() && controller && !settings.setpointSettings().autoOwnerUnit &&
           controller->externalAutoChanges() == seenExternal;
}
void steerAuto() {
    std::lock_guard<std::mutex> lock(controlMutex);
    if (settings.autoSingleBoot() ||
        !controller || SettingsStore::isFactoryResetting() || !settings.setpointSettingsAvailable() ||
        !controller->isHealthy() || !(settings.get().modeMask & MODE_CAP_AUTO)) return;
    const auto cfg = settings.setpointSettings();
    const auto external = controller->externalAutoChanges();
    if (cfg.autoOwnerUnit || external != seenExternal) return;
    refresh();
    const auto state = withIntent(controller->getEffectiveState(), controller->heldCommand());
    if (!state.power || state.mode != CN105_MODE_AUTO ||
        published.mode[2].availability != Availability::Ready || !std::isfinite(state.roomTemp)) return;

    // Below/above the band, steer to that boundary. Inside it, follow room
    // temperature with the existing coarse deadband so the unit can idle.
    const float heat = effectiveBand.heat2 / 2.f, cool = effectiveBand.cool2 / 2.f;
    const float desired = std::clamp(state.roomTemp, heat, cool);
    const float deadband = state.roomTemp < heat || state.roomTemp > cool ? 0.25f : 1.f;
    const float target = normalizeTarget(desired, published.mode[2], published.encoding);
    if (validateTarget(target, published.mode[2]) != Result::Ok ||
        fabsf(target - state.targetTemp) <= deadband ||
        controller->setpointDelivery(target) != SetpointDelivery::None) return;

    // User commands and band edits cannot interleave under controlMutex. Unit
    // observations use the driver lock, so check their revision again at staging.
    controller->queueAutoTarget(target, resolvedState, external);
}
void updateAutoOwnership() {
    std::lock_guard<std::mutex> lock(controlMutex);
    if (!controller || settings.autoSingleBoot()) return;
    const auto external = controller->externalAutoChanges();
    if (external == seenExternal) return;
    const auto actual = controller->getState();
    if (!actual.connected || actual.encoding == Encoding::Unknown) return;
    if (!actual.power || actual.mode != CN105_MODE_AUTO || importAutoTargetLocked(actual.targetTemp))
        seenExternal = external;
}

namespace {
// Single imports nothing, so a restart into Range would otherwise steer from
// a stale band. The unit keeps Auto until someone edits the range here.
bool handAutoToUnitLocked() {
    const auto actual = controller->getState();
    if (actual.connected && actual.encoding != Encoding::Unknown && actual.power &&
        actual.mode == CN105_MODE_AUTO && importAutoTargetLocked(actual.targetTemp)) return true;
    auto next = settings.setpointSettings();
    next.autoOwnerUnit = true;
    return settings.saveSetpointSettings(next);
}
}
Result saveAutoSingle(bool single) {
    std::lock_guard<std::mutex> lock(controlMutex);
    if (!controller || SettingsStore::isFactoryResetting()) return Result::Unavailable;
    if (settings.get().autoSingleSetpoint == single) return Result::Ok;
    if (!single && settings.autoSingleBoot()) {
        if (!settings.setpointSettingsAvailable()) return Result::Unavailable;
        if (!handAutoToUnitLocked()) return Result::StorageFailure;
    }
    settings.get().autoSingleSetpoint = single;
    settings.save();
    return Result::Ok;
}

void loop() {
    std::lock_guard<std::mutex> lock(controlMutex);
    if (!controller) return;
    const auto caps = controller->rangeCapabilities().detected;
    auto cache = settings.detectedRanges();
    for (int i = 0; i < 3; ++i) if (caps.freshMask & (1 << i)) {
        cache.mode[i] = caps.mode[i]; cache.validMask |= 1 << i;
    }
    cache.freshMask = 0;
    settings.saveDetectedRanges(cache);
    refresh();
    if (!settings.setpointSettingsAvailable()) {
        controller->holdCommands();
        const auto held = controller->heldCommand();
        // Even an unavailable policy cannot trap an accepted power-off behind
        // temperature intent. Other commands wait for deliberate storage repair.
        if (held.power && !*held.power && !held.mode && !held.targetTemp)
            controller->resumeCommands(held, resolvedState);
        return;
    }
    const auto state = controller->getState();
    if (!state.connected || state.encoding == Encoding::Unknown) return;
    auto cfg = settings.setpointSettings();
    if (cfg.lastEncoding != state.encoding) {
        cfg.lastEncoding = state.encoding;
        if (!settings.saveSetpointSettings(cfg) && !settings.setpointSettingsAvailable()) {
            controller->holdCommands(); return;
        }
        refresh();
    }
    if (controller->commandsHeld()) {
        auto held = controller->heldCommand();
        if (normalizeCommand(held, published, state) == Result::Ok)
            controller->resumeCommands(held, resolvedState);
    }
    if (state.settingsRevision != seenRevision) {
        seenRevision = state.settingsRevision;
        enforce();
    }
}
}  // namespace SetpointControl
