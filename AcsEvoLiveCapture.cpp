//---------------------------------------------------------------------------
// AcsEvoLiveCapture.cpp — AC EVO live capture from shared memory.
//
// Implements:
//   - AcsEvoOpenSharedMemory / AcsEvoReadPhysics / AcsEvoReadGraphics /
//     AcsEvoReadStatic / AcsEvoCloseSharedMemory
//   - AcsEvoFeedLap: feeds one frame (physics + graphics) into LapStats
//   - AcsEvoIsOnTrack / AcsEvoIsInPits: pit/track detection
//   - AcsEvoDetectLapChange: npos wrap detection
//---------------------------------------------------------------------------

#include <windows.h>
#include <cmath>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>
#include <cstring>
#include "AcsEvoLiveCapture.h"
#include "LiveChannelResolver.h"   // JSON-table-driven channel feed (LIVE_CAPTURE_JSON_MIGRATION_PLAN)
#include "SessionRecorder.h"       // optional high-frequency sample buffer for LD export

// ---------------------------------------------------------------------------
// Shared memory handles (module-level, single instance)
// ---------------------------------------------------------------------------
static HANDLE g_hPhysics  = NULL;
static void*  g_pPhysics  = NULL;
static HANDLE g_hGraphics = NULL;
static void*  g_pGraphics = NULL;
static HANDLE g_hStatic   = NULL;
static void*  g_pStatic   = NULL;

// rad/s -> deg/s (post-table handling sample only; the table row uses its own factor).
static const double DEG_PER_RAD = 180.0 / 3.14159265358979323846;

std::string AcsEvoGetChannelTableWarning();   // defined below

// ---------------------------------------------------------------------------
// One-time diagnostic: dump the RAW shared-memory values for every channel we
// feed, so a "0°" report can be attributed to the game (writes 0.0f) vs our
// pipeline (drops/zeroes it). Written next to the exe as ac_evo_diag.txt on
// the first frame after connect; delete this block once diagnosed.
// ---------------------------------------------------------------------------
static void AcsEvoDumpRawDiagOnce(const SPageFilePhysics& phys, const SPageFileGraphicEvo& gfx) {
    static bool done = false;
    if (done) return;
    done = true;

    char path[MAX_PATH] = {0};
    GetModuleFileNameA(NULL, path, MAX_PATH);
    char* p = path + strlen(path);
    while (p > path && *p != '\\' && *p != '/') --p;
    size_t remaining = MAX_PATH - static_cast<size_t>(p - path);
    strcpy_s(p, remaining, "\\ac_evo_diag.txt");

    FILE* f = NULL;
    fopen_s(&f, path, "w");
    if (!f) return;

    fprintf(f, "AC EVO raw shared-memory dump (first frame)\n\n");
    for (int i = 0; i < 4; ++i) {
        const char* w[4] = {"FL", "FR", "RL", "RR"};
        fprintf(f, "%s: tyreTempI=%.2f  tyreTempM=%.2f  tyreTempO=%.2f  tyreTemp=%.2f\n",
                w[i], (double)phys.tyreTempI[i], (double)phys.tyreTempM[i],
                (double)phys.tyreTempO[i], (double)phys.tyreTemp[i]);
    }
    fprintf(f, "\n");
    for (int i = 0; i < 4; ++i) {
        const char* w[4] = {"FL", "FR", "RL", "RR"};
        fprintf(f, "%s: pressure=%.2f  brakeTemp=%.1f  wheelAngSpeed=%.3f  suspTravel=%.5f\n",
                w[i], (double)phys.wheelsPressure[i], (double)phys.brakeTemp[i],
                (double)phys.wheelAngularSpeed[i], (double)phys.suspensionTravel[i]);
    }
    fprintf(f, "\n");
    for (int i = 0; i < 4; ++i) {
        const char* w[4] = {"FL", "FR", "RL", "RR"};
        const SMEvoTyreState& t = (&gfx.tyre_lf)[i];   // lf, rf, lr, rr are contiguous in the struct
        fprintf(f, "%s gfx: temp_c=%.2f  left=%.2f  center=%.2f  right=%.2f\n",
                w[i], (double)t.tyre_temperature_c, (double)t.tyre_temperature_left,
                (double)t.tyre_temperature_center, (double)t.tyre_temperature_right);
    }
    fprintf(f, "\n");
    fprintf(f, "waterTemp=%.1f  oil_temp(gfx)=%.2f  rideHeight[0]=%.5f  rideHeight[1]=%.5f\n",
            (double)phys.waterTemp, (double)gfx.oil_temperature_c,
            (double)phys.rideHeight[0], (double)phys.rideHeight[1]);

    // Frame-sync sanity check (plan §6): sim-reported frame rate for the adaptive poll interval + status line.
    fprintf(f, "\n");
    fprintf(f, "player_fps=%d  player_fps_avg=%d\n", gfx.player_fps, gfx.player_fps_avg);

    fclose(f);
}

