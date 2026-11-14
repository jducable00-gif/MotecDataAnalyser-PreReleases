//---------------------------------------------------------------------------
// MotecSetupAnalyzer.h — Types et API d'analyse de la télémétrie MoTeC.
//
// Définit les structures de données (LapStats, histogrammes de butées,
// statistiques de comportement), les enums (ChannelProfile, ReportLanguage),
// la structure Thresholds (seuils configurables) et déclare les fonctions
// d'analyse (parseMotecLd) et de génération de rapport
// (buildLapReport / buildBestThreeLapReport).
//
// Les implémentations se trouvent dans analyze_motec_setup.cpp.
//---------------------------------------------------------------------------

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cmath>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "LapReportGenerator.h"

enum class ChannelProfile {
    EvoM1 = 0,
    AMS2 = 1,
    Pmr = 2,          // PMR UDP live capture (no file import — profile only selects the live backend)
    R3e = 3,          // RaceRoom shared-memory live capture ("$R3E")
    Generic = AMS2
};

enum class ReportLanguage {
    English = 0,
    French = 1
};

// ===============================
// Thresholds (configurable via MotecDataAnalyser.thresholds.ini)
// Loaded once at startup; changes to the file only take effect on restart.
// ===============================
struct Thresholds {
    // General (minimum sample counts)
    int minDamperSamples = 20;
    int minHandlingSamples = 8;
    int minSteerYawSamples = 20;
    int handlingConfidenceSamples = 30;

    // Tyre pressure (psi) - common across channel profiles
    double pressureLowPsi = 23.0;
    double pressureHighPsi = 33.0;
    double pressureSideDiffMax = 1.5;

    // Rear vs front OUTER tyre temp diff for suspension balance check (deg C).
    // max(rear outers) - max(front outers) > this value -> "rear too stiff".
    // max(rear outers) <  max(front outers)              -> "rear too soft" (any negative triggers it).
    // Per-wheel worst-case comparison: no pair averaging, so the values
    // shown in the message match exactly what is displayed on the tyre chart.
    double rearOuterVsFrontStiffC = 5.0;

    // Camber target band (C): inner - outer spread per axle.
    // Front tyres run a wider acceptable window than rear tyres.
    double camberMinSpreadFront = 4.0;   // front: min inner-outer spread
    double camberMaxSpreadFront = 10.0;  // front: max inner-outer spread
    double camberMinSpreadRear  = 2.0;   // rear:  min inner-outer spread
    double camberMaxSpreadRear  = 7.0;   // rear:  max inner-outer spread

    // Tyre temperature left/right imbalance (C)
    double tyreCentreTempLrDiffMax = 8.0;   // centre surface temp L/R difference
    double tyreAirTempLrDiffMax = 5.0;      // air temp L/R difference (EVO TYRE_TAIR_XX)

    // Differential locking torque - per profile
    double lsdHighEvo = 500.0;
    double lsdLowEvo = 80.0;
    double lsdHighAms2 = 450.0;
    double lsdLowAms2 = 100.0;

    // Damper velocity (mm/s)
    double damperLowSpeedThreshold = 50.0;
    double damperAsymmetryThreshold = 8.0;
    double damperHighSpeedShareMax = 25.0;
    double damperLowSpeedShareMin = 92.0;
    double damperLowSpeedStdDevMax = 25.0;

    // Damper 4-zone targets (%): single target per zone, band = [target - tol, target + tol]
    double damperHscTarget = 15.0;   // Hi Comp (high-speed compression)
    double damperLscTarget = 35.0;   // Lo Comp (low-speed compression)
    double damperLsrTarget = 35.0;   // Lo Rebound (low-speed rebound)
    double damperHsrTarget = 15.0;   // Hi Rebound (high-speed rebound)
    // Per-zone tolerance: points outside target before escalating NORMAL?MODERATE?IMPORTANT
    double damperZoneToleranceHsc = 5.0;   // Hi Comp (high-speed compression)
    double damperZoneToleranceLsc = 5.0;   // Lo Comp (low-speed compression)
    double damperZoneToleranceLsr = 5.0;   // Lo Rebound (low-speed rebound)
    double damperZoneToleranceHsr = 5.0;   // Hi Rebound (high-speed rebound)

    // Handling detection
    double brakePedalMin = 10.0;
    double throttleMaxBraking = 35.0;
    double throttleMinAccelerating = 25.0;
    double steerMinCornering = 5.0;
    double latGMinCornering = 0.25;
    double steerChangeMin = 0.5;
    double yawChangeMin = 1.0;
    double latGMaxUndersteer = 1.5;
    double yawMinOversteer = 15.0;
    double steerChangeMaxOversteer = 1.0;
    double understeerRatioMin = 0.35;
    double oversteerRatioMin = 0.35;

    // Phase segmentation (driving-phase classifier, spec §2). Persisted in the
    // [PhaseSegmentation] INI section — no hardcoded universal values in logic.
    double phaseLatGMaxStraight = 0.15;      // |latG| below this on a straight (g)
    double phaseLongGMinAccel   = 0.08;      // longG above this counts as real acceleration (g); flat coasting stays Unknown
    double phaseSteerStableMax  = 2.5;       // |d(steer)| per frame for "steady steering" in MidCorner (deg)
    double phaseLatGMidMin      = 0.30;      // MinCorner: moderate lateral load lower bound (g)
    double phaseExitSteerDrop   = 1.5;       // ExitReThrottle: steer magnitude must fall by this much vs previous frame (deg)
    double phaseExitLatGMax     = 0.45;      // ...or latG must drop below this while accelerating out of MidCorner (g)
    double phaseBrakeDecelPeakMin   = 1.2;   // InitialBrake: peak deceleration zone, |longG| above this while braking (g)
    double phaseSpeedDropPerSec     = 3.0;   // BrakingApproach/InitialBrake: speed must fall by at least this vs previous frame (km/h per sample interval)
    double phaseLayoutBiasMinRatio  = 2.5;   // L/R corner-count ratio above which the minority direction is a likely layout artifact

    // Thermal (C)
    double waterTempMax = 95.0;
    double oilTempMax = 115.0;

    // Suspension
    double suspensionTravelMax = 8.0;
    double rideHeightDiffMax = 4.0;
    double suspensionVelocityMax = 5.0;
    double suspensionAvgTravelFrontMax = 50.0;
    double suspensionAvgTravelRearMax = 50.0;

    // Brake
    double brakeTempDiffMax = 25.0;
    double brakeBiasMax = 18.0;
    double brakeBiasFrontAvgMin = 90.0;
    double brakeTorqueMin = 1.0;
    double brakeChannelMismatchMax = 100.0;
    double brakeTorqueFrontShareMax = 65.0;
    double brakeTorqueFrontShareMin = 40.0;
    double brakeTorqueLrDiffRatio = 0.15;
    double brakeBiasSettingFrontMax = 60.0;
    double brakeBiasSettingRearMin = 40.0;

    // Channel mismatch detection
    double pressureChannelMismatchMax = 4.0;
    double pressureChannelMismatchMin = 2.0;
    double suspensionChannelMismatchMax = 5.0;
    double suspensionChannelMismatchMin = 2.0;

    // TCS / ABS
    double tcsActiveMax = 10.0;
    double absActiveMax = 10.0;

    // Wheel slip
    double wheelSlipMax = 15.0;

    // Wheel speed imbalance (km/h)
    double wheelSpeedFrontDiffMax = 6.0;
    double wheelSpeedRearDiffMax = 6.0;
    double wheelSpeedTempDiffMax = 8.0;

    // Traction loss
    double tractionRearDiffMax = 7.0;
    double tractionRearTempDiffMax = 8.0;
    double tractionLongGMax = 1.4;
    double tractionFrontDiffMax = 7.0;
    double tractionRearDiffMin = 5.0;
    double tractionRearTempSpreadMax = 10.0;
    double tractionRearPressureSpreadMax = 2.0;

    // Bumpstop
    double bumpStopMin = 0.1;

    // Aero
    double aeroBalanceMax = 8.0;
    double aeroHeightDiffMax = 5.0;
    double aeroSpeedMin = 280.0;

    // Steering / yaw correlation
    double steerYawCorrUndersteer = 0.30;
    double steerYawCorrOversteer = 0.75;
    double speedLowMax = 80.0;
    double speedMediumMax = 140.0;

    // Stability score
    double stabilityScoreMin = 70.0;
    double stabilityCorrUndersteer = 0.25;
    double stabilityCorrOversteer = 0.75;
    double stabilityWheelSpeedDiffMax = 6.0;
    double stabilityPressureDiffMax = 1.5;
    double stabilityTempDiffMax = 8.0;

    // Lap selection
    double lapTimeGapMax = 10.0;
    double minValidLapTimeMultiplier = 0.7; // Valid lap window: best * multiplier (lower bound)
    double maxValidLapTimeMultiplier = 1.3; // Valid lap window: best * multiplier (upper bound)

    // Confidence scoring
    double confidenceLow = 0.30;
    double confidenceMedium = 0.60;
    double confidenceHigh = 0.85;
    int minLapsForReproducibility = 3;
    double histogramYOffset = 20.0;   // Percentage headroom above the tallest histogram bin
    double histogramMinYPct = 20.0;   // Minimum displayed Y-axis range (%) — floor so small data isn't stretched to full scale
	double damperSlowSpeedRange = 50.0; // Symmetrical range around 0 to highlight slow damper section

    // Per-profile accessors
    double pressureLow(ChannelProfile) const {
        return pressureLowPsi;
    }
    double pressureHigh(ChannelProfile) const {
        return pressureHighPsi;
    }
    double lsdHigh(ChannelProfile profile) const {
        return (profile == ChannelProfile::AMS2) ? lsdHighAms2 : lsdLowEvo;
    }
    double lsdLow(ChannelProfile profile) const {
        return (profile == ChannelProfile::AMS2) ? lsdLowAms2 : lsdLowEvo;
    }
};

extern Thresholds g_thresholds;

// ===============================
// AnalysisSettings: centralizes the thresholds used by the analysis engine and
// keeps the rule layer independent from the UI and simulator-specific code.
// ===============================
struct TyreThresholds {
    // Per-axle inner/outer temp spread band (C)
    double camberMinSpreadFront = 4.0;   // front: min
    double camberMaxSpreadFront = 10.0;  // front: max
    double camberMinSpreadRear  = 2.0;   // rear:  min
    double camberMaxSpreadRear  = 7.0;   // rear:  max
    double innerOuterSpreadC = 5.0;
    double sideImbalanceMaxC = 8.0;

    // Rear vs front OUTER tyre temp diff (deg C) -> suspension balance check on Tyre tab.
    double rearOuterVsFrontStiffC = 5.0;
};

struct EngineThresholds {
    double waterTempWarnC = 100.0;
    double oilTempWarnC = 110.0;
};

// Per-wheel brake fade window (deg C). Unlike engine cooling, there is no sane
// universal default for absolute disc temperature — it varies wildly by car
// (~350-850 deg C) and only exists in the per-car VehicleDb reference data. So
// both flags start false: with an unknown vehicle the rule produces NO finding
// (no silent-gap risk), exactly like a stricter engine-cooling override. The
// LapReportGenerator populates these from Ams2VehicleDb when the car is known.
struct WheelBrakeTemps {
    bool   hasFadeLow  = false;   // FadeRangeBelow present ?
    double fadeLowC    = 0.0;     // below this -> disc too cold to be in its operating window (cold-side efficiency loss)
    bool   hasFadeHigh = false;   // FadeRangeAbove present ?
    double fadeHighC   = 0.0;     // above this -> overheating / thermal fade

    WheelBrakeTemps() {}          // C++Builder: constructeur explicite vide requis
};

struct BrakeThresholds {
    WheelBrakeTemps wheels[4];    // [FL, FR, RL, RR] -- index: kWheelFl=0..kWheelRr=3
};

struct HandlingThresholds {
    double wheelSlipMax = 15.0;
    double steerMinCornering = 5.0;
    double yawChangeMin = 1.0;
    double yawMinOversteer = 15.0;
    double steerChangeMin = 0.5;
};

struct AnalysisSettings {
    TyreThresholds tyres;
    EngineThresholds engine;
    BrakeThresholds brakes;       // per-wheel fade window (populated from VehicleDb when car is known)
    HandlingThresholds handling;
    bool tyreAnalysisEnabled = true;
    bool engineAnalysisEnabled = true;
    bool brakeTempAnalysisEnabled = true;   // gate for the absolute disc-temp rule
    bool pressureAnalysisEnabled = true;
    ReportLanguage language = ReportLanguage::English;
};

enum class AnalysisAlias {
    WaterTemp,
    OilTemp,
    SteerAngleAvg,
    YawRateAvg,
    WheelSlipFrontAvg,
    WheelSlipRearAvg,
    TyrePressFL,
    TyrePressFR,
    TyrePressRL,
    TyrePressRR,
    TyreTempFLInner,
    TyreTempFLMiddle,
    TyreTempFLOuter,
    TyreTempFRInner,
    TyreTempFRMiddle,
    TyreTempFROuter,
    TyreTempRLInner,
    TyreTempRLMiddle,
    TyreTempRLOuter,
    TyreTempRRInner,
    TyreTempRRMiddle,
    TyreTempRROuter,
    RideHeightFL,
    RideHeightFR,
    RideHeightRL,
    RideHeightRR,
    Rake,

    // --- Handling evidence (Phase 0: diagnostic engine) ---
    SteerYawCorrelation,          // Pearson r of steer vs yaw over the lap [-1..+1]
    UndersteerIndicatorsEntryPct,   // % of corner-entry samples flagged understeer [0..100]
    OversteerIndicatorsExitPct,     // % of corner-exit samples flagged oversteer  [0..100]
    HandlingSampleCountEntry,       // total corner-entry indicator sample count
    HandlingSampleCountExit,        // total corner-exit indicator sample count

    // --- Per-corner wheel slip (replaces front/rear averages for diagnostics) ---
    WheelSlipLF,
    WheelSlipRF,
    WheelSlipLR,
    WheelSlipRR,

    // --- Brakes ---
    BrakeTorqueFrontAvg,          // mean of LF+RF brake torque (Nm)
    BrakeTorqueRearAvg,           // mean of LR+RR brake torque (Nm)
    BrakeBiasSettingAvg,          // AMS2: "Brake Bias Setting" channel average
    TcsActivePct,                 // % of samples with TCS active [0..100]
    AbsActivePct,                 // % of samples with ABS active  [0..100]
    BrakeTempLF,                  // front-left brake/disc temperature (deg C)
    BrakeTempRF,                  // front-right brake/disc temperature (deg C)
    BrakeTempLR,                  // rear-left brake/disc temperature (deg C)
    BrakeTempRR,                  // rear-right brake/disc temperature (deg C)

