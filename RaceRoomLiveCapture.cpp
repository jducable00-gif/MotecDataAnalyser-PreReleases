//---------------------------------------------------------------------------
// RaceRoomLiveCapture.cpp — RaceRoom (R3E) live capture from shared memory.
//
// Implements:
//   - R3eOpenSharedMemory / R3eReadFrame / R3eCloseSharedMemory ("$R3E", v3.x)
//   - R3eFeedLap: feeds one frame into LapStats accumulators (raw dict + table feed)
//   - R3eIsOnTrack / R3eIsInPits: pit/track detection
//---------------------------------------------------------------------------

#include <windows.h>
#include <cmath>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <cstring>
#include <tlhelp32.h>
#include "RaceRoomLiveCapture.h"
#include "LiveChannelResolver.h"   // JSON-table-driven channel feed (LIVE_CAPTURE_JSON_MIGRATION_PLAN)
#include "SessionRecorder.h"       // optional high-frequency sample buffer for LD export

// ---------------------------------------------------------------------------
// Shared memory handle + frame-sync state (module-level, single instance)
// ---------------------------------------------------------------------------
static HANDLE g_hMapFile = NULL;
static void*  g_pSharedMem = NULL;

// Last player.game_simulation_ticks we actually processed. A read is "new" only when the
// current ticks value differs from this one. Reset on connect so the first real frame after
// (re)connect always counts as new.
static r3e_int32 s_lastProcessedTicks = -1;

void R3eResetFrameTracking() { s_lastProcessedTicks = -1; }

// Non-fatal note recorded at open time (version drift), surfaced via R3eGetChannelTableWarning().
static std::string g_r3eOpenNote;

std::string R3eGetChannelTableWarning();   // defined below
// Resolved channel table for RaceRoom LIVE rows (mapping.json), built ONCE on first use.
// Unit conversions live in mapping.json, not here: this function only extracts RAW values
// from the shared-memory frame and lets FeedLapFromTable() apply factor/offset per row.
static std::string g_r3eChannelTableWarning;

// ---------------------------------------------------------------------------
// R3eOpenSharedMemory — open "$R3E" read-only, tolerate minor version drift (plan §5).
// The layout grows forward: a newer 3.x struct is safe to memcpy as long as we only touch
// fields present in v3.6. A major-version mismatch is recorded but NOT fatal so the user can
// still see data if kwstudios ships an incompatible-but-compatible-in-practice bump.
// ---------------------------------------------------------------------------
bool R3eOpenSharedMemory(std::string& errorMessage)
{
    if (g_hMapFile != NULL)
        return true; // already open

    g_r3eOpenNote.clear();

    g_hMapFile = OpenFileMappingA(FILE_MAP_READ, FALSE, R3E_SHARED_MEMORY_NAME);   // "$R3E"
    if (g_hMapFile == NULL)
    {
        errorMessage = "RaceRoom shared memory not found. Is RaceRoom running?";
        return false;
    }

    g_pSharedMem = MapViewOfFile(g_hMapFile, FILE_MAP_READ, 0, 0, 0);
    if (g_pSharedMem == NULL)
    {
        CloseHandle(g_hMapFile);
        g_hMapFile = NULL;
        errorMessage = "Failed to map RaceRoom shared memory.";
        return false;
    }

    const r3e_shared* sm = static_cast<const r3e_shared*>(g_pSharedMem);
    if (sm->version_major != R3E_VERSION_MAJOR)
    {
        // Non-fatal: record and continue. Only 3.x-present fields are read by this app.
        g_r3eOpenNote = "R3E shared memory major version mismatch (got " +
                        std::to_string(sm->version_major) + ", expected " +
                        std::to_string(R3E_VERSION_MAJOR) + "); data may be unreliable.";
    }

    // Fresh (re)connect: the first real frame must count as new.
    R3eResetFrameTracking();
    return true;
}

