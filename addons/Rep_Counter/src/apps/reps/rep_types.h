// Rep Counter: shared enums and small helpers.
//
// Pure C++ (no Arduino headers) so the detector, the session model and the UI
// renderer can all be compiled and unit-tested on the host.
#pragma once
#include <stdint.h>

namespace reps {

// What the user is doing. The detector counts with one of two motion
// "channels" (see Channel); the exercise is the label shown to the user.
enum class Exercise : uint8_t {
  None  = 0,
  Curl  = 1,   // elbow hinge: curls, hammer curls, triceps extensions
  Press = 2,   // vertical push with the forearm upright: overhead / bench press
  Raise = 3,   // straight-arm raise: lateral or front raise
  Row   = 4,   // vertical pull with the hand below the elbow: dumbbell row
};
constexpr int kExerciseCount = 5;

// Exercise selection on the watch. Auto lets the detector classify; the
// others pin both the label and the counting channel.
enum class Mode : uint8_t { Auto = 0, Curl = 1, Press = 2, Raise = 3, Row = 4 };
constexpr int kModeCount = 5;

enum class Wrist : uint8_t { Left = 0, Right = 1 };

// Counting channel. Rotation = the forearm swings through a big angle, so the
// gravity direction sweeps across the watch (curls, raises). Linear = the
// forearm keeps its orientation and the watch moves along it (press, row).
enum class Channel : uint8_t { None = 0, Rotation = 1, Linear = 2 };

inline Exercise modeExercise(Mode m) {
  switch (m) {
    case Mode::Curl:  return Exercise::Curl;
    case Mode::Press: return Exercise::Press;
    case Mode::Raise: return Exercise::Raise;
    case Mode::Row:   return Exercise::Row;
    default:          return Exercise::None;
  }
}

inline Channel exerciseChannel(Exercise e) {
  switch (e) {
    case Exercise::Curl:
    case Exercise::Raise: return Channel::Rotation;
    case Exercise::Press:
    case Exercise::Row:   return Channel::Linear;
    default:              return Channel::None;
  }
}

// "Curl", "Press", ... (title case, for lists).
inline const char *exerciseName(Exercise e) {
  switch (e) {
    case Exercise::Curl:  return "Curl";
    case Exercise::Press: return "Press";
    case Exercise::Raise: return "Raise";
    case Exercise::Row:   return "Row";
    default:              return "Lift";
  }
}

// "CURL", "PRESS", ... (caps, for the main screen label).
inline const char *exerciseCaps(Exercise e) {
  switch (e) {
    case Exercise::Curl:  return "CURL";
    case Exercise::Press: return "PRESS";
    case Exercise::Raise: return "RAISE";
    case Exercise::Row:   return "ROW";
    default:              return "LIFT";
  }
}

// Plural used in summaries: "12 curls", "1 press".
inline const char *exercisePlural(Exercise e, unsigned n) {
  bool one = (n == 1);
  switch (e) {
    case Exercise::Curl:  return one ? "curl"  : "curls";
    case Exercise::Press: return one ? "press" : "presses";
    case Exercise::Raise: return one ? "raise" : "raises";
    case Exercise::Row:   return one ? "row"   : "rows";
    default:              return one ? "rep"   : "reps";
  }
}

inline const char *modeName(Mode m) {
  switch (m) {
    case Mode::Auto:  return "Auto";
    case Mode::Curl:  return "Curl";
    case Mode::Press: return "Press";
    case Mode::Raise: return "Raise";
    case Mode::Row:   return "Row";
  }
  return "Auto";
}

inline const char *modeCaps(Mode m) {
  switch (m) {
    case Mode::Auto:  return "AUTO";
    case Mode::Curl:  return "CURL";
    case Mode::Press: return "PRESS";
    case Mode::Raise: return "RAISE";
    case Mode::Row:   return "ROW";
  }
  return "AUTO";
}

}  // namespace reps