    // --- Wheel speeds (axle averages) ---
    WheelSpeedFrontAvg,
    WheelSpeedRearAvg,

    // --- Suspension travel & bumpstop engagement per corner ---
    SusTravelMeanLF,
    SusTravelMeanRF,
    SusTravelMeanLR,
    SusTravelMeanRR,
    BumpStopEngagementPctLF,      // % of samples with bumpstop active [0..100]
    BumpStopEngagementPctRF,
    BumpStopEngagementPctLR,
    BumpStopEngagementPctRR,

    // --- Damper histogram summaries (axle-level) ---
    DamperAsymmetryFront,         // mean bump-rebound asymmetry % across LF+RF [0..100]
    DamperAsymmetryRear,          // mean bump-rebound asymmetry % across LR+RR
    DamperHighSpeedShareFront,    // high-speed share % (bump+rebound) front axle
    DamperHighSpeedShareRear,

    // --- Direct suspension velocity from sim feed (AMS2 "Susp Vel XX") ---
    SuspVelMeanLF,
    SuspVelMeanRF,
    SuspVelMeanLR,
    SuspVelMeanRR,

    // --- Vehicle / aero / environment ---
    AeroBalanceAvg,               // EVO: aero balance channel average
    LsdTorqueAvg,                 // differential locking torque (Nm)
    LatGAvg,                      // mean lateral G over the lap
    ThrottleAvgPct,               // mean throttle [0..100]
    LongGAvg,                     // mean longitudinal G over the lap
    BrakePedalAvgPct,             // mean brake pedal input [0..100]
    FuelLevelAvg,                 // fuel level (ratio or L — profile-dependent)
    TrackTempAvg,
    AmbientTempAvg,

    // --- Tyre air temps per corner (EVO TYRE_TAIR_XX) ---
    TyreAirTempLF,
    TyreAirTempRF,
    TyreAirTempLR,
    TyreAirTempRR
};

enum class VirtualChannelOperation {
    DifferenceOfAverages
};

enum class MappingSource {
    MotecFile,
    LiveSharedMemory,
    Virtual
};

enum class ChannelMappingType {
    Raw,
    Virtual
};

struct ChannelMapping {
    std::string alias;
    ChannelMappingType type = ChannelMappingType::Raw;
    ChannelProfile profile = ChannelProfile::Generic;
    MappingSource source = MappingSource::MotecFile;
    std::string channel;
    std::string standardUnit;
    std::string sourceUnit;
    double factor = 1.0;
    double offset = 0.0;
    AnalysisAlias virtualAlias = AnalysisAlias::Rake;
    VirtualChannelOperation operation = VirtualChannelOperation::DifferenceOfAverages;
    std::vector<AnalysisAlias> positiveInputs;
    std::vector<AnalysisAlias> negativeInputs;
};

// ===============================
// DataProvider: single access gateway for channel values. This is the first
// abstraction layer of the refactor plan and allows rules to request aliases or
// virtual channels independently from the simulator-specific mapping.
// ===============================
class DataProvider {
public:
    void clear() {
        m_values.clear();
        m_virtualMappings.clear();
    }

    void setValue(AnalysisAlias alias, double value) {
        m_values[alias] = value;
    }

    void setChannelMappings(const std::vector<ChannelMapping>& mappings) {
        m_virtualMappings.clear();
        for (std::size_t i = 0; i < mappings.size(); ++i) {
            if (mappings[i].type == ChannelMappingType::Virtual)
                m_virtualMappings[mappings[i].virtualAlias] = mappings[i];
        }
    }

    bool hasValue(AnalysisAlias alias) const {
        double value = 0.0;
        return getValue(alias, value);
    }

    bool getValue(AnalysisAlias alias, double& outValue) const {
        return getValueInternal(alias, outValue, 0);
    }

    const std::map<AnalysisAlias, double>& values() const {
        return m_values;
    }

private:
    bool getValueInternal(AnalysisAlias alias, double& outValue, std::size_t depth) const {
        if (depth > 16)
            return false;

        std::map<AnalysisAlias, double>::const_iterator it = m_values.find(alias);
        if (it != m_values.end()) {
            outValue = it->second;
            return true;
        }

        std::map<AnalysisAlias, ChannelMapping>::const_iterator definition = m_virtualMappings.find(alias);
        if (definition == m_virtualMappings.end() ||
            definition->second.operation != VirtualChannelOperation::DifferenceOfAverages ||
            definition->second.positiveInputs.empty() || definition->second.negativeInputs.empty())
            return false;

        double positiveSum = 0.0;
        for (std::size_t i = 0; i < definition->second.positiveInputs.size(); ++i) {
            double value = 0.0;
            if (!getValueInternal(definition->second.positiveInputs[i], value, depth + 1))
                return false;
            positiveSum += value;
        }

        double negativeSum = 0.0;
        for (std::size_t i = 0; i < definition->second.negativeInputs.size(); ++i) {
            double value = 0.0;
            if (!getValueInternal(definition->second.negativeInputs[i], value, depth + 1))
                return false;
            negativeSum += value;
        }

        outValue = positiveSum / definition->second.positiveInputs.size() -
                   negativeSum / definition->second.negativeInputs.size();
        return std::isfinite(outValue);
    }

    std::map<AnalysisAlias, double> m_values;
    std::map<AnalysisAlias, ChannelMapping> m_virtualMappings;
};

enum class AnalysisSeverity {
    Info = 0,
    Warning = 1,
    Critical = 2
};

struct AnalysisResult {
    AnalysisSeverity severity = AnalysisSeverity::Info;
    std::string category;
    std::string expectedValue;   // logic sentence: the condition/thresholds that triggered this finding (rendered first)
    std::string title;
    std::string description;
    std::string recommendation;
};

inline std::string normalizeCategoryName(const std::string& category) {
    std::string normalized = category;
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });
    return normalized;
}

inline ReportSection mapAnalysisResultToReportSection(const AnalysisResult& result) {
    const std::string category = normalizeCategoryName(result.category);
    if (category == "ENGINE" || category == "ENGINECOOLING" || category == "COOLING")
        return ReportSection::EngineCooling;
    if (category == "TYRES" || category == "TYRE")
        return ReportSection::Tyre;
    if (category == "BRAKES" || category == "BRAKE")
        return ReportSection::Brake;
    if (category == "SUSPENSION" || category == "DAMPER")
        return ReportSection::Suspension;
    if (category == "HANDLING" || category == "TRACTION" || category == "STEERING")
        return ReportSection::Handling;
    return ReportSection::General;
}

class AnalysisRule {
public:
    virtual ~AnalysisRule() {}
    virtual std::vector<AnalysisAlias> requiredInputs() const = 0;
    virtual std::vector<AnalysisResult> evaluate(const DataProvider& provider,
                                                const AnalysisSettings& settings) const = 0;
};

class AnalysisRegistry {
public:
    void addRule(std::unique_ptr<AnalysisRule> rule) {
        if (rule)
            m_rules.push_back(std::move(rule));
    }

    std::vector<AnalysisResult> evaluateAll(const DataProvider& provider,
                                           const AnalysisSettings& settings) const {
        std::vector<AnalysisResult> results;
        for (std::size_t i = 0; i < m_rules.size(); ++i) {
            const std::vector<AnalysisResult> ruleResults = m_rules[i]->evaluate(provider, settings);
            results.insert(results.end(), ruleResults.begin(), ruleResults.end());
        }
        return results;
    }

private:
    std::vector<std::unique_ptr<AnalysisRule> > m_rules;
};

class EngineTemperatureRule : public AnalysisRule {
public:
    std::vector<AnalysisAlias> requiredInputs() const override {
        return std::vector<AnalysisAlias>{ AnalysisAlias::WaterTemp, AnalysisAlias::OilTemp };
    }

    std::vector<AnalysisResult> evaluate(const DataProvider& provider,
                                        const AnalysisSettings& settings) const override {
        std::vector<AnalysisResult> results;
        auto tr = [&](const std::string& en, const std::string& fr) -> std::string {
            return settings.language == ReportLanguage::French ? fr : en;
        };

        if (settings.engineAnalysisEnabled) {
            auto fmtC = [](double v) -> std::string {
                char buf[32];
                snprintf(buf, sizeof(buf), "%.1f", v);
                return std::string(buf);
            };

            double waterTemp = 0.0;
            if (provider.getValue(AnalysisAlias::WaterTemp, waterTemp) && waterTemp > settings.engine.waterTempWarnC) {
                AnalysisResult result;
                result.severity = AnalysisSeverity::Critical;
                result.category = "Engine";
                result.expectedValue = tr("Water at ", "Eau à ") + fmtC(waterTemp)
                    + tr(" deg C exceeds warning threshold of ", " deg C depasse le seuil d'alerte de ")
                    + fmtC(settings.engine.waterTempWarnC) + tr(" deg C", " deg C");
                result.title = tr("Water temperature high", "Température d'eau élevée");
                result.description = tr("Water temperature exceeds the warning threshold.", "La température de l'eau dépasse le seuil d'alerte.");
                result.recommendation = tr("Check radiator flow, coolant level and fan operation.", "Vérifier le flux du radiateur, le niveau de liquide de refroidissement et le fonctionnement du ventilateur.");
                results.push_back(result);
            }

            double oilTemp = 0.0;
            if (provider.getValue(AnalysisAlias::OilTemp, oilTemp) && oilTemp > settings.engine.oilTempWarnC) {
                AnalysisResult result;
                result.severity = AnalysisSeverity::Critical;
                result.category = "Engine";
                result.expectedValue = tr("Oil at ", "Huile à ") + fmtC(oilTemp)
                    + tr(" deg C exceeds warning threshold of ", " deg C depasse le seuil d'alerte de ")
                    + fmtC(settings.engine.oilTempWarnC) + tr(" deg C", " deg C");
                result.title = tr("Oil temperature high", "Température d'huile élevée");
                result.description = tr("Oil temperature exceeds the warning threshold.", "La température de l'huile dépasse le seuil d'alerte.");
                result.recommendation = tr("Verify oil cooling and reduce sustained load.", "Vérifier le refroidissement de l'huile et réduire la charge soutenue.");
                results.push_back(result);
            }
        }

        return results;
    }
};

// Absolute per-wheel disc temperature vs the car's fade window (VehicleDb Layer 1).
// Mirrors EngineTemperatureRule: a finding is only produced when BOTH the measured
// value AND an effective threshold exist for that wheel. With no vehicle reference
// data, every has* flag stays false and this rule emits nothing — so there is never
// a silent gap from comparing against a made-up universal number.
class BrakeTemperatureRule : public AnalysisRule {
public:
    std::vector<AnalysisAlias> requiredInputs() const override {
        return std::vector<AnalysisAlias>{
            AnalysisAlias::BrakeTempLF, AnalysisAlias::BrakeTempRF,
            AnalysisAlias::BrakeTempLR, AnalysisAlias::BrakeTempRR
        };
    }

    std::vector<AnalysisResult> evaluate(const DataProvider& provider,
                                        const AnalysisSettings& settings) const override {
        std::vector<AnalysisResult> results;

        if (!settings.brakeTempAnalysisEnabled)
            return results;

        auto tr = [&](const std::string& en, const std::string& fr) -> std::string {
            return settings.language == ReportLanguage::French ? fr : en;
        };

        // Locale-independent number formatting for the expected-value sentences.
        auto fmtC = [](double v) -> std::string {
            char buf[32];
            snprintf(buf, sizeof(buf), "%.1f", v);
            return std::string(buf);
        };

        static const char* positions[] = { "FL", "FR", "RL", "RR" };
        static const AnalysisAlias tempAliases[] = {
            AnalysisAlias::BrakeTempLF, AnalysisAlias::BrakeTempRF,
            AnalysisAlias::BrakeTempLR, AnalysisAlias::BrakeTempRR
        };

        for (std::size_t i = 0; i < 4; ++i) {
            const std::string pos = positions[i];
            double temp = 0.0;
            if (!provider.getValue(tempAliases[i], temp))
                continue;   // no measured value on this wheel -> nothing to compare

            const WheelBrakeTemps& wbt = settings.brakes.wheels[i];

            // High side: disc above the fade-high window -> overheating / thermal fade.
            if (wbt.hasFadeHigh && temp > wbt.fadeHighC) {
                AnalysisResult result;
                result.severity = AnalysisSeverity::Critical;
                result.category = "Brake";   // maps to ReportSection::Brake via mapAnalysisResultToReportSection
                result.expectedValue = tr(pos + " disc at ", pos + " frein à ") + fmtC(temp)
                    + tr(" deg C exceeds the fade-high threshold of ", " deg C depasse le seuil de fadage haut de ")
                    + fmtC(wbt.fadeHighC) + tr(" deg C", " deg C");
                result.title = pos + tr(" brake temperature high (fade risk)", " température frein élevée (risque de fadage)");
                result.description = pos + tr(" disc is running above the vehicle's fade-high window; braking efficiency will drop.", " disque au-dessus de la plage haute du véhicule ; l'efficacité du freinage va chuter.");
                result.recommendation = tr("Reduce sustained hard-braking load, check brake cooling and consider a higher-temperature pad compound.", "Réduire les phases de fort freinage soutenu, vérifier le refroidissement des freins et envisager un garnissage haute température.");
                results.push_back(result);
            }

            // Low side: disc below the fade-low window -> too cold to be in its operating range.
            if (wbt.hasFadeLow && temp < wbt.fadeLowC) {
                AnalysisResult result;
                result.severity = AnalysisSeverity::Warning;
                result.category = "Brake";   // maps to ReportSection::Brake via mapAnalysisResultToReportSection
                result.expectedValue = tr(pos + " disc at ", pos + " frein à ") + fmtC(temp)
					+ tr(" deg C is below the fade-low threshold of ", " deg C est sous le seuil d'éfficacité ")
                    + fmtC(wbt.fadeLowC) + tr(" deg C", " deg C");
                result.title = pos + tr(" brake temperature low (cold)", " température frein basse (froid)");
				result.description = pos + tr(" disc is below the vehicle's operating window; pads are not at full efficiency.", " disque sous la plage de fonctionnement du véhicule ; les plaquettes de freins ne sont pas à pleine efficacité.");
				result.recommendation = tr("check for brake cooling ducts.", "vérifier que les entrées de refroidissement.");
				results.push_back(result);
            }
        }

        return results;
    }
};

