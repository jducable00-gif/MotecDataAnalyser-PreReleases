//---------------------------------------------------------------------------
// Ams2LiveCapture.cpp — AMS2 live capture from shared memory.
//
// Implements:
//   - Ams2OpenSharedMemory / Ams2ReadFrame / Ams2CloseSharedMemory
//   - Ams2FeedLap: feeds one frame into LapStats accumulators
//   - Ams2IsOnTrack / Ams2IsInPits: pit/track detection
//---------------------------------------------------------------------------

#include <windows.h>
#include <cmath>
#include <string>
#include <unordered_map>
#include <cstring>
#include <tlhelp32.h>
#include "Ams2LiveCapture.h"
#include "LiveChannelResolver.h"   // JSON-table-driven channel feed (LIVE_CAPTURE_JSON_MIGRATION_PLAN)
#include "SessionRecorder.h"       // optional high-frequency sample buffer for LD export

// ---------------------------------------------------------------------------
// Shared memory handle + frame-sync state (module-level, single instance)
// ---------------------------------------------------------------------------
static HANDLE g_hMapFile = NULL;
static void*  g_pSharedMem = NULL;

// Last mSequenceNumber we actually processed. A read is "new" only when the
// current even sequence differs from this value. Reset to 0 on connect /
// session reset so the first real frame after (re)connect always counts as new.
static unsigned int s_lastProcessedSeq = 0;

void Ams2ResetFrameTracking() { s_lastProcessedSeq = 0; }

// ---------------------------------------------------------------------------
// Ams2OpenSharedMemory
// ---------------------------------------------------------------------------
bool Ams2OpenSharedMemory(std::string& errorMessage)
{
    if (g_hMapFile != NULL)
        return true; // already open

    g_hMapFile = OpenFileMappingA(FILE_MAP_READ, FALSE, "$pcars2$");
    if (g_hMapFile == NULL)
    {
        errorMessage = "AMS2 shared memory not found. Is AMS2 running?";
        return false;
    }

    g_pSharedMem = MapViewOfFile(g_hMapFile, FILE_MAP_READ, 0, 0, 0);
    if (g_pSharedMem == NULL)
    {
        CloseHandle(g_hMapFile);
        g_hMapFile = NULL;
        errorMessage = "Failed to map AMS2 shared memory.";
        return false;
    }

    // Verify version
    const Ams2SharedMemory* sm = static_cast<const Ams2SharedMemory*>(g_pSharedMem);
    if (sm->mVersion != SHARED_MEMORY_VERSION)
    {
        errorMessage = "AMS2 shared memory version mismatch (got " +
                       std::to_string(sm->mVersion) + ", expected " +
                       std::to_string(SHARED_MEMORY_VERSION) + ").";
        UnmapViewOfFile(g_pSharedMem);
        g_pSharedMem = NULL;
        CloseHandle(g_hMapFile);
        g_hMapFile = NULL;
        return false;
    }

    // Fresh (re)connect: the first real frame must count as new.
    Ams2ResetFrameTracking();
    return true;
}

// ---------------------------------------------------------------------------
// Ams2ReadFrame — double-check sequence number to avoid torn reads AND detect
// genuinely new frames via change-detection on mSequenceNumber (plan §4).
// refresh rate is related to graphic frame rate ( max seen around 200Hz despite physics at 300hz)
// so better to run Vsynch off
// ---------------------------------------------------------------------------
FrameReadStatus Ams2ReadFrame(Ams2SharedMemory& out)
{
    if (g_pSharedMem == NULL)
        return FrameReadStatus::NoNewData;

    const Ams2SharedMemory* sm = static_cast<const Ams2SharedMemory*>(g_pSharedMem);

    // Read sequence number, copy data, re-read sequence number.
    unsigned int seq1 = sm->mSequenceNumber;
    if (seq1 & 1)
        return FrameReadStatus::TornRetry;   // write in progress -> retry next tick

    // Copy the struct (it's ~4-5 KB, fast enough at 60 Hz)
    memcpy(&out, sm, sizeof(Ams2SharedMemory));

    unsigned int seq2 = sm->mSequenceNumber;
    if (seq1 != seq2)
        return FrameReadStatus::TornRetry;   // torn read -> retry next tick

    if (seq1 == s_lastProcessedSeq)
        return FrameReadStatus::NoNewData;   // nothing new since last processed frame

    s_lastProcessedSeq = seq1;               // commit: this is a fresh, consistent frame
    return FrameReadStatus::NewFrame;
}

