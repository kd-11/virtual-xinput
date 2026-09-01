#include "detect.h"

namespace vx {

const float kMoveThreshold    = 0.45f;
const float kReleaseThreshold = 0.20f;
const int   kRestSamples      = 25;

bool StateIsNeutral(const RawState& cur, const RawState& base, const DeviceCaps& caps) {
    for (int i = 0; i < caps.buttonCount; ++i)
        if (cur.button[i] != base.button[i]) return false;
    for (int i = 0; i < caps.povCount; ++i)
        if (cur.pov[i] != base.pov[i]) return false;
    for (int i = 0; i < kAxisCount; ++i) {
        if (!caps.axisPresent[i]) continue;
        float d = cur.axis[i] - base.axis[i];
        if (d < 0) d = -d;
        if (d > kReleaseThreshold) return false;
    }
    return true;
}

AxisMapping AxisFromDetection(const Detection& d) {
    AxisMapping m;
    if (d.kind == Detection::Button) {
        m.kind   = AxisMapping::Kind::Button;
        m.button = d.button;
        return m;
    }
    if (d.kind != Detection::Axis) return m;

    m.kind   = AxisMapping::Kind::Axis;
    m.axis   = (DiAxis)d.axisIndex;
    m.invert = (d.value < d.baseline);
    return m;
}

AxisMapping TriggerFromDetection(const Detection& d) {
    AxisMapping m;
    if (d.kind == Detection::Button) {
        m.kind   = AxisMapping::Kind::Button;
        m.button = d.button;
        return m;
    }
    if (d.kind != Detection::Axis) return m;

    m.kind = AxisMapping::Kind::Axis;
    m.axis = (DiAxis)d.axisIndex;

    bool restsAtEnd = (d.baseline < -0.6f) || (d.baseline > 0.6f);
    if (restsAtEnd) {
        m.half   = AxisHalf::Full;
        m.invert = (d.baseline > 0.6f);   // rests high, so the travel is downward
    } else {
        // Rests near centre: this trigger owns one half of the axis.
        m.half   = (d.value > d.baseline) ? AxisHalf::Positive : AxisHalf::Negative;
        m.invert = false;
    }
    return m;
}

ButtonMapping ButtonFromDetection(const Detection& d) {
    ButtonMapping m;
    if (d.kind == Detection::Button) {
        m.kind   = ButtonMapping::Kind::Button;
        m.button = d.button;
    } else if (d.kind == Detection::Pov) {
        m.kind    = ButtonMapping::Kind::Pov;
        m.pov     = d.pov;
        m.povMask = PovToMask(d.povValue);
    } else if (d.kind == Detection::Axis) {
        m.kind          = ButtonMapping::Kind::Axis;
        m.axis          = (DiAxis)d.axisIndex;
        m.axisPositive  = (d.value > d.baseline);
        m.axisThreshold = 0.5f;
    }
    return m;
}

// ---------------------------------------------------------------------------
// InputDetector
// ---------------------------------------------------------------------------

void InputDetector::Begin(const DeviceCaps& caps) {
    caps_    = caps;
    base_    = RawState();
    result_  = Detection();
    samples_ = 0;
    phase_   = Phase::Sampling;
}

void InputDetector::Cancel() {
    phase_   = Phase::Idle;
    result_  = Detection();
    samples_ = 0;
}

float InputDetector::SampleProgress() const {
    if (phase_ != Phase::Sampling) return 1.0f;
    return (float)samples_ / (float)kRestSamples;
}

void InputDetector::Feed(const RawState& cur, bool polled) {
    if (!polled || phase_ == Phase::Idle || phase_ == Phase::Done) return;

    switch (phase_) {
    case Phase::Sampling:
        // The most recent good sample is the rest position. Repeated reads
        // rather than one give the device time to settle after acquisition.
        base_ = cur;
        if (++samples_ >= kRestSamples) phase_ = Phase::Waiting;
        return;

    case Phase::Waiting: {
        Detection found;

        for (int i = 0; i < caps_.buttonCount && !found.IsSet(); ++i) {
            if (cur.button[i] && !base_.button[i]) {
                found.kind   = Detection::Button;
                found.button = i;
            }
        }

        for (int i = 0; i < caps_.povCount && !found.IsSet(); ++i) {
            if (cur.pov[i] >= 0 && base_.pov[i] < 0) {
                found.kind     = Detection::Pov;
                found.pov      = i;
                found.povValue = cur.pov[i];
            }
        }

        if (!found.IsSet()) {
            // Largest deflection wins, so a stick that also nudges a
            // neighbouring axis still resolves to the axis actually pushed.
            float best = kMoveThreshold;
            for (int i = 0; i < kAxisCount; ++i) {
                if (!caps_.axisPresent[i]) continue;
                float d   = cur.axis[i] - base_.axis[i];
                float mag = d < 0 ? -d : d;
                if (mag > best) {
                    best            = mag;
                    found.kind      = Detection::Axis;
                    found.axisIndex = i;
                    found.baseline  = base_.axis[i];
                    found.value     = cur.axis[i];
                }
            }
        }

        if (found.IsSet()) {
            result_ = found;
            phase_  = Phase::Releasing;
        }
        return;
    }

    case Phase::Releasing:
        if (StateIsNeutral(cur, base_, caps_)) phase_ = Phase::Done;
        return;

    default:
        return;
    }
}

} // namespace vx