class TyreTemperatureRule : public AnalysisRule {
public:
    std::vector<AnalysisAlias> requiredInputs() const override {
        return std::vector<AnalysisAlias>{
            AnalysisAlias::TyreTempFLInner, AnalysisAlias::TyreTempFLMiddle, AnalysisAlias::TyreTempFLOuter,
            AnalysisAlias::TyreTempFRInner, AnalysisAlias::TyreTempFRMiddle, AnalysisAlias::TyreTempFROuter,
            AnalysisAlias::TyreTempRLInner, AnalysisAlias::TyreTempRLMiddle, AnalysisAlias::TyreTempRLOuter,
            AnalysisAlias::TyreTempRRInner, AnalysisAlias::TyreTempRRMiddle, AnalysisAlias::TyreTempRROuter
        };
    }

    std::vector<AnalysisResult> evaluate(const DataProvider& provider,
                                        const AnalysisSettings& settings) const override {
        std::vector<AnalysisResult> results;

        if (!settings.tyreAnalysisEnabled)
            return results;

        auto tr = [&](const std::string& en, const std::string& fr) -> std::string {
            return settings.language == ReportLanguage::French ? fr : en;
        };

        // Locale-independent number formatting for the expected-value sentences.
        auto fmtC = [](double v) -> std::string {
            char buf[32];
            snprintf(buf, sizeof(buf), "%.1f", v);
            return std::string(buf);
        };

        static const char* positions[] = { "FL", "FR", "RL", "RR" };
        static const AnalysisAlias innerAliases[] = {
            AnalysisAlias::TyreTempFLInner, AnalysisAlias::TyreTempFRInner,
            AnalysisAlias::TyreTempRLInner, AnalysisAlias::TyreTempRRInner
        };
        static const AnalysisAlias middleAliases[] = {
            AnalysisAlias::TyreTempFLMiddle, AnalysisAlias::TyreTempFRMiddle,
            AnalysisAlias::TyreTempRLMiddle, AnalysisAlias::TyreTempRRMiddle
        };
        static const AnalysisAlias outerAliases[] = {
            AnalysisAlias::TyreTempFLOuter, AnalysisAlias::TyreTempFROuter,
            AnalysisAlias::TyreTempRLOuter, AnalysisAlias::TyreTempRROuter
        };

        for (std::size_t i = 0; i < 4; ++i) {
            const std::string pos = positions[i];

            // Per-axle spread thresholds: front tyres use a wider window than rear.
            // Positions array order is {FL, FR, RL, RR} → indices 0/1 are front axle.
            const bool   isFrontAxle = (i < 2);
            const double minSpread   = isFrontAxle ? g_thresholds.camberMinSpreadFront : g_thresholds.camberMinSpreadRear;
            const double maxSpread   = isFrontAxle ? g_thresholds.camberMaxSpreadFront : g_thresholds.camberMaxSpreadRear;

            double inner  = 0.0;
            double middle = 0.0;
            double outer  = 0.0;

            const bool hasInner = provider.getValue(innerAliases[i], inner);
            const bool hasMiddle = provider.getValue(middleAliases[i], middle);
            const bool hasOuter = provider.getValue(outerAliases[i], outer);

			if (hasInner && hasOuter) {
				if (inner <= outer + minSpread) {
					AnalysisResult result;
					result.severity = AnalysisSeverity::Warning;
					result.category = "Tyres";
					result.expectedValue = tr("Expected value for inner-outer temp spread more than ",
				                                "Valeur attendue pour l'écart de température interne/externe supérieur à ") + fmtC(minSpread) + tr(" C", " °C");
					result.title = pos + tr(" insufficient negative camber", " carrossage négatif insuffisant");
					result.description = pos + tr(" tyre inner band is not hotter than the outer band.", " pneu : la bande interne n'est pas plus chaude que la bande externe.");
					result.recommendation = tr("Increase negative camber slightly and re-check on comparable laps.", "Augmenter légèrement le carrossage négatif et revérifier sur des tours comparables.");
					results.push_back(result);
				}
			}

			if (hasInner && hasOuter) {
				if (inner >= outer + maxSpread) {
					AnalysisResult result;
					result.severity = AnalysisSeverity::Warning;
					result.category = "Tyres";
					result.expectedValue = tr("Expected value for inner-outer temp spread less than ",
				                                "Valeur attendue pour l'écart de température interne/externe inférieur à ") + fmtC(maxSpread) + tr(" C", " °C");
					result.title = pos + tr(" too much negative camber", " carrossage négatif excessif");
					result.description = pos + tr(" tyre inner band is too much hotter than the outer band.", " pneu : la bande interne est trop chaude par rapport à la bande externe.");
					result.recommendation = tr("decrease negative camber slightly and re-check on comparable laps.", "Diminuer légèrement le carrossage négatif et revérifier sur des tours comparables.");
					results.push_back(result);
				}
			}

			if (hasInner && hasMiddle && hasOuter) {
                const double shoulderMin = std::min(inner, outer);
                const double shoulderMax = std::max(inner, outer);

                if (middle <= shoulderMin) {
                    AnalysisResult result;
                    result.severity = AnalysisSeverity::Warning;
                    result.category = "Tyres";
                    result.expectedValue = tr("Expected value for center band temperature between both shoulders (inner and outer).",
                                                "Valeur attendue : la bande centrale doit se situer entre les deux épaules (interne et externe).");
                    result.title = pos + tr(" center band below shoulders", " bande centrale sous les épaules");
                    result.description = pos + tr(" tyre center band is not strictly between the inner and outer bands; both shoulders are as hot or hotter.", " pneu : la bande centrale n'est pas strictement entre les bandes interne et externe ; les deux épaules sont aussi chaudes ou plus chaudes.");
                    result.recommendation = tr("Raise the starting pressure slightly and re-check the hot tyre pressure and temperature profile.", "Augmenter légèrement la pression de départ et revérifier la pression et le profil de température du pneu à chaud.");
                    results.push_back(result);
                } else if (middle >= shoulderMax) {
                    AnalysisResult result;
                    result.severity = AnalysisSeverity::Warning;
                    result.category = "Tyres";
                    result.expectedValue = tr("Expected value for center band temperature between both shoulders (inner and outer).",
                                                "Valeur attendue : la bande centrale doit se situer entre les deux épaules (interne et externe).");
                    result.title = pos + tr(" center band above shoulders", " bande centrale au-dessus des épaules");
                    result.description = pos + tr(" tyre center band is not strictly between the inner and outer bands; the center is as hot or hotter.", " pneu : la bande centrale n'est pas strictement entre les bandes interne et externe ; le centre est aussi chaud ou plus chaud.");
                    result.recommendation = tr("Reduce the starting pressure slightly and re-check the hot tyre pressure and temperature profile.", "Réduire légèrement la pression de départ et revérifier la pression et le profil de température du pneu à chaud.");
                    results.push_back(result);
                }
            }
        }

        return results;
    }
};

class TyreSuspensionBalanceRule : public AnalysisRule {
public:
    std::vector<AnalysisAlias> requiredInputs() const override {
        return std::vector<AnalysisAlias>{
            AnalysisAlias::TyreTempFLOuter, AnalysisAlias::TyreTempFROuter,
            AnalysisAlias::TyreTempRLOuter, AnalysisAlias::TyreTempRROuter
        };
    }

    std::vector<AnalysisResult> evaluate(const DataProvider& provider,
                                        const AnalysisSettings& settings) const override {
        std::vector<AnalysisResult> results;

        auto tr = [&](const std::string& en, const std::string& fr) -> std::string {
            return settings.language == ReportLanguage::French ? fr : en;
        };

        if (!settings.tyreAnalysisEnabled)
            return results;

        double fl = 0.0;
        double fr = 0.0;
        double rl = 0.0;
        double rr = 0.0;
        bool hasFl = provider.getValue(AnalysisAlias::TyreTempFLOuter, fl);
        bool hasFr = provider.getValue(AnalysisAlias::TyreTempFROuter, fr);
        bool hasRl = provider.getValue(AnalysisAlias::TyreTempRLOuter, rl);
        bool hasRr = provider.getValue(AnalysisAlias::TyreTempRROuter, rr);

        if (!hasFl || !hasFr || !hasRl || !hasRr)
            return results;

        // Compare the hottest wheel of each axle directly — no averaging,
        // so every value in the message is visible on the tyre chart.
        const double frontMax = std::max(fl, fr);
        const double rearMax  = std::max(rl, rr);
        const double diffC    = rearMax - frontMax;

        if (diffC > settings.tyres.rearOuterVsFrontStiffC) {
            AnalysisResult result;
            result.severity = AnalysisSeverity::Warning;
            result.category = "Tyres";
            result.title = tr("Rear suspension may be too stiff",
                              "Suspension arrière peut-être trop dure");
            char descEn[240];
            snprintf(descEn, sizeof(descEn),
                     "Rear outer tyre temperature is %.1f C higher than the front (rear RL/RR: %.1f / %.1f vs front FL/FR: %.1f / %.1f).",
                     diffC, rl, rr, fl, fr);
            char descFr[300];
            snprintf(descFr, sizeof(descFr),
                     "La température externe des pneus arrière est %.1f C plus élevée que l'avant (arrière RL/RR : %.1f / %.1f vs avant FL/FR : %.1f / %.1f).",
                     diffC, rl, rr, fl, fr);
            result.description = tr(descEn, descFr);
            result.recommendation = tr("Soften the rear suspension (spring rate / damper) to bring rear tyre temps in line with the front.",
                                       "Adoucir la suspension arrière (taux de ressort / amortisseur) pour aligner les températures des pneus arrière sur l'avant.");
            results.push_back(result);
        } else if (diffC < 0.0) {
            AnalysisResult result;
            result.severity = AnalysisSeverity::Warning;
            result.category = "Tyres";
            result.title = tr("Rear suspension may be too soft",
                              "Suspension arrière peut-être trop molle");
            char descEn[240];
            snprintf(descEn, sizeof(descEn),
                     "Rear outer tyre temperature is %.1f C lower than the front (rear RL/RR: %.1f / %.1f vs front FL/FR: %.1f / %.1f).",
                     -diffC, rl, rr, fl, fr);
            char descFr[300];
            snprintf(descFr, sizeof(descFr),
                     "La température externe des pneus arrière est %.1f C plus basse que l'avant (arrière RL/RR : %.1f / %.1f vs avant FL/FR : %.1f / %.1f).",
                     -diffC, rl, rr, fl, fr);
            result.description = tr(descEn, descFr);
            result.recommendation = tr("Stiffen the rear suspension (spring rate / damper) to bring rear tyre temps in line with the front.",
                                       "Resserrer la suspension arrière (taux de ressort / amortisseur) pour aligner les températures des pneus arrière sur l'avant.");
            results.push_back(result);
        }

        return results;
    }
};

class HandlingBalanceRule : public AnalysisRule {
public:
    std::vector<AnalysisAlias> requiredInputs() const override {
        return std::vector<AnalysisAlias>{
            AnalysisAlias::SteerAngleAvg, AnalysisAlias::YawRateAvg,
            AnalysisAlias::WheelSlipRearAvg, AnalysisAlias::WheelSlipFrontAvg
        };
    }

    std::vector<AnalysisResult> evaluate(const DataProvider& provider,
                                        const AnalysisSettings& settings) const override {
        std::vector<AnalysisResult> results;

        auto tr = [&](const std::string& en, const std::string& fr) -> std::string {
            return settings.language == ReportLanguage::French ? fr : en;
        };

        double steer = 0.0;
        double yaw = 0.0;
        double rearSlip = 0.0;
        double frontSlip = 0.0;

        const bool hasSteer = provider.getValue(AnalysisAlias::SteerAngleAvg, steer);
        const bool hasYaw = provider.getValue(AnalysisAlias::YawRateAvg, yaw);
        const bool hasRearSlip = provider.getValue(AnalysisAlias::WheelSlipRearAvg, rearSlip);
        const bool hasFrontSlip = provider.getValue(AnalysisAlias::WheelSlipFrontAvg, frontSlip);

        if (hasRearSlip && rearSlip > settings.handling.wheelSlipMax) {
            AnalysisResult result;
            result.severity = AnalysisSeverity::Warning;
            result.category = "Handling";
            result.title = tr("Rear wheel slip high", "Patinage des roues arrière élevé");
            result.description = tr("Rear wheels are spending too much time slipping compared to the handling baseline.",
                                    "Les roues arrière patinent trop par rapport à la référence de comportement.");
            result.recommendation = tr("Reduce rear slip with more rear grip, a softer differential setup or better traction.",
                                       "Réduire le patinage arrière avec plus d'adhérence arrière, un différentiel plus souple ou une meilleure traction.");
            results.push_back(result);
        }

        if (hasFrontSlip && frontSlip > settings.handling.wheelSlipMax) {
            AnalysisResult result;
            result.severity = AnalysisSeverity::Warning;
            result.category = "Handling";
            result.title = tr("Front wheel slip high", "Patinage des roues avant élevé");
            result.description = tr("Front wheels are slipping more than expected under cornering or braking.",
                                    "Les roues avant patinent plus que prévu en virage ou au freinage.");
            result.recommendation = tr("Check brake bias, front tyre load and front grip balance.",
                                       "Vérifier le répartiteur de freinage, la charge des pneus avant et l'équilibre d'adhérence avant.");
            results.push_back(result);
        }

        if (hasSteer && hasYaw) {
            if (std::abs(steer) > settings.handling.steerMinCornering && std::abs(yaw) < settings.handling.yawChangeMin) {
                AnalysisResult result;
                result.severity = AnalysisSeverity::Info;
                result.category = "Handling";
                result.title = tr("Potential understeer", "Sous-virage potentiel");
                result.description = tr("Steering input is present while yaw response remains low, suggesting understeer.",
                                        "Une sollicitation du volant est présente tandis que la réponse en lacet reste faible, suggérant un sous-virage.");
                result.recommendation = tr("Increase front load or reduce front grip imbalance to improve turn-in.",
                                           "Augmenter la charge avant ou réduire le déséquilibre d'adhérence avant pour améliorer l'entrée en virage.");
                results.push_back(result);
            }

            if (std::abs(yaw) > settings.handling.yawMinOversteer && std::abs(steer) < settings.handling.steerChangeMin) {
                AnalysisResult result;
                result.severity = AnalysisSeverity::Warning;
                result.category = "Handling";
                result.title = tr("Potential oversteer", "Survirage potentiel");
                result.description = tr("The car is rotating faster than expected for the steering input.",
                                        "La voiture tourne plus vite que prévu pour la sollicitation du volant.");
                result.recommendation = tr("Reduce rear slip or add rear grip to improve stability.",
                                           "Réduire le patinage arrière ou ajouter de l'adhérence arrière pour améliorer la stabilité.");
                results.push_back(result);
            }
        }

        return results;
    }
};

