//---------------------------------------------------------------------------
// RaceRoomLiveCapture.h — RaceRoom (R3E) live capture from shared memory.
//
// Mirrors the Ams2LiveCapture API surface 1:1 so MotecSetupForm can treat both
// profiles identically in its connect / close dispatch and capture thread.
// Shared-memory layout is defined by r3e.h ("$R3E", v3.x, kwstudios-sweden/r3e-api).
//---------------------------------------------------------------------------

#ifndef RaceRoomLiveCaptureH
#define RaceRoomLiveCaptureH

#include <string>
#include "r3e.h"            // packed shared-memory layout v3.x ("$R3E"); unit factors live in mapping.json rows, not here
#include "MotecSetupAnalyzer.h"   // LapStats, ChannelProfile, RideHeightUnit
#include "Ams2LiveCapture.h"      // FrameReadStatus enum

// Open the "$R3E" shared memory block read-only and validate version_major == 3.
// A minor-version mismatch is tolerated (layout grows forward) but recorded in a
// warning retrievable via R3eGetChannelTableWarning(). Returns false only when
// the map does not exist or cannot be mapped, with errorMessage set.
bool R3eOpenSharedMemory(std::string& err);

// Read one frame into out using torn-read protection: player.game_simulation_ticks
// is sampled before and after a full struct copy; mismatch -> TornRetry (retry next tick).
FrameReadStatus R3eReadFrame(r3e_shared& out);

void R3eResetFrameTracking();   // reset the "last seen ticks" baseline (call on connect)
void R3eCloseSharedMemory();    // idempotent UnmapViewOfFile/CloseHandle; safe when not open

// Feed one frame into LapStats accumulators: builds a raw channel dictionary and lets
// LiveChannel::FeedLapFromTable() apply mapping.json factor/offset per row, then adds the
// post-table derived samples (damper travel stats, handling sample, steer/yaw correlation).
void R3eFeedLap(LapStats& lap, const r3e_shared& frame, double timeSeconds);

std::string R3eGetChannelTableWarning();   // one-time table-validation report; empty when clean

bool R3eIsOnTrack(const r3e_shared& frame);
bool R3eIsInPits(const r3e_shared& frame);
bool R3eIsProcessRunning();                // "RRRE" in the process list (wcsstr, like AMS2)

#endif   // RaceRoomLiveCaptureH