// ---------------------------------------------------------------------------
// R3eReadFrame — torn-read guard on player.game_simulation_ticks sampled before and after a
// full struct copy, plus change-detection so unchanged frames report NoNewData (plan §5).
// The sim writes at its physics rate (~400 ticks/s); the capture thread polls every ~8 ms.
// ---------------------------------------------------------------------------
FrameReadStatus R3eReadFrame(r3e_shared& out)
{
    if (g_pSharedMem == NULL)
        return FrameReadStatus::NoNewData;

    const r3e_shared* sm = static_cast<const r3e_shared*>(g_pSharedMem);

    // Read ticks, copy data, re-read ticks.
    r3e_int32 t1 = sm->player.game_simulation_ticks;
    memcpy(&out, sm, sizeof(r3e_shared));   // ~tens of KB (includes all-drivers array), fine at 60 Hz

    r3e_int32 t2 = sm->player.game_simulation_ticks;
    if (t1 != t2)
        return FrameReadStatus::TornRetry;   // write in progress -> retry next tick

    if (t1 == s_lastProcessedTicks)
        return FrameReadStatus::NoNewData;   // nothing new since last processed frame

    s_lastProcessedTicks = t1;               // commit: this is a fresh, consistent frame
    return FrameReadStatus::NewFrame;
}