// ===============================
// CauseEntry: one possible cause for a detected symptom, with a weight (0-1)
// and the suggested adjustment direction.
// ===============================
struct CauseEntry {
    std::string cause;        // e.g. "rear_rebound_too_high"
    double weight;            // 0.0 — 1.0 (relative probability)
    std::string adjustment;   // e.g. "reduce_rear_rebound"
    std::string direction;    // e.g. "softer", "stiffer", "reduce", "increase"
};

// ===============================
// Recommendation: structured diagnostic output replacing the old flat RecItem.
// Contains confidence, evidence, possible causes, and validation channels.
// ===============================
struct Recommendation {
    std::string symptom;              // e.g. "rear_traction_loss_on_exit"
    std::string text;                 // display text (localized)
    bool issue = false;               // true = problem detected
    double confidence = 0.0;          // 0.0 — 1.0
    std::string dataQuality;          // "HIGH" / "MEDIUM" / "LOW" / "INSUFFICIENT"
    std::vector<std::string> evidence;   // e.g. ["rear wheel slip +14%", "rebound velocity 320 mm/s"]
    std::vector<CauseEntry> possibleCauses;
    std::string direction;            // "softer" / "stiffer" / "reduce" / "increase"
    std::string suggestedChange;      // "-1 click" or "slightly" if amplitude uncertain
    std::vector<std::string> validationChannels; // e.g. ["rear_wheel_slip", "exit_speed"]
    std::string context;              // e.g. "corner exit, throttle > 70%"
};

// ===============================
// Phase 1 — Diagnostic Engine Types
// ===============================

/// Declares which channels a diagnostic rule needs to run at all (required)
/// and which improve confidence when present but are not mandatory (optional).
struct ChannelRequirements {
    std::vector<AnalysisAlias> required;   // ALL must be present → else abstain entirely
    std::vector<AnalysisAlias> optional;   // missing reduces confidence, recorded on finding
};

/// A single diagnostic detection: one symptom with evidence, ranked causes and traceability.
struct DiagnosticFinding {
    std::string symptomId;           // matches RuleConfig catalog id (e.g. "rear_traction_loss_on_exit")
    AnalysisSeverity severity = AnalysisSeverity::Info;
    double confidence = 0.0;         // internal score [0..1] before label mapping
    std::string dataQualityLabel;    // "LOW" / "MEDIUM" / "HIGH" (from g_thresholds)
    std::vector<std::string> evidence;   // concrete measurements, tr()-localized
    std::vector<CauseEntry> rankedCauses;// sorted by adjusted weight desc — full list always
    std::vector<AnalysisAlias> missingChannels;  // traceability: optional channels that were absent
    std::vector<std::string> confounders;        // e.g. "ABS active on 40% of braking samples"
    std::string phaseWindow;         // human-readable context, e.g. "corner exit"

    bool empty() const { return symptomId.empty(); }
};

/// Abstract base for Phase-1 diagnostic detectors (separate from the legacy AnalysisRule).
class DiagnosticRule {
public:
    virtual ~DiagnosticRule() = default;
    /// Channels this rule needs to evaluate. If any required channel is missing,
    /// the detector must return an empty finding (abstain entirely — no silent gaps).
    virtual ChannelRequirements channelRequirements() const = 0;
    /// Returns a DiagnosticFinding with symptomId == "" if data is insufficient.
    virtual DiagnosticFinding evaluate(const DataProvider& provider,
                                       const AnalysisSettings& settings) const = 0;
};

/// Registry of diagnostic rules — mirrors the legacy AnalysisRegistry pattern.
struct DiagnosticRegistry {
    std::vector<std::unique_ptr<DiagnosticRule>> rules;

    void add(std::unique_ptr<DiagnosticRule> rule) { rules.push_back(std::move(rule)); }

    /// Evaluate all registered detectors against a DataProvider. Empty findings are dropped.
    std::vector<DiagnosticFinding> evaluateAll(const DataProvider& provider,
                                               const AnalysisSettings& settings) const {
        std::vector<DiagnosticFinding> findings;
        for (const auto& r : rules) {
            DiagnosticFinding f = r->evaluate(provider, settings);
            if (!f.empty()) findings.push_back(std::move(f));
        }
        return findings;
    }

    bool empty() const { return rules.empty(); }
};

// --- Confidence scoring helpers (plan §Step 5) -----------------------------

/// Compute the weighted 4-component confidence score.
/// @param thresholdProximity  min(1, |value|/threshold - 1) for primary indicator [0..1]
/// @param sampleCount         number of samples backing the primary measurement
/// @param lapsReproduced      how many individual laps show this symptom (>=1 if detected here)
/// @param channelCoverage     available declared channels / total declared [0..1]
inline double computeDiagnosticConfidence(double thresholdProximity, int sampleCount,
                                          int lapsReproduced, double channelCoverage) {
    const Thresholds& t = g_thresholds;
    double proximityScore  = 40.0 * std::min(1.0, std::max(0.0, thresholdProximity));
    double sampleScore     = 25.0 * std::min(1.0, static_cast<double>(sampleCount) / (t.minHandlingSamples > 0 ? t.minHandlingSamples : 8));
    int minLaps            = t.minLapsForReproducibility > 0 ? t.minLapsForReproducibility : 3;
    double reproScore      = 20.0 * std::min(1.0, static_cast<double>(lapsReproduced) / minLaps);
    double coverageScore   = 15.0 * std::max(0.0, channelCoverage);

    double score = (proximityScore + sampleScore + reproScore + coverageScore) / 100.0;

    // Single-lap cap: confidence cannot exceed MEDIUM boundary with only one lap
    if (lapsReproduced <= 1 && t.confidenceMedium > 0.0) {
        score = std::min(score, t.confidenceMedium);
    }
    return score;
}

/// Map a [0..1] confidence to the LOW / MEDIUM / HIGH label from g_thresholds.
inline const char* diagnosticConfidenceLabel(double confidence) {
    if (confidence >= g_thresholds.confidenceHigh)   return "HIGH";
    if (confidence >= g_thresholds.confidenceMedium) return "MEDIUM";
    return "LOW";
}

/// Check channel requirements against a DataProvider.
/// Returns false immediately if any REQUIRED channel is missing (abstain).
/// On success, fills outMissingOptional with absent optional channels for traceability.
inline bool checkChannelRequirements(const ChannelRequirements& req, const DataProvider& provider,
                                     std::vector<AnalysisAlias>& outMissingOptional) {
    for (const auto alias : req.required) {
        if (!provider.hasValue(alias)) return false;  // hard gate — no silent gaps
    }

    outMissingOptional.clear();
    for (const auto alias : req.optional) {
        if (!provider.hasValue(alias)) outMissingOptional.push_back(alias);
    }
    return true;
}

/// Compute channel coverage ratio: available / total declared. Returns 1.0 when nothing is declared.
inline double computeChannelCoverage(const ChannelRequirements& req, const DataProvider& provider) {
    int total = static_cast<int>(req.required.size()) + static_cast<int>(req.optional.size());
    if (total == 0) return 1.0;

    int available = 0;
    for (const auto alias : req.required)   if (provider.hasValue(alias)) ++available;
    for (const auto alias : req.optional)   if (provider.hasValue(alias)) ++available;

    return static_cast<double>(available) / total;
}

// ===============================
// StatSummary (robuste)
// ===============================
struct StatSummary {
    double sum  = 0.0;
    double min  = std::numeric_limits<double>::infinity();
    double max  = -std::numeric_limits<double>::infinity();
    std::size_t count = 0;

    void add(double value) {
        if (!std::isfinite(value)) return;
        sum += value;
        if (value < min) min = value;
        if (value > max) max = value;
        ++count;
    }

    double average() const {
        if (count == 0) return 0.0;
        return sum / static_cast<double>(count);
    }
};

// ===============================
// LapDelta: first/last value tracker for per-lap rate calculations
// Used to compute consumption/wear rates from start-of-lap vs end-of-lap values.
// ===============================
struct LapDelta {
    double first = 0.0;
    double last  = 0.0;
    std::size_t count = 0;

    void record(double v) {
        if (!std::isfinite(v)) return;
        if (count == 0) first = v;
        last = v;
        ++count;
    }

    bool valid() const { return count >= 2; }

    // Rate of change over the lap: positive means value increased.
    double delta() const { return valid() ? last - first : 0.0; }
};

// ===============================
// VelocityHistogram: distribution of damper shaft speed samples (mm/s),
// used to evaluate whether the curve approaches an ideal bell shape.
// ===============================
struct VelocityHistogram {
    static const int kBinCount = 41; // -205..+205 mm/s in 10 mm/s bins, center bin at [-5, 5]

    std::size_t bins[kBinCount] = {};
    std::size_t totalCount = 0;
    double sum = 0.0;
    double sumSq = 0.0;

    static double binWidth() { return 10.0; }
    static double rangeMax() { return 205.0; }

    void add(double velocityMmPerSec) {
        if (!std::isfinite(velocityMmPerSec)) return;

        // Filter out extreme noise spikes (e.g., > 500 mm/s) to avoid distorting stats.
        if (std::abs(velocityMmPerSec) > 500.0) return;

        totalCount++;
        sum += velocityMmPerSec;
        sumSq += velocityMmPerSec * velocityMmPerSec;

        // Only increment bins for values within the visible range to avoid outer-bin bloating.
        if (velocityMmPerSec >= -rangeMax() && velocityMmPerSec <= rangeMax()) {
            int bin = static_cast<int>(std::floor((velocityMmPerSec + rangeMax()) / binWidth()));
            if (bin < 0) bin = 0;
            if (bin >= kBinCount) bin = kBinCount - 1;
            bins[bin]++;
        }
    }

    double mean() const {
        return totalCount == 0 ? 0.0 : sum / static_cast<double>(totalCount);
    }

    double stdDev() const {
        if (totalCount < 2) return 0.0;
        double m = mean();
        double variance = sumSq / static_cast<double>(totalCount) - m * m;
        return variance > 0.0 ? std::sqrt(variance) : 0.0;
    }

    // Percentage of samples whose velocity falls in [loInclusive, hiExclusive).
    double percentInRange(double loInclusive, double hiExclusive) const {
        if (totalCount == 0) return 0.0;
        std::size_t c = 0;
        for (int i = 0; i < kBinCount; ++i) {
            double binCenter = -rangeMax() + (i + 0.5) * binWidth();
            if (binCenter >= loInclusive && binCenter < hiExclusive)
                c += bins[i];
        }
        return 100.0 * static_cast<double>(c) / static_cast<double>(totalCount);
    }

    void merge(const VelocityHistogram& other) {
        for (int i = 0; i < kBinCount; ++i)
            bins[i] += other.bins[i];
        totalCount += other.totalCount;
        sum += other.sum;
        sumSq += other.sumSq;
    }

    // Returns % of samples in [-slowRange, 0)
    double percentReboundLow(double slowRange) const {
        if (totalCount == 0) return 0.0;
        std::size_t c = 0;
        for (int i = 0; i < kBinCount; ++i) {
            double v = -rangeMax() + (i + 0.5) * binWidth();
            if (v < 0 && v >= -slowRange) c += bins[i];
        }
        return 100.0 * static_cast<double>(c) / static_cast<double>(totalCount);
    }

    // Returns % of samples in (-infinity, -slowRange)
    double percentReboundHigh(double slowRange) const {
        if (totalCount == 0) return 0.0;
        std::size_t c = 0;
        for (int i = 0; i < kBinCount; ++i) {
            double v = -rangeMax() + (i + 0.5) * binWidth();
            if (v < -slowRange) c += bins[i];
        }
        return 100.0 * static_cast<double>(c) / static_cast<double>(totalCount);
    }

    // Returns % of samples in (slowRange, +infinity)
    double percentBumpHigh(double slowRange) const {
        if (totalCount == 0) return 0.0;
        std::size_t c = 0;
        for (int i = 0; i < kBinCount; ++i) {
            double v = -rangeMax() + (i + 0.5) * binWidth();
            if (v > slowRange) c += bins[i];
        }
        return 100.0 * static_cast<double>(c) / static_cast<double>(totalCount);
    }

    // Returns % of samples in (0, slowRange]
    double percentBumpLow(double slowRange) const {
        if (totalCount == 0) return 0.0;
        std::size_t c = 0;
        for (int i = 0; i < kBinCount; ++i) {
            double v = -rangeMax() + (i + 0.5) * binWidth();
            if (v > 0 && v <= slowRange) c += bins[i];
        }
        return 100.0 * static_cast<double>(c) / static_cast<double>(totalCount);
    }

    // Returns average percentage of the side: (Slow% + Fast%) / 2
    double percentReboundAvg(double slowRange) const {
        return (percentReboundLow(slowRange) + percentReboundHigh(slowRange)) / 2.0;
    }

    double percentBumpAvg(double slowRange) const {
        return (percentBumpLow(slowRange) + percentBumpHigh(slowRange)) / 2.0;
    }

    // Returns average velocity of samples in (slowRange, +infinity)
    double bumpAverage(double slowRange) const {
        std::size_t c = 0;
        double s = 0.0;
        for (int i = 0; i < kBinCount; ++i) {
            double v = -rangeMax() + (i + 0.5) * binWidth();
            if (v > slowRange) {
                c += bins[i];
                s += bins[i] * v;
            }
        }
        return (c == 0) ? 0.0 : s / static_cast<double>(c);
    }

    // --- 4-zone percentages on BIN CENTERS, same rule as the combined chart
    // legend in MotecSetupForm::computeCombinedCornerStats(): a bin is slow iff
    // |center| <= slowRange (the center bin at ~0 belongs to NEITHER side).
    // Sign convention: positive velocity = compression/bump.