// ---------------------------------------------------------------------------
// One-time diagnostic: record raw suspensionTravel samples over ~3 seconds so a
// skewed damper histogram can be attributed to the game (sparse/quantized writes)
// vs our d(travel)/dt derivation. Written next to the exe as ac_evo_travel_diag.txt;
// delete this block once diagnosed.
// ---------------------------------------------------------------------------
struct TravelDiagState {
    bool        active = false;      // armed on first call, stops after kMaxSamples changed-readings
    double      tStartSec  = 0.0;
    double      lastLoggedT = 0.0;   // wall-clock time of the previous logged row (for dt_ms column)
    int         samplesWritten = 0;
    const int   kMaxSamples = 600;   // ~3 s at a typical sim rate for CHANGED readings only
};

static void AcsEvoDumpTravelDiag(const SPageFilePhysics& phys, const SPageFileGraphicEvo& gfx, double timeSeconds) {
    static TravelDiagState st;

    auto writePathTo = [](char* out) {
        char full[MAX_PATH] = {0};
        GetModuleFileNameA(NULL, full, MAX_PATH);
        char* p = full + strlen(full);
        while (p > full && *p != '\\' && *p != '/') --p;
        strcpy_s(p, MAX_PATH - static_cast<size_t>(p - full), "\\ac_evo_travel_diag.txt");
        strncpy(out, full, 259); out[259] = '\0';
    };

    if (!st.active) {
        // Arm on first call. Open the file lazily so we don't create it when not driving.
        char path[MAX_PATH]; writePathTo(path);
        FILE* f = NULL; fopen_s(&f, path, "w");
        if (f) {
            fprintf(f, "# AC EVO suspensionTravel raw sample dump\n"
                       "# Rows are logged only on polls where at least one wheel's travel changed vs the previous poll.\n"
                       "# dt_ms = wall-clock ms since the PREVIOUS LOGGED ROW; dXX = mm change of that wheel between those two rows.\n"
                       "# t_sec   dt_ms     delta_time_ms(game)  FL_mm        dFL         FR_mm        dFR         LR_mm        dLR         RR_mm        dRR\n");
            fclose(f);
        }
        st.active = true;
        st.tStartSec = timeSeconds;
        return;
    }

    if (st.samplesWritten >= st.kMaxSamples) {
        char path[MAX_PATH]; writePathTo(path);
        FILE* f = NULL; fopen_s(&f, path, "a");
        if (f) { fprintf(f, "# END: %d changed samples over %.2fs\n", st.samplesWritten, timeSeconds - st.tStartSec); fclose(f); }
        return;   // one-shot per process run
    }

    const double curMm[4] = { phys.suspensionTravel[0]*1000.0, phys.suspensionTravel[1]*1000.0,
                              phys.suspensionTravel[2]*1000.0, phys.suspensionTravel[3]*1000.0 };

    static double lastPoll[4]   = {0.0, 0.0, 0.0, 0.0};
    static bool   haveLastPoll  = false;
    if (!haveLastPoll) { for (int i=0;i<4;++i) lastPoll[i]=curMm[i]; haveLastPoll=true; return; }

    // Log only when at least one wheel changed since the previous poll — mirrors addSample's skip rule.
    bool anyChanged = false;
    for (int i=0;i<4;++i) if (std::abs(curMm[i] - lastPoll[i]) > 1e-6) { anyChanged = true; break; }

    // d[] is relative to the previous LOGGED row: that's what addSample() divides by dt.
    static double lastLogged[4] = {0.0, 0.0, 0.0, 0.0};
    if (!st.lastLoggedT) for (int i=0;i<4;++i) lastLogged[i]=curMm[i];

    double d[4] = { curMm[0]-lastLogged[0], curMm[1]-lastLogged[1], curMm[2]-lastLogged[2], curMm[3]-lastLogged[3] };

    for (int i=0;i<4;++i) lastPoll[i]=curMm[i];
    if (!anyChanged) return;   // nothing moved since previous poll — addSample would skip this too

    char path[MAX_PATH]; writePathTo(path);
    FILE* f = NULL; fopen_s(&f, path, "a");
    if (f) {
        fprintf(f, "%.4f   %8.3f     %-9d      %.5f  %+10.6f   %.5f  %+10.6f   %.5f  %+10.6f   %.5f  %+10.6f\n",
                timeSeconds - st.tStartSec,
                (timeSeconds - st.lastLoggedT) * 1000.0, /* wall-clock ms since previous logged row */
                gfx.delta_time_ms,
                curMm[0], d[0], curMm[1], d[1], curMm[2], d[2], curMm[3], d[3]);
        fclose(f);
    }

    for (int i=0;i<4;++i) lastLogged[i]=curMm[i];
    st.lastLoggedT = timeSeconds;
    ++st.samplesWritten;
}
// Resolved channel table for EVOM1 LIVE rows (mapping.json), built ONCE on first use.
// Unit conversions live in mapping.json, not here: this function only extracts RAW values
// from the shared-memory frames and lets FeedLapFromTable() apply factor/offset per row.
std::string g_acsEvoChannelTableWarning;
static LiveChannel::ResolvedChannelTable& AcsEvoGetChannelTable()
{
    static LiveChannel::ResolvedChannelTable table = [] {
        LiveChannel::ResolvedChannelTable t;
        t.build(ChannelProfile::EvoM1);   // EVOM1 + Generic rows
        return t;
    }();
    // One-time validation: surface (never fatal) any mapping row that failed to resolve.
    static bool validated = false;
    if (!validated) {
        std::string report;
        // Required raw channel names - a JSON/dict-key typo would otherwise cause
        // silent per-frame skips, so every name the dict below is built from must exist in mapping.json.
        const std::vector<std::string> required = [] {
            std::vector<std::string> v;
            v.push_back("physics.speedKmh");
            v.push_back("physics.rpms");
            v.push_back("physics.gas");
            v.push_back("physics.brake");
            v.push_back("physics.steerAngle");
            v.push_back("physics.localAngularVel[1]");
            v.push_back("physics.accG[0]");
            v.push_back("physics.accG[1]");
            for (int i = 0; i < 4; ++i) {
                char key[64];
                snprintf(key, sizeof(key), "physics.wheelAngularSpeed[%d]", i);   v.push_back(key);
                snprintf(key, sizeof(key), "physics.brakeTemp[%d]", i);           v.push_back(key);
                snprintf(key, sizeof(key), "physics.wheelsPressure[%d]", i);      v.push_back(key);
                snprintf(key, sizeof(key), "physics.brakeTorque[%d]", i);         v.push_back(key);
                snprintf(key, sizeof(key), "physics.slipRatio[%d]", i);           v.push_back(key);
                snprintf(key, sizeof(key), "physics.suspensionTravel[%d]", i);    v.push_back(key);
            }
            v.push_back("physics.tcinAction");
            v.push_back("physics.absInAction");
            v.push_back("physics.brakeBias");
            v.push_back("graphics.oil_temperature_c");
            v.push_back("physics.waterTemp");
            v.push_back("physics.roadTemp");
            v.push_back("physics.airTemp");
            v.push_back("physics.fuel");
            v.push_back("physics.rideHeight[0]");
            v.push_back("physics.rideHeight[1]");
            return v;
        }();
        table.validate(required, report);
        g_acsEvoChannelTableWarning = report.empty() ? "" : "AC EVO live channel table: " + report;
        validated = true;
    }
    return table;
}