// ---------------------------------------------------------------------------
// Ams2CloseSharedMemory
// ---------------------------------------------------------------------------
void Ams2CloseSharedMemory()
{
    if (g_pSharedMem != NULL)
    {
        UnmapViewOfFile(g_pSharedMem);
        g_pSharedMem = NULL;
    }
    if (g_hMapFile != NULL)
    {
        CloseHandle(g_hMapFile);
        g_hMapFile = NULL;
    }
}

// ---------------------------------------------------------------------------
// Ams2IsProcessRunning
// ---------------------------------------------------------------------------
bool Ams2IsProcessRunning()
{
    bool found = false;
    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnapshot != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32 pe;
        pe.dwSize = sizeof(PROCESSENTRY32);
        if (Process32First(hSnapshot, &pe)) {
            do {
                // Check if the process name contains "AMS2" (handles AMS2.exe, AMS2AVX.exe, etc.)
                if (wcsstr(pe.szExeFile, L"AMS2") != NULL) {
                    found = true;
                    break;
                }
            } while (Process32Next(hSnapshot, &pe));
        }
        CloseHandle(hSnapshot);
    }
    return found;
}

// ---------------------------------------------------------------------------
// Ams2IsOnTrack / Ams2IsInPits
// ---------------------------------------------------------------------------
bool Ams2IsOnTrack(const Ams2SharedMemory& frame)
{
    // Game must be in playing state
    if (frame.mGameState != GAME_INGAME_PLAYING)
        return false;

    // Car must not be in pits
    if (frame.mPitMode != PIT_MODE_NONE)
        return false;

    return true;
}

bool Ams2IsInPits(const Ams2SharedMemory& frame)
{
    return frame.mPitMode != PIT_MODE_NONE;
}

// ---------------------------------------------------------------------------
// Ams2FeedLap — feed one frame into LapStats accumulators
// ---------------------------------------------------------------------------
//
// Unit conversions (from data/reference/plan_ams2.md):
//   mSpeed: m/s → ×3.6 → km/h
//   mRpm: direct
//   mThrottle: 0-1 → ×100 → %
//   mBrake: 0-1 → ×100 → %
//   mSteering: -1..1 → ×360 → degrees
//   mAngularVelocity[1]: rad/s → ×180/π → deg/s (yaw)
//   mLocalAcceleration[0]: m/s² → /9.81 → g (long)
//   mLocalAcceleration[2]: m/s² → /9.81 → g (lat)
//   mSuspensionTravel[4]: meters → ×1000 → mm
//   mAirPressure[4]: PSI direct
//   mTyreTempLeft/Center/Right[4]: °C direct
//   mBrakeTempCelsius[4]: °C direct
//   mRideHeight[4]: cm direct
//   mBrakeBias: 0-1 → ×100 → %
//   mFuelLevel: 0-1 → ×100 → %
//   mOilTempCelsius: °C direct
//   mWaterTempCelsius: °C direct
//   mTrackTemperature: °C direct
//   mAmbientTemperature: °C direct
//
// Tyre temp mapping (Left/Right → Inner/Outer):
//   Left tyres (LF=0, LR=2): mTyreTempLeft = Outer, mTyreTempRight = Inner
//   Right tyres (RF=1, RR=3): mTyreTempLeft = Inner, mTyreTempRight = Outer
// ---------------------------------------------------------------------------

static const double DEG_PER_RAD = 180.0 / 3.14159265358979323846;
static const double G = 9.81;

