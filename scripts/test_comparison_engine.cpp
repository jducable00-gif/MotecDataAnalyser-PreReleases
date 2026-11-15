// =============================================================================
//  test_comparison_engine.cpp — Phase 4 standalone test harness.
//
//  Covers (per docs/IMPLEMENTATION_PLAN_DIAGNOSTIC_ENGINE.md step-15):
//    A. Driving-phase segmentation correctness (clear patterns, ambiguous ->
//       Unknown not guessed, threshold-edge boundaries).
//    B. Channel-absent matrix: rules degrade gracefully / abstain with a
//       traceability reason when required channels are missing per profile.
//    C. Multi-candidate cause ranking order + confidence-label boundaries at
//       0.30 / 0.60 / 0.85 (rankCauses modulation x1.25 / none / x0.75).
//    D. L/R asymmetry verdicts: comparable -> repeated; track bias -> artifact;
//       insufficient data -> inconclusive.
//    E. Gate-logic primitives: single-lap confidence cap at MEDIUM(0.60), label
//       boundaries, and the categorical-gate predicate (MEDIUM+ AND EVO profile).
//    F. Non-regression of the 4 legacy rules on a fixture lap set (identical
//       AnalysisResult vectors) + clean-lap no-finding case.
//
//  Pure ASCII on purpose (bcc32c encodes narrow literals as CP1252; French text
//  is built accent-free). Console main() does not clash with vcl.lib's WinMain
//  (lazy member resolution). g_thresholds is owned by analyze_motec_setup.cpp —
//  do NOT redefine it here.
// =============================================================================

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include "MotecSetupAnalyzer.h"   // LapStats, StatSummary, g_thresholds, rules, DataProvider
#include "DiagnosticEngine.h"     // diag:: detectors, rankCauses, evaluateLapWithDiagnostics
#include "VehicleDb.h"            // Ams2VehicleDb -- base vehicules AMS2 embarquee (Layer 1)
#include "LapReportGenerator.h"   // LapReportGenerator::generate, ReportResult, ReportSection
#include "ReleaseInfo.h"          // parseLatestReleaseJson / formatReleaseDate (section J)

// DiagnosticFinding (struct) and diagnosticConfidenceLabel() live in GLOBAL scope
// (MotecSetupAnalyzer.h), not inside namespace diag. Re-expose them under diag:: so
// the test code can refer to them uniformly with the other diagnostic symbols.
namespace diag {
    typedef ::DiagnosticFinding DiagnosticFinding;   // type alias into global struct
    using ::diagnosticConfidenceLabel;               // function declaration import
}

