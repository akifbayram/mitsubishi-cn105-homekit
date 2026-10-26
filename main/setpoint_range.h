#pragma once

#include <cstdint>

namespace SetpointRange {
enum class Mode : uint8_t { Heat, CoolDry, Auto };
enum class Encoding : uint8_t { Unknown, Legacy, Enhanced };
enum class Selection : uint8_t { Compatibility, Automatic, Manual };
enum class Source : uint8_t { Compatibility, Fresh, Cached, Fallback, Manual };
enum class Availability : uint8_t { Ready, AwaitingBaseline, Incompatible };
enum class Result : uint8_t {
    Ok, Invalid, OutsideRange, AwaitingBaseline, Incompatible,
    Unavailable, StorageFailure
};
struct Range { int16_t min2, max2; };
struct Band { int16_t heat2, cool2; };
struct Preference { Selection selection; Range manual; };
struct Preferences { Preference mode[3]; };
struct Detection { Range mode[3]; uint8_t validMask, freshMask; };
struct ResolvedMode {
    Range range; uint8_t step2; Source source; Availability availability;
};
struct Snapshot {
    ResolvedMode mode[3]; Range advertised; uint8_t advertisedStep2, ftabId;
    Encoding encoding; uint32_t generation;
};
// Unknown validates against the enhanced envelope; command availability is separate.
Result validateManual(Mode mode, Range range, Encoding encoding);
// Detection masks use bit 0 Heat, bit 1 CoolDry, bit 2 Auto. enabledModes uses
// mode_caps.h masks. No state is mutated; the returned generation is always zero.
Snapshot resolve(const Preferences&, const Detection&, Encoding current,
                 Encoding lastKnown, uint8_t enabledModes);
// Validate the original finite command before calling normalizeTarget.
Result validateTarget(float c, const ResolvedMode&);
// Clamp and round once to a reachable target; nonfinite input returns NaN.
// Compatibility preserves the former half-degree rounding then legacy truncation.
float normalizeTarget(float c, const ResolvedMode&, Encoding);
// Requires a resolved Auto range with at least a 2 C gap and a nonzero step.
// Preserve the clamped midpoint when expanding; exact grid ties choose lower.
Band projectBand(Band band, const ResolvedMode&);
}  // namespace SetpointRange
