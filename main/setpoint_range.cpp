#include "setpoint_range.h"
#include "mode_caps.h"

#include <algorithm>
#include <cmath>

namespace SetpointRange {
namespace {
constexpr Range compatibilityRange{32, 61};
constexpr Range enhancedRange{20, 62};
constexpr Range legacyRange{32, 62};
constexpr uint8_t modeMasks[] = {
    MODE_CAP_HEAT, MODE_CAP_COOL | MODE_CAP_DRY, MODE_CAP_AUTO
};

uint8_t stepFor(Encoding encoding) {
    return encoding == Encoding::Legacy ? 2 : 1;
}

Range envelopeFor(Encoding encoding) {
    if (encoding == Encoding::Legacy) return legacyRange;
    if (encoding == Encoding::Enhanced) return enhancedRange;
    return compatibilityRange;
}

bool usable(Mode mode, Range range) {
    return range.min2 < range.max2 &&
           (mode != Mode::Auto || range.max2 - range.min2 >= 4);
}

// Automatic information is advisory; move its endpoints inward to the wire grid.
bool automaticRange(Mode mode, Range input, Encoding encoding, Range& output) {
    if (input.min2 >= input.max2) return false;
    const Range envelope = envelopeFor(encoding);
    const int step = stepFor(encoding);
    const int low = std::max<int>(input.min2, envelope.min2);
    const int high = std::min<int>(input.max2, envelope.max2);
    if (low > high) return false;
    output = {static_cast<int16_t>(((low + step - 1) / step) * step),
              static_cast<int16_t>((high / step) * step)};
    return usable(mode, output);
}
}  // namespace

Result validateManual(Mode mode, Range range, Encoding encoding) {
    // Unknown can check a saved edit's structure, but cannot authorize a SET.
    const Range envelope = encoding == Encoding::Legacy ? legacyRange : enhancedRange;
    const int step = stepFor(encoding);
    if (!usable(mode, range) || range.min2 < envelope.min2 ||
        range.max2 > envelope.max2 || range.min2 % step || range.max2 % step)
        return Result::Invalid;
    return Result::Ok;
}

Snapshot resolve(const Preferences& preferences, const Detection& detection,
                 Encoding current, Encoding lastKnown, uint8_t enabledModes) {
    Snapshot snapshot{};
    snapshot.encoding = current;
    const Encoding presentation = current == Encoding::Unknown ? lastKnown : current;
    for (int i = 0; i < 3; ++i) {
        const auto mode = static_cast<Mode>(i);
        const auto& preference = preferences.mode[i];
        auto& resolved = snapshot.mode[i];
        resolved.availability = current == Encoding::Unknown ?
            Availability::AwaitingBaseline : Availability::Ready;
        resolved.step2 = stepFor(presentation);
        if (preference.selection == Selection::Manual) {
            resolved.range = preference.manual;
            resolved.source = Source::Manual;
            // Preserve the saved presentation if the current wire grid cannot use it.
            if (validateManual(mode, preference.manual, presentation) != Result::Ok)
                resolved.step2 = 1;
            if (current != Encoding::Unknown &&
                validateManual(mode, preference.manual, current) != Result::Ok)
                resolved.availability = Availability::Incompatible;
        } else if (preference.selection == Selection::Compatibility) {
            resolved.range = compatibilityRange;
            resolved.step2 = 1;
            resolved.source = Source::Compatibility;
            resolved.availability = Availability::Ready;
        } else if ((detection.validMask & (1 << i)) &&
                   automaticRange(mode, detection.mode[i], presentation, resolved.range)) {
            resolved.source = detection.freshMask & (1 << i) ? Source::Fresh : Source::Cached;
        } else {
            automaticRange(mode, compatibilityRange, presentation, resolved.range);
            resolved.source = Source::Fallback;
        }
    }

    bool first = true;
    for (int i = 0; i < 3; ++i) {
        if (!(enabledModes & modeMasks[i])) continue;
        const auto& mode = snapshot.mode[i];
        if (first) {
            snapshot.advertised = mode.range;
            snapshot.advertisedStep2 = mode.step2;
            first = false;
        } else {
            snapshot.advertised.min2 = std::min(snapshot.advertised.min2, mode.range.min2);
            snapshot.advertised.max2 = std::max(snapshot.advertised.max2, mode.range.max2);
            snapshot.advertisedStep2 = std::min(snapshot.advertisedStep2, mode.step2);
        }
    }
    if (first) {
        snapshot.advertised = compatibilityRange;
        snapshot.advertisedStep2 = 1;
    }
    snapshot.ftabId = snapshot.advertised.min2 >= compatibilityRange.min2 &&
                      snapshot.advertised.max2 <= compatibilityRange.max2 ? 1 : 0;
    return snapshot;
}

Result validateTarget(float c, const ResolvedMode& mode) {
    if (!std::isfinite(c)) return Result::Invalid;
    if (mode.availability == Availability::AwaitingBaseline) return Result::AwaitingBaseline;
    if (mode.availability == Availability::Incompatible) return Result::Incompatible;
    if (c < mode.range.min2 / 2.0f || c > mode.range.max2 / 2.0f)
        return Result::OutsideRange;
    return Result::Ok;
}

float normalizeTarget(float c, const ResolvedMode& mode, Encoding encoding) {
    if (!std::isfinite(c) || !mode.step2 || mode.range.min2 > mode.range.max2)
        return NAN;
    const float clamped = std::clamp(c, mode.range.min2 / 2.0f, mode.range.max2 / 2.0f);
    if (mode.source == Source::Compatibility) {
        const float halfDegree = std::round(clamped * 2.0f) / 2.0f;
        return encoding == Encoding::Enhanced ? halfDegree : std::trunc(halfDegree);
    }
    const int step = mode.step2;
    const int low = ((mode.range.min2 + step - 1) / step) * step;
    const int high = (mode.range.max2 / step) * step;
    if (low > high) return NAN;
    const float rounded = std::round(clamped * 2.0f / step) * step;
    return std::clamp(rounded, static_cast<float>(low), static_cast<float>(high)) / 2.0f;
}

Band projectBand(Band band, const ResolvedMode& mode) {
    const int step = mode.step2;
    const int low = ((mode.range.min2 + step - 1) / step) * step;
    const int high = (mode.range.max2 / step) * step;
    const int gap = ((4 + step - 1) / step) * step;
    int heat = std::clamp<int>(band.heat2, low, high);
    int cool = std::clamp<int>(band.cool2, low, high);
    // Input is twice the desired half-degree coordinate, keeping midpoint ties exact.
    const auto nearestLower = [step](int twiceValue) {
        return ((twiceValue + step - 1) / (2 * step)) * step;
    };
    if (cool - heat < gap) {
        heat = nearestLower(heat + cool - gap);
        cool = heat + gap;
    } else {
        heat = nearestLower(heat * 2);
        cool = nearestLower(cool * 2);
    }
    if (heat < low) {
        cool += low - heat;
        heat = low;
    }
    if (cool > high) {
        heat -= cool - high;
        cool = high;
    }
    return {static_cast<int16_t>(heat), static_cast<int16_t>(cool)};
}
}  // namespace SetpointRange