// ---------------------------------------------------------------------------
// Tiny assertion framework (no external deps). Counts pass/fail; exits non-zero
// on any failure so the .bat can detect it.
// ---------------------------------------------------------------------------
static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond) do { \
    ++g_checks; \
    if (!(cond)) { \
        ++g_failures; \
        std::printf("FAIL [%s:%d] %s\n", __FILE__, __LINE__, #cond); \
    } \
} while (0)

#define CHECK_MSG(cond, msg) do { \
    ++g_checks; \
    if (!(cond)) { \
        ++g_failures; \
        std::printf("FAIL [%s:%d] %s  (%s)\n", __FILE__, __LINE__, #cond, (msg)); \
    } \
} while (0)

static void section(const char* name) { std::printf("\n== %s ==\n", name); }

// Local NaN helper (the codebase uses numeric_limits directly; this keeps call
// sites terse). Not in any namespace so it resolves unqualified.
inline double NaN() { return std::numeric_limits<double>::quiet_NaN(); }

// ---------------------------------------------------------------------------
// Fixture helpers. A "valid" lap has a positive finite duration inside the
// median-based selection window and rowCount>0 so buildAverageLapData() yields
// a non-empty averaged lap (the comparison engine's validity gate). All channel
// StatSummarys start empty (count==0) => alias absent => rules abstain.
// ---------------------------------------------------------------------------
static LapStats makeValidLap(int number = 1, const char* track = "TestTrack",
                             const char* car = "TestCar") {
    LapStats lap;
    lap.lapNumber   = number;
    lap.startTime   = 0.0;
    lap.endTime     = 90.0;      // duration 90s == median => inside [63,117] window
    lap.rowCount    = 500;       // >0 so averaged rowCount>0 (validity gate)
    lap.trackName   = track ? track : "";
    lap.vehicleName = car ? car : "";
    return lap;
}

// Add N identical samples to a StatSummary (average == value).
static void fill(StatSummary& s, double value, int n = 10) {
    for (int i = 0; i < n; ++i) s.add(value);
}

// Find a diagnostic finding by id in a vector.
static const diag::DiagnosticFinding* findById(const std::vector<diag::DiagnosticFinding>& v,
                                               const char* id) {
    for (std::size_t i = 0; i < v.size(); ++i)
        if (v[i].symptomId == id) return &v[i];
    return nullptr;
}

// ===========================================================================
// A. Driving-phase segmentation correctness
// ===========================================================================
static void testPhaseSegmentation() {
    section("A: Phase segmentation");

    // --- Clear straight + stable acceleration pattern -----------------------
    // throttle>25, brake<10, |latG|<=0.15, longG>=0.08  => StraightStableAccel(1)
    LapStats lap;
    for (int i = 0; i < 40; ++i) {
        // speed must be finite & decreasing-by-<3km/h to avoid braking branch: keep constant
        lap.addHandlingSample(/*steer*/0.0, /*yaw*/0.0, /*latG*/0.10, /*longG*/0.20,
                              /*brake*/5.0,  /*throttle*/80.0, /*suspAsym*/NaN(),
                              /*speedKmh*/150.0);
    }
    CHECK(lap.phases[static_cast<int>(DrivingPhase::StraightStableAccel)].sampleCount() == 40u);
    // No other phase should have absorbed samples from this pure pattern.
    for (int p = 2; p < kDrivingPhaseCount; ++p) {
        CHECK(lap.phases[p].sampleCount() == 0u);
    }

    // --- Ambiguous data: low latG, no throttle/brake signal -> Unknown ------
    LapStats lapAmbig;
    for (int i = 0; i < 30; ++i) {
        // Not braking (brake<10), not accelerating (throttle<=25), |latG| below cornering,
        // longG low => no branch matches -> stays Unknown(0). Never guessed.
        lapAmbig.addHandlingSample(/*steer*/1.0, /*yaw*/0.3, /*latG*/0.05, /*longG*/0.02,
                                   /*brake*/4.0,  /*throttle*/10.0, NaN(), 90.0);
    }
    // Unknown samples are never bucketed into phases[] at all — the classifier only
    // records a sample when it fully matches a known phase. "Never guessed" therefore
    // means: zero classified samples and every real-phase bucket empty (checked below).
    CHECK(lapAmbig.phaseClassifiedSamples == 0u);
    for (int p = 1; p < kDrivingPhaseCount; ++p) {
        CHECK(lapAmbig.phases[p].sampleCount() == 0u);
    }

    // --- Boundary: latG exactly at phaseLatGMidMin(0.30g) with stable steer --
    // MidCorner requires absLatG >= 0.30 and dSteer <= 2.5, not braking/accelerating.
    LapStats lapMid;
    for (int i = 0; i < 25; ++i) {
        lapMid.addHandlingSample(/*steer*/18.0, /*yaw*/4.0, /*latG*/0.30, /*longG*/-0.05,
                                 /*brake*/6.0,   /*throttle*/12.0, NaN(), 95.0);
    }
    CHECK(lapMid.phases[static_cast<int>(DrivingPhase::MidCorner)].sampleCount() == 25u);

    // --- Boundary: speed drop exactly at phaseSpeedDropPerSec(3.0 km/h) ------
    // Braking (brake>10, throttle<35) + hasSpeed + speedDrop>=3 => braking branch fires.
    LapStats lapBrk;
    for (int i = 0; i < 20; ++i) {
        const double spd = 140.0 - static_cast<double>(i) * 3.0;   // drops exactly 3 km/h per frame
        lapBrk.addHandlingSample(/*steer*/2.0, /*yaw*/-6.0, /*latG*/0.10, /*longG*/-1.5,
                                 /*brake*/70.0, /*throttle*/8.0, NaN(), spd);
    }
    // InitialBrake(3) is the peak-decel branch (|longG|>1.2). The FIRST frame of a fresh
    // lap has no prior-speed reference, so its speed-drop can't be computed and it stays
    // Unknown; frames 1..N-1 each drop exactly phaseSpeedDropPerSec(3.0) => InitialBrake.
    CHECK(lapBrk.phases[static_cast<int>(DrivingPhase::InitialBrake)].sampleCount() == 19u);

    std::printf("   segmentation: straight/ambiguous/mid-corner-edge/braking-edge OK\n");
}

// ===========================================================================
// B. Channel-absent matrix — graceful degradation / abstention with reason
// ===========================================================================
static void testChannelAbsentMatrix() {
    section("B: Channel-absent matrix (graceful degradation)");

    AnalysisSettings settings = buildDefaultAnalysisSettings();   // INI-driven thresholds

    // --- B1. No rear wheel slip at all -> RearTractionLossOnExitRule abstains --
    LapStats noSlip;                       // every channel empty by default
    const std::vector<diag::DiagnosticFinding> fNo = diag::evaluateLapWithDiagnostics(noSlip, settings);
    CHECK(findById(fNo, "rear_traction_loss_on_exit") == nullptr);

    // --- B2. Rear slip present but NO ride height/Rake -> bumpstop rule (if it
    //        fired) would carry the reduced-confidence confounder; here we prove
    //        Rake is simply absent from a lap with no aero heights set. --------
    DataProvider prov = buildDataProviderFromLap(noSlip);
    double rakeVal = 12345.0;
    CHECK(prov.getValue(AnalysisAlias::Rake, rakeVal) == false);   // alias unset -> absent

    // --- B3. Wheel-speed channels absent (AMS2 has no true wheel speeds): the
    //        brake-instability rule must abstain rather than fabricate a value. --
    LapStats noWheelSpeed;                 // only front brake torque set, NO rear speed/torque
    fill(noWheelSpeed.brakeTorqueLF, 300.0);   // both corners required for axle avg:
    fill(noWheelSpeed.brakeTorqueRF, 280.0);   // -> BrakeTorqueFrontAvg present (front only)
    const std::vector<diag::DiagnosticFinding> fBrk = diag::evaluateLapWithDiagnostics(noWheelSpeed, settings);
    CHECK(findById(fBrk, "brake_instability") == nullptr);        // abstains: no rear speed/torque

    // --- B4. A lap that DOES fire bumpstop with Rake absent -> confounder text --
    LapStats bsLap;
    fill(bsLap.bumpStopLF, 1.0, 20);       // average*100 = 100% engagement (>=5 threshold)
    const std::vector<diag::DiagnosticFinding> fBs = diag::evaluateLapWithDiagnostics(bsLap, settings);
    const diag::DiagnosticFinding* bsF = findById(fBs, "bumpstop_engagement");
    CHECK_MSG(bsF != nullptr, "expected bumpstop finding to fire at 100% LF engagement");
    if (bsF) {
        bool hasRakeConfounder = false;
        for (std::size_t i = 0; i < bsF->confounders.size(); ++i)
            if (bsF->confounders[i].find("No ride-height data available") != std::string::npos)
                hasRakeConfounder = true;
        CHECK_MSG(hasRakeConfounder, "expected reduced-confidence confounder when Rake absent");
    }

    // --- B5. missingChannels traceability: a lap with rear slip but no optional --
    LapStats partialSlip;
    fill(partialSlip.wheelSlipLR, 20.0);   // required present -> rule fires
    fill(partialSlip.wheelSlipRR, 18.0);
    const std::vector<diag::DiagnosticFinding> fPart = diag::evaluateLapWithDiagnostics(partialSlip, settings);
    const diag::DiagnosticFinding* partF = findById(fPart, "rear_traction_loss_on_exit");
    CHECK_MSG(partF != nullptr, "expected rear traction finding with slip 20/18 (threshold 15)");
    if (partF) {
        // Optional channels (ThrottleAvgPct etc.) are absent -> recorded for traceability.
        bool throttleMissing = false;
        for (std::size_t i = 0; i < partF->missingChannels.size(); ++i)
            if (partF->missingChannels[i] == AnalysisAlias::ThrottleAvgPct) throttleMissing = true;
        CHECK_MSG(throttleMissing, "expected ThrottleAvgPct listed in missingChannels");
    }

    std::printf("   channel-absent: abstain + traceability reasons OK\n");
}

// ===========================================================================
// C. Cause ranking order + confidence-label boundaries (0.30 / 0.60 / 0.85)
// ===========================================================================
static void testCauseRankingAndLabels() {
    section("C: Cause ranking + confidence labels at boundaries");

    // Base weights for rear_traction_loss_on_exit from RuleConfig::buildDefaults():
    //   rear_rebound_too_high .45 / rear_spring_too_stiff .25 / differential .20 / pressure_low .10
    const double base[4] = { 0.45, 0.25, 0.20, 0.10 };

    DataProvider emptyProv;   // no values -> rankCauses only modulates by confidence

    auto expectOrderAndScale = [&](double conf, double scaleFactor, const char* label) {
        std::vector<CauseEntry> ranked = diag::rankCauses("rear_traction_loss_on_exit", emptyProv, conf);
        CHECK_MSG(ranked.size() == 4u, "expected full cause list (size 4)");
        if (ranked.size() != 4) return;
        // Order must be preserved under uniform scaling.
        for (int i = 0; i < 4; ++i) {
            const double expected = std::min(1.0, base[i] * scaleFactor);
            CHECK_MSG(std::fabs(ranked[i].weight - expected) < 1e-9, label);
            if (i > 0)
                CHECK_MSG(ranked[i-1].weight >= ranked[i].weight, "ranking must be non-increasing");
        }
    };

    // conf = 0.85 -> HIGH boundary -> x1.25 boost: [0.5625, 0.3125, 0.25, 0.125]
    expectOrderAndScale(0.85, 1.25, "conf=0.85 => x1.25");
    // conf = 0.90 -> above HIGH -> still x1.25 (same values)
    { std::vector<CauseEntry> r = diag::rankCauses("rear_traction_loss_on_exit", emptyProv, 0.90);
      CHECK_MSG(r.size()==4 && std::fabs(r[0].weight-0.5625)<1e-9, "conf=0.90 => x1.25 top cause"); }

    // conf = 0.30 -> NOT below confidenceLow(0.30) -> unchanged base weights
    expectOrderAndScale(0.30, 1.00, "conf=0.30 => no modulation (boundary inclusive)");
    // conf = 0.50 -> mid band -> unchanged
    { std::vector<CauseEntry> r = diag::rankCauses("rear_traction_loss_on_exit", emptyProv, 0.50);
      CHECK_MSG(r.size()==4 && std::fabs(r[0].weight-0.45)<1e-9, "conf=0.50 => base unchanged"); }

    // conf = 0.29 -> below confidenceLow(0.30) -> x0.75 dampen: [0.3375,...]
    expectOrderAndScale(0.29, 0.75, "conf=0.29 => x0.75");

    // --- Confidence label boundaries ----------------------------------------
    CHECK(diag::diagnosticConfidenceLabel(0.84) == std::string("MEDIUM"));   // just under HIGH
    CHECK(diag::diagnosticConfidenceLabel(0.85) == std::string("HIGH"));     // at HIGH boundary
    CHECK(diag::diagnosticConfidenceLabel(0.60) == std::string("MEDIUM"));   // at MEDIUM boundary
    CHECK(diag::diagnosticConfidenceLabel(0.29) == std::string("LOW"));      // below LOW band top

    std::printf("   ranking order + label boundaries (0.30/0.60/0.85) OK\n");
}

// ===========================================================================
// D. L/R asymmetry verdicts (compareLrInComparablePhases)
// ===========================================================================
static void testLRAsymmetryVerdicts() {
    section("D: L/R asymmetry verdicts");

    const int minSamples = g_thresholds.minHandlingSamples > 0 ? g_thresholds.minHandlingSamples : 8;

    // --- D1. Comparable conditions -> RepeatedInComparableConditions ---------
    LapStats comparable;
    for (int i = 0; i < minSamples + 2; ++i) {   // both sides comfortably above minimum, ratio ~1
        comparable.phases[static_cast<int>(DrivingPhase::MidCorner)].latGLeftHander.add(0.8);
        comparable.phases[static_cast<int>(DrivingPhase::MidCorner)].latGRighther.add(-0.75);
    }
    PhaseAsymmetryResult r1 = compareLrInComparablePhases(comparable);
    CHECK(r1.verdict == AsymmetryVerdict::RepeatedInComparableConditions);

    // --- D2. Track bias (one-directional) -> LikelyLayoutPhaseArtifact -------
    LapStats biased;
    for (int i = 0; i < minSamples + 4; ++i)     // left only, right empty => lo==0 branch
        biased.phases[static_cast<int>(DrivingPhase::MidCorner)].latGLeftHander.add(0.9);
    PhaseAsymmetryResult r2 = compareLrInComparablePhases(biased);
    CHECK(r2.verdict == AsymmetryVerdict::LikelyLayoutPhaseArtifact);

    // --- D3. Insufficient data -> InconclusiveInsufficientData ---------------
    LapStats sparse;                              // 2 per side: total(4) < minSamples AND each side < min
    for (int i = 0; i < 2; ++i) {
        sparse.phases[static_cast<int>(DrivingPhase::MidCorner)].latGLeftHander.add(0.8);
        sparse.phases[static_cast<int>(DrivingPhase::MidCorner)].latGRighther.add(-0.75);
    }
    PhaseAsymmetryResult r3 = compareLrInComparablePhases(sparse);
    CHECK(r3.verdict == AsymmetryVerdict::InconclusiveInsufficientData);

    std::printf("   L/R verdicts: comparable/artifact/insufficient OK\n");
}

// E. Gate-logic primitives (single-lap confidence cap + label boundaries).
// NOTE: the full step-12 categorical gate (renderDiagnosticBlock / canCategorizeSetting)
// is file-static inside LapReportGenerator.cpp and exercised by integration tests;
// here we validate its constituent predicates that a standalone harness CAN reach.
// ===========================================================================
static void testGateLogicPrimitives() {
    section("E: Gate-logic primitives (confidence cap + label boundaries)");

    // Single-lap evaluation can NEVER exceed the MEDIUM(0.60) confidence cap, so it
    // can never be labelled HIGH and can never trigger the x1.25 top-cause boost via a
    // real finding. Verify by driving a strong rear-slip lap through the full pipeline:
    LapStats hot;   fill(hot.wheelSlipLR, 40.0); fill(hot.wheelSlipRR, 38.0);
    AnalysisSettings settings = buildDefaultAnalysisSettings();
    const std::vector<diag::DiagnosticFinding> fHot = diag::evaluateLapWithDiagnostics(hot, settings);
    const diag::DiagnosticFinding* hotF = findById(fHot, "rear_traction_loss_on_exit");
    CHECK_MSG(hotF != nullptr, "expected rear traction finding on 40% slip lap");
    if (hotF) {
        CHECK_MSG(hotF->confidence <= g_thresholds.confidenceMedium + 1e-9,
                  "single-lap confidence must be capped at MEDIUM(0.60)");
        CHECK(hotF->dataQualityLabel != std::string("HIGH"));   // never HIGH on a single lap
    }

    // Label boundaries that feed the categorical gate (gate A: >= MEDIUM).
    CHECK(std::strcmp(diag::diagnosticConfidenceLabel(0.59), "LOW")    == 0);   // just under MEDIUM boundary
    CHECK(std::strcmp(diag::diagnosticConfidenceLabel(g_thresholds.confidenceMedium), "MEDIUM") == 0);
    CHECK(std::strcmp(diag::diagnosticConfidenceLabel(g_thresholds.confidenceHigh),     "HIGH")   == 0);

    // Categorical gate predicate (replicated): MEDIUM+ confidence AND EVO profile only.
    auto canCategorize = [](double conf, ChannelProfile prof) {
        return conf >= g_thresholds.confidenceMedium && prof == ChannelProfile::EvoM1;
    };
    CHECK(canCategorize(0.60, ChannelProfile::EvoM1)  == true);   // passes at MEDIUM on EVO
    CHECK(canCategorize(0.59, ChannelProfile::EvoM1)  == false);  // blocked below MEDIUM
    CHECK(canCategorize(0.85, ChannelProfile::AMS2)   == false);  // non-EVO profile never categorical

    std::printf("   gate primitives: single-lap cap + label/profile predicates OK\n");
}

// ===========================================================================
// F. Non-regression of the 4 legacy rules on a fixture lap set.
// =============================================================================
static void testLegacyRuleNonRegression() {
    section("F: Legacy rule non-regression (identical AnalysisResult vectors)");

    // --- G1. Clean lap -> zero findings from all four legacy rules -----------
    LapStats clean = makeValidLap(1);   // no channels set at all
    const std::vector<AnalysisResult> rClean = evaluateLapWithRules(clean, AnalysisSettings());
    CHECK_MSG(rClean.empty(), "clean lap must yield NO legacy findings");

    // --- G2. Engine over-temp -> exactly one Critical 'Engine' finding --------
    LapStats hotEng = makeValidLap(1);
    fill(hotEng.waterTemp, 105.0);      // > engine.waterTempWarnC (default AnalysisSettings: 100)
    const std::vector<AnalysisResult> rHot = evaluateLapWithRules(hotEng, AnalysisSettings());
    int engCriticals = 0; bool waterTitleSeen = false;
    for (std::size_t i = 0; i < rHot.size(); ++i) {
        if (rHot[i].severity == AnalysisSeverity::Critical && rHot[i].category == "Engine") ++engCriticals;
        if (rHot[i].title.find("Water temperature high") != std::string::npos) waterTitleSeen = true;
    }
    CHECK_MSG(engCriticals >= 1, "expected a Critical Engine finding for 105C water");
    CHECK(waterTitleSeen);

    // --- G3. Rear outer vs front outer temp -> suspension balance (Tyre tab) --
    LapStats stiff = makeValidLap(1);
    fill(stiff.tyreOuterTempLF, 90.0); fill(stiff.tyreOuterTempRF, 92.0);   // front max = 92
    fill(stiff.tyreOuterTempLR, 104.0); fill(stiff.tyreOuterTempRR, 106.0); // rear  max = 106 -> diff +14 > threshold(5)
    const std::vector<AnalysisResult> rStiff = evaluateLapWithRules(stiff, AnalysisSettings());
    bool stiffSeen = false; int brakeWarnings = 0;
    for (std::size_t i = 0; i < rStiff.size(); ++i) {
        if (rStiff[i].severity == AnalysisSeverity::Warning && rStiff[i].category == "Tyres") ++brakeWarnings;
        if (rStiff[i].title.find("Rear suspension may be too stiff") != std::string::npos) stiffSeen = true;
    }
    CHECK_MSG(stiffSeen, "expected 'Rear suspension may be too stiff' finding");
    CHECK(brakeWarnings >= 1);

    // --- G3b. Rear cooler than front -> rear suspension may be too soft -------
    LapStats softLap = makeValidLap(2);
    fill(softLap.tyreOuterTempLF, 95.0); fill(softLap.tyreOuterTempRF, 97.0);   // front avg ~96
    fill(softLap.tyreOuterTempLR, 82.0); fill(softLap.tyreOuterTempRR, 84.0);   // rear  avg ~83 -> diff -13 < 0
    const std::vector<AnalysisResult> rSoft = evaluateLapWithRules(softLap, AnalysisSettings());
    bool softSeen = false; int brakeWarnings2 = 0;
    for (std::size_t i = 0; i < rSoft.size(); ++i) {
        if (rSoft[i].severity == AnalysisSeverity::Warning && rSoft[i].category == "Tyres") ++brakeWarnings2;
        if (rSoft[i].title.find("Rear suspension may be too soft") != std::string::npos) softSeen = true;
    }
    CHECK_MSG(softSeen, "expected 'Rear suspension may be too soft' finding");
    CHECK(brakeWarnings2 >= 1);

    // --- G3c. Rear only slightly hotter (within threshold) -> NO Brakes -------
    LapStats balLap = makeValidLap(3);
    fill(balLap.tyreOuterTempLF, 90.0); fill(balLap.tyreOuterTempRF, 92.0);   // front max = 92
    fill(balLap.tyreOuterTempLR, 93.5); fill(balLap.tyreOuterTempRR, 94.5);   // rear  max = 94.5 -> diff +2.5 (<= threshold)
    const std::vector<AnalysisResult> rBal = evaluateLapWithRules(balLap, AnalysisSettings());
    int suspensionFindings = 0;
    for (std::size_t i = 0; i < rBal.size(); ++i) {
        if (rBal[i].category == "Tyres" &&
            rBal[i].title.find("suspension") != std::string::npos) ++suspensionFindings;
    }
    CHECK_MSG(suspensionFindings == 0, "balanced tyre temps must yield NO suspension balance findings");

    // --- G4. High rear wheel slip -> one Warning 'Handling' finding -----------
    LapStats slipLap = makeValidLap(1);
    fill(slipLap.wheelSlipLR, 20.0);   // > handling.wheelSlipMax (default: 15)
    const std::vector<AnalysisResult> rSlip = evaluateLapWithRules(slipLap, AnalysisSettings());
    bool rearSlipSeen = false; int handlingWarnings = 0;
    for (std::size_t i = 0; i < rSlip.size(); ++i) {
        if (rSlip[i].severity == AnalysisSeverity::Warning && rSlip[i].category == "Handling") ++handlingWarnings;
        if (rSlip[i].title.find("Rear wheel slip high") != std::string::npos) rearSlipSeen = true;
    }
    CHECK_MSG(rearSlipSeen, "expected 'Rear wheel slip high' finding");
    CHECK(handlingWarnings >= 1);

    // --- G5. Determinism: same input -> identical result vector (size+titles) --
    const std::vector<AnalysisResult> rHot2 = evaluateLapWithRules(hotEng, AnalysisSettings());
    CHECK(rHot.size() == rHot2.size());
    bool titlesMatch = true;
    for (std::size_t i = 0; i < rHot.size(); ++i) if (rHot[i].title != rHot2[i].title) { titlesMatch = false; break; }
    CHECK(titlesMatch);

    std::printf("   legacy rules: clean/engine/pressure/slip/determinism OK\n");
}

// G. Embedded AMS2 vehicle database sanity checks (Layer 1).
// ===========================================================================
static void testVehicleDb() {
    section("G: Vehicle DB (embedded AMS2 data, Layer 1)");

    const Ams2VehicleDb& db = Ams2VehicleDb::instance();

    // H0: The embedded JSON parsed successfully and has a reasonable count.
    CHECK_MSG(db.isValid(), "Ams2VehicleDb must be valid after parsing the embedded JSON");
    CHECK_MSG(db.vehicleCount() >= 100, "expected at least 100 vehicles in the DB");

    // H1: Find a known car by exact name (Alpine A110 GT4 Evo is first in source).
    VehicleInfo alpine;
    bool found = db.find("Alpine_A110_GT4_Evo", alpine);
    CHECK_MSG(found, "find('Alpine_A110_GT4_Evo') must succeed");
    if (found) {
        CHECK(std::fabs(alpine.chassis.massKg - 1165.0) < 0.5);          // Mass: 1165 kg
        CHECK_MSG(alpine.engine.hasOptOilTemp, "Alpine A110 must have OptimumOilTemp");
        if (alpine.engine.hasOptOilTemp)
            CHECK(std::fabs(alpine.engine.optOilTempC - 110.0) < 0.5);   // 110 deg C

        // H2: Brake data present for all four wheels with sane temp ranges.
        bool brakesOk = true;
        for (int w = Ams2VehicleDb::kWheelFl; w <= Ams2VehicleDb::kWheelRr; ++w) {
            const WheelBrakeData& b = alpine.chassis.brakes[w];
            if (!b.present || b.optTempMinC < 100.0 || b.optTempMaxC > 900.0 ||
                b.optTempMinC >= b.optTempMaxC) { brakesOk = false; break; }
        }
        CHECK_MSG(brakesOk, "Alpine A110 must have valid brake temp ranges on all 4 wheels");

        // H3: CG data present.
        CHECK(alpine.chassis.hasCgHeight);
    }

    // H4: Case-insensitive lookup works (lowercase query).
    VehicleInfo alpineLower;
    bool foundLower = db.find("alpine_a110_gt4_evo", alpineLower);
    CHECK_MSG(foundLower, "find() must be case-insensitive");
    if (found && foundLower)
        CHECK(alpine.chassis.massKg == alpineLower.chassis.massKg);

    // H5: Unknown name returns false.
    VehicleInfo ghost;
    bool foundGhost = db.find("Nonexistent_Car_XYZ_999", ghost);
    CHECK(!foundGhost);

    // H6: CG data parsing (CgRangeData). The DB stores fixed scalar values only;
    // the generator rejects object-form ranges, so all cars have hasFixedValue=true.
    if (found) {
        const CgRangeData& cgRt = alpine.chassis.cgRight;
        CHECK(cgRt.present);
        CHECK(!cgRt.isAdjustable());                 // scalaire => fixe
        CHECK(std::fabs(cgRt.valueM() - 0.56f) < 1e-6f);   // CGRightRange: ~0.56 (float precision)

        const CgRangeData& cgRe = alpine.chassis.cgRear;
        CHECK(!cgRe.isAdjustable());                 // scalaire => fixe
        CHECK(std::fabs(cgRe.valueM() - 0.5f) < 1e-6f);   // CGRearRange: ~0.5 (float precision)
    }

    // Stock USA Gen1 : fixed scalar CG values from the DB row.
    VehicleInfo stock;
    bool foundStock = db.find("stock_usa_gen1", stock);   // '_' doit matcher ' '
    CHECK_MSG(foundStock, "find('stock_usa_gen1') must succeed (underscore normalization)");
    if (foundStock) {
        const CgRangeData& cgRt = stock.chassis.cgRight;
        CHECK(cgRt.present && !cgRt.isAdjustable());  // scalaire fixe dans le DB
        CHECK(std::fabs(cgRt.valueM() - 0.48f) < 1e-6f);   // CGRightRange: ~0.48

        const CgRangeData& cgRe = stock.chassis.cgRear;
        CHECK(cgRe.present && !cgRe.isAdjustable());  // scalaire fixe dans le DB
        CHECK(std::fabs(cgRe.valueM() - 0.458f) < 1e-6f);   // CGRearRange: ~0.458
    }

    std::printf("   vehicle DB: valid, %u cars, find/case-insensitive/unknown OK\n",
                static_cast<unsigned>(db.vehicleCount()));
}

// ===========================================================================
// H. Engine cooling per-car thresholds + optimal oil temp info line (Layer 1).
// =============================================================================
static void testEngineCoolingPerCar() {
    section("H: Engine cooling per-vehicle override (AMS2 Layer 1)");

    const Ams2VehicleDb& db = Ams2VehicleDb::instance();
    VehicleInfo alpine;
    if (!db.find("Alpine_A110_GT4_Evo", alpine)) {
        std::printf("   SKIP: Alpine not in DB\n");
        return;
    }

    // I0. Per-car oil threshold override suppresses a finding that the global default would raise.
    //     Default AnalysisSettings.oilTermWarnC = 110, but Alpine OilWarningTemp = 130.
    //     Set oil to 125 C: with per-car override -> NO warning; without it -> Critical.
    LapStats lapOverride = makeValidLap(1);
    fill(lapOverride.oilTemp, 125.0);   // between default (110) and Alpine's (130)
    lapOverride.vehicleName = "Alpine_A110_GT4_Evo";

    ReportResult rrOverride = LapReportGenerator::generate(
        lapOverride, ReportLanguage::English, ChannelProfile::AMS2);
    const SectionData& engSecOv = rrOverride[ReportSection::EngineCooling];
    bool oilWarnSeen = (engSecOv.content.find("Oil temperature high") != std::string::npos);
    CHECK_MSG(!oilWarnSeen, "I0: per-car OilWarningTemp=130 must suppress warning at 125C");

    // I1. Same lap WITHOUT vehicleName -> global default (g_thresholds.oilTempMax = 115) applies -> finding raised.
    LapStats lapNoVehicle = makeValidLap(1);
    fill(lapNoVehicle.oilTemp, 125.0);   // > g_thresholds.oilTempMax=115
    lapNoVehicle.vehicleName.clear();

    ReportResult rrDefault = LapReportGenerator::generate(
        lapNoVehicle, ReportLanguage::English, ChannelProfile::AMS2);
    const SectionData& engSecDef = rrDefault[ReportSection::EngineCooling];
    bool oilWarnSeenDefault = (engSecDef.content.find("Oil temperature high") != std::string::npos);
    CHECK_MSG(oilWarnSeenDefault, "I1: without vehicleName the global default 115C must trigger at 125C");

    // I1b. The finding shows the global (default) oil threshold of 115.0 in its expected-value line.
    bool defShowsDefaultThreshold = engSecDef.content.find("exceeds warning threshold of 115.0") != std::string::npos;
    CHECK_MSG(defShowsDefaultThreshold, "I1b: finding must display the global default oil threshold (115.0)");

    // I2. Per-car water threshold override suppresses a finding that the global default would raise.
    //     g_thresholds.waterTempMax = 95, Alpine WaterWarningTemp = 115.
    LapStats lapWaterOv = makeValidLap(1);
    fill(lapWaterOv.waterTemp, 110.0);   // between default (100) and Alpine's (115)
    lapWaterOv.vehicleName = "Alpine_A110_GT4_Evo";

    ReportResult rrWaterOv = LapReportGenerator::generate(
        lapWaterOv, ReportLanguage::English, ChannelProfile::AMS2);
    const SectionData& engSecWov = rrWaterOv[ReportSection::EngineCooling];
    bool waterWarnSeen = (engSecWov.content.find("Water temperature high") != std::string::npos);
    CHECK_MSG(!waterWarnSeen, "I2: per-car WaterWarningTemp=115 must suppress warning at 110C");

    // I3. Optimal oil temp info line present for a known car (English).
    bool optLineSeen = engSecOv.content.find("Optimal oil temperature") != std::string::npos;
    CHECK_MSG(optLineSeen, "I3: 'Optimal oil temperature' info line must appear in EngineCooling");

    // I4. Info line contains the vehicle name and a plausible temp value (105-120 C range).
    bool hasName = engSecOv.content.find("Alpine_A110_GT4_Evo") != std::string::npos;
    CHECK_MSG(hasName, "I4: info line must contain the vehicle name");

    // I5. No optimal oil temp line for unknown/empty vehicleName (already checked in I1's result).
    bool optLineDefault = engSecDef.content.find("Optimal oil temperature") != std::string::npos;
    CHECK_MSG(!optLineDefault, "I5: no 'Optimal oil temperature' line when vehicleName is empty");

    // I6. French language variant produces the French info label (ASCII-safe).
    LapStats lapFr = makeValidLap(1);
    fill(lapFr.oilTemp, 90.0);   // well below any threshold -> no findings, just info
    lapFr.vehicleName = "Alpine_A110_GT4_Evo";

    ReportResult rrFr = LapReportGenerator::generate(
        lapFr, ReportLanguage::French, ChannelProfile::AMS2);
    const SectionData& engSecFr = rrFr[ReportSection::EngineCooling];
    bool frLineSeen = (engSecFr.content.find("Temperature d'huile optimale") != std::string::npos);
    CHECK_MSG(frLineSeen, "I6: French 'Temperature d\\'huile optimale' info line must appear");

    // I6b. The same info line now also carries the alert limits from the DB
    // (Alpine A110 GT4 Evo : OilWarningTemp 130, WaterWarningTemp 115).
    bool frOilAlertSeen = (engSecFr.content.find("Alerte huile : 130") != std::string::npos);
    CHECK_MSG(frOilAlertSeen, "I6b: French info line must include 'Alerte huile : 130'");
    bool frWaterAlertSeen = (engSecFr.content.find("Alerte eau : 115") != std::string::npos);
    CHECK_MSG(frWaterAlertSeen, "I6b: French info line must include 'Alerte eau : 115'");

    // I7. Oil temp ABOVE the per-car threshold still triggers a finding (override raises bar).
    LapStats lapHotOil = makeValidLap(1);
    fill(lapHotOil.oilTemp, 135.0);   // > Alpine's 130 -> must trigger even with override
    lapHotOil.vehicleName = "Alpine_A110_GT4_Evo";

    ReportResult rrHotOil = LapReportGenerator::generate(
        lapHotOil, ReportLanguage::English, ChannelProfile::AMS2);
    const SectionData& engSecHot = rrHotOil[ReportSection::EngineCooling];
    bool hotOilWarnSeen = (engSecHot.content.find("Oil temperature high") != std::string::npos);
    CHECK_MSG(hotOilWarnSeen, "I7: oil at 135C > per-car threshold 130 must still trigger warning");

    // I7b. The finding now shows the effective (per-car) threshold in its expected-value line.
    bool hotShowsPerCarThreshold = engSecHot.content.find("exceeds warning threshold of 130.0") != std::string::npos;
    CHECK_MSG(hotShowsPerCarThreshold, "I7b: finding must display the per-car oil threshold (130.0)");

    // I8. Unknown vehicle name -> no override applied (same as empty).
    LapStats lapUnknown = makeValidLap(1);
    fill(lapUnknown.oilTemp, 125.0);   // between default and Alpine's threshold
    lapUnknown.vehicleName = "Totally_Fake_Car_999";

    ReportResult rrUnknown = LapReportGenerator::generate(
        lapUnknown, ReportLanguage::English, ChannelProfile::AMS2);
    const SectionData& engSecUnk = rrUnknown[ReportSection::EngineCooling];
    bool unkOilWarnSeen = (engSecUnk.content.find("Oil temperature high") != std::string::npos);
    CHECK_MSG(unkOilWarnSeen, "I8: unknown vehicleName must NOT apply per-car override");

    // I9. Info line absent for unknown car name.
    bool unkOptLine = engSecUnk.content.find("Optimal oil temperature") != std::string::npos;
    CHECK_MSG(!unkOptLine, "I9: no 'Optimal oil temperature' line for unknown vehicle");

    // I10. Non-AMS2 profile (EvoM1) with a name that happens to match an AMS2 car — 
    //      the DB lookup is purely by name so it WILL still apply; this documents
    //      current behavior (name-based, not profile-gated). If we later add gating,
    //      flip this expectation.
    LapStats lapEvo = makeValidLap(1);
    fill(lapEvo.oilTemp, 90.0);   // no findings expected regardless
    lapEvo.vehicleName = "Alpine_A110_GT4_Evo";

    ReportResult rrEvo = LapReportGenerator::generate(
        lapEvo, ReportLanguage::English, ChannelProfile::EvoM1);
    const SectionData& engSecEvo = rrEvo[ReportSection::EngineCooling];
    bool evoOptLineSeen = (engSecEvo.content.find("Optimal oil temperature") != std::string::npos);
    // Current behavior: name-based lookup applies regardless of profile.
    CHECK_MSG(evoOptLineSeen, "I10: info line appears even on EvoM1 profile (name-based lookup)");

    std::printf("   engine cooling per-car: override + info line OK\n");
}

// ===========================================================================
// I. Brake disc temperature per-vehicle fade window (Layer 1).
// =============================================================================
static void testBrakePerCar() {
    section("I: Brake temp per-vehicle fade window (AMS2 Layer 1)");

    const Ams2VehicleDb& db = Ams2VehicleDb::instance();
    VehicleInfo alpine;
    if (!db.find("Alpine_A110_GT4_Evo", alpine)) {
        std::printf("   SKIP: Alpine not in DB\n");
        return;
    }

    // J0. Disc temp INSIDE the per-car fade window (350-850 C for Alpine) -> no finding,
    //     even though 500C would be "hot" by any universal standard. Documents that there is
    //     NO global default: without vehicle data this rule emits nothing at all.
    LapStats lapInWindow = makeValidLap(1);
    fill(lapInWindow.brakeTempLF, 500.0);   // inside Alpine's [350, 850] window
    lapInWindow.vehicleName = "Alpine_A110_GT4_Evo";

    ReportResult rrInWindow = LapReportGenerator::generate(
        lapInWindow, ReportLanguage::English, ChannelProfile::AMS2);
    const SectionData& brakeSecWin = rrInWindow[ReportSection::Brake];
    bool inWindowFindingSeen = (brakeSecWin.content.find("FL brake temperature") != std::string::npos);
    CHECK_MSG(!inWindowFindingSeen, "J0: disc at 500C inside per-car fade window must NOT raise a finding");

    // J1. Same extreme temp WITHOUT vehicleName -> has* flags stay false -> NO finding at all.
    //     (Contrast with engine I1 which DOES trigger on the global default.) This documents
    //     the no-universal-default design for absolute disc temperatures.
    LapStats lapNoVehicle = makeValidLap(1);
    fill(lapNoVehicle.brakeTempLF, 900.0);   // above Alpine's fade-high (850) but...
    lapNoVehicle.vehicleName.clear();

    ReportResult rrNoVehicle = LapReportGenerator::generate(
        lapNoVehicle, ReportLanguage::English, ChannelProfile::AMS2);
    const SectionData& brakeSecNV = rrNoVehicle[ReportSection::Brake];
    bool noVehicleFindingSeen = (brakeSecNV.content.find("FL brake temperature") != std::string::npos);
    CHECK_MSG(!noVehicleFindingSeen, "J1: without vehicleName the rule must emit nothing (no universal default)");

    // J2. Disc ABOVE the per-car fade-high threshold -> Critical finding with effective threshold shown.
    LapStats lapHot = makeValidLap(1);
    fill(lapHot.brakeTempLF, 900.0);   // > Alpine's FadeRangeAbove (850)
    lapHot.vehicleName = "Alpine_A110_GT4_Evo";

    ReportResult rrHot = LapReportGenerator::generate(
        lapHot, ReportLanguage::English, ChannelProfile::AMS2);
    const SectionData& brakeSecHot = rrHot[ReportSection::Brake];
    bool hotFindingSeen = (brakeSecHot.content.find("FL brake temperature high") != std::string::npos);
    CHECK_MSG(hotFindingSeen, "J2: disc at 900C > per-car fade-high 850 must raise a finding");

    // J2b. The expected-value line shows the effective (per-car) threshold of 850.0.
    bool hotShowsPerCarThreshold = brakeSecHot.content.find("exceeds the fade-high threshold of 850.0") != std::string::npos;
    CHECK_MSG(hotShowsPerCarThreshold, "J2b: finding must display the per-car fade-high threshold (850.0)");

    // J3. Disc BELOW the per-car fade-low threshold -> Warning finding with effective threshold shown.
    LapStats lapCold = makeValidLap(1);
    fill(lapCold.brakeTempLF, 300.0);   // < Alpine's FadeRangeBelow (350)
    lapCold.vehicleName = "Alpine_A110_GT4_Evo";

    ReportResult rrCold = LapReportGenerator::generate(
        lapCold, ReportLanguage::English, ChannelProfile::AMS2);
    const SectionData& brakeSecCold = rrCold[ReportSection::Brake];
    bool coldFindingSeen = (brakeSecCold.content.find("FL brake temperature low") != std::string::npos);
    CHECK_MSG(coldFindingSeen, "J3: disc at 300C < per-car fade-low 350 must raise a finding");

    // J3b. The expected-value line shows the effective (per-car) threshold of 350.0.
    bool coldShowsPerCarThreshold = brakeSecCold.content.find("fade-low threshold of 350.0") != std::string::npos;
    CHECK_MSG(coldShowsPerCarThreshold, "J3b: finding must display the per-car fade-low threshold (350.0)");

    // J4. Unknown vehicle name -> no override applied -> NO finding even at extreme temp.
    LapStats lapUnknown = makeValidLap(1);
    fill(lapUnknown.brakeTempLF, 900.0);   // would exceed Alpine's window if it were matched
    lapUnknown.vehicleName = "Totally_Fake_Car_999";

    ReportResult rrUnknown = LapReportGenerator::generate(
        lapUnknown, ReportLanguage::English, ChannelProfile::AMS2);
    const SectionData& brakeSecUnk = rrUnknown[ReportSection::Brake];
    bool unkFindingSeen = (brakeSecUnk.content.find("FL brake temperature") != std::string::npos);
    CHECK_MSG(!unkFindingSeen, "J4: unknown vehicleName must NOT apply per-car fade window");

    // J5. Only the wheel with data is checked; other wheels stay silent even when out of range.
    LapStats lapOneWheel = makeValidLap(1);
    fill(lapOneWheel.brakeTempLF, 900.0);   // hot -> finding for FL only
    lapOneWheel.vehicleName = "Alpine_A110_GT4_Evo";

    ReportResult rrOneWheel = LapReportGenerator::generate(
        lapOneWheel, ReportLanguage::English, ChannelProfile::AMS2);
    const SectionData& brakeSecOW = rrOneWheel[ReportSection::Brake];
    bool flSeen = (brakeSecOW.content.find("FL brake temperature high") != std::string::npos);
    bool otherWheelsSilent = (brakeSecOW.content.find("FR brake temperature") == std::string::npos)
                          && (brakeSecOW.content.find("RL brake temperature") == std::string::npos)
                          && (brakeSecOW.content.find("RR brake temperature") == std::string::npos);
    CHECK_MSG(flSeen, "J5: hot FL disc must raise a finding");
    CHECK_MSG(otherWheelsSilent, "J5b: wheels without measured data must stay silent");

    // J6. French language variant produces the French title (ASCII-safe check).
    LapStats lapFr = makeValidLap(1);
    fill(lapFr.brakeTempLF, 900.0);   // > Alpine's fade-high -> finding in FR too
    lapFr.vehicleName = "Alpine_A110_GT4_Evo";

    ReportResult rrFr = LapReportGenerator::generate(
        lapFr, ReportLanguage::French, ChannelProfile::AMS2);
    const SectionData& brakeSecFr = rrFr[ReportSection::Brake];
    // The French title contains accented chars ("température", "élevée"), so we match on the
    // pure-ASCII parenthetical "(risque de fadage)" which only appears in the FR variant.
    bool frTitleSeen = (brakeSecFr.content.find("(risque de fadage)") != std::string::npos);
    CHECK_MSG(frTitleSeen, "J6: French finding title must appear for hot disc");

    std::printf("   brake temp per-car fade window OK\n");
}

// ===========================================================================
// J. Release info : parse JSON GitHub release + format date FR/EN
//    (fonctions pures de ReleaseInfo.h/.cpp - aucun reseau dans ces tests)
// ===========================================================================
static void testReleaseInfo() {
    section("J: Release info (parseLatestReleaseJson / formatReleaseDate)");

    // --- parse : echantillon positif complet -------------------------------
    const std::string sample =
        "{\n"
        "  \"tag_name\": \"v5.7\",\n"
        "  \"html_url\": \"https://github.com/jducable00-gif/MotecDataAnalyser/releases/tag/v5.7\",\n"
        "  \"published_at\": \"2026-10-24T18:30:00Z\",\n"
        "  \"body\": \"Nouveautes : version dynamique + notes de release\"\n"
        "}\n";

    AppRelease rel;
    CHECK_MSG(parseLatestReleaseJson(sample, rel), "parse JSON GitHub valide");
    CHECK(rel.tagName == "v5.7");
    CHECK(rel.htmlUrl.find("releases/tag/v5.7") != std::string::npos);
    CHECK(rel.publishedAt == "2026-10-24T18:30:00Z");
    CHECK(!rel.body.empty());

    // --- parse : champs optionnels absents ----------------------------------
    AppRelease relMin;
    const std::string minimal = "{\"tag_name\": \"v5.8\"}";
    CHECK_MSG(parseLatestReleaseJson(minimal, relMin), "parse JSON avec tag_name seul");
    CHECK(relMin.tagName == "v5.8");
    CHECK(relMin.htmlUrl.empty());

    // --- parse : cas negatifs -------------------------------------------------
    AppRelease bad;
    CHECK(!parseLatestReleaseJson("{ json invalide", bad));        // syntaxe casse
    CHECK(!parseLatestReleaseJson("{\"html_url\": \"x\"}", bad));   // tag_name absent
    CHECK(!parseLatestReleaseJson("{\"tag_name\": \"\"}", bad));     // tag_name vide

    // --- format date : FR / EN -------------------------------------------------
    const std::string iso = "2026-10-24T18:30:00Z";
    CHECK(formatReleaseDate(iso, true)  == "24 oct. 2026");   // aout sans accent (CP1252)
    CHECK(formatReleaseDate(iso, false) == "Oct 24, 2026");

    const std::string isoJan = "2027-01-03T08:00:00Z";
    CHECK(formatReleaseDate(isoJan, true)  == "3 janv. 2027");   // jour sans zero initial
    CHECK(formatReleaseDate(isoJan, false) == "Jan 3, 2027");

    // --- format date : valeurs invalides -> renvoyees telles quelles ----------
    CHECK(formatReleaseDate("pas une date", true) == "pas une date");                    // pas de tirets
    CHECK(formatReleaseDate("1999-13-40T00:00:00Z", false) == "1999-13-40T00:00:00Z");   // hors plage
}

// ===========================================================================
int main() {
    std::printf("Phase 4 diagnostic-engine test harness\n");
    std::printf("-----------------------------------------------\n");

    // Sanity-assert the INI-driven thresholds we rely on (defaults when no INI present).
    CHECK(g_thresholds.wheelSlipMax == 15.0);
    CHECK(std::fabs(g_thresholds.confidenceMedium - 0.60) < 1e-9);
    CHECK(std::fabs(g_thresholds.confidenceHigh   - 0.85) < 1e-9);

    testPhaseSegmentation();
    testChannelAbsentMatrix();
    testCauseRankingAndLabels();
    testLRAsymmetryVerdicts();
    testGateLogicPrimitives();
    testLegacyRuleNonRegression();
    testVehicleDb();
    testEngineCoolingPerCar();
    testBrakePerCar();
    testReleaseInfo();

    std::printf("\n-----------------------------------------------\n");
    if (g_failures == 0) {
        std::printf("ALL %d CHECKS PASSED\n", g_checks);
        return 0;
    }
    std::printf("%d/%d CHECKS FAILED\n", g_failures, g_checks);
    return 1;
}