    double percentLsc(double B) const { return pctZone(B, +1, false); }   // 0 < v <= B (low-speed compression)
    double percentHsc(double B) const { return pctZone(B, +1, true);  }   // v > +B     (high-speed compression)
    double percentLsr(double B) const { return pctZone(B, -1, false); }   // -B <= v < 0 (low-speed rebound)
    double percentHsr(double B) const { return pctZone(B, -1, true);  }   // v < -B     (high-speed rebound)

private:
    // Bin-center rule identical to MotecSetupForm::computeCombinedCornerStats():
    // side split on the sign of the bin center (the ~0 center bin belongs to NEITHER
    // side), then slow iff |center| <= B. Returns % of all samples in that zone.
    double pctZone(double slowRange, int sideSign, bool highSpeed) const {
        if (totalCount == 0) return 0.0;
        std::size_t c = 0;
        for (int i = 0; i < kBinCount; ++i) {
            double v = -rangeMax() + (i + 0.5) * binWidth();
            const int sign = (v > 0.0) ? 1 : ((v < 0.0) ? -1 : 0);   // center bin (~0): neither side
            if (sign != sideSign) continue;
            const bool fast = std::abs(v) > slowRange + 1e-9;
            if (fast == highSpeed) c += bins[i];
        }
        return 100.0 * static_cast<double>(c) / static_cast<double>(totalCount);
    }
};

// ===============================
// DamperCornerStats: derives shaft velocity from consecutive suspension
// travel samples (dTravel/dt) and accumulates it into a VelocityHistogram.
// ===============================
struct DamperCornerStats {
    VelocityHistogram histogram;
    double lastTravel = std::numeric_limits<double>::quiet_NaN();
    double lastTime = std::numeric_limits<double>::quiet_NaN();

    void addSample(double travelMm, double timeSeconds) {
        if (!std::isfinite(lastTravel)) {         // first sample: record baseline and return
            lastTravel = travelMm;
            lastTime   = timeSeconds;
            return;
        }

        const bool changed = std::abs(travelMm - lastTravel) > 1e-6;
        if (!changed) return;                     // stale page — keep previous baseline intact

        // dt = wall-clock elapsed since the PREVIOUS new frame (QPC precision).
        // This naturally equals one game physics step (~3–5 ms at 270 Hz) plus small poll jitter.
        const double dt = timeSeconds - lastTime;
        if (!(dt > 0.001 && dt < 0.5)) return;    // sanity guard (shouldn't happen in practice)

        histogram.add((travelMm - lastTravel) / dt);
        lastTravel = travelMm;   // update baseline ONLY on a genuine new frame
        lastTime = timeSeconds;
    }

    void merge(const DamperCornerStats& other) {
        histogram.merge(other.histogram);
    }
};

// ===============================
// Damper 4-zone analysis (HSC/LSC/LSR/HSR)
// Sign convention: positive velocity = compression/bump, negative = rebound.
// Zone split at ±damperSlowSpeedRange mm/s on BIN CENTERS (same rule as the
// combined chart legend in MotecSetupForm::computeCombinedCornerStats):
//   |v| <= B  -> low speed : LSC (compression) / LSR (rebound)
//   v > +B    -> HSC       ;  v < -B -> HSR
// ===============================
enum class DamperZoneSeverity { Normal, Moderate, Important };

struct DamperZoneResult {
    double pct = 0.0;                    // measured percentage of samples in zone
    DamperZoneSeverity severity = DamperZoneSeverity::Normal;
    bool aboveTarget = false;            // true if above target max, false if below target min
};

struct DamperZoneReport {
    DamperZoneResult hsc;
    DamperZoneResult lsc;
    DamperZoneResult lsr;
    DamperZoneResult hsr;

    // Global balances (diagnostic only)
    double compressionPct = 0.0;         // HSC + LSC
    double reboundPct = 0.0;             // LSR + HSR
    double lowSpeedPct = 0.0;            // LSC + LSR
    double highSpeedPct = 0.0;           // HSC + HSR

    bool hasData = false;                // enough samples to evaluate
    std::string text;                    // localized recommendation (empty if all normal)

    bool anyIssue() const {
        // Any zone outside its tolerance band (Moderate or Important) is an issue.
        return hsc.severity != DamperZoneSeverity::Normal ||
               lsc.severity != DamperZoneSeverity::Normal ||
               lsr.severity != DamperZoneSeverity::Normal ||
               hsr.severity != DamperZoneSeverity::Normal;
    }
};

struct HandlingPhaseStats {
    std::size_t sampleCount = 0;
    std::size_t understeerIndicators = 0;
    std::size_t oversteerIndicators = 0;
    std::size_t brakingSamples = 0;
    std::size_t accelerationSamples = 0;
    double suspensionAsymmetrySum = 0.0;

    void addIndicator(bool understeer, bool oversteer, bool braking,
                      bool accelerating, double suspensionAsymmetry) {
        ++sampleCount;
        if (understeer) ++understeerIndicators;
        if (oversteer) ++oversteerIndicators;
        if (braking) ++brakingSamples;
        if (accelerating) ++accelerationSamples;
        if (std::isfinite(suspensionAsymmetry))
            suspensionAsymmetrySum += suspensionAsymmetry;
    }

    void merge(const HandlingPhaseStats& other) {
        sampleCount += other.sampleCount;
        understeerIndicators += other.understeerIndicators;
        oversteerIndicators += other.oversteerIndicators;
        brakingSamples += other.brakingSamples;
        accelerationSamples += other.accelerationSamples;
        suspensionAsymmetrySum += other.suspensionAsymmetrySum;
    }
};

// ===============================
// Driving-phase segmentation (spec §2)
// Each frame is classified into exactly one phase by LapStats::addHandlingSample().
// Unknown = criteria not met — never guessed. All thresholds live in g_thresholds
// ([PhaseSegmentation] INI section).
// ===============================
enum class DrivingPhase : unsigned char {
    Unknown = 0,             // insufficient data to classify (spec §2: use "unknown", don't guess)
    StraightStableAccel,     // throttle > threshold, low latG, no brake
    BrakingApproach,         // braking + speed decreasing, pre-corner
    InitialBrake,            // peak deceleration zone while braking
    CornerEntryRelease,      // brake releasing / steering demand growing (existing cornerEntry bucket)
    MidCorner,               // steady steering, moderate latG, throttle < threshold
    ExitReThrottle           // existing cornerExit bucket: accel rising + steer decreasing

};

static const int kDrivingPhaseCount = 7;   // one past last phase — size of LapStats::phases[]

// Per-phase signal accumulator (modeled on VelocityHistogram). Only finite values are kept.
struct PhaseAccumulator {
    StatSummary latG;                 // all samples of the phase
    StatSummary steerAngle;
    StatSummary yawRate;
    StatSummary wheelSlipFrontAvg;   // per-frame front-axle slip average, when available

    // Per-side lateral load: enables L/R comparison WITHIN comparable phases (spec §4).
    StatSummary latGLeftHander;      // samples on left-handers (steer > cornering threshold)
    StatSummary latGRighther;        // ...right-handers. NOTE: field name avoids the "Right" keyword clash in bcc32c.

    std::size_t leftHanderSamples = 0;     // |steer| above cornering threshold, steer > 0 (left)
    std::size_t rightHanderSamples = 0;    // ...and steer < 0 (right). Used for layout-artifact detection.

    void add(double gLat, double steerDeg, double yawDps) {
        if (!std::isfinite(gLat)) return;
        latG.add(gLat);
        steerAngle.add(steerDeg);     // StatSummary ignores non-finite values itself
        yawRate.add(yawDps);
        const bool cornering = std::abs(steerDeg) > g_thresholds.steerMinCornering && std::isfinite(steerDeg);
        if (cornering) {
            if (steerDeg > 0.0) { ++leftHanderSamples; latGLeftHander.add(gLat); }
            else               { ++rightHanderSamples; latGRighther.add(gLat); }
        }
    }

    void addWheelSlipFront(double slipPct) { wheelSlipFrontAvg.add(slipPct); }   // no-op when NaN/absent

    std::size_t sampleCount() const { return latG.count; }

    bool hasData(int minSamples = 1) const {
        return static_cast<int>(latG.count) >= minSamples && minSamples > 0;
    }

    void merge(const PhaseAccumulator& other) {
        // StatSummary is sum/min/max/count — recombine directly.
        if (other.latG.count == 0) return;   // nothing to add (min/max stay untouched when empty)
        latG.sum += other.latG.sum;
        steerAngle.sum += other.steerAngle.sum;
        yawRate.sum += other.yawRate.sum;
        wheelSlipFrontAvg.sum += other.wheelSlipFrontAvg.sum;
        if (other.latG.min < latG.min)  latG.min = other.latG.min;
        if (other.latG.max > latG.max)  latG.max = other.latG.max;
        if (other.steerAngle.count > 0 && other.steerAngle.min < steerAngle.min) steerAngle.min = other.steerAngle.min;
        if (other.steerAngle.count > 0 && other.steerAngle.max > steerAngle.max) steerAngle.max = other.steerAngle.max;
        if (other.yawRate.count > 0 && other.yawRate.min < yawRate.min)          yawRate.min = other.yawRate.min;
        if (other.yawRate.count > 0 && other.yawRate.max > yawRate.max)          yawRate.max = other.yawRate.max;
        if (other.wheelSlipFrontAvg.count > 0 && other.wheelSlipFrontAvg.min < wheelSlipFrontAvg.min) wheelSlipFrontAvg.min = other.wheelSlipFrontAvg.min;
        if (other.wheelSlipFrontAvg.count > 0 && other.wheelSlipFrontAvg.max > wheelSlipFrontAvg.max) wheelSlipFrontAvg.max = other.wheelSlipFrontAvg.max;
        latGLeftHander.sum += other.latGLeftHander.sum;
        latGRighther.sum   += other.latGRighther.sum;
        if (other.latGLeftHander.count > 0 && other.latGLeftHander.min < latGLeftHander.min) latGLeftHander.min = other.latGLeftHander.min;
        if (other.latGLeftHander.count > 0 && other.latGLeftHander.max > latGLeftHander.max) latGLeftHander.max = other.latGLeftHander.max;
        if (other.latGRighther.count   > 0 && other.latGRighther.min   < latGRighther.min)   latGRighther.min   = other.latGRighther.min;
        if (other.latGRighther.count   > 0 && other.latGRighther.max   > latGRighther.max)   latGRighther.max   = other.latGRighther.max;
        latG.count += other.latG.count;
        steerAngle.count += other.steerAngle.count;
        yawRate.count += other.yawRate.count;
        wheelSlipFrontAvg.count += other.wheelSlipFrontAvg.count;
        latGLeftHander.count += other.latGLeftHander.count;
        latGRighther.count   += other.latGRighther.count;
        leftHanderSamples  += other.leftHanderSamples;
        rightHanderSamples += other.rightHanderSamples;
    }

    // Layout-balance ratio: max(L,R) / min(L+R). High value = phase data dominated by one corner direction.
    double layoutBiasRatio() const {
        if (leftHanderSamples + rightHanderSamples == 0) return 1.0;   // no directional info — neutral, not an artifact signal
        const std::size_t hi = leftHanderSamples > rightHanderSamples ? leftHanderSamples : rightHanderSamples;
        const std::size_t lo = (leftHanderSamples + rightHanderSamples) - hi;
        if (lo == 0) return static_cast<double>(hi);   // one direction only -> maximal bias
        return static_cast<double>(hi) / static_cast<double>(lo);
    }
};

// Asymmetry verdicts (spec §4): a difference observed between comparable conditions,
// one likely explained by track/phase mix, or too little data to tell.
enum class AsymmetryVerdict {
    InconclusiveInsufficientData = 0,   // not enough samples in the compared phases — no conclusion at all
    LikelyLayoutPhaseArtifact,          // comparable-phase sample counts are heavily skewed (e.g., track has more R-handers)
    RepeatedInComparableConditions      // both sides have sufficient data and the difference persists -> genuine candidate signal
};

enum class AsymmetryAxis { FrontAxle = 0, RearAxle = 1 };   // L/R comparison on one axle; front/rear uses phase averages directly

struct PhaseAsymmetryResult {
    AsymmetryVerdict verdict = AsymmetryVerdict::InconclusiveInsufficientData;
    double leftAvg = std::numeric_limits<double>::quiet_NaN();     // metric average over left-hand corner samples of the compared phases
    double rightAvg = std::numeric_limits<double>::quiet_NaN();    // ...right-hand
    int leftSamples = 0;
    int rightSamples = 0;

    bool isArtifact() const { return verdict == AsymmetryVerdict::LikelyLayoutPhaseArtifact; }
};

enum class RideHeightUnit {
    Millimeters,
    Centimeters
};

// ===============================
// LapStats (blind? + coh?rent MoTeC)
// ===============================
struct LapStats {
    int lapNumber = 0;
    double startTime = 0.0;
    double endTime = 0.0;
    std::size_t rowCount = 0;
    RideHeightUnit rideHeightUnit = RideHeightUnit::Millimeters;

    std::string driverName;
    std::string vehicleName;
    std::string trackName;

    // Core driving channels
    StatSummary speed;
    StatSummary throttle;
    StatSummary brakePedal;
    StatSummary rpm;
    StatSummary steerAngle;
    StatSummary yawRate;
    StatSummary latG;
    StatSummary longG;

    // Wheel speeds
    StatSummary wheelSpeedLF;
    StatSummary wheelSpeedRF;
    StatSummary wheelSpeedLR;
    StatSummary wheelSpeedRR;

    // Brake temps
    StatSummary brakeTempLF;
    StatSummary brakeTempRF;
    StatSummary brakeTempLR;
    StatSummary brakeTempRR;

    // Tyre pressures
    StatSummary pressureLF;
    StatSummary pressureRF;
    StatSummary pressureLR;
    StatSummary pressureRR;

    // Tyre centre temps
    StatSummary surfTempCFL;
    StatSummary surfTempCRF;
    StatSummary surfTempCLR;
    StatSummary surfTempCRR;

    // Tyre shoulder temps (inner / centre / outer)
    StatSummary tyreInnerTempLF;
    StatSummary tyreCentreTempLF;
    StatSummary tyreOuterTempLF;
    StatSummary tyreInnerTempRF;
    StatSummary tyreCentreTempRF;
    StatSummary tyreOuterTempRF;
    StatSummary tyreInnerTempLR;
    StatSummary tyreCentreTempLR;
    StatSummary tyreOuterTempLR;
    StatSummary tyreInnerTempRR;
    StatSummary tyreCentreTempRR;
    StatSummary tyreOuterTempRR;

    // Tyre air temps (EVO TYRE_TAIR_XX)
    StatSummary tyreAirTempLF;
    StatSummary tyreAirTempRF;
    StatSummary tyreAirTempLR;
    StatSummary tyreAirTempRR;

    // Brake torque (EVO BRAKE_TORQUE_XX)
    StatSummary brakeTorqueLF;
    StatSummary brakeTorqueRF;
    StatSummary brakeTorqueLR;
    StatSummary brakeTorqueRR;