// ---------------------------------------------------------------------------
// R3eCloseSharedMemory — idempotent.
// ---------------------------------------------------------------------------
void R3eCloseSharedMemory()
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
// R3eIsProcessRunning — "RRRE" in the process list (handles RRRE.exe, etc.)
// ---------------------------------------------------------------------------
bool R3eIsProcessRunning()
{
    bool found = false;
    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnapshot != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32 pe;
        pe.dwSize = sizeof(PROCESSENTRY32);
        if (Process32First(hSnapshot, &pe)) {
            do {
                if (wcsstr(pe.szExeFile, L"RRRE") != NULL) {
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
// R3eIsOnTrack / R3eIsInPits — in_pitlane is -1/0/1; pit_state: None=0 .. Exiting=4.
// Pause/menu state (game_paused, game_in_menus) is handled by the form-level lap logic.
// ---------------------------------------------------------------------------
bool R3eIsOnTrack(const r3e_shared& frame)
{
    return frame.in_pitlane != 1;   // -1 = N/A treated as on-track to keep data flowing
}

// pit_state values per r3e.h @805 comment: -1 N/A, 0 None, 1 Requested stop,
// 2 Entered pitlane heading for pitspot, 3 Stopped at pitspot, 4 Exiting pitspot. No enum exists; use literal.
bool R3eIsInPits(const r3e_shared& frame)
{
    return frame.in_pitlane == 1 || (frame.pit_state > 0 && frame.pit_state <= 4);   // 4 = "Exiting pitspot" per r3e.h @805 comment
}

// ---------------------------------------------------------------------------
// Channel table + warning plumbing
// ---------------------------------------------------------------------------
static LiveChannel::ResolvedChannelTable& R3eGetChannelTable()
{
    static LiveChannel::ResolvedChannelTable table = [] {
        LiveChannel::ResolvedChannelTable t;
        t.build(ChannelProfile::R3e);   // profile-specific rows only (no generic fallback for RaceRoom)
        return t;
    }();
    // One-time validation: surface (never fatal) any mapping row that failed to resolve.
    static bool validated = false;
    if (!validated) {
        std::string report;
        const std::vector<std::string> none;   // empty -> no per-alias requirements, informational only for R3E
        table.validate(none, report);
        g_r3eChannelTableWarning = report.empty() ? "" : "RaceRoom live channel table: " + report;
        validated = true;
    }
    return table;
}

std::string R3eGetChannelTableWarning()
{
    std::string w = g_r3eOpenNote;
    if (!g_r3eChannelTableWarning.empty())
        w += (w.empty() ? "" : " | ") + g_r3eChannelTableWarning;
    return w;
}

// ---------------------------------------------------------------------------
// R3eFeedLap — feed one frame into LapStats accumulators.
//
// Unit conversions live in mapping.json rows, not here: this function only extracts RAW values
// from the shared-memory frame and lets FeedLapFromTable() apply factor/offset per row.
// Raw dict keys MUST match the "channel" field of mapping.json R3E LIVE rows EXACTLY.
//
// Wheel order in r3e.h (FL=0, FR=1, RL=2, RR=3) is already the app's internal wheel order — no reordering.
// Tyre temp positions: R3E reports LEFT/CENTER/RIGHT across each tread; left-side wheels map
// LEFT -> Outer / RIGHT -> Inner and right-side wheels mirror that (= AMS2 convention). The flat
// tireTemp0..11 keys use slot = wheelIndex*3 + {Inner=0, Middle=1, Outer=2}.
// ---------------------------------------------------------------------------

static const double DEG_PER_RAD = 180.0 / 3.14159265358979323846;   // local handling sample: rad/s -> deg/s (table row uses factor 57.29577951)
static const double G = 9.81;                                       // local handling sample only — the dict carries RAW m/s^2

void R3eFeedLap(LapStats& lap, const r3e_shared& frame, double timeSeconds, SessionRecorder* rec)
{
    LiveChannel::ResolvedChannelTable& table = R3eGetChannelTable();

    auto idxKey = [](const char* name, int i) {
        char buf[40]; snprintf(buf, sizeof(buf), "%s[%d]", name, i); return std::string(buf);
    };

    // --- Raw channel dictionary (RAW values only - no unit conversion here). ---
    std::unordered_map<std::string, double> raw;

    raw["car_speed"] = static_cast<double>(frame.car_speed);   // m/s -> km/h x3.6 via table row (Vehicle_Speed)
    if (std::isfinite(frame.engine_rps))
        raw["engine_rps"] = static_cast<double>(frame.engine_rps);   // rad/s -> RPM x95.49297 via table factor

    // Pedals: RAW 0..1 in the dict; row factor x100 does the % conversion (Throttle_Pos/Brake_Pos).
    // Omitted on N/A (-1, e.g. AI/remote players) so no fake zero is injected into sinks.
    if (std::isfinite(frame.throttle) && frame.throttle >= 0.0f) raw["throttle"] = static_cast<double>(frame.throttle);
    if (std::isfinite(frame.brake)    && frame.brake    >= 0.0f) raw["brake"]    = static_cast<double>(frame.brake);
    if (std::isfinite(frame.clutch)   && frame.clutch   >= 0.0f) raw["clutch"]   = static_cast<double>(frame.clutch);

    // Engine temps: degC direct (factor-1 rows), omitted when <0/N/A per row contract.
    if (std::isfinite(frame.engine_temp)     && frame.engine_temp     >= 0.0f) raw["engine_temp"]     = static_cast<double>(frame.engine_temp);
    if (std::isfinite(frame.engine_oil_temp) && frame.engine_oil_temp >= 0.0f) raw["engine_oil_temp"] = static_cast<double>(frame.engine_oil_temp);

    // Yaw rate: rad/s -> deg/s via table factor 57.29577951 [sign to verify on first capture, plan Q3].
    if (std::isfinite(frame.player.local_angular_velocity.y))
        raw["angVelY"] = frame.player.local_angular_velocity.y;

    // Steering: RAW -1..1 in the dict; row factor x360 like AMS2 mSteering. True wheel angle for the
    // local handling sample below uses steer_lock_degrees (multi-value derivation, not a single-factor row).
    if (std::isfinite(frame.steer_input_raw))
        raw["steer_input_raw"] = static_cast<double>(frame.steer_input_raw);

    // G-forces: RAW m/s^2 in the dict; table rows convert to g via factor 0.10197162 (AMS2 convention).
    // Car-local axes per r3e.h @941 (+X left, +Z back): latG = x, longG = z with NO sign flip here [verify live].
    if (std::isfinite(frame.player.local_g_force.x)) raw["latG"]  = static_cast<double>(frame.player.local_g_force.x);
    if (std::isfinite(frame.player.local_g_force.z)) raw["longG"] = static_cast<double>(frame.player.local_g_force.z);

    // Fuel ratio precomputed (capacity > 0 guard); missing key -> table skips it, no NaN sink value.
    if (std::isfinite(frame.fuel_left) && std::isfinite(frame.fuel_capacity) && frame.fuel_capacity > 0.0f)
        raw["fuelLevelRatio"] = static_cast<double>(frame.fuel_left / frame.fuel_capacity);

    // TCS/ABS active flags: aid_settings tc/abs (-1 N/A, 0 off, 1 on, 5 currently active).
    // Bool convention like AMS2 (unit "bool", factor 1): only the "currently active" state counts as on.
    raw["tcsActive"] = frame.aid_settings.tc == 5 ? 1.0 : 0.0;
    raw["absActive"] = frame.aid_settings.abs == 5 ? 1.0 : 0.0;

    // Brake bias: R3E reports the REAR fraction (r3e.h @1010, e.g. 0.3 = 30% rear); app expects % FRONT,
    // so invert in C++ — a single-factor row cannot express this [VERIFY direction live, plan Q6].
    double brakeBiasPct = 50.0;   // default when N/A (-1) or non-finite
    if (std::isfinite(frame.brake_bias) && frame.brake_bias >= 0.0f)
        brakeBiasPct = (1.0 - static_cast<double>(frame.brake_bias)) * 100.0;
    raw["brakeBiasPct"] = brakeBiasPct;

    // --- Per-wheel indexed channels + tyre temps (FL=0 FR=1 RL=2 RR=3). ---
    for (int i = 0; i < R3E_TIRE_INDEX_MAX; ++i) {
        if (std::isfinite(frame.tire_pressure[i]) && frame.tire_pressure[i] >= 0.0f)
            raw[idxKey("tire_pressure", i)] = static_cast<double>(frame.tire_pressure[i]);   // kPa -> psi x0.1450377

        const r3e_tire_temp& tt = frame.tire_temp[i];
        double tempSum = 0.0; int tempCount = 0;
        for (int pos = R3E_TIRE_TEMP_LEFT; pos < R3E_TIRE_TEMP_INDEX_MAX; ++pos) {   // LEFT=0 CENTER=1 RIGHT=2 across the tread
            const float t = tt.current_temp[pos];
            if (!std::isfinite(t) || t < 0.0f) continue;                       // -1 N/A guard (omit on missing, plan §5)

            // Position -> app slot {Inner=0, Middle=1, Outer=2}: left-side wheels map LEFT->Outer /
            // RIGHT->Inner and right-side wheels mirror that (= AMS2 convention). Flat key = wheelIndex*3 + slot.
            const bool isLeftSide = (i == R3E_TIRE_FRONT_LEFT || i == R3E_TIRE_REAR_LEFT);   // FL=0, RL=2
            const int slot = isLeftSide ? 2 - pos : pos;

            char key[16]; snprintf(key, sizeof(key), "tireTemp%d", i * 3 + slot);
            raw[key] = static_cast<double>(t);                                  // degC direct (factor-1 rows)

            tempSum += t; ++tempCount;                                          // per-wheel average accumulator
        }
        if (tempCount > 0)
            raw[idxKey("tireAvgTemp", i)] = tempSum / tempCount;                // Tyre_Temp_Avg_* row, degC f=1

        const r3e_brake_temp& bt = frame.brake_temp[i];
        if (std::isfinite(bt.current_temp) && bt.current_temp >= 0.0f)
            raw[idxKey("brake_temp", i)]     = static_cast<double>(bt.current_temp);                     // degC direct

        const r3e_playerdata& p = frame.player;   // suspension/ride fields are playerdata-only (no top-level dups in v3.6)
        if (std::isfinite(p.suspension_deflection[i]))
            raw[idxKey("suspensionDeflection", i)] = static_cast<double>(p.suspension_deflection[i]);   // m -> mm x1000 via table row

        if (std::isfinite(p.suspension_velocity[i]))
            raw[idxKey("damperVelocity", i)]  = static_cast<double>(p.suspension_velocity[i]);          // m/s -> mm/s x1000 via table row

        if (std::isfinite(frame.tire_speed[i]) && frame.tire_speed[i] >= 0.0f)
            raw[idxKey("wheelSpeed", i)]      = static_cast<double>(frame.tire_speed[i]);               // m/s -> km/h x3.6

        if (std::isfinite(p.ride_height[i]))
            raw[idxKey("ride_height", i)]     = static_cast<double>(p.ride_height[i]);                  // m -> mm x1000 via table row

        if (std::isfinite(frame.tire_wear[i]) && frame.tire_wear[i] >= 0.0f)
            raw[idxKey("tireWear", i)]        = static_cast<double>(frame.tire_wear[i]);                // native ratio, f=1 rows (-1 N/A omitted)
    }

    if (std::isfinite(frame.gear))   // dict-only extras: no LIVE mapping row today, harmless keys
        raw["gear"] = static_cast<double>(frame.gear);
    if (std::isfinite(static_cast<float>(frame.steer_lock_degrees)))
        raw["steer_lock_degrees"] = static_cast<double>(frame.steer_lock_degrees);   // dict-only extra

    if (rec) rec->feed(table, raw, timeSeconds);   // LD export buffer (optional)
    LiveChannel::FeedLapFromTable(lap, raw, table, timeSeconds);

    // --- Post-table derived samples (AMS2 parity). ---
    lap.rideHeightUnit = RideHeightUnit::Millimeters;   // ride heights arrive in mm via x1000 rows

    const double travelLF = frame.player.suspension_deflection[R3E_TIRE_FRONT_LEFT] * 1000.0;   // m -> mm (damper TRAVEL-derived stats)
    const double travelRF = frame.player.suspension_deflection[R3E_TIRE_FRONT_RIGHT] * 1000.0;
    const double travelLR = frame.player.suspension_deflection[R3E_TIRE_REAR_LEFT]   * 1000.0;
    const double travelRR = frame.player.suspension_deflection[R3E_TIRE_REAR_RIGHT]  * 1000.0;

    lap.damperLF.addSample(travelLF, timeSeconds);
    lap.damperRF.addSample(travelRF, timeSeconds);
    lap.damperLR.addSample(travelLR, timeSeconds);
    lap.damperRR.addSample(travelRR, timeSeconds);

    // Handling sample + steer/yaw correlation (true wheel angle in degrees). [VERIFY sign live]
    double steerDeg = 0.0;   // raw input -1..1 scaled by lock to true degrees
    if (std::isfinite(frame.steer_input_raw) && frame.steer_lock_degrees > 0)
        steerDeg = static_cast<double>(frame.steer_input_raw) * static_cast<double>(frame.steer_lock_degrees);

    const double yawDps   = std::isfinite(frame.player.local_angular_velocity.y) ? frame.player.local_angular_velocity.y * DEG_PER_RAD : 0.0;
    // Local handling sample only: g units via /9.81, NO sign flip (AMS2 template convention); dict stays RAW m/s^2 above.
    const double latG     = std::isfinite(frame.player.local_g_force.x) ? frame.player.local_g_force.x / G : 0.0;
    const double longG    = std::isfinite(frame.player.local_g_force.z) ? frame.player.local_g_force.z / G : 0.0;

    // Pedal % for the local sample only (dict carries RAW 0..1); N/A (-1, AI/remote) -> 0.
    const auto pedalLocal = [](float v) { return (std::isfinite(v) && v >= 0.0f) ? static_cast<double>(v) : 0.0; };
    const double brakePct     = pedalLocal(frame.brake) * 100.0;
    const double throttlePct  = pedalLocal(frame.throttle) * 100.0;

    lap.addHandlingSample(steerDeg, yawDps, latG, longG, brakePct, throttlePct, (travelLF - travelRF) / 2.0, frame.car_speed * 3.6);
    lap.addSteerYaw(steerDeg, yawDps);

    ++lap.rowCount;
}
