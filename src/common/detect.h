#pragma once

#include "config.h"
#include "mapping.h"

// Working out which physical control someone just moved, and what mapping that
// implies.
//
// This lives in vx_common rather than in a configurator because both the
// console wizard and the windowed one need it and it must not drift between
// them. The trigger inference in particular is not something a user can be
// asked to explain to the app, and getting it wrong produces the
// "triggers half-pressed at rest" bug that is hard to attribute.
namespace vx {

// An axis must move by at least this much from rest before it counts as a
// deliberate movement rather than noise or drift.
extern const float kMoveThreshold;
// Everything must fall back inside this band before another capture is taken.
extern const float kReleaseThreshold;

// How many polls are averaged into the resting position. At the console's 10ms
// sleep this is a quarter second; the GUI feeds it one sample per frame.
extern const int kRestSamples;

struct Detection {
    enum Kind { None, Axis, Button, Pov } kind;
    int   axisIndex;
    float baseline;      // where the axis sat before it moved
    float value;         // where it moved to
    int   button;
    int   pov;
    int   povValue;

    Detection()
        : kind(None), axisIndex(-1), baseline(0), value(0),
          button(-1), pov(-1), povValue(-1) {}

    bool IsSet() const { return kind != None; }
};

// True when nothing is displaced from the recorded rest state.
bool StateIsNeutral(const RawState& cur, const RawState& base, const DeviceCaps& caps);

// Turns a detected movement into a mapping. XInput's positive direction is
// right and up, so an axis that moved negative gets inverted.
AxisMapping AxisFromDetection(const Detection& d);

// Triggers need more care than sticks: the same physical trigger may be a full
// axis resting at one end, or half of an axis that rests centred and is shared
// with the other trigger. Which one it is can be told from where the axis sat
// before it was pressed, which is why the rest position is sampled first.
AxisMapping TriggerFromDetection(const Detection& d);

ButtonMapping ButtonFromDetection(const Detection& d);

// Frame-driven capture of a single control.
//
// Feed it one poll at a time and ask what phase it is in; it never blocks and
// never sleeps, so a console loop and a render loop can both drive it.
//
//   Begin()      -> Sampling   recording the rest position
//                -> Waiting    watching for something to move
//                -> Releasing  something moved; waiting for it to be let go
//                -> Done       result is final
//
// The release phase exists so that letting go of a control is not read as the
// answer to the next question.
class InputDetector {
public:
    enum class Phase { Idle, Sampling, Waiting, Releasing, Done };

    InputDetector() : phase_(Phase::Idle), samples_(0) {}

    void Begin(const DeviceCaps& caps);
    void Cancel();

    // One poll. Pass polled=false for a tick where no fresh sample was
    // available, so a device that is briefly unreadable does not corrupt the
    // rest position with a stale or zeroed state.
    void Feed(const RawState& cur, bool polled);

    Phase CurrentPhase() const { return phase_; }
    bool  IsActive()     const { return phase_ != Phase::Idle && phase_ != Phase::Done; }
    bool  IsDone()       const { return phase_ == Phase::Done; }

    // Valid from the moment the phase reaches Releasing.
    const Detection& Result() const { return result_; }

    // Rest-sampling progress, 0..1, for a UI that wants to show it.
    float SampleProgress() const;

private:
    Phase      phase_;
    DeviceCaps caps_;
    RawState   base_;
    Detection  result_;
    int        samples_;
};

} // namespace vx
