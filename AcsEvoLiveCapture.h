//---------------------------------------------------------------------------
// AcsEvoLiveCapture.h — AC EVO live capture from shared memory.
//
// AC EVO exposes three shared memory objects (session-scoped, "Local\" prefix):
//   Local\acevo_pmf_physics  -> SPageFilePhysics
//   Local\acevo_pmf_graphics -> SPageFileGraphicEvo
//   Local\acevo_pmf_static   -> SPageFileStaticEvo
//
// Provides functions to open/read/close those objects and to feed a single
// frame (physics + graphics) into the LapStats accumulators.
//---------------------------------------------------------------------------

#pragma once

#include <string>
#include "AcsEvoSharedMemory.h"
#include "MotecSetupAnalyzer.h"

// Open the three AC EVO shared memory objects and map them into our address
// space. Returns true on success; on failure, errorMessage contains a
// description.
bool AcsEvoOpenSharedMemory(std::string& errorMessage);

// Read a single consistent physics frame.
// Frame-sync read status for the AC EVO combined reader. Mirrors AMS2's
// FrameReadStatus so LiveTimerTimer can apply one uniform "process only on a
// genuinely new frame" policy across both sims (plan §5).
enum class EvoReadStatus { NoNewData, TornRetry, NewFrame };

// Combined physics + graphics read with per-page change-detection. Each page is
// guarded by its own packetId double-check AND tracked against the last-processed
// token for that page: a page whose packetId has not advanced since we last used it
// contributes no new data this tick (it may still be mid-write -> TornRetry). The
// frame as a whole counts as NewFrame only when at least one page produced fresh,
// consistent data. This prevents duplicate/stale processing and torn reads while the
// 16 ms poll runs slower than AC EVO's ~90 Hz write rate (plan §2/§5).
// physUsableOut: set to false ONLY when a fresh physics page failed the plausibility cross-check — its values must not be fed into lap stats, but gfx-based detection still runs on this tick. Pass NULL if unused.
EvoReadStatus AcsEvoReadFrame(SPageFilePhysics& physOut, SPageFileGraphicEvo& gfxOut, bool* physUsableOut);

// Reset per-page last-processed packet tracking AND the plausibility snapshot/counter.
// Call on connect and after a session reset so the first real frame from each page
// counts as NewFrame again (plan §5).
void AcsEvoResetFrameTracking();

// Optional inter-cycle plausibility cross-check (plan §5 step 5). Enabled by default; toggle at
// runtime if false-positives appear on exotic content. When enabled, a grossly implausible jump in
// key physics channels vs the last accepted frame is treated as suspected torn data and skipped.
void AcsEvoSetPlausibilityCheck(bool enabled);
bool   AcsEvoGetPlausibilityCheck();

// Diagnostic: number of frames rejected by the plausibility cross-check since connect/reset.
unsigned long AcsEvoGetPlausibilityRejectCount();

// Read the static object (track name, session, lap length, ...).
// Returns true on success.
bool AcsEvoReadStatic(SPageFileStaticEvo& out);

// Unmap and close all three shared memory handles.
void AcsEvoCloseSharedMemory();

// Feed one shared-memory frame (physics + graphics) into the LapStats
// accumulators. Unit conversions live in mapping.json EVOM1 LIVE rows: this
// function only extracts RAW values from the frames and lets the generic
// table-driven feed apply factor/offset per row; multi-value derived samples
// (damper velocity, handling sample, steer/yaw) stay post-table by design.
// timeSeconds is the wall-clock time (GetTickCount64()/1000.0).
class SessionRecorder;   // optional high-frequency sample buffer for LD export (SessionRecorder.h)
void AcsEvoFeedLap(LapStats& lap, const SPageFilePhysics& phys,
                   const SPageFileGraphicEvo& gfx, double timeSeconds, SessionRecorder* rec = nullptr);

// Returns a non-empty warning string if any EVOM1 LIVE mapping row failed to
// resolve at startup (empty table or missing raw channel names). Empty = OK.
std::string AcsEvoGetChannelTableWarning();

// Returns true when the car is on track (game live, car on the track).
bool AcsEvoIsOnTrack(const SPageFileGraphicEvo& gfx);

// Returns true when the car is in the pit lane, pit entry/exit, or pit box.
bool AcsEvoIsInPits(const SPageFileGraphicEvo& gfx);

// Note: lap-change detection (npos wrap) lives in MotecSetupForm::processLiveFrameEVO(),
// which tracks npos across frames and finalizes the lap on a ~1 -> 0 position wrap.