    // Wheel slip ratio (EVO WHEEL_SR_XX)
    StatSummary wheelSlipLF;
    StatSummary wheelSlipRF;
    StatSummary wheelSlipLR;
    StatSummary wheelSlipRR;

    // Direct suspension velocity (AMS2 Susp Vel XX)
    VelocityHistogram suspVelLF;
    VelocityHistogram suspVelRF;
    VelocityHistogram suspVelLR;
    VelocityHistogram suspVelRR;

    // Brake bias setting (AMS2 Brake Bias Setting)
    StatSummary brakeBiasSetting;

    // TCS / ABS activation (EVO TC/ABS, AMS2 TCS Active/ABS Active)
    StatSummary tcsActive;
    StatSummary absActive;

    // Suspension
    StatSummary susTravelLF;
    StatSummary susTravelRF;
    StatSummary susTravelLR;
    StatSummary susTravelRR;

    StatSummary bumpStopLF;
    StatSummary bumpStopRF;
    StatSummary bumpStopLR;
    StatSummary bumpStopRR;

    // Damper shaft velocity histograms, derived from suspension travel
    DamperCornerStats damperLF;
    DamperCornerStats damperRF;
    DamperCornerStats damperLR;
    DamperCornerStats damperRR;

    // Entry and exit handling evidence derived from the synchronized samples.
    HandlingPhaseStats cornerEntry;
    HandlingPhaseStats cornerExit;
    bool handlingHasPrevious = false;
    double previousHandlingSteer = 0.0;
    double previousHandlingYaw = 0.0;

    // Driving-phase segmentation (spec §2): per-frame classifier state + accumulators.
    PhaseAccumulator phases[kDrivingPhaseCount];   // indexed by static_cast<int>(phase) — size is one past ExitReThrottle
    std::size_t phaseClassifiedSamples = 0;        // samples that met ALL criteria of a known phase (confidence numerator)
    std::size_t handlingSampleTotal = 0;           // every sample accepted into addHandlingSample() (denominator)

    DrivingPhase currentPhase = DrivingPhase::Unknown;   // previous frame's verdict — used by ExitReThrottle ("out of MidCorner")
    double previousPhaseSpeedKmh = std::numeric_limits<double>::quiet_NaN();  // NaN until the first finite speed arrives
    bool phaseHasPreviousSpeed = false;

    // Aero
    StatSummary aeroHeight0;
    StatSummary aeroHeight1;
    StatSummary aeroHeight2;
    StatSummary aeroHeight3;
    StatSummary aeroBalance;

    // Differential
    StatSummary lsdTorque;

    // Environmental / fuel / thermal monitoring
    StatSummary oilTemp;
    StatSummary waterTemp;
    StatSummary fuelLevel;
    StatSummary fuelPressure;
    StatSummary trackTemp;
    StatSummary ambientTemp;

    // --- Prediction: per-lap first/last deltas for rate calculations ---
    LapDelta fuelLevelEnds;          // start/end fuel level (ratio) -> consumption L/lap
    StatSummary  fuelCapacityL;      // tank capacity in litres (constant, averaged)

    LapDelta tyreWearLF, tyreWearRF, tyreWearLR, tyreWearRR;   // % worn per lap
    LapDelta padLifeFL, padLifeFR, padLifeRL, padLifeRR;       // EVO: life remaining 1->0
    LapDelta discLifeFL, discLifeFR, discLifeRL, discLifeRR;   // EVO: life remaining 1->0
    LapDelta brakeDamageLF, brakeDamageRF, brakeDamageLR, brakeDamageRR; // AMS2: damage 0->up
    LapDelta clutchWear;                                                    // AMS2 only

    // Steering/Yaw correlation
    double steerYawCov = 0.0;
    double steerSum = 0.0;
    double yawSum = 0.0;
    double steerSqSum = 0.0;
    double yawSqSum = 0.0;
    std::size_t steerYawCount = 0;   // IMPORTANT : compteur ddi

    // ===============================
    // addHandlingSample (blind) + driving-phase classifier (spec §2).
    // speedKmh is optional: when absent/NaN, phases that need a frame-to-frame
    // speed delta degrade to Unknown — never guess. All thresholds live in
    // g_thresholds ([PhaseSegmentation] INI section), no hardcoded values here.
    // ===============================
    void addHandlingSample(double steer, double yaw, double latG, double longG,
                           double brake, double throttle, double suspensionAsymmetry,
                           double speedKmh = std::numeric_limits<double>::quiet_NaN()) {
        if (!std::isfinite(steer) || !std::isfinite(yaw) ||
            !std::isfinite(latG) || !std::isfinite(longG))
            return;

        const bool braking = brake > g_thresholds.brakePedalMin && throttle < g_thresholds.throttleMaxBraking;
        const bool accelerating = throttle > g_thresholds.throttleMinAccelerating && brake < g_thresholds.brakePedalMin;
        const bool cornering = std::abs(steer) > g_thresholds.steerMinCornering && std::abs(latG) > g_thresholds.latGMinCornering;
        if (handlingHasPrevious && cornering) {
            const double steerChange = std::abs(steer) - std::abs(previousHandlingSteer);
            const double yawChange = std::abs(yaw) - std::abs(previousHandlingYaw);
            const bool steeringDemandGrowing = steerChange > g_thresholds.steerChangeMin;
            const bool yawResponseGrowing = yawChange > g_thresholds.yawChangeMin;
            const bool understeer = steeringDemandGrowing && !yawResponseGrowing && std::abs(latG) < g_thresholds.latGMaxUndersteer;
            const bool oversteer = yawResponseGrowing && std::abs(yaw) > g_thresholds.yawMinOversteer &&
                                   std::abs(steer) <= std::abs(previousHandlingSteer) + g_thresholds.steerChangeMaxOversteer;

            if (braking)
                cornerEntry.addIndicator(understeer, oversteer, true, false, suspensionAsymmetry);
            if (accelerating)
                cornerExit.addIndicator(understeer, oversteer, false, true, suspensionAsymmetry);
        }

        // ---- Driving-phase classifier (spec §2): one verdict per frame -----------------
        // Priority: InitialBrake -> BrakingApproach -> CornerEntryRelease -> ExitReThrottle
        //          -> MidCorner  -> StraightStableAccel -> Unknown.
        // Every phase requires ALL of its criteria; anything not fully met stays Unknown.
        const bool hasSpeed = std::isfinite(speedKmh);
        double speedDropPerSec = 0.0;   // positive while decelerating (prev - cur)
        if (hasSpeed && phaseHasPreviousSpeed)
            speedDropPerSec = previousPhaseSpeedKmh - speedKmh;

        const bool brakingNow     = brake > g_thresholds.brakePedalMin && throttle < g_thresholds.throttleMaxBraking;   // == `braking` above, kept explicit for the classifier block
        const double absLatG      = std::abs(latG);
        const double steerDropDeg = handlingHasPrevious ? (std::abs(previousHandlingSteer) - std::abs(steer)) : 0.0;

        DrivingPhase phase = DrivingPhase::Unknown;
        if (brakingNow && hasSpeed && speedDropPerSec >= g_thresholds.phaseSpeedDropPerSec) {
            // Braking with a real frame-to-frame speed loss: peak zone vs approach.
            if (std::abs(longG) > g_thresholds.phaseBrakeDecelPeakMin)
                phase = DrivingPhase::InitialBrake;      // deep in the brake application
            else
                phase = DrivingPhase::BrakingApproach;   // braking, decelerating, not at peak load yet
        } else if (!brakingNow && cornering && (currentPhase == DrivingPhase::BrakingApproach || currentPhase == DrivingPhase::InitialBrake)) {
            phase = DrivingPhase::CornerEntryRelease;    // brake released onto steering demand
        } else if (accelerating && !brakingNow && currentPhase == DrivingPhase::MidCorner &&
                   (steerDropDeg >= g_thresholds.phaseExitSteerDrop || absLatG < g_thresholds.phaseExitLatGMax)) {
            phase = DrivingPhase::ExitReThrottle;        // accel rising + steer/latG falling out of the corner
        } else if (!brakingNow && !accelerating && cornering && absLatG >= g_thresholds.phaseLatGMidMin) {
            const double dSteerDeg = handlingHasPrevious ? std::abs(std::abs(steer) - std::abs(previousHandlingSteer)) : 0.0;
            if (dSteerDeg <= g_thresholds.phaseSteerStableMax)
                phase = DrivingPhase::MidCorner;         // steady steering under moderate lateral load, no throttle/brake demand
        } else if (!brakingNow && accelerating && absLatG <= g_thresholds.phaseLatGMaxStraight && longG >= g_thresholds.phaseLongGMinAccel) {
            phase = DrivingPhase::StraightStableAccel;   // real acceleration on a straight (flat coasting stays Unknown)
        }

        if (phase != DrivingPhase::Unknown) {
            ++phaseClassifiedSamples;                    // confidence numerator: criteria fully met, never guessed
            phases[static_cast<int>(phase)].add(latG, steer, yaw);   // per-side split happens inside add() when cornering
        }

        previousHandlingSteer = steer;
        previousHandlingYaw = yaw;
        handlingHasPrevious = true;

        currentPhase = phase;                            // Unknown breaks transition chains (entry/exit) — by design
        ++handlingSampleTotal;                           // confidence denominator: every accepted sample
        if (hasSpeed) {                                  // NaN speed never poisons the previous-speed state
            previousPhaseSpeedKmh = speedKmh;
            phaseHasPreviousSpeed = true;
        }
    }

    // Fraction of handling samples that met ALL criteria of a known driving phase.
    double segmentationConfidence() const {
        return handlingSampleTotal == 0 ? 0.0 : static_cast<double>(phaseClassifiedSamples) / static_cast<double>(handlingSampleTotal);
    }

    // ===============================
    // addSteerYaw (blind)
    // ===============================
    void addSteerYaw(double steer, double yaw) {
        if (!std::isfinite(steer) || !std::isfinite(yaw))
            return;

        steerSum   += steer;
        yawSum     += yaw;
        steerSqSum += steer * steer;
        yawSqSum   += yaw * yaw;
        steerYawCov += steer * yaw;
        ++steerYawCount;
    }

    // ===============================
    // steerYawCorrelation (blind)
    // ===============================
    double steerYawCorrelation() const {
        std::size_t n = steerYawCount;
        if (n < 2)
            return 0.0;

        double sSum   = steerSum;
        double ySum   = yawSum;
        double sSqSum = steerSqSum;
        double ySqSum = yawSqSum;
        double cov    = steerYawCov;

        double num = cov * n - sSum * ySum;
        double denLeft  = sSqSum * n - sSum * sSum;
        double denRight = ySqSum * n - ySum * ySum;

        if (denLeft <= 0.0 || denRight <= 0.0)
            return 0.0;

        double den = std::sqrt(denLeft * denRight);
        if (den == 0.0)
            return 0.0;

        return num / den;
    }
};

// ===============================
// L/R asymmetry within comparable driving phases (spec §4).
// Compares per-side lateral load accumulated in the cornering phases only — a global
// lap average mixes straights, entries and exits of both directions and is NOT valid
// evidence. Verdicts: inconclusive / likely layout-phase artifact / repeated difference
// under comparable conditions. This feeds rules as EVIDENCE or CONFOUNDER ONLY; it must
// never stand alone as a mechanical conclusion (spec §4).
// ===============================
inline PhaseAsymmetryResult compareLrInComparablePhases(const LapStats& lap) {
    const int minSamples = g_thresholds.minHandlingSamples > 0 ? g_thresholds.minHandlingSamples : 8;

    // Cornering phases where per-side lateral load is meaningful (no straights, no braking).
    static const DrivingPhase kComparablePhases[] = {
        DrivingPhase::CornerEntryRelease,
        DrivingPhase::MidCorner,
        DrivingPhase::ExitReThrottle
    };

    double leftSum = 0.0, rightSum = 0.0;
    std::size_t leftCount = 0, rightCount = 0;
    for (std::size_t i = 0; i < sizeof(kComparablePhases) / sizeof(kComparablePhases[0]); ++i) {
        const PhaseAccumulator& p = lap.phases[static_cast<int>(kComparablePhases[i])];
        if (p.latGLeftHander.count > 0) { leftSum += p.latGLeftHander.sum;   leftCount  += p.latGLeftHander.count; }
        if (p.latGRighther.count   > 0) { rightSum += p.latGRighther.sum;    rightCount += p.latGRighther.count; }
    }

    PhaseAsymmetryResult result;
    const std::size_t total = leftCount + rightCount;
    if (total < static_cast<std::size_t>(minSamples)) {
        return result;   // InconclusiveInsufficientData — default verdict, no conclusion at all
    }

    // Layout artifact: comparable-phase samples heavily skewed toward one corner direction.
    const std::size_t hi = leftCount > rightCount ? leftCount : rightCount;
    const std::size_t lo = total - hi;
    if (lo == 0 || static_cast<double>(hi) / static_cast<double>(lo) >= g_thresholds.phaseLayoutBiasMinRatio) {
        result.verdict = AsymmetryVerdict::LikelyLayoutPhaseArtifact;   // one-directional track mix — not a mechanical signal
    } else if (leftCount >= static_cast<std::size_t>(minSamples) && rightCount >= static_cast<std::size_t>(minSamples)) {
        result.verdict = AsymmetryVerdict::RepeatedInComparableConditions;  // both sides sufficient, difference persists under comparable conditions
    }

    result.leftSamples  = static_cast<int>(leftCount);
    result.rightSamples = static_cast<int>(rightCount);
    if (leftCount > 0)  result.leftAvg  = leftSum / static_cast<double>(leftCount);
    if (rightCount > 0) result.rightAvg = rightSum / static_cast<double>(rightCount);

    return result;
}

// ===============================
// Analysis pipeline helpers: convert the legacy LapStats data model to the new
// rule-based contract without forcing the UI layer to know anything about the
// simulator-specific channel names.
// ===============================
inline AnalysisSettings buildDefaultAnalysisSettings() {
    AnalysisSettings settings;
    // Per-axle inner/outer spread thresholds (front vs rear)
    settings.tyres.camberMinSpreadFront = g_thresholds.camberMinSpreadFront;
    settings.tyres.camberMaxSpreadFront = g_thresholds.camberMaxSpreadFront;
    settings.tyres.camberMinSpreadRear  = g_thresholds.camberMinSpreadRear;
    settings.tyres.camberMaxSpreadRear  = g_thresholds.camberMaxSpreadRear;
    // Legacy single-value field kept for backward compatibility (uses front max)
    settings.tyres.innerOuterSpreadC = g_thresholds.camberMaxSpreadFront;
    settings.tyres.sideImbalanceMaxC = g_thresholds.tyreCentreTempLrDiffMax;
    settings.tyres.rearOuterVsFrontStiffC     = g_thresholds.rearOuterVsFrontStiffC;
    settings.engine.waterTempWarnC = g_thresholds.waterTempMax;
    settings.engine.oilTempWarnC = g_thresholds.oilTempMax;
    settings.handling.wheelSlipMax = g_thresholds.wheelSlipMax;
    settings.handling.steerMinCornering = g_thresholds.steerMinCornering;
    settings.handling.yawChangeMin = g_thresholds.yawChangeMin;
    settings.handling.yawMinOversteer = g_thresholds.yawMinOversteer;
    settings.handling.steerChangeMin = g_thresholds.steerChangeMin;
    return settings;
}