// ---------------------------------------------------------------------------
// AcsEvoOpenSharedMemory
// ---------------------------------------------------------------------------
bool AcsEvoOpenSharedMemory(std::string& errorMessage)
{
    if (g_pPhysics != NULL && g_pGraphics != NULL) {
        return true; // already open
    }

    // Physics
    g_hPhysics = OpenFileMappingA(FILE_MAP_READ, FALSE, ACEVO_SM_PHYSICS);
    if (g_hPhysics == NULL)
    {
        errorMessage = "AC EVO physics shared memory not found. Is AC EVO running?";
        return false;
    }
    g_pPhysics = MapViewOfFile(g_hPhysics, FILE_MAP_READ, 0, 0, 0);
    if (g_pPhysics == NULL)
    {
        CloseHandle(g_hPhysics);
        g_hPhysics = NULL;
        errorMessage = "Failed to map AC EVO physics shared memory.";
        return false;
    }

    // Graphics
    g_hGraphics = OpenFileMappingA(FILE_MAP_READ, FALSE, ACEVO_SM_GRAPHICS);
    if (g_hGraphics == NULL)
    {
        errorMessage = "AC EVO graphics shared memory not found. Is AC EVO running?";
        UnmapViewOfFile(g_pPhysics);
        g_pPhysics = NULL;
        CloseHandle(g_hPhysics);
        g_hPhysics = NULL;
        return false;
    }
    g_pGraphics = MapViewOfFile(g_hGraphics, FILE_MAP_READ, 0, 0, 0);
    if (g_pGraphics == NULL)
    {
        CloseHandle(g_hGraphics);
        g_hGraphics = NULL;
        UnmapViewOfFile(g_pPhysics);
        g_pPhysics = NULL;
        CloseHandle(g_hPhysics);
        g_hPhysics = NULL;
        errorMessage = "Failed to map AC EVO graphics shared memory.";
        return false;
    }

    // Static (optional — not required for capture, but useful for metadata)
    g_hStatic = OpenFileMappingA(FILE_MAP_READ, FALSE, ACEVO_SM_STATIC);
    if (g_hStatic != NULL)
    {
        g_pStatic = MapViewOfFile(g_hStatic, FILE_MAP_READ, 0, 0, 0);
        if (g_pStatic == NULL)
        {
            CloseHandle(g_hStatic);
            g_hStatic = NULL;
        }
    }

    // Fresh connect: reset per-page change-detection so the first real frame from each
    // page counts as NewFrame again (plan §5). The "already open" early-return above does
    // NOT reach here, so an in-progress session is not disturbed.
    AcsEvoResetFrameTracking();

    return true;
}