std::string Ams2GetChannelTableWarning();   // defined below
// Resolved channel table for AMS2 LIVE rows (mapping.json), built ONCE on first use.
// Unit conversions live in mapping.json, not here: this function only extracts RAW values
// from the shared-memory frame and lets FeedLapFromTable() apply factor/offset per row.
std::string g_ams2ChannelTableWarning;
static LiveChannel::ResolvedChannelTable& Ams2GetChannelTable()
{
    static LiveChannel::ResolvedChannelTable table = [] {
        LiveChannel::ResolvedChannelTable t;
        t.build(ChannelProfile::AMS2);   // Generic == AMS2, so generic rows are included too
        return t;
    }();
    // One-time validation: surface (never fatal) any mapping row that failed to resolve.
    static bool validated = false;
    if (!validated) {
        std::string report;
        const std::vector<std::string> none;   // empty -> no per-alias requirements, informational only for AMS2
        table.validate(none, report);
        g_ams2ChannelTableWarning = report.empty() ? "" : "AMS2 live channel table: " + report;
        validated = true;
    }
    return table;
}

void Ams2FeedLap(LapStats& lap, const Ams2SharedMemory& frame, double timeSeconds, SessionRecorder* rec)
{
    LiveChannel::ResolvedChannelTable& table = Ams2GetChannelTable();

    // --- Raw channel dictionary (RAW values only - no unit conversion here). ---
    // Keys MUST match the "channel" field of mapping.json AMS2 LIVE rows EXACTLY.
    std::unordered_map<std::string, double> raw;
    raw["mSpeed"]                 = frame.mSpeed;                       // m/s  (Vehicle_Speed + Wheel_Speed_* x3.6)
    raw["mRpm"]                   = frame.mRpm;                         // RPM
    raw["mThrottle"]              = frame.mThrottle;                    // 0..1 -> % via table factor 100
    raw["mBrake"]                 = frame.mBrake;                       // 0..1 -> % via table factor 100
    raw["mSteering"]              = frame.mSteering;                    // -1..1 -> deg x360 via table (Steering_Angle)
    raw["mAngularVelocity[1]"]    = frame.mAngularVelocity[1];          // rad/s -> deg/s via table (Yaw_Rate)
    raw["mLocalAcceleration[0]"]  = frame.mLocalAcceleration[0];        // m/s^2 -> G x0.10197162 (G_Long)
    raw["mLocalAcceleration[2]"]  = frame.mLocalAcceleration[2];        // m/s^2 -> G x0.10197162 (G_Lat)
    for (int i = 0; i < TYRE_MAX; ++i) {   // wheel order: FL=0, FR=1, RL=2, RR=3
        char key[48];
        snprintf(key, sizeof(key), "mSuspensionTravel[%d]", i);    raw[key] = frame.mSuspensionTravel[i];     // m -> mm x1000 (susTravel*)
        snprintf(key, sizeof(key), "mSuspensionVelocity[%d]", i);  raw[key] = frame.mSuspensionVelocity[i];   // m/s -> mm/s x1000 (suspVel*)
        snprintf(key, sizeof(key), "mAirPressure[%d]", i);         raw[key] = frame.mAirPressure[i];          // kPa -> psi via table factor 0.1450377
        snprintf(key, sizeof(key), "mTyreSlipSpeed[%d]", i);       raw[key] = frame.mTyreSlipSpeed[i];        // m/s -> km/h x3.6 (wheel slip)
        snprintf(key, sizeof(key), "mBrakeTempCelsius[%d]", i);    raw[key] = frame.mBrakeTempCelsius[i];     // C
        snprintf(key, sizeof(key), "mRideHeight[%d]", i);          raw[key] = frame.mRideHeight[i];           // cm -> mm x10 (aeroHeight*)
        // Tyre tread temps: the L/R swap per wheel position is a non-linear mapping that stays in C++ by design;
        // JSON rows already encode it via mTyreTempLeft/Center/Right[N] channels.
        snprintf(key, sizeof(key), "mTyreTempLeft[%d]", i);        raw[key] = frame.mTyreTempLeft[i];         // C
        snprintf(key, sizeof(key), "mTyreTempCenter[%d]", i);      raw[key] = frame.mTyreTempCenter[i];       // C
        snprintf(key, sizeof(key), "mTyreTempRight[%d]", i);       raw[key] = frame.mTyreTempRight[i];        // C
        snprintf(key, sizeof(key), "mTyreInternalAirTemp[%d]", i); raw[key] = frame.mTyreInternalAirTemp[i];  // C (NEW: was unpopulated before)
    }
    raw["mFuelLevel"]             = frame.mFuelLevel * 100.0;        // % - pre-computed so the ratio row lands in percent, matching EVO/PMR + old C++
    raw["mOilTempCelsius"]        = frame.mOilTempCelsius;           // C
    raw["mWaterTempCelsius"]      = frame.mWaterTempCelsius;         // C
    raw["mTrackTemperature"]      = frame.mTrackTemperature;         // C
    raw["mAmbientTemperature"]    = frame.mAmbientTemperature;       // C

    // Prediction: tyre wear, fuel capacity, brake damage (increases), clutch wear.
    for (int i = 0; i < TYRE_MAX; ++i) {
        char key[48];
        snprintf(key, sizeof(key), "mTyreWear[%d]", i);       raw[key] = frame.mTyreWear[i];         // native ratio -> % via table if needed
        snprintf(key, sizeof(key), "mBrakeDamage[%d]", i);    raw[key] = frame.mBrakeDamage[i];      // 0..1 damage (increases)
    }
    raw["mFuelCapacity"]          = frame.mFuelCapacity;               // L tank capacity
    if (frame.mClutchWear >= 0.0f)                                     // guard: AMS2 may report -1 when unset
        raw["mClutchWear"]         = frame.mClutchWear;                // native ratio
    if (frame.mBrakeBias >= 0.0f)                                     // guard stays in C++: AMS2 reports -1 when unset
        raw["mBrakeBias"]         = frame.mBrakeBias;                 // 0..1 -> % x100 via table
    raw["mCarFlags_TCS"]          = (frame.mCarFlags & CAR_TCS) ? 1.0 : 0.0;   // synthetic bit-flag channel (TCS_Active)
    raw["mAntiLockActive"]        = frame.mAntiLockActive ? 1.0 : 0.0;         // bool -> numeric (ABS_Active)
    if (rec) rec->feed(table, raw, timeSeconds);   // LD export buffer (optional)
    // --- Generic table-driven feed: v = raw * factor + offset -> LapStats sink. ---
    LiveChannel::FeedLapFromTable(lap, raw, table, timeSeconds);
    lap.rideHeightUnit = RideHeightUnit::Millimeters;   // aeroHeight* now arrive in mm (table x10 from cm) - downstream Rake identical to old cm+Centimeters path
    // --- Damper stats: addSample derives shaft velocity from travel deltas over wall-clock time.
    // Stays post-table by design (multi-value derived sample, not a single-channel sink). ---
    double travelLF = frame.mSuspensionTravel[TYRE_FRONT_LEFT] * 1000.0;   // m -> mm
    double travelRF = frame.mSuspensionTravel[TYRE_FRONT_RIGHT] * 1000.0;
    double travelLR = frame.mSuspensionTravel[TYRE_REAR_LEFT] * 1000.0;
    double travelRR = frame.mSuspensionTravel[TYRE_REAR_RIGHT] * 1000.0;
    lap.damperLF.addSample(travelLF, timeSeconds);
    lap.damperRF.addSample(travelRF, timeSeconds);
    lap.damperLR.addSample(travelLR, timeSeconds);
    lap.damperRR.addSample(travelRR, timeSeconds);
    // --- Handling sample (understeer/oversteer detection) - multi-value derived sample. ---
    double steer = frame.mSteering * 360.0;
    double yaw   = frame.mAngularVelocity[1] * DEG_PER_RAD;
    double latG  = frame.mLocalAcceleration[2] / G;
    double longG = frame.mLocalAcceleration[0] / G;
    double brake     = frame.mBrake * 100.0;
    double throttle  = frame.mThrottle * 100.0;
    double suspAsymmetry = (travelLF - travelRF) / 2.0;
    lap.addHandlingSample(steer, yaw, latG, longG, brake, throttle, suspAsymmetry, frame.mSpeed * 3.6);   // m/s -> km/h
    // --- Steer/Yaw correlation ---
    lap.addSteerYaw(steer, yaw);
    // --- Row count ---
    lap.rowCount++;
}

std::string Ams2GetChannelTableWarning() { return g_ams2ChannelTableWarning; }
