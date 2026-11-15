//---------------------------------------------------------------------------
// test_pmr_feed.cpp — Gate 3 standalone self-test for the PMR feed + metadata.
//
// Verifies (per docs/PMR_UDP_LIVE_CAPTURE_PLAN.md, GATE 3):
//   F1: ~60 consecutive decoded telemetry frames fed through PmrFeedLap() into a
//       fresh LapStats — key averages printed and checked for non-zero + plausibility.
//   F2: Direct-velocity histograms (suspVel*) have totalCount == frame count, with
//       means matching the builder's damperVelocity inputs in mm/s.
//   A1: buildDataProviderFromLap() resolves EVERY alias from plan Step 3.9 to a
//       non-empty value — proving PMR feeds the rules exactly like AMS2/EVO do,
//       with zero mapping.json changes.
//   M1: Metadata accessors (PmrGetTrackName / PmrGetDriverName / PmrGetCurrentSector
//       / PmrIsInPits) return correct values after a real UDP exchange of RaceInfo +
//       ParticipantState datagrams, and sane defaults for unknown vehicle ids.
//
// Build with bcc32c:
//   call "C:\Program Files (x86)\Embarcadero\Studio\23.0\bin\rsvars.bat"
//   cd scripts
//   bcc32c -I.. -o test_pmr_feed.exe ..\PmrLiveCapture.cpp test_pmr_feed.cpp ws_32.lib
//---------------------------------------------------------------------------

#include <winsock2.h>          // MUST precede windows.h (SOCKET type)
#pragma comment(lib, "Ws2_32.lib")
#include <windows.h>           // Sleep / CreateThread / GetTickCount

#include <cstdio>
#include <cstring>
#include <cmath>
#include <vector>

#include "pmr_test_common.h"   // Writer + packet builders (identical wire data to Gates 1 & 2)
#include "RuleConfig.h"        // loadChannelMappings() — needed before PmrGetChannelTable's one-shot build


// Satisfy the extern in MotecSetupAnalyzer.h (pulled transitively via PmrLiveCapture.h).
Thresholds g_thresholds = Thresholds();