// ============================================================================
// Frame-sync change-detection for AC EVO (plan §5). One last-processed token per
// page; -1 means "no frame processed yet" so the first real read always counts as
// new. Reset to -1 on connect / session reset via AcsEvoResetFrameTracking().
// ============================================================================

static int s_lastPhysPacket = -1;   // last-processed physics packetId (-1 = none)
static int s_lastGfxPacket  = -1;   // last-processed graphics packetId (-1 = none)

// Optional inter-cycle plausibility cross-check (plan §5 step 5). Enabled by default so a grossly
// implausible jump is treated as suspected torn data and skipped. Toggle at runtime via
// AcsEvoSetPlausibilityCheck() if false-positives appear on exotic content; the snapshot below holds
// the last *accepted* physics frame's guard fields for comparison.
static bool             s_plausibilityEnabled = true;  // flag guarding the cross-check (plan §5)
static SPageFilePhysics s_lastPhys{};                  // zeroed on connect/reset
static bool             s_haveLastPhys    = false;     // no accepted physics frame yet
static unsigned long    s_plausibilityRejects = 0;     // diagnostic: suspected torn frames skipped

void AcsEvoResetFrameTracking() {
    s_lastPhysPacket      = -1;
    s_lastGfxPacket       = -1;
    s_haveLastPhys        = false;   // first real frame after reset has nothing to compare against
    s_plausibilityRejects = 0;       // fresh session -> restart the diagnostic counter
}

void AcsEvoSetPlausibilityCheck(bool enabled) { s_plausibilityEnabled = enabled; }
bool   AcsEvoGetPlausibilityCheck()           { return s_plausibilityEnabled; }
unsigned long AcsEvoGetPlausibilityRejectCount() { return s_plausibilityRejects; }

