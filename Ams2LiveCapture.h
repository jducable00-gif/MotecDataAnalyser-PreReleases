//---------------------------------------------------------------------------
// Ams2LiveCapture.h — AMS2 live capture from shared memory.
//
// Provides functions to open/read/close the AMS2 shared memory object
// ($pcars2$) and to feed a single frame into the LapStats accumulators.
//---------------------------------------------------------------------------

#pragma once

#include <string>
#include "Ams2SharedMemory.h"
#include "MotecSetupAnalyzer.h"

// Open the AMS2 shared memory object and map it into our address space.
// Returns true on success; on failure, errorMessage contains a description.
bool Ams2OpenSharedMemory(std::string& errorMessage);

// Outcome of an attempt to read one frame from shared memory (see plan §4).
enum class FrameReadStatus { NoNewData, TornRetry, NewFrame };

// Read a single consistent frame from shared memory and detect whether it is
// genuinely new since the last processed tick. Uses the mSequenceNumber
// double-check pattern to avoid torn reads AND change-detection on the same
// token so unchanged polls are skipped (no duplicate/stale work).
//   NewFrame  -> `out` holds a fresh, consistent frame; safe to process once.
//   TornRetry -> write in progress / copy was torn; retry next tick.
//   NoNewData -> nothing new since the last processed frame; do not re-process.
FrameReadStatus Ams2ReadFrame(Ams2SharedMemory& out);

// Reset change-detection state so the first real frame after (re)connect or a
// session reset is treated as NewFrame again. Call on connect and on any path
// that resets the live session.
void Ams2ResetFrameTracking();

// Unmap and close the shared memory handle.
void Ams2CloseSharedMemory();

// Feed one shared-memory frame into the LapStats accumulators.
// Per-channel unit conversions come from mapping.json (AMS2 LIVE rows) via a
// resolved channel table; this function only extracts raw values and feeds
// them through LiveChannel::FeedLapFromTable().
// timeSeconds is the wall-clock time (GetTickCount64()/1000.0) used for
// damper velocity derivation.
class SessionRecorder;   // optional high-frequency sample buffer for LD export (SessionRecorder.h)
void Ams2FeedLap(LapStats& lap, const Ams2SharedMemory& frame, double timeSeconds, SessionRecorder* rec = nullptr);

// Returns a human-readable warning string if any AMS2 LIVE mapping rows failed to
// resolve (empty table or aliases without a LapStats sink). Empty when healthy.
std::string Ams2GetChannelTableWarning();

// Returns true when the car is on track (not in pits, game is playing).
bool Ams2IsOnTrack(const Ams2SharedMemory& frame);

// Returns true when the car is in the pit lane or garage.
bool Ams2IsInPits(const Ams2SharedMemory& frame);

// Returns true if the AMS2 process (AMS2.exe) is currently running.
bool Ams2IsProcessRunning();