// ===============================
// Damper histogram metric extraction for diagnostic rules.
// Returns asymmetry % and high-speed share % from a corner's velocity histogram,
// using the same zone split as detail::evaluateDamperCorner().
// Both values are 0.0 when sample count is below g_thresholds.minDamperSamples.
// ===============================
struct DamperMetrics {
    double asymmetryPct = 0.0;      // (bump% - rebound%) in [−100..+100]
    double highSpeedSharePct = 0.0; // % of samples above low-speed threshold
};

inline DamperMetrics computeDamperMetrics(const VelocityHistogram& h) {
    if (h.totalCount < static_cast<std::size_t>(g_thresholds.minDamperSamples))
        return DamperMetrics{};

    const double lo = g_thresholds.damperLowSpeedThreshold; // mm/s (~50)
    const double hi = VelocityHistogram::rangeMax() + 1.0;
    const double negHi = -VelocityHistogram::rangeMax() - 1.0;

    const double lowBump     = h.percentInRange(0.0, lo);
    const double highBump    = h.percentInRange(lo, hi);
    const double lowRebound  = h.percentInRange(-lo, 0.0);
    const double highRebound = h.percentInRange(negHi, -lo);

    DamperMetrics m;
    m.asymmetryPct      = (lowBump + highBump) - (lowRebound + highRebound);
    m.highSpeedSharePct = highBump + highRebound;
    return m;
}

inline void addSummaryValue(DataProvider& provider, AnalysisAlias alias,
                           const StatSummary& summary) {
    if (summary.count > 0)
        provider.setValue(alias, summary.average());
}

// Sets an axle-average value only when BOTH corner summaries have data.
// This ensures a missing channel on one side does not produce a half-axle average.
inline void addAxleAverage(DataProvider& provider, AnalysisAlias alias,
                           const StatSummary& left, const StatSummary& right) {
    if (left.count > 0 && right.count > 0)
        provider.setValue(alias, (left.average() + right.average()) * 0.5);
}

inline DataProvider buildDataProviderFromLap(const LapStats& lap) {
    DataProvider provider;

    addSummaryValue(provider, AnalysisAlias::WaterTemp, lap.waterTemp);
    addSummaryValue(provider, AnalysisAlias::OilTemp, lap.oilTemp);
    addSummaryValue(provider, AnalysisAlias::SteerAngleAvg, lap.steerAngle);
    addSummaryValue(provider, AnalysisAlias::YawRateAvg, lap.yawRate);
    addSummaryValue(provider, AnalysisAlias::WheelSlipFrontAvg, lap.wheelSlipLF);
    addSummaryValue(provider, AnalysisAlias::WheelSlipRearAvg, lap.wheelSlipLR);
    addSummaryValue(provider, AnalysisAlias::TyrePressFL, lap.pressureLF);
    addSummaryValue(provider, AnalysisAlias::TyrePressFR, lap.pressureRF);
    addSummaryValue(provider, AnalysisAlias::TyrePressRL, lap.pressureLR);
    addSummaryValue(provider, AnalysisAlias::TyrePressRR, lap.pressureRR);
    addSummaryValue(provider, AnalysisAlias::TyreTempFLInner, lap.tyreInnerTempLF);
    addSummaryValue(provider, AnalysisAlias::TyreTempFLMiddle, lap.tyreCentreTempLF);
    addSummaryValue(provider, AnalysisAlias::TyreTempFLOuter, lap.tyreOuterTempLF);
    addSummaryValue(provider, AnalysisAlias::TyreTempFRInner, lap.tyreInnerTempRF);
    addSummaryValue(provider, AnalysisAlias::TyreTempFRMiddle, lap.tyreCentreTempRF);
    addSummaryValue(provider, AnalysisAlias::TyreTempFROuter, lap.tyreOuterTempRF);
    addSummaryValue(provider, AnalysisAlias::TyreTempRLInner, lap.tyreInnerTempLR);
    addSummaryValue(provider, AnalysisAlias::TyreTempRLMiddle, lap.tyreCentreTempLR);
    addSummaryValue(provider, AnalysisAlias::TyreTempRLOuter, lap.tyreOuterTempLR);
    addSummaryValue(provider, AnalysisAlias::TyreTempRRInner, lap.tyreInnerTempRR);
    addSummaryValue(provider, AnalysisAlias::TyreTempRRMiddle, lap.tyreCentreTempRR);
    addSummaryValue(provider, AnalysisAlias::TyreTempRROuter, lap.tyreOuterTempRR);

    const double rideHeightToMm = lap.rideHeightUnit == RideHeightUnit::Centimeters ? 10.0 : 1.0;
    const bool hasRearCornerHeights = lap.aeroHeight2.count > 0 && lap.aeroHeight3.count > 0;
    const bool hasAxleHeights = lap.aeroHeight0.count > 0 && lap.aeroHeight1.count > 0;
    if (hasRearCornerHeights) {
        provider.setValue(AnalysisAlias::RideHeightFL, lap.aeroHeight0.average() * rideHeightToMm);
        provider.setValue(AnalysisAlias::RideHeightFR, lap.aeroHeight1.average() * rideHeightToMm);
        provider.setValue(AnalysisAlias::RideHeightRL, lap.aeroHeight2.average() * rideHeightToMm);
        provider.setValue(AnalysisAlias::RideHeightRR, lap.aeroHeight3.average() * rideHeightToMm);
    } else if (hasAxleHeights) {
        provider.setValue(AnalysisAlias::RideHeightFL, lap.aeroHeight0.average() * rideHeightToMm);
        provider.setValue(AnalysisAlias::RideHeightFR, lap.aeroHeight0.average() * rideHeightToMm);
        provider.setValue(AnalysisAlias::RideHeightRL, lap.aeroHeight1.average() * rideHeightToMm);
        provider.setValue(AnalysisAlias::RideHeightRR, lap.aeroHeight1.average() * rideHeightToMm);
    }

    // --- Handling evidence (Phase 0) ---
    if (lap.steerYawCount >= static_cast<std::size_t>(g_thresholds.minSteerYawSamples)) {
        provider.setValue(AnalysisAlias::SteerYawCorrelation, lap.steerYawCorrelation());
    }

    const auto setHandlingPct = [](DataProvider& p, AnalysisAlias alias,
                                   std::size_t indicators, std::size_t total) {
        if (total > 0)
            p.setValue(alias, 100.0 * static_cast<double>(indicators) / static_cast<double>(total));
    };

    setHandlingPct(provider, AnalysisAlias::UndersteerIndicatorsEntryPct,
                   lap.cornerEntry.understeerIndicators, lap.cornerEntry.sampleCount);
    setHandlingPct(provider, AnalysisAlias::OversteerIndicatorsExitPct,
                   lap.cornerExit.oversteerIndicators, lap.cornerExit.sampleCount);

    if (lap.cornerEntry.sampleCount > 0)
        provider.setValue(AnalysisAlias::HandlingSampleCountEntry,
                          static_cast<double>(lap.cornerEntry.sampleCount));
    if (lap.cornerExit.sampleCount > 0)
        provider.setValue(AnalysisAlias::HandlingSampleCountExit,
                          static_cast<double>(lap.cornerExit.sampleCount));

    // --- Per-corner wheel slip (Phase 0) ---
    addSummaryValue(provider, AnalysisAlias::WheelSlipLF, lap.wheelSlipLF);
    addSummaryValue(provider, AnalysisAlias::WheelSlipRF, lap.wheelSlipRF);
    addSummaryValue(provider, AnalysisAlias::WheelSlipLR, lap.wheelSlipLR);
    addSummaryValue(provider, AnalysisAlias::WheelSlipRR, lap.wheelSlipRR);

    // --- Brakes (Phase 0) ---
    addAxleAverage(provider, AnalysisAlias::BrakeTorqueFrontAvg, lap.brakeTorqueLF, lap.brakeTorqueRF);
    addAxleAverage(provider, AnalysisAlias::BrakeTorqueRearAvg, lap.brakeTorqueLR, lap.brakeTorqueRR);
    // Per-wheel disc temperature (deg C) — consumed by BrakeTemperatureRule.
    addSummaryValue(provider, AnalysisAlias::BrakeTempLF, lap.brakeTempLF);
    addSummaryValue(provider, AnalysisAlias::BrakeTempRF, lap.brakeTempRF);
    addSummaryValue(provider, AnalysisAlias::BrakeTempLR, lap.brakeTempLR);
    addSummaryValue(provider, AnalysisAlias::BrakeTempRR, lap.brakeTempRR);
    addSummaryValue(provider, AnalysisAlias::BrakeBiasSettingAvg, lap.brakeBiasSetting);
    addSummaryValue(provider, AnalysisAlias::TcsActivePct, lap.tcsActive);
    addSummaryValue(provider, AnalysisAlias::AbsActivePct, lap.absActive);

    // --- Wheel speeds (Phase 0) ---
    addAxleAverage(provider, AnalysisAlias::WheelSpeedFrontAvg, lap.wheelSpeedLF, lap.wheelSpeedRF);
    addAxleAverage(provider, AnalysisAlias::WheelSpeedRearAvg, lap.wheelSpeedLR, lap.wheelSpeedRR);

    // --- Suspension travel & bumpstop (Phase 0) ---
    addSummaryValue(provider, AnalysisAlias::SusTravelMeanLF, lap.susTravelLF);
    addSummaryValue(provider, AnalysisAlias::SusTravelMeanRF, lap.susTravelRF);
    addSummaryValue(provider, AnalysisAlias::SusTravelMeanLR, lap.susTravelLR);
    addSummaryValue(provider, AnalysisAlias::SusTravelMeanRR, lap.susTravelRR);

    // Bumpstop engagement: StatSummary stores 0/1 per sample → average = fraction active.
    const auto setBumpStopPct = [](DataProvider& p, AnalysisAlias alias,
                                   const StatSummary& s) {
        if (s.count > 0)
            p.setValue(alias, s.average() * 100.0);
    };

    setBumpStopPct(provider, AnalysisAlias::BumpStopEngagementPctLF, lap.bumpStopLF);
    setBumpStopPct(provider, AnalysisAlias::BumpStopEngagementPctRF, lap.bumpStopRF);
    setBumpStopPct(provider, AnalysisAlias::BumpStopEngagementPctLR, lap.bumpStopLR);
    setBumpStopPct(provider, AnalysisAlias::BumpStopEngagementPctRR, lap.bumpStopRR);

    // --- Damper histogram summaries (Phase 0) ---
    const auto addDamperAxle = [](DataProvider& p,
                                  AnalysisAlias asymmetryAlias, AnalysisAlias highSpeedAlias,
                                  const VelocityHistogram& leftH, const VelocityHistogram& rightH) {
        const bool hasLeft  = leftH.totalCount  >= static_cast<std::size_t>(g_thresholds.minDamperSamples);
        const bool hasRight = rightH.totalCount >= static_cast<std::size_t>(g_thresholds.minDamperSamples);

        if (hasLeft && hasRight) {
            // Average both corners for a representative axle value.
            const DamperMetrics lm = computeDamperMetrics(leftH);
            const DamperMetrics rm = computeDamperMetrics(rightH);
            p.setValue(asymmetryAlias,  (lm.asymmetryPct + rm.asymmetryPct) * 0.5);
            p.setValue(highSpeedAlias,  (lm.highSpeedSharePct + rm.highSpeedSharePct) * 0.5);
        } else if (hasLeft || hasRight) {
            // Only one corner available — use it as-is rather than half-axle average.
            const DamperMetrics m = computeDamperMetrics(hasLeft ? leftH : rightH);
            p.setValue(asymmetryAlias,  m.asymmetryPct);
            p.setValue(highSpeedAlias,  m.highSpeedSharePct);
        }
    };

    addDamperAxle(provider,
                  AnalysisAlias::DamperAsymmetryFront, AnalysisAlias::DamperHighSpeedShareFront,
                  lap.damperLF.histogram, lap.damperRF.histogram);
    addDamperAxle(provider,
                  AnalysisAlias::DamperAsymmetryRear, AnalysisAlias::DamperHighSpeedShareRear,
                  lap.damperLR.histogram, lap.damperRR.histogram);

    // --- Direct suspension velocity from sim feed (Phase 0) ---
    // suspVel* are VelocityHistograms — use mean directly:
    if (lap.suspVelLF.totalCount  > 0) provider.setValue(AnalysisAlias::SuspVelMeanLF, lap.suspVelLF.mean());
    if (lap.suspVelRF.totalCount  > 0) provider.setValue(AnalysisAlias::SuspVelMeanRF, lap.suspVelRF.mean());
    if (lap.suspVelLR.totalCount  > 0) provider.setValue(AnalysisAlias::SuspVelMeanLR, lap.suspVelLR.mean());
    if (lap.suspVelRR.totalCount  > 0) provider.setValue(AnalysisAlias::SuspVelMeanRR, lap.suspVelRR.mean());

    // --- Vehicle / aero / environment (Phase 0) ---
    addSummaryValue(provider, AnalysisAlias::AeroBalanceAvg, lap.aeroBalance);
    addSummaryValue(provider, AnalysisAlias::LsdTorqueAvg,   lap.lsdTorque);
    addSummaryValue(provider, AnalysisAlias::LatGAvg,        lap.latG);
    addSummaryValue(provider, AnalysisAlias::ThrottleAvgPct, lap.throttle);
    addSummaryValue(provider, AnalysisAlias::LongGAvg,       lap.longG);
    addSummaryValue(provider, AnalysisAlias::BrakePedalAvgPct, lap.brakePedal);
    addSummaryValue(provider, AnalysisAlias::FuelLevelAvg,   lap.fuelLevel);
    addSummaryValue(provider, AnalysisAlias::TrackTempAvg,   lap.trackTemp);
    addSummaryValue(provider, AnalysisAlias::AmbientTempAvg, lap.ambientTemp);

    // --- Tyre air temps per corner (Phase 0) ---
    addSummaryValue(provider, AnalysisAlias::TyreAirTempLF, lap.tyreAirTempLF);
    addSummaryValue(provider, AnalysisAlias::TyreAirTempRF, lap.tyreAirTempRF);
    addSummaryValue(provider, AnalysisAlias::TyreAirTempLR, lap.tyreAirTempLR);
    addSummaryValue(provider, AnalysisAlias::TyreAirTempRR, lap.tyreAirTempRR);

    return provider;
}