// ---------------------------------------------------------------------------
// AcsEvoPlausible — inter-cycle plausibility cross-check (plan §5 step 5). Compares key guard
// channels of the current physics frame against the last accepted one and rejects a grossly
// implausible jump as suspected torn/corrupt data. Thresholds are deliberately generous: they exist
// only to catch half-written floats, not to model real physics — if a legitimate hard transient trips
// them, raise the limit (do NOT remove the check).
// ---------------------------------------------------------------------------
bool AcsEvoPlausible(const SPageFilePhysics& cur, const SPageFilePhysics& prev) {
    auto saneDelta = [](float a, float b, double maxStep) -> bool {
        return std::abs((double)a - (double)b) <= maxStep;   // compare in double for precision
    };
    if (!saneDelta(cur.speedKmh, prev.speedKmh, 30.0))       return false; // >30 km/h/tick implausible
    if (!saneDelta(cur.gas,     prev.gas,      1.5))          return false;
    if (!saneDelta(cur.brake,   prev.brake,    1.5))          return false;
    for (int i = 0; i < 4; ++i)                               // tyre core temps move slowly
        if (!saneDelta(cur.tyreCoreTemperature[i], prev.tyreCoreTemperature[i], 20.0)) return false;
    if (std::abs((double)(cur.gear - prev.gear)) > 3.0)       return false; // gear blip cap ±3
    return true;
}

// ---------------------------------------------------------------------------
// Internal page readers. Each reads one shared-memory page with a packetId double-
// check to detect torn writes (plan §2). Returns true if the read is consistent,
// false on failure/torn-read. Not exposed publicly: use AcsEvoReadFrame() which
// combines both pages and applies change-detection in a single call so LiveTimer
// processes at most one fresh frame per tick.
// ---------------------------------------------------------------------------
static bool ReadPhysicsPage(SPageFilePhysics& out)
{
    if (g_pPhysics == NULL) return false;

    const SPageFilePhysics* sm = static_cast<const SPageFilePhysics*>(g_pPhysics);

    int id1 = sm->packetId;
    memcpy(&out, sm, sizeof(SPageFilePhysics));
    int id2 = sm->packetId;

    // Torn read: packetId changed between the two reads. Discard and retry next tick.
    return (id1 == id2);
}

static bool ReadGraphicsPage(SPageFileGraphicEvo& out)
{
    if (g_pGraphics == NULL) return false;

    const SPageFileGraphicEvo* sm = static_cast<const SPageFileGraphicEvo*>(g_pGraphics);

    int id1 = sm->packetId;
    memcpy(&out, sm, sizeof(SPageFileGraphicEvo));
    int id2 = sm->packetId;

    // Torn read: packetId changed between the two reads. Discard and retry next tick.
    return (id1 == id2);
}

// ---------------------------------------------------------------------------
// AcsEvoReadFrame — combined physics + graphics read with per-page change-
// detection (plan §5). Reads both pages, applies a torn-check to each, then treats
// the frame as NewFrame only when at least one page advanced past its last-processed
// token. Pages that failed their torn-check this tick simply contribute nothing and
// are retried next tick; tokens for them are NOT committed (plan §5). A physics frame
// rejected by the plausibility cross-check is likewise uncommitted/unfed, but it does
// NOT block graphics acceptance — gfx-driven lap/pit detection must keep running.
// ---------------------------------------------------------------------------
// physUsableOut: set to false when a fresh physics page failed the plausibility cross-check — its values must NOT be fed into lap stats, but graphics is still accepted and processed normally.
EvoReadStatus AcsEvoReadFrame(SPageFilePhysics& physOut, SPageFileGraphicEvo& gfxOut, bool* physUsableOut)
{
    if (physUsableOut != NULL) *physUsableOut = true;

    if (g_pPhysics == NULL && g_pGraphics == NULL) {
        return EvoReadStatus::NoNewData;   // nothing mapped / not connected
    }

    const bool physOk = ReadPhysicsPage(physOut);
    const bool gfxOk  = ReadGraphicsPage(gfxOut);

    if (!physOk && !gfxOk) {
        // Both pages failed their torn-check (or are unmapped). Nothing usable this tick.
        return EvoReadStatus::TornRetry;
    }

    const int physId = physOk ? physOut.packetId : -1;
    const int gfxId  = gfxOk  ? gfxOut.packetId  : -1;

    // Per-page change-detection: a page is "new" only if it read OK AND its token has
    // advanced past the last one we processed (packetIds are monotonically increasing).
    const bool physNew = physOk && (physId != s_lastPhysPacket);
    const bool gfxNew  = gfxOk && (gfxId != s_lastGfxPacket);

    if (!physNew && !gfxNew) {
        // Both pages are consistent but neither advanced -> we already processed this data.
        return EvoReadStatus::NoNewData;
    }

    // Optional inter-cycle plausibility cross-check (plan §5 step 5): a grossly implausible physics frame is
    // treated as suspected torn/corrupt data. Its values must not be fed into the lap, BUT it no longer blocks
    // the whole tick: graphics commits independently below so gfx-driven detection (pit-exit / line-crossing via
    // car_location + npos) keeps firing even when a physics spike is rejected. The token IS committed for both
    // pages — otherwise every later frame would be re-compared against this stale snapshot and the rejection
    // loop would lock out NewFrame forever (this was exactly what killed pit-exit / line-crossing detection).
    bool physRejected = false;   // fresh physics page failed plausibility -> its values must not be fed into lap stats
    if (s_plausibilityEnabled && physOk && s_haveLastPhys) {
        if (!AcsEvoPlausible(physOut, s_lastPhys)) {
            ++s_plausibilityRejects;   // diagnostic: count suspected torn frames
            physRejected = true;       // values excluded from lap data ONLY — token still committed below (see comment above)
        }
    }

    // Commit only the tokens for pages that produced fresh, consistent data so a page that failed its
    // torn-check is retried on the next tick (plan §5). Also refresh the plausibility snapshot with this
    // accepted physics frame. A plausibility-rejected frame still commits: it IS a real sim write we saw —
    // holding back its token would make every later frame compare against stale data and lock out NewFrame forever.
    if (physOk) { s_lastPhysPacket = physId; s_lastPhys = physOut; s_haveLastPhys = true; }
    if (gfxOk)  s_lastGfxPacket   = gfxId;

    // Default (set at entry) is true. Mark unusable when the physics page was rejected above OR failed its torn-check — in both cases physOut may hold stale/half-written values that must not be fed into lap stats, while gfx-based detection runs normally on this same tick.
    if ((physRejected || !physOk) && physUsableOut != NULL) *physUsableOut = false;

    return EvoReadStatus::NewFrame;
}