static int s_failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { ++s_failures; std::printf("FAIL line %d: %s\n", __LINE__, #cond); } \
} while(0)


// ===========================================================================
// F1 + F2 — feed ~60 frames through PmrFeedLap, check averages & histograms.
// Expected values derived from pmr_test_common.h builder inputs (constant per frame):
//   speed 55.1 m/s -> 198.36 km/h | rpm 9500 | steer -0.18 rad -> -10.29 deg
//   yawRate -0.1 rad/s -> -5.73 deg/s | latG 4.5/9.81 = +0.4587 g
//   longG -6.2/9.81 = -0.6320 g | wheelSpeeds 216/234/252/270 km/h
//   brake temps 450/470/490/510 C | pressures (bar*14.5038) ~34.81..36.98 PSI
//   tyre temps LF[90,85,98] RF[100,95,108] LR[110,105,118] RR[120,115,128] C
//   wheelSlip 87/88/89/90 % | oil 145 C | water 108 C | fuel 82.4/148 = 55.68%
//   brakeBiasSetting 51% | tcsActive 100% (flag=1) | absActive 100% (byte=1)
//   suspVel* means -150/-170/-190/-210 mm/s, totalCount == frame count each.
// ===========================================================================

static const int kFrames = 60;

static void testFeed() {
    std::printf("--- F1: feed %d frames through PmrFeedLap ---\n", kFrames);

    // Build one datagram, decode it once into a packet (decode is pure — reusing the
    // same decoded packet per frame keeps this deterministic and fast).
    Writer w; buildVehicleTelemetry(w);   // vehicleId=7, 4 wheels, all blocks populated
    PmrVehicleTelemetryPacket pkt{};
    CHECK(PmrDecodeVehicleTelemetry(w.data(), w.len(), pkt));

    LapStats lap = LapStats();            // fresh — no prior session state
    for (int i = 0; i < kFrames; ++i) {
        const double tSec = static_cast<double>(i) * 0.1;   // dt=0.1 s so damper addSample() derives dTravel/dt
        PmrFeedLap(lap, pkt, tSec);
    }

    CHECK(lap.rowCount == kFrames);       // every frame counted (wheels.size()==4 guard passed)

    std::printf("   rowCount          = %zu\n", lap.rowCount);
    std::printf("   speed             = %.2f km/h  (expect ~198.36)\n", lap.speed.average());
    CHECK(std::fabs(lap.speed.average() - 55.1 * 3.6) < 0.01);

    std::printf("   rpm               = %.1f     (expect 9500)\n", lap.rpm.average());
    CHECK(std::fabs(lap.rpm.average() - 9500.0) < 1.0);

    const double kDegPerRad = 180.0 / 3.14159265358979;
    std::printf("   steerAngle        = %.3f deg (expect ~-10.29)\n", lap.steerAngle.average());
    CHECK(std::fabs(lap.steerAngle.average() - (-0.18 * kDegPerRad)) < 0.05);

    std::printf("   yawRate           = %.3f deg/s (expect ~-5.7296)\n", lap.yawRate.average());
    CHECK(std::fabs(lap.yawRate.average() - (-0.1 * kDegPerRad)) < 0.05);

    std::printf("   latG              = %.4f g (expect ~+0.4587)\n", lap.latG.average());
    CHECK(std::fabs(lap.latG.average() - (4.5 / 9.81)) < 0.002);

    std::printf("   longG             = %.4f g (expect ~-0.6320)\n", lap.longG.average());
    CHECK(std::fabs(lap.longG.average() - (-6.2 / 9.81)) < 0.002);

    std::printf("   throttle          = %.1f %% (expect 72)\n", lap.throttle.average());
    CHECK(std::fabs(lap.throttle.average() - 72.0) < 0.5);
    std::printf("   brakePedal        = %.1f %% (expect 5)\n", lap.brakePedal.average());
    CHECK(std::fabs(lap.brakePedal.average() - 5.0) < 0.5);

    // Wheel speeds: linearSpeed m/s * 3.6 -> km/h per corner.
    const double wsExp[4] = {216.0, 234.0, 252.0, 270.0};   // (60+idx*5) * 3.6
    std::printf("   wheelSpeeds       = %.1f / %.1f / %.1f / %.1f km/h\n",
                lap.wheelSpeedLF.average(), lap.wheelSpeedRF.average(),
                lap.wheelSpeedLR.average(), lap.wheelSpeedRR.average());
    CHECK(std::fabs(lap.wheelSpeedLF.average() - wsExp[0]) < 0.5);
    CHECK(std::fabs(lap.wheelSpeedRF.average() - wsExp[1]) < 0.5);
    CHECK(std::fabs(lap.wheelSpeedLR.average() - wsExp[2]) < 0.5);
    CHECK(std::fabs(lap.wheelSpeedRR.average() - wsExp[3]) < 0.5);

    // Brake temps: C direct per corner (450 + idx*20).
    const double btExp[4] = {450.0, 470.0, 490.0, 510.0};
    std::printf("   brakeTemps        = %.1f / %.1f / %.1f / %.1f C\n",
                lap.brakeTempLF.average(), lap.brakeTempRF.average(),
                lap.brakeTempLR.average(), lap.brakeTempRR.average());
    CHECK(std::fabs(lap.brakeTempLF.average() - btExp[0]) < 0.5);
    CHECK(std::fabs(lap.brakeTempRF.average() - btExp[1]) < 0.5);
    CHECK(std::fabs(lap.brakeTempLR.average() - btExp[2]) < 0.5);
    CHECK(std::fabs(lap.brakeTempRR.average() - btExp[3]) < 0.5);

    // Tyre pressures: bar * 14.5038 -> PSI (2.40 + idx*0.05).
    const double kBarToPsi = 14.5038;
    std::printf("   pressures         = %.2f / %.2f / %.2f / %.2f PSI\n",
                lap.pressureLF.average(), lap.pressureRF.average(),
                lap.pressureLR.average(), lap.pressureRR.average());
    CHECK(std::fabs(lap.pressureLF.average() - 2.40 * kBarToPsi) < 0.1);
    CHECK(std::fabs(lap.pressureRF.average() - 2.45 * kBarToPsi) < 0.1);
    CHECK(std::fabs(lap.pressureLR.average() - 2.50 * kBarToPsi) < 0.1);
    CHECK(std::fabs(lap.pressureRR.average() - 2.55 * kBarToPsi) < 0.1);

    // Tyre temps: tread vector [inner, middle, outer] per wheel (base = 90 + idx*10).
    const double ttExp[4][3] = {   //            inner mid outer
        /*LF*/ { 90.0, 85.0, 98.0 },
        /*RF*/ {100.0, 95.0, 108.0},
        /*LR*/ {110.0,105.0, 118.0},
        /*RR*/ {120.0,115.0, 128.0} };

    std::printf("   tyre temps LF     = %.1f / %.1f / %.1f C\n",
                lap.tyreInnerTempLF.average(), lap.tyreCentreTempLF.average(), lap.tyreOuterTempLF.average());
    std::printf("   tyre temps RF     = %.1f / %.1f / %.1f C\n",
                lap.tyreInnerTempRF.average(), lap.tyreCentreTempRF.average(), lap.tyreOuterTempRF.average());
    std::printf("   tyre temps LR     = %.1f / %.1f / %.1f C\n",
                lap.tyreInnerTempLR.average(), lap.tyreCentreTempLR.average(), lap.tyreOuterTempLR.average());
    std::printf("   tyre temps RR     = %.1f / %.1f / %.1f C\n",
                lap.tyreInnerTempRR.average(), lap.tyreCentreTempRR.average(), lap.tyreOuterTempRR.average());

    CHECK(std::fabs(lap.tyreInnerTempLF.average()  - ttExp[0][0]) < 0.5);
    CHECK(std::fabs(lap.tyreCentreTempLF.average() - ttExp[0][1]) < 0.5);
    CHECK(std::fabs(lap.tyreOuterTempLF.average()  - ttExp[0][2]) < 0.5);
    CHECK(std::fabs(lap.tyreInnerTempRF.average()  - ttExp[1][0]) < 0.5);
    CHECK(std::fabs(lap.tyreCentreTempRF.average() - ttExp[1][1]) < 0.5);
    CHECK(std::fabs(lap.tyreOuterTempRF.average()  - ttExp[1][2]) < 0.5);
    CHECK(std::fabs(lap.tyreInnerTempLR.average()  - ttExp[2][0]) < 0.5);
    CHECK(std::fabs(lap.tyreCentreTempLR.average() - ttExp[2][1]) < 0.5);
    CHECK(std::fabs(lap.tyreOuterTempLR.average()  - ttExp[2][2]) < 0.5);
    CHECK(std::fabs(lap.tyreInnerTempRR.average()  - ttExp[3][0]) < 0.5);
    CHECK(std::fabs(lap.tyreCentreTempRR.average() - ttExp[3][1]) < 0.5);
    CHECK(std::fabs(lap.tyreOuterTempRR.average()  - ttExp[3][2]) < 0.5);

    // Wheel slip: ratio * 100 -> % (0.87 + idx*0.01).
    std::printf("   wheelSlip         = %.1f / %.1f / %.1f / %.1f %%\n",
                lap.wheelSlipLF.average(), lap.wheelSlipRF.average(),
                lap.wheelSlipLR.average(), lap.wheelSlipRR.average());
    CHECK(std::fabs(lap.wheelSlipLF.average() - 87.0) < 0.5);
    CHECK(std::fabs(lap.wheelSlipRF.average() - 88.0) < 0.5);
    CHECK(std::fabs(lap.wheelSlipLR.average() - 89.0) < 0.5);
    CHECK(std::fabs(lap.wheelSlipRR.average() - 90.0) < 0.5);

    // Thermal / fuel / setup flags.
    std::printf("   oilTemp           = %.1f C (expect 145)\n", lap.oilTemp.average());
    CHECK(std::fabs(lap.oilTemp.average() - 145.0) < 0.5);

    std::printf("   waterTemp         = %.1f C (expect 108)\n", lap.waterTemp.average());
    CHECK(std::fabs(lap.waterTemp.average() - 108.0) < 0.5);

    std::printf("   fuelLevel         = %.2f %% (expect ~55.68)\n", lap.fuelLevel.average());
    CHECK(std::fabs(lap.fuelLevel.average() - (82.4 / 148.0 * 100.0)) < 0.1);

    std::printf("   brakeBiasSetting  = %.1f %% (expect 51)\n", lap.brakeBiasSetting.average());
    CHECK(std::fabs(lap.brakeBiasSetting.average() - 51.0) < 0.5);

    std::printf("   tcsActive         = %.1f %% (expect 100 — flag set in builder)\n", lap.tcsActive.average());
    CHECK(std::fabs(lap.tcsActive.average() - 100.0) < 0.5);

    std::printf("   absActive         = %.1f %% (expect 100 — byte=1 in builder)\n", lap.absActive.average());
    CHECK(std::fabs(lap.absActive.average() - 100.0) < 0.5);

    // F2: direct-velocity damper histograms — PMR's key advantage over AMS2/EVO.
    const double dvExp[4] = {-150.0, -170.0, -190.0, -210.0};   // m/s * 1000 -> mm/s
    std::printf("   suspVelLF         count=%zu mean=%.1f (expect %d / %.1f)\n",
                lap.suspVelLF.totalCount, lap.suspVelLF.mean(), kFrames, dvExp[0]);
    std::printf("   suspVelRF         count=%zu mean=%.1f (expect %d / %.1f)\n",
                lap.suspVelRF.totalCount, lap.suspVelRF.mean(), kFrames, dvExp[1]);
    std::printf("   suspVelLR         count=%zu mean=%.1f (expect %d / %.1f)\n",
                lap.suspVelLR.totalCount, lap.suspVelLR.mean(), kFrames, dvExp[2]);
    std::printf("   suspVelRR         count=%zu mean=%.1f (expect %d / %.1f)\n",
                lap.suspVelRR.totalCount, lap.suspVelRR.mean(), kFrames, dvExp[3]);

    CHECK(lap.suspVelLF.totalCount == static_cast<std::size_t>(kFrames));   // |v| < 500 mm/s -> no rejections
    CHECK(lap.suspVelRF.totalCount == static_cast<std::size_t>(kFrames));
    CHECK(lap.suspVelLR.totalCount == static_cast<std::size_t>(kFrames));
    CHECK(lap.suspVelRR.totalCount == static_cast<std::size_t>(kFrames));

    CHECK(std::fabs(lap.suspVelLF.mean() - dvExp[0]) < 1.0);
    CHECK(std::fabs(lap.suspVelRF.mean() - dvExp[1]) < 1.0);
    CHECK(std::fabs(lap.suspVelLR.mean() - dvExp[2]) < 1.0);
    CHECK(std::fabs(lap.suspVelRR.mean() - dvExp[3]) < 1.0);

    // Travel-derived fallback histograms: constant travel per frame -> dTravel/dt = 0,
    // so their means are ~0 (harmless — pickDamperHistogram prefers suspVel* when non-empty).
    std::printf("   damperLF.fallback count=%zu mean=%.2f mm/s (travel-derived; direct takes precedence)\n",
                lap.damperLF.histogram.totalCount, lap.damperLF.histogram.mean());

    // ---- A1: DataProvider alias resolution (plan Step 3.9 checklist) --------
    std::printf("--- A1: buildDataProviderFromLap() resolves every rule alias ---\n");

    const LapStats& ref = lap;   // same object, just a named reference for clarity below
    DataProvider provider = buildDataProviderFromLap(ref);

    struct AliasCheck { AnalysisAlias alias; double expected; };
    static const AliasCheck kChecks[] = {
        // EngineTemperatureRule inputs:
        {AnalysisAlias::WaterTemp,          108.0},
        {AnalysisAlias::OilTemp,            145.0},
        // HandlingBalanceRule inputs (note: Front=Rear aliases are LF/LR only per buildDataProviderFromLap):
        {AnalysisAlias::SteerAngleAvg,      -0.18 * kDegPerRad},
        {AnalysisAlias::YawRateAvg,         -0.1  * kDegPerRad},
        {AnalysisAlias::WheelSlipFrontAvg,   87.0},    // = wheelSlipLF (not a front average)
        {AnalysisAlias::WheelSlipRearAvg,    89.0},    // = wheelSlipLR
        // Tyre pressure inputs (per-wheel):
        {AnalysisAlias::TyrePressFL,         2.40 * kBarToPsi},
        {AnalysisAlias::TyrePressFR,         2.45 * kBarToPsi},
        {AnalysisAlias::TyrePressRL,         2.50 * kBarToPsi},
        {AnalysisAlias::TyrePressRR,         2.55 * kBarToPsi},
        // TyreTemperatureRule inputs — all twelve:
        {AnalysisAlias::TyreTempFLInner,     ttExp[0][0]},
        {AnalysisAlias::TyreTempFLMiddle,    ttExp[0][1]},
        {AnalysisAlias::TyreTempFLOuter,     ttExp[0][2]},
        {AnalysisAlias::TyreTempFRInner,     ttExp[1][0]},
        {AnalysisAlias::TyreTempFRMiddle,    ttExp[1][1]},
        {AnalysisAlias::TyreTempFROuter,     ttExp[1][2]},
        {AnalysisAlias::TyreTempRLInner,     ttExp[2][0]},
        {AnalysisAlias::TyreTempRLMiddle,    ttExp[2][1]},
        {AnalysisAlias::TyreTempRLOuter,     ttExp[2][2]},
        {AnalysisAlias::TyreTempRRInner,     ttExp[3][0]},
        {AnalysisAlias::TyreTempRRMiddle,    ttExp[3][1]},
        {AnalysisAlias::TyreTempRROuter,     ttExp[3][2]}
    };

    int resolved = 0;
    for (size_t i = 0; i < sizeof(kChecks) / sizeof(kChecks[0]); ++i) {
        double v = -1.0e9;   // sentinel far outside any plausible telemetry value
        bool ok = provider.getValue(kChecks[i].alias, v);
        if (!ok || std::fabs(v - kChecks[i].expected) > 0.5) {
            ++s_failures;
            std::printf("FAIL alias %zu: resolved=%d value=%.3f expected~%.2f\n",
                        i + 1, ok ? 1 : 0, v, kChecks[i].expected);
        } else {
            ++resolved;
        }
    }
    const size_t totalAliases = sizeof(kChecks) / sizeof(kChecks[0]);
    std::printf("   %zu/%zu rule aliases resolved to plausible values\n", (size_t)resolved, totalAliases);
    CHECK(resolved == static_cast<int>(totalAliases));

    // Rake is NOT in the Step 3.9 checklist: PMR has no ride-height channel so the four
    // RideHeight* aliases stay empty and the virtual Rake cannot resolve — documented
    // acceptable degradation (plan Step 3.7). Assert it does not crash when queried.
    double rake = -1.0e9;
    bool haveRake = provider.getValue(AnalysisAlias::Rake, rake);   // expected false for PMR laps
    std::printf("   Rake resolves=%d (expected 0 — no ride-height channel on the wire)\n", haveRake ? 1 : 0);
}


// ===========================================================================
// M1 — metadata accessors over a real UDP exchange (mirrors Gate-2 S4 pattern).
// Sender thread emits RaceInfo + ParticipantState(42); main drains and checks the
// four Phase-3 accessors against known builder values, plus unknown-id defaults.
// ===========================================================================

static const int kTestPort = 7591;   // distinct from Gate 2's port — no cross-test interference if both run concurrently

static SOCKET s_senderSock    = INVALID_SOCKET;
static volatile LONG s_stopSender = 0;
static std::vector<uint8_t> s_pktRaceInfo, s_pktParticipant;


// One burst: RaceInfo then ParticipantState. ~50 ms cadence => a handful of bursts in the drain window.
static DWORD WINAPI SenderProc(LPVOID) {
    sockaddr_in dst = {};
    dst.sin_family      = AF_INET;
    dst.sin_addr.s_addr = htonl(INADDR_LOOPBACK);   // 127.0.0.1 — unicast loopback, no multicast needed
    dst.sin_port        = htons((u_short)kTestPort);

    const struct sockaddr* dstAddr = reinterpret_cast<const struct sockaddr*>(&dst);   // C++: no implicit derived->base
    while (!InterlockedCompareExchange(&s_stopSender, 0, 0)) {
        sendto(s_senderSock, (const char*)s_pktRaceInfo.data(),      (int)s_pktRaceInfo.size(),     0, dstAddr, sizeof(dst));
        sendto(s_senderSock, (const char*)s_pktParticipant.data(),   (int)s_pktParticipant.size(),  0, dstAddr, sizeof(dst));
        Sleep(50);
    }
    return 0;
}


static void testMetadata() {
    std::printf("--- M1: metadata accessors over live UDP ---\n");

    // Build the two datagrams once (identical wire bytes to Gate 2).
    { Writer w; buildRaceInfo(w);           s_pktRaceInfo.assign(w.data(), w.data() + w.len()); }
    std::printf("   raceInfo %zu B\n",      s_pktRaceInfo.size());
    { Writer w; buildParticipantState(w, /*vehicleId=*/42, /*isPlayer=*/true);
               s_pktParticipant.assign(w.data(), w.data() + w.len()); }
    std::printf("   participant(42) %zu B\n", s_pktParticipant.size());

    // Unknown-id defaults BEFORE any packet has arrived (fresh process state).
    CHECK(PmrGetTrackName().empty());                       // no RaceInfo yet -> ""
    CHECK(PmrGetDriverName(999) == "");                     // unknown id -> ""
    CHECK(PmrGetCurrentSector(999) == -1);                  // unknown id -> -1 (wire default)
    CHECK(!PmrIsInPits(999));                               // unknown id -> false

    std::string err;
    bool ok = PmrOpenSocket(kTestPort, err);
    if (!ok) { std::printf("   OPEN FAILED: %s — aborting M1\n", err.c_str()); ++s_failures; return; }
    CHECK(PmrIsConnected());

    s_senderSock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    CHECK(s_senderSock != INVALID_SOCKET);
    InterlockedExchange(&s_stopSender, 0);
    HANDLE hThread = CreateThread(NULL, 0, SenderProc, NULL, CREATE_SUSPENDED, NULL);
    if (hThread) { ResumeThread(hThread); std::printf("   sender thread started -> 127.0.0.1:%d\n", kTestPort); }

    // Drain at ~10 Hz until both metadata sources are populated (bounded: max ~3 s).
    int totalDrained = 0;
    bool haveTrack = false, haveDriver = false;
    DWORD tStart = GetTickCount();
    while ((GetTickCount() - tStart) < 3000 && !(haveTrack && haveDriver)) {
        Sleep(100);
        totalDrained += PmrDrainAllPackets();   // non-blocking — returns promptly each tick

        const std::string track = PmrGetTrackName();
        if (!track.empty()) haveTrack = true;
        if (PmrGetCurrentSector(42) != -1 || !PmrGetDriverName(42).empty()) haveDriver = true;
    }
    CHECK(totalDrained > 0);                    // gate: packets actually flowed

    std::printf("   drained %d datagrams in %lu ms\n", totalDrained, (unsigned long)(GetTickCount() - tStart));

    const std::string trackName = PmrGetTrackName();
    std::printf("   track           = \"%s\"  (expect \"Silverstone\")\n", trackName.c_str());
    CHECK(trackName == "Silverstone");          // from RaceInfo builder input

    const std::string driver42 = PmrGetDriverName(42);
    std::printf("   driver[42]      = \"%s\"  (expect \"Charles Leclerc\")\n", driver42.c_str());
    CHECK(driver42 == "Charles Leclerc");       // from ParticipantState builder input

    const int sector42 = PmrGetCurrentSector(42);
    std::printf("   sector[42]      = %d  (expect 2)\n", sector42);
    CHECK(sector42 == 2);                       // currentSector in the builder

    const bool pits42 = PmrIsInPits(42);
    std::printf("   inPits[42]      = %d  (expect 0 — inPits byte is 0)\n", pits42 ? 1 : 0);
    CHECK(!pits42);

    // Unknown id AFTER packets arrived: still the documented defaults.
    CHECK(PmrGetDriverName(999) == "");         // no state ever received for vehicle 999
    CHECK(PmrGetCurrentSector(999) == -1);
    CHECK(!PmrIsInPits(999));

    // Stop the sender and confirm clean shutdown (no hang, cache stays valid).
    InterlockedExchange(&s_stopSender, 1);
    if (hThread) {
        DWORD wr = WaitForSingleObject(hThread, 5000);   // hard cap — a hang would fail the gate
        CHECK(wr == WAIT_OBJECT_0);
        CloseHandle(hThread); hThread = NULL;
    }

    PmrDrainAllPackets();                       // flush anything in flight (may be > 0)
    int idle1 = PmrDrainAllPackets(), idle2 = PmrDrainAllPackets();   // two consecutive empty drains
    CHECK(idle1 == 0 && idle2 == 0);            // non-blocking proof: returns promptly with nothing pending

    const std::string trackAfterStop = PmrGetTrackName();             // cache must survive sender stop + close
    CHECK(trackAfterStop == "Silverstone");

    closesocket(s_senderSock); s_senderSock = INVALID_SOCKET;         // release the test port for Gate 2 re-runs
    PmrCloseSocket();                                                 // idempotent lifecycle end (mirrors S6)
}


// ===========================================================================
int main() {
    std::printf("=== PMR Gate 3: feed + metadata self-test ===\n");

    WSADATA wsa;                                                // init Winsock for this process's own raw sockets
    CHECK(WSAStartup(MAKEWORD(2, 2), &wsa) == 0);              // (PmrOpenSocket does its own lazy startup too — ref-counted per process)

    // Load channel mappings BEFORE any PmrFeedLap call so the one-shot PMR table
    // resolves aliases correctly. Path is relative to scripts/ where the exe runs.
    RuleConfig::instance().loadChannelMappings("..\\MotecDataAnalyser.mapping.json");

    testFeed();          // F1 + F2 + A1 — pure decode/feed path, no socket
    testMetadata();      // M1 — real UDP exchange for the Phase-3 accessors

    std::printf("===========================================\n");
    if (s_failures == 0) {
        std::printf("ALL TESTS PASSED\n");
        return 0;
    }
    std::printf("%d FAILURE(S)\n", s_failures);
    return 1;
}