inline AnalysisRegistry buildDefaultAnalysisRegistry() {
    AnalysisRegistry registry;
    registry.addRule(std::unique_ptr<AnalysisRule>(new EngineTemperatureRule()));
    registry.addRule(std::unique_ptr<AnalysisRule>(new BrakeTemperatureRule()));   // per-wheel disc temp vs VehicleDb fade window (Brake tab)
    registry.addRule(std::unique_ptr<AnalysisRule>(new TyreTemperatureRule()));
    registry.addRule(std::unique_ptr<AnalysisRule>(new TyreSuspensionBalanceRule()));   // rear vs front outer temp -> suspension balance (Tyre tab)
    registry.addRule(std::unique_ptr<AnalysisRule>(new HandlingBalanceRule()));
    return registry;
}

// ===============================
// Damper-velocity issue detection
// (moved from RecommendationEngine.h/.cpp)
// ===============================

namespace detail {

struct DamperFinding {
    bool issue = false;
    std::string text;
};

// Evaluates one corner's damper-velocity histogram against an ideal symmetric bell shape
// (low/high-speed bump vs rebound split at ~50 mm/s, the common sim-racing damper cutoff).
inline DamperFinding evaluateDamperCorner(const DamperCornerStats& stats,
                                          const std::string& cornerLabelEn,
                                          const std::string& cornerLabelFr,
                                          ReportLanguage language) {
    auto tr = [&](const std::string& en, const std::string& fr) -> std::string {
        return language == ReportLanguage::French ? fr : en;
    };

    DamperFinding finding;
    const VelocityHistogram& h = stats.histogram;
    if (h.totalCount < g_thresholds.minDamperSamples)
        return finding;

    const double lowSpeedThreshold = g_thresholds.damperLowSpeedThreshold; // mm/s (~2 in/s)
    double lowBump    = h.percentInRange(0.0, lowSpeedThreshold);
    double highBump   = h.percentInRange(lowSpeedThreshold, VelocityHistogram::rangeMax() + 1.0);
    double lowRebound  = h.percentInRange(-lowSpeedThreshold, 0.0);
    double highRebound = h.percentInRange(-VelocityHistogram::rangeMax() - 1.0, -lowSpeedThreshold);

    double asymmetry      = (lowBump + highBump) - (lowRebound + highRebound);
    double highSpeedShare = highBump + highRebound;
    double lowSpeedShare  = lowBump + lowRebound;

    std::string label = tr(cornerLabelEn, cornerLabelFr);

    if (std::abs(asymmetry) > g_thresholds.damperAsymmetryThreshold) {
        finding.issue = true;
        if (asymmetry > 0.0) {
            finding.text = label + tr(
                ": spends more time compressing than rebounding; increase rebound damping (slow and fast) slightly, or soften bump damping, so the shock returns faster.",
                " : passe plus de temps en compression qu'en detente ; augmentez legerement l'amortissement en detente (lent et rapide), ou assouplissez la compression, pour que l'amortisseur revienne plus vite.");
        } else {
            finding.text = label + tr(
                ": spends more time extending than compressing; soften rebound damping slightly, or increase bump damping, to rebalance the curve.",
                " : passe plus de temps en detente qu'en compression ; assouplissez legerement l'amortissement en detente, ou augmentez la compression, pour reequilibrer la courbe.");
        }
        return finding;
    }

    if (highSpeedShare > g_thresholds.damperHighSpeedShareMax) {
        finding.issue = true;
        finding.text = label + tr(
            ": a large share of movement happens at high shaft speed; increase high-speed bump/rebound damping to round off the velocity curve toward a bell shape.",
            " : une grande part du mouvement se produit a haute vitesse de tige ; augmentez l'amortissement haute vitesse (compression/detente) pour arrondir la courbe vers une forme de cloche.");
        return finding;
    }

    if (lowSpeedShare > g_thresholds.damperLowSpeedShareMin && h.stdDev() < g_thresholds.damperLowSpeedStdDevMax) {
        finding.issue = true;
        char pctBuf[16];
        snprintf(pctBuf, sizeof(pctBuf), "%d", (int)(lowSpeedShare + 0.5));
        std::string pct = pctBuf;
        finding.text = label + tr(
            (": the suspension barely moves (" + pct + "% of the time below 50 mm/s); low-speed damping is likely too stiff, holding the wheel down and preventing it from absorbing bumps. Soften low-speed bump/rebound slightly so the suspension can work.")
            , " : la suspension bouge tres peu (" + pct + "% du temps a moins de 50 mm/s) ; l'amortissement basse vitesse est probablement trop raide et empeche la roue d'absorber les irregularites. Assouplissez legerement la compression/detente basse vitesse pour laisser la suspension travailler.");
        return finding;
    }

    finding.issue = false;
    finding.text = label + tr(
        ": velocity distribution looks close to a balanced bell shape; no damper change indicated.",
        " : la distribution de vitesse est proche d'une forme de cloche equilibree ; aucun changement d'amortisseur necessaire.");
    return finding;
}

// Classify a single zone percentage against its target value.
// The per-zone tolerance defines the acceptable band [target - tol, target + tol];
// values inside are Normal. Beyond that: deviation <= 2*tol -> Moderate,
// otherwise Important (deviation measured from the nearest band edge).
inline DamperZoneResult classifyDamperZone(double pct, double target, double tolerance) {
    // Values inside [target - tol, target + tol] are Normal (no recommendation).
    // Only a deviation beyond that widened band is flagged.
    DamperZoneResult r;
    r.pct = pct;
    if (pct >= target - tolerance && pct <= target + tolerance) {
        r.severity = DamperZoneSeverity::Normal;
    } else {
        double deviation = (pct > target + tolerance)
            ? (pct - (target + tolerance)) : ((target - tolerance) - pct);
        r.aboveTarget = (pct > target);
        r.severity = (deviation <= tolerance * 2.0)
            ? DamperZoneSeverity::Moderate
            : DamperZoneSeverity::Important;
    }
    return r;
}

// Evaluates a damper-velocity histogram against 4-zone targets.
inline DamperZoneReport evaluateDamperZones(const VelocityHistogram& h, ReportLanguage language) {
    auto tr = [&](const std::string& en, const std::string& fr) -> std::string {
        return language == ReportLanguage::French ? fr : en;
    };

    DamperZoneReport report;
    if (h.totalCount < g_thresholds.minDamperSamples)
        return report;

    report.hasData = true;

    // Zone boundaries: ±damperSlowSpeedRange mm/s on bin centers — same rule as the
    // combined chart legend, so report values match what is drawn. Sign convention:
    // positive velocity = compression/bump, negative = rebound/détente.
    const double B = g_thresholds.damperSlowSpeedRange;

    // Compute 4 zone percentages (bin-center rule: slow iff |center| <= B)
    double lscPct = h.percentLsc(B);   // 0 < v <= +B : low-speed compression
    double hscPct = h.percentHsc(B);   // v > +B      : high-speed compression
    double lsrPct = h.percentLsr(B);   // -B <= v < 0 : low-speed rebound
    double hsrPct = h.percentHsr(B);   // v < -B      : high-speed rebound

    // Classify each zone (single target + per-zone tolerance band)
    report.hsc = classifyDamperZone(hscPct, g_thresholds.damperHscTarget, g_thresholds.damperZoneToleranceHsc);
    report.lsc = classifyDamperZone(lscPct, g_thresholds.damperLscTarget, g_thresholds.damperZoneToleranceLsc);
    report.lsr = classifyDamperZone(lsrPct, g_thresholds.damperLsrTarget, g_thresholds.damperZoneToleranceLsr);
    report.hsr = classifyDamperZone(hsrPct, g_thresholds.damperHsrTarget, g_thresholds.damperZoneToleranceHsr);

    // Global balances
    report.compressionPct = hscPct + lscPct;
    report.reboundPct     = lsrPct + hsrPct;
    report.lowSpeedPct    = lscPct + lsrPct;
    report.highSpeedPct   = hscPct + hsrPct;

    // Generate localized text
    if (!report.anyIssue()) {
        report.text = tr(
            "velocity distribution is within target ranges for all 4 zones; no damper change indicated.",
            "la distribution de vitesse est dans les plages cibles pour les 4 zones ; aucun changement d'amortisseur necessaire.");
        return report;
    }

    // Human-readable zone labels (localized)
    struct ZoneInfo { const char* en; const char* fr; DamperZoneResult& r; };
    ZoneInfo zones[4] = {
        { "High-speed Comp.",  "Compression haute vitesse",  report.hsc },
        { "Low-speed Comp.",   "Compression basse vitesse",  report.lsc },
        { "Low-speed Rebound", "Détente basse vitesse",      report.lsr },
        { "High-speed Rebound","Détente haute vitesse",      report.hsr }
    };

    // Build description of out-of-target zones + suggested action
    const char* descEn[4] = { "high-speed compression", "low-speed compression",
                              "low-speed rebound", "high-speed rebound" };
    const char* descFr[4] = { "la compression haute vitesse", "la compression basse vitesse",
                              "la détente basse vitesse", "la détente haute vitesse" };
    double tTgt[4] = { g_thresholds.damperHscTarget, g_thresholds.damperLscTarget,
                      g_thresholds.damperLsrTarget, g_thresholds.damperHsrTarget };
    double tTol[4] = { g_thresholds.damperZoneToleranceHsc, g_thresholds.damperZoneToleranceLsc,
                       g_thresholds.damperZoneToleranceLsr, g_thresholds.damperZoneToleranceHsr };
    auto fmtPct1 = [](double v) -> std::string {
        char buf[32]; snprintf(buf, sizeof(buf), "%.1f", v); return std::string(buf);
    };

    std::string parts;
    for (int i = 0; i < 4; ++i) {
        if (zones[i].r.severity == DamperZoneSeverity::Normal) continue;
        const char* name = (language == ReportLanguage::French) ? zones[i].fr : zones[i].en;
        const char* desc = (language == ReportLanguage::French) ? descFr[i] : descEn[i];

        std::string pctStr   = fmtPct1(zones[i].r.pct);
        // Show the tolerance-widened band (target +/- tol) so it is clear which range is accepted.
        std::string targetStr = fmtPct1(tTgt[i] - tTol[i]) + "-" + fmtPct1(tTgt[i] + tTol[i]);

        const char* action = zones[i].r.aboveTarget
            ? (language == ReportLanguage::French ? "raidir " : "stiffen ")
            : (language == ReportLanguage::French ? "adoucir " : "soften ");

        // One line per out-of-target zone so each recommendation reads clearly.
        const char* bandLabel = (language == ReportLanguage::French) ? "(bande toleree " : "(allowed band ";
        std::string part = std::string("  - ") + name + ": " + pctStr + "% " + bandLabel + targetStr + "%) -> " + action + desc;
        if (!parts.empty()) parts += "\n";
        parts += part;
    }

    report.text = tr(
        ("outside target: " + parts + "."),
        ("hors cible : " + parts + "."));

    return report;
}

} // namespace detail

// True when a corner's damper-velocity distribution indicates an issue
// (any of the 4 zones HSC/LSC/LSR/HSR is Moderate or Important).
inline bool hasDamperIssue(const VelocityHistogram& h) {
    DamperZoneReport report = detail::evaluateDamperZones(h, ReportLanguage::English);
    return report.hasData && report.anyIssue();
}

// Human-readable damper recommendation for one corner (localized).
// cornerIndex: 0=LF, 1=RF, 2=LR, 3=RR. Empty string if not enough samples.
inline std::string getDamperRecommendation(const VelocityHistogram& h, int cornerIndex, ReportLanguage language) {
    static const char* enLabels[4] = { "FL", "FR", "RL", "RR" };
    static const char* frLabels[4] = { "FL", "FR", "RL", "RR" }; // same codes as the graph, both languages
    if (cornerIndex < 0 || cornerIndex > 3) return std::string();
    DamperZoneReport report = detail::evaluateDamperZones(h, language);
    if (!report.hasData) return std::string();
    const char* label = (language == ReportLanguage::French) ? frLabels[cornerIndex] : enLabels[cornerIndex];
    return std::string(label) + " — " + report.text;
}

// Pick the best velocity histogram for a corner: prefer the direct
// suspension-velocity channel when it has data, otherwise fall back to
// the travel-derivative histogram. Mirrors the display logic in MotecSetupForm.
inline const VelocityHistogram& pickDamperHistogram(const LapStats& lap, int cornerIndex) {
    switch (cornerIndex) {
        case 0: return lap.suspVelLF.totalCount > 0 ? lap.suspVelLF : lap.damperLF.histogram;
        case 1: return lap.suspVelRF.totalCount > 0 ? lap.suspVelRF : lap.damperRF.histogram;
        case 2: return lap.suspVelLR.totalCount > 0 ? lap.suspVelLR : lap.damperLR.histogram;
        case 3: return lap.suspVelRR.totalCount > 0 ? lap.suspVelRR : lap.damperRR.histogram;
        default: return lap.damperLF.histogram;
    }
}

// ===============================
// External functions
// ===============================
LapStats buildAverageLapData(const std::vector<LapStats>& laps);

ReportResult buildAverageLapReport(const std::vector<LapStats>& laps,
                                  ReportLanguage language = ReportLanguage::English,
                                  ChannelProfile profile = ChannelProfile::EvoM1);

std::vector<AnalysisResult> evaluateLapWithRules(const LapStats& lap,
                                                const AnalysisSettings& settings = AnalysisSettings());

typedef void (*AnalysisProgressCallback)(int percent);
std::string resolveChannelKey(const std::string& rawName,
                             ChannelProfile profile = ChannelProfile::EvoM1);

bool parseMotecLd(const std::string& path,
                  std::vector<LapStats>& laps,
                  std::string& errorMessage,
                  AnalysisProgressCallback progressCallback = NULL,
                  ChannelProfile profile = ChannelProfile::EvoM1);

// Returns the averaged LapStats object (all but last lap).
// Requires at least 2 laps to be valid (returns empty if < 2).
LapStats buildAverageLapData(const std::vector<LapStats>& laps);