// ---------------------------------------------------------------------------
// AcsEvoReadStatic
// ---------------------------------------------------------------------------
bool AcsEvoReadStatic(SPageFileStaticEvo& out)
{
    if (g_pStatic == NULL)
        return false;

    const SPageFileStaticEvo* sm = static_cast<const SPageFileStaticEvo*>(g_pStatic);
    memcpy(&out, sm, sizeof(SPageFileStaticEvo));
    return true;
}

// ---------------------------------------------------------------------------
// AcsEvoCloseSharedMemory
// ---------------------------------------------------------------------------
void AcsEvoCloseSharedMemory()
{
    if (g_pPhysics != NULL)
    {
        UnmapViewOfFile(g_pPhysics);
        g_pPhysics = NULL;
    }
    if (g_hPhysics != NULL)
    {
        CloseHandle(g_hPhysics);
        g_hPhysics = NULL;
    }
    if (g_pGraphics != NULL)
    {
        UnmapViewOfFile(g_pGraphics);
        g_pGraphics = NULL;
    }
    if (g_hGraphics != NULL)
    {
        CloseHandle(g_hGraphics);
        g_hGraphics = NULL;
    }
    if (g_pStatic != NULL)
    {
        UnmapViewOfFile(g_pStatic);
        g_pStatic = NULL;
    }
    if (g_hStatic != NULL)
    {
        CloseHandle(g_hStatic);
        g_hStatic = NULL;
    }
}

// ---------------------------------------------------------------------------
// AcsEvoIsOnTrack / AcsEvoIsInPits
// ---------------------------------------------------------------------------
bool AcsEvoIsOnTrack(const SPageFileGraphicEvo& gfx)
{
    // Relaxed: if car_location is TRACK, we are on track regardless of AC_LIVE status.
    // This prevents "Waiting for car to leave pits" hangs if the game reports a non-LIVE status while driving.
    return (gfx.car_location == ACEVO_TRACK);
}

bool AcsEvoIsInPits(const SPageFileGraphicEvo& gfx)
{
    return (gfx.car_location == ACEVO_PITLANE) ||
           (gfx.car_location == ACEVO_PITENTRY) ||
           (gfx.car_location == ACEVO_PITEXIT) ||
           gfx.is_in_pit_box;
}

