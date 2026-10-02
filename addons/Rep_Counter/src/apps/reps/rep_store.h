// Rep Counter: persistence in the app's own NVS namespace ("rep-counter").
// BaseOS's "ewatch" namespace is never touched. Each record is a versioned
// blob (see rep_session.h); anything unreadable falls back to defaults.
#pragma once
#include "rep_session.h"

namespace repstore {
static const char *const kNamespace = "rep-counter";
void load(reps::Settings &s, reps::DayLog &log, reps::History &hist);
void saveSettings(const reps::Settings &s);
void saveLog(const reps::DayLog &log);
void saveHist(const reps::History &hist);
bool loadLog(reps::DayLog &log);          // for the serial `log` command
}  // namespace repstore