// ---------------------------------------------------------------------------
// AcsEvoFeedLap — feed one frame into LapStats accumulators (table-driven)
// ---------------------------------------------------------------------------
// Unit conversions live in mapping.json EVOM1 LIVE rows; this function only
// extracts RAW values from the physics/graphics frames. Pre-computed percent
// values (fuel, TCS/ABS) are fed as 0..100 so f=1 ratio rows land in percent,
// matching old C++ behaviour exactly.
// ---------------------------------------------------------------------------

void AcsEvoFeedLap(LapStats& lap, const SPageFilePhysics& phys,
                   const SPageFileGraphicEvo& gfx, double timeSeconds, SessionRecorder* rec)
{
    LiveChannel::ResolvedChannelTable& table = AcsEvoGetChannelTable();

    AcsEvoDumpRawDiagOnce(phys, gfx);   // one-time raw-value dump -> ac_evo_diag.txt (delete after diagnosis)
    AcsEvoDumpTravelDiag(phys, gfx, timeSeconds);  // travel sample trace -> ac_evo_travel_diag.txt (damper histogram investigation)


    // --- Raw channel dictionary (RAW values only - no unit conversion here). ---
    // Keys MUST match the "channel" field of mapping.json EVOM1 LIVE rows EXACTLY.
    std::unordered_map<std::string, double> raw;
    raw["physics.speedKmh"]            = phys.speedKmh;                    // km/h (Vehicle_Speed)
    raw["physics.rpms"]                = phys.rpms;                        // RPM
    raw["physics.gas"]                 = phys.gas;                         // 0..1 -> % x100 via table
    raw["physics.brake"]               = phys.brake;                       // 0..1 -> % x100 via table (Brake_Pos)
    raw["physics.steerAngle"]          = phys.steerAngle;                  // -1..1 -> deg x360 via table
    raw["physics.localAngularVel[1]"]  = phys.localAngularVel[1];          // rad/s -> deg/s via table (Yaw_Rate)
    raw["physics.accG[0]"]             = phys.accG[0];                     // g direct (lat)
    raw["physics.accG[1]"]             = phys.accG[1];                     // g direct (long)
    for (int i = 0; i < 4; ++i) {   // wheel order: FL=0, FR=1, RL=2, RR=3
        char key[64];
        snprintf(key, sizeof(key), "physics.wheelAngularSpeed[%d]", i); raw[key] = phys.wheelAngularSpeed[i];  // rad/s -> km/h via table factor (r=0.33m)
        snprintf(key, sizeof(key), "physics.brakeTemp[%d]", i);         raw[key] = phys.brakeTemp[i];          // C
        snprintf(key, sizeof(key), "physics.wheelsPressure[%d]", i);    raw[key] = phys.wheelsPressure[i];     // PSI direct (f=1)
        snprintf(key, sizeof(key), "physics.brakeTorque[%d]", i);       raw[key] = phys.brakeTorque[i];        // Nm
        snprintf(key, sizeof(key), "physics.slipRatio[%d]", i);         raw[key] = phys.slipRatio[i];          // ratio -> % x100 via table (Wheel_Slip_Ratio_*)
        snprintf(key, sizeof(key), "physics.suspensionTravel[%d]", i);  raw[key] = phys.suspensionTravel[i];   // m -> mm x1000 via table
    }
    raw["physics.tcinAction"]          = phys.tcinAction ? 100.0 : 0.0;     // pre-computed % (TCS_In_Action, f=1)
    raw["physics.absInAction"]         = phys.absInAction ? 100.0 : 0.0;    // pre-computed % (ABS_In_Action, f=1)
    raw["physics.brakeBias"]           = phys.brakeBias;                    // 0..1 -> % x100 via table
    raw["graphics.oil_temperature_c"]  = gfx.oil_temperature_c;             // C (oil temp lives on the graphics page)
    raw["physics.waterTemp"]           = phys.waterTemp;                    // C
    raw["physics.roadTemp"]            = phys.roadTemp;                     // C (track surface)
    raw["physics.airTemp"]             = phys.airTemp;                      // C ambient
    raw["physics.fuel"]                = phys.fuel * 100.0;                 // % - pre-computed so the ratio row lands in percent, matching old C++
    raw["physics.rideHeight[0]"]       = phys.rideHeight[0];               // m -> cm x100 via table (aeroHeight0)
    raw["physics.rideHeight[1]"]        = phys.rideHeight[1];               // m -> cm x100 via table (aeroHeight1)

    // Prediction: tyre wear, fuel capacity, pad/disc life remaining.
    for (int i = 0; i < 4; ++i) {
        char key[64];
        snprintf(key, sizeof(key), "physics.tyreWear[%d]", i);   raw[key] = phys.tyreWear[i];     // native ratio -> % via table if needed
        snprintf(key, sizeof(key), "physics.padLife[%d]", i);    raw[key] = phys.padLife[i];      // 0..1 life remaining (decreases)
        snprintf(key, sizeof(key), "physics.discLife[%d]", i);   raw[key] = phys.discLife[i];     // 0..1 life remaining (decreases)
    }
    raw["graphics.max_fuel"]           = gfx.max_fuel;                        // L tank capacity

    // Per-band tyre temps: AC EVO writes these ONLY on the graphics page, not physics.
    // The physics arrays (tyreTempI/M/O) are always 0.0f in practice.
    raw["graphics.tyre_lf.temp_left"]   = gfx.tyre_lf.tyre_temperature_left;   // C
    raw["graphics.tyre_lf.temp_center"]  = gfx.tyre_lf.tyre_temperature_center;
    raw["graphics.tyre_lf.temp_right"]   = gfx.tyre_lf.tyre_temperature_right;
    raw["graphics.tyre_rf.temp_left"]   = gfx.tyre_rf.tyre_temperature_left;   // C
    raw["graphics.tyre_rf.temp_center"]  = gfx.tyre_rf.tyre_temperature_center;
    raw["graphics.tyre_rf.temp_right"]   = gfx.tyre_rf.tyre_temperature_right;
    raw["graphics.tyre_lr.temp_left"]   = gfx.tyre_lr.tyre_temperature_left;   // C
    raw["graphics.tyre_lr.temp_center"]  = gfx.tyre_lr.tyre_temperature_center;
    raw["graphics.tyre_lr.temp_right"]   = gfx.tyre_lr.tyre_temperature_right;
    raw["graphics.tyre_rr.temp_left"]   = gfx.tyre_rr.tyre_temperature_left;   // C
    raw["graphics.tyre_rr.temp_center"]  = gfx.tyre_rr.tyre_temperature_center;
    raw["graphics.tyre_rr.temp_right"]   = gfx.tyre_rr.tyre_temperature_right;

    if (rec) rec->feed(table, raw, timeSeconds);   // LD export buffer (optional)
    // --- Generic table-driven feed: v = raw * factor + offset -> LapStats sink. ---
    LiveChannel::FeedLapFromTable(lap, raw, table, timeSeconds);
    lap.rideHeightUnit = RideHeightUnit::Centimeters;   // aeroHeight0/1 arrive in cm (table x10 from m) - identical to old C++ path

    // --- Damper stats: addSample derives shaft velocity from travel deltas.
    // Only changed readings (new game frames) produce a sample; stale polls are skipped entirely.
    // dt = QPC wall-clock since the previous new frame ≈ one physics step at 270 Hz sim rate. ---
    double travelLF = phys.suspensionTravel[0] * 1000.0;   // m -> mm
    double travelRF = phys.suspensionTravel[1] * 1000.0;
    double travelLR = phys.suspensionTravel[2] * 1000.0;
    double travelRR = phys.suspensionTravel[3] * 1000.0;
    lap.damperLF.addSample(travelLF, timeSeconds);
    lap.damperRF.addSample(travelRF, timeSeconds);
    lap.damperLR.addSample(travelLR, timeSeconds);
    lap.damperRR.addSample(travelRR, timeSeconds);

    // --- Handling sample (understeer/oversteer detection) - multi-value derived sample. ---
    double steer = phys.steerAngle * 360.0;
    double yaw   = phys.localAngularVel[1] * DEG_PER_RAD;
    double latG  = phys.accG[0];
    double longG = phys.accG[1];
    double brake     = phys.brake * 100.0;
    double throttle  = phys.gas * 100.0;
    double suspAsymmetry = (travelLF - travelRF) / 2.0;
    lap.addHandlingSample(steer, yaw, latG, longG, brake, throttle, suspAsymmetry, phys.speedKmh);   // km/h direct

    // --- Steer/Yaw correlation ---
    lap.addSteerYaw(steer, yaw);

    // --- Row count ---
    lap.rowCount++;
}

std::string AcsEvoGetChannelTableWarning() { return g_acsEvoChannelTableWarning; }
