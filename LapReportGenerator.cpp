//---------------------------------------------------------------------------
// LapReportGenerator.cpp - Rule-only report assembly.
//---------------------------------------------------------------------------

#include <algorithm>
#include <cmath>
#include <set>
#include <sstream>
#include <utility>
#include <vector>

#include "MotecSetupAnalyzer.h"
#include "LapReportGenerator.h"
#include "VehicleDb.h"          // Layer 1: per-vehicle reference data (AMS2)
#include "DiagnosticEngine.h"   // Phase 3: diagnostic findings + recommendation gate

namespace {

double lapDuration(const LapStats& lap) {
    return lap.endTime - lap.startTime;
}

// Result of lap selection: the laps that pass the validity window plus a full
// breakdown (included / excluded with reasons) for the Summary tab.
struct LapSelectionResult {
    std::vector<const LapStats*> valid;
    LapSelectionInfo info;
};

LapSelectionResult selectValidLaps(const std::vector<LapStats>& laps) {
    LapSelectionResult out;
    out.info.totalLaps = static_cast<int>(laps.size());

    // Collect positive finite lap durations (per index).
    std::vector<double> durations(laps.size(), 0.0);
    for (std::size_t i = 0; i < laps.size(); ++i) {
        const double d = lapDuration(laps[i]);
        if (!std::isfinite(d) || d <= 0.0)
            continue;
        durations[i] = d;
    }

    // Anchor 'best' to the median duration. The median is robust to a single
    // outlier on either side (short partial-lap stub OR long outlier lap),
    // as long as outliers are under 50% of laps — which they always are in
    // practice. This replaces the earlier "min-above-stub-floor" heuristic,
    // which broke when a single long lap pushed the median up and caused the
    // floor to discard every real (short) lap.
    std::vector<double> sorted;
    for (std::size_t i = 0; i < durations.size(); ++i)
        if (durations[i] > 0.0)
            sorted.push_back(durations[i]);

    double best = 0.0;
    if (!sorted.empty()) {
        std::sort(sorted.begin(), sorted.end());
        best = sorted[sorted.size() / 2]; // median
    }

    const double lo = best > 0.0 ? best * g_thresholds.minValidLapTimeMultiplier : 0.0;
    const double hi = best > 0.0 ? best * g_thresholds.maxValidLapTimeMultiplier : 0.0;

    for (std::size_t i = 0; i < laps.size(); ++i) {
        const double d = lapDuration(laps[i]);
        if (!std::isfinite(d) || d <= 0.0) {
            out.info.excludedLaps.push_back(
                LapSelectionInfo::ExcludedLap{laps[i].lapNumber, std::isfinite(d) ? d : 0.0,
                                              LapSelectionInfo::ExcludedLap::Reason::NonFinite});
            continue;
        }
        if (best > 0.0 && d >= lo && d <= hi) {
            out.valid.push_back(&laps[i]);
            out.info.includedLaps.push_back(laps[i].lapNumber);
        } else if (d < lo) {
            out.info.excludedLaps.push_back(
                LapSelectionInfo::ExcludedLap{laps[i].lapNumber, d,
                                              LapSelectionInfo::ExcludedLap::Reason::TooShort});
        } else {
            out.info.excludedLaps.push_back(
                LapSelectionInfo::ExcludedLap{laps[i].lapNumber, d,
                                              LapSelectionInfo::ExcludedLap::Reason::TooLong});
        }
    }
    return out;
}

// Re-classify a window-based selection so that "included" reflects the exact set
// of laps actually averaged (e.g. all-but-last for average mode, top-3 for best
// three). Laps that passed the window but were not used are marked ValidButUnused.
LapSelectionInfo refineSelectionInfo(const LapSelectionResult& base,
                                     const std::vector<const LapStats*>& selected) {
    LapSelectionInfo info;
    info.totalLaps = base.info.totalLaps;

    std::set<int> includedSet;
    for (std::size_t i = 0; i < selected.size(); ++i) {
        info.includedLaps.push_back(selected[i]->lapNumber);
        includedSet.insert(selected[i]->lapNumber);
    }

    // Index the window-based exclusions by lap number for reason lookup.
    for (std::size_t i = 0; i < base.info.excludedLaps.size(); ++i) {
        const LapSelectionInfo::ExcludedLap& e = base.info.excludedLaps[i];
        if (!includedSet.count(e.lapNumber))
            info.excludedLaps.push_back(e);
    }

    // Any valid lap not in the selected set was dropped by the averaging rule.
    for (std::size_t i = 0; i < base.valid.size(); ++i) {
        const int n = base.valid[i]->lapNumber;
        if (!includedSet.count(n))
            info.excludedLaps.push_back(
                LapSelectionInfo::ExcludedLap{n, lapDuration(*base.valid[i]),
                                              LapSelectionInfo::ExcludedLap::Reason::ValidButUnused});
    }

    std::sort(info.excludedLaps.begin(), info.excludedLaps.end(),
              [](const LapSelectionInfo::ExcludedLap& a, const LapSelectionInfo::ExcludedLap& b) {
                  return a.lapNumber < b.lapNumber;
              });
    return info;
}

void averageSummary(StatSummary& target, const StatSummary& source) {
    if (source.count == 0)
        return;
    target.add(source.average());
    target.min = std::min(target.min, source.min);
    target.max = std::max(target.max, source.max);
}

// Accumulates a per-lap LapDelta rate into running sum/count.
struct DeltaAccum { double sum = 0; std::size_t n = 0; };
static void addLapDelta(DeltaAccum& acc, const LapDelta& d) {
    if (d.valid()) { acc.sum += d.delta(); ++acc.n; }
}

// Writes the mean rate back into a LapStats field: first=0 sentinel, last=<meanRate>.
static void writeMeanRate(LapDelta& target, const DeltaAccum& acc) {
    if (acc.n == 0) return;
    target.first = 0.0;
    target.last  = acc.sum / static_cast<double>(acc.n);
    target.count = acc.n;   // reuse count as "number of laps contributing"
}


LapStats averageSelectedLaps(const std::vector<const LapStats*>& selected) {
    LapStats average;
    if (selected.empty())
        return average;

    average.rideHeightUnit = selected[0]->rideHeightUnit;
    average.lapNumber = selected[0]->lapNumber;
    average.driverName  = selected[0]->driverName;
    average.vehicleName = selected[0]->vehicleName;
    average.trackName   = selected[0]->trackName;

    // Prediction: per-lap rate accumulators (mean of first→last deltas across laps).
    DeltaAccum accFuel, accTyreLF, accTyreRF, accTyreLR, accTyreRR;
    DeltaAccum accPadFL, accPadFR, accPadRL, accPadRR;
    DeltaAccum accDiscFL, accDiscFR, accDiscRL, accDiscRR;
    DeltaAccum accBrakeDmgLF, accBrakeDmgRF, accBrakeDmgLR, accBrakeDmgRR;
    DeltaAccum accClutch;

    for (std::size_t i = 0; i < selected.size(); ++i) {
        const LapStats& lap = *selected[i];
        average.startTime += lap.startTime;
        average.endTime += lap.endTime;
        average.rowCount += lap.rowCount;

        averageSummary(average.waterTemp, lap.waterTemp);
        averageSummary(average.oilTemp, lap.oilTemp);
        averageSummary(average.steerAngle, lap.steerAngle);
        averageSummary(average.yawRate, lap.yawRate);
        averageSummary(average.wheelSlipLF, lap.wheelSlipLF);
        averageSummary(average.wheelSlipRF, lap.wheelSlipRF);
        averageSummary(average.wheelSlipLR, lap.wheelSlipLR);
        averageSummary(average.wheelSlipRR, lap.wheelSlipRR);
        averageSummary(average.pressureLF, lap.pressureLF);
        averageSummary(average.pressureRF, lap.pressureRF);
        averageSummary(average.pressureLR, lap.pressureLR);
        averageSummary(average.pressureRR, lap.pressureRR);
        averageSummary(average.tyreInnerTempLF, lap.tyreInnerTempLF);
        averageSummary(average.tyreCentreTempLF, lap.tyreCentreTempLF);
        averageSummary(average.tyreOuterTempLF, lap.tyreOuterTempLF);
        averageSummary(average.tyreInnerTempRF, lap.tyreInnerTempRF);
        averageSummary(average.tyreCentreTempRF, lap.tyreCentreTempRF);
        averageSummary(average.tyreOuterTempRF, lap.tyreOuterTempRF);
        averageSummary(average.tyreInnerTempLR, lap.tyreInnerTempLR);
        averageSummary(average.tyreCentreTempLR, lap.tyreCentreTempLR);
        averageSummary(average.tyreOuterTempLR, lap.tyreOuterTempLR);
        averageSummary(average.tyreInnerTempRR, lap.tyreInnerTempRR);
        averageSummary(average.tyreCentreTempRR, lap.tyreCentreTempRR);
        averageSummary(average.tyreOuterTempRR, lap.tyreOuterTempRR);
        averageSummary(average.aeroHeight0, lap.aeroHeight0);
        averageSummary(average.aeroHeight1, lap.aeroHeight1);
        averageSummary(average.aeroHeight2, lap.aeroHeight2);
        averageSummary(average.aeroHeight3, lap.aeroHeight3);

        // Suspension velocity histograms (direct channel + derived d(travel)/dt)
        average.damperLF.merge(lap.damperLF);
        average.damperRF.merge(lap.damperRF);
        average.damperLR.merge(lap.damperLR);
        average.damperRR.merge(lap.damperRR);
        average.suspVelLF.merge(lap.suspVelLF);
        average.suspVelRF.merge(lap.suspVelRF);
        average.suspVelLR.merge(lap.suspVelLR);
        average.suspVelRR.merge(lap.suspVelRR);

        // Suspension travel & bumpstop summaries
        averageSummary(average.susTravelLF, lap.susTravelLF);
        averageSummary(average.susTravelRF, lap.susTravelRF);
        averageSummary(average.susTravelLR, lap.susTravelLR);
        averageSummary(average.susTravelRR, lap.susTravelRR);
        averageSummary(average.bumpStopLF, lap.bumpStopLF);
        averageSummary(average.bumpStopRF, lap.bumpStopRF);
        averageSummary(average.bumpStopLR, lap.bumpStopLR);
        averageSummary(average.bumpStopRR, lap.bumpStopRR);

        // Core driving channels
        averageSummary(average.speed, lap.speed);
        averageSummary(average.throttle, lap.throttle);
        averageSummary(average.brakePedal, lap.brakePedal);
        averageSummary(average.rpm, lap.rpm);
        averageSummary(average.latG, lap.latG);
        averageSummary(average.longG, lap.longG);

        // Wheel speeds
        averageSummary(average.wheelSpeedLF, lap.wheelSpeedLF);
        averageSummary(average.wheelSpeedRF, lap.wheelSpeedRF);
        averageSummary(average.wheelSpeedLR, lap.wheelSpeedLR);
        averageSummary(average.wheelSpeedRR, lap.wheelSpeedRR);

        // Brake temps
        averageSummary(average.brakeTempLF, lap.brakeTempLF);
        averageSummary(average.brakeTempRF, lap.brakeTempRF);
        averageSummary(average.brakeTempLR, lap.brakeTempLR);
        averageSummary(average.brakeTempRR, lap.brakeTempRR);

        // Surface temps
        averageSummary(average.surfTempCFL, lap.surfTempCFL);
        averageSummary(average.surfTempCRF, lap.surfTempCRF);
        averageSummary(average.surfTempCLR, lap.surfTempCLR);
        averageSummary(average.surfTempCRR, lap.surfTempCRR);

        // Tyre air temps
        averageSummary(average.tyreAirTempLF, lap.tyreAirTempLF);
        averageSummary(average.tyreAirTempRF, lap.tyreAirTempRF);
        averageSummary(average.tyreAirTempLR, lap.tyreAirTempLR);
        averageSummary(average.tyreAirTempRR, lap.tyreAirTempRR);

        // Brake torque
        averageSummary(average.brakeTorqueLF, lap.brakeTorqueLF);
        averageSummary(average.brakeTorqueRF, lap.brakeTorqueRF);
        averageSummary(average.brakeTorqueLR, lap.brakeTorqueLR);
        averageSummary(average.brakeTorqueRR, lap.brakeTorqueRR);

        // Misc / systems
        averageSummary(average.brakeBiasSetting, lap.brakeBiasSetting);
        averageSummary(average.tcsActive, lap.tcsActive);
        averageSummary(average.absActive, lap.absActive);
        averageSummary(average.aeroBalance, lap.aeroBalance);
        averageSummary(average.lsdTorque, lap.lsdTorque);

        // Environmental / fuel
        averageSummary(average.fuelLevel, lap.fuelLevel);
        averageSummary(average.fuelPressure, lap.fuelPressure);
        averageSummary(average.trackTemp, lap.trackTemp);
        averageSummary(average.ambientTemp, lap.ambientTemp);

        // Prediction: accumulate per-lap first→last deltas for rate averaging.
        addLapDelta(accFuel,       lap.fuelLevelEnds);
        averageSummary(average.fuelCapacityL, lap.fuelCapacityL);   // constant tank size → simple mean
        addLapDelta(accTyreLF,     lap.tyreWearLF);
        addLapDelta(accTyreRF,     lap.tyreWearRF);
        addLapDelta(accTyreLR,     lap.tyreWearLR);
        addLapDelta(accTyreRR,     lap.tyreWearRR);
        addLapDelta(accPadFL,      lap.padLifeFL);
        addLapDelta(accPadFR,      lap.padLifeFR);
        addLapDelta(accPadRL,      lap.padLifeRL);
        addLapDelta(accPadRR,      lap.padLifeRR);
        addLapDelta(accDiscFL,     lap.discLifeFL);
        addLapDelta(accDiscFR,     lap.discLifeFR);
        addLapDelta(accDiscRL,     lap.discLifeRL);
        addLapDelta(accDiscRR,     lap.discLifeRR);
        addLapDelta(accBrakeDmgLF, lap.brakeDamageLF);
        addLapDelta(accBrakeDmgRF, lap.brakeDamageRF);
        addLapDelta(accBrakeDmgLR, lap.brakeDamageLR);
        addLapDelta(accBrakeDmgRR, lap.brakeDamageRR);
        addLapDelta(accClutch,     lap.clutchWear);

        // Handling phase stats
        average.cornerEntry.merge(lap.cornerEntry);
        average.cornerExit.merge(lap.cornerExit);

        // Driving-phase segmentation (pool per-phase accumulators + confidence counters)
        for (int i = 0; i < kDrivingPhaseCount; ++i)
            average.phases[i].merge(lap.phases[i]);
        average.phaseClassifiedSamples += lap.phaseClassifiedSamples;
        average.handlingSampleTotal    += lap.handlingSampleTotal;

        // Steer/yaw correlation (pool across laps)
        average.steerYawCov += lap.steerYawCov;
        average.steerSum    += lap.steerSum;
        average.yawSum      += lap.yawSum;
        average.steerSqSum  += lap.steerSqSum;
        average.yawSqSum    += lap.yawSqSum;
        average.steerYawCount += lap.steerYawCount;
    }

    average.startTime /= static_cast<double>(selected.size());
    average.endTime /= static_cast<double>(selected.size());

    // Prediction: write mean per-lap rates back into LapDelta fields.
    writeMeanRate(average.fuelLevelEnds,  accFuel);
    writeMeanRate(average.tyreWearLF,     accTyreLF);
    writeMeanRate(average.tyreWearRF,     accTyreRF);
    writeMeanRate(average.tyreWearLR,     accTyreLR);
    writeMeanRate(average.tyreWearRR,     accTyreRR);
    writeMeanRate(average.padLifeFL,      accPadFL);
    writeMeanRate(average.padLifeFR,      accPadFR);
    writeMeanRate(average.padLifeRL,      accPadRL);
    writeMeanRate(average.padLifeRR,      accPadRR);
    writeMeanRate(average.discLifeFL,     accDiscFL);
    writeMeanRate(average.discLifeFR,     accDiscFR);
    writeMeanRate(average.discLifeRL,     accDiscRL);
    writeMeanRate(average.discLifeRR,     accDiscRR);
    writeMeanRate(average.brakeDamageLF,  accBrakeDmgLF);
    writeMeanRate(average.brakeDamageRF,  accBrakeDmgRF);
    writeMeanRate(average.brakeDamageLR,  accBrakeDmgLR);
    writeMeanRate(average.brakeDamageRR,  accBrakeDmgRR);
    writeMeanRate(average.clutchWear,     accClutch);

    return average;
}

std::string severityLabel(AnalysisSeverity severity, ReportLanguage language) {
    if (severity == AnalysisSeverity::Critical)
        return language == ReportLanguage::French ? "[CRITIQUE] " : "[CRITICAL] ";
    if (severity == AnalysisSeverity::Warning)
        return language == ReportLanguage::French ? "[ATTENTION] " : "[WARNING] ";
    return "[INFO] ";
}

} // namespace

// Builds a human-readable list of lap numbers, e.g. "3, 4, 5, 6"
static std::string formatLapList(const std::vector<int>& laps) {
    std::ostringstream oss;
    for (std::size_t i = 0; i < laps.size(); ++i) {
        if (i > 0) oss << ", ";
        oss << laps[i];
    }
    return oss.str();
}

// Formats a duration in seconds as "M:SS.mmm" (e.g. 81.65 → "1:21.650").
static std::string formatLapTime(double seconds) {
    if (!std::isfinite(seconds) || seconds <= 0.0)
        return "";
    const int totalMs = static_cast<int>(seconds * 1000.0 + 0.5);
    const int ms  = totalMs % 1000;
    const int s   = (totalMs / 1000) % 60;
    const int m   = totalMs / 60000;
    char buf[32];
    snprintf(buf, sizeof(buf), "%d:%02d.%03d", m, s, ms);
    return std::string(buf);
}

// Returns a short label for an exclusion reason.
static std::string exclusionReasonLabel(LapSelectionInfo::ExcludedLap::Reason reason, ReportLanguage lang) {
    switch (reason) {
    case LapSelectionInfo::ExcludedLap::Reason::NonFinite:
        return lang == ReportLanguage::French ? "durée invalide" : "invalid duration";
    case LapSelectionInfo::ExcludedLap::Reason::TooShort:
        return lang == ReportLanguage::French ? "trop court (outlier)" : "too short (outlier)";
    case LapSelectionInfo::ExcludedLap::Reason::TooLong:
        return lang == ReportLanguage::French ? "trop long (outlier)" : "too long (outlier)";
    case LapSelectionInfo::ExcludedLap::Reason::ValidButUnused:
        return lang == ReportLanguage::French ? "valide mais non inclus dans la moyenne"
                                              : "valid but not used in average";
    }
    return "";
}

// Checks whether a StatSummary has any samples.
static inline bool hasData(const StatSummary& s) { return s.count > 0; }

// Builds the per-section data-availability breakdown for the Summary tab.
static std::string buildDataAvailabilityText(const LapStats& lap, ReportLanguage lang) {
    const bool fr = (lang == ReportLanguage::French);
    std::ostringstream oss;

    struct SectionCheck {
        const char* enName;
        const char* frName;
        bool available;
    };

    // Gather availability for each major data section
    const bool brakeTempOk   = hasData(lap.brakeTempLF) || hasData(lap.brakeTempRF) ||
                               hasData(lap.brakeTempLR) || hasData(lap.brakeTempRR);
    const bool tyreTempOk    = hasData(lap.tyreInnerTempLF) || hasData(lap.tyreCentreTempLF) ||
                               hasData(lap.tyreOuterTempLF) || hasData(lap.surfTempCFL);
    const bool pressureOk    = hasData(lap.pressureLF) || hasData(lap.pressureRF) ||
                               hasData(lap.pressureLR) || hasData(lap.pressureRR);
    const bool suspTravelOk  = hasData(lap.susTravelLF) || hasData(lap.susTravelRF) ||
                               hasData(lap.susTravelLR) || hasData(lap.susTravelRR);
    const bool damperOk      = lap.damperLF.histogram.totalCount > 0 || lap.damperRF.histogram.totalCount > 0 ||
                               lap.damperLR.histogram.totalCount > 0 || lap.damperRR.histogram.totalCount > 0;
    const bool wheelSlipOk   = hasData(lap.wheelSlipLF) || hasData(lap.wheelSlipRF) ||
                               hasData(lap.wheelSlipLR) || hasData(lap.wheelSlipRR);
    const bool brakeTorqueOk = hasData(lap.brakeTorqueLF) || hasData(lap.brakeTorqueRF) ||
                               hasData(lap.brakeTorqueLR) || hasData(lap.brakeTorqueRR);
    const bool engineTempOk  = hasData(lap.waterTemp) || hasData(lap.oilTemp);

    std::vector<SectionCheck> sections;
    sections.push_back({"Brake temperatures", "Températures de freins", brakeTempOk});
    sections.push_back({"Tyre temperatures", "Températures des pneus", tyreTempOk});
    sections.push_back({"Tyre pressures", "Pressions des pneus", pressureOk});
    sections.push_back({"Suspension travel", "Course de suspension", suspTravelOk});
    sections.push_back({"Damper histograms", "Histogrammes d'amortisseurs", damperOk});
    sections.push_back({"Wheel slip", "Glissement des roues", wheelSlipOk});
    sections.push_back({"Brake torque", "Couple de freinage", brakeTorqueOk});
    sections.push_back({"Engine / oil temps", "Températures moteur / huile", engineTempOk});

    // Only show the breakdown if at least one section is missing data
    bool anyMissing = false;
    for (std::size_t i = 0; i < sections.size(); ++i) {
        if (!sections[i].available) { anyMissing = true; break; }
    }
    if (!anyMissing) return "";

    oss << "\n\n" << (fr ? "Données non disponibles :" : "Data not available:");
    for (std::size_t i = 0; i < sections.size(); ++i) {
        if (!sections[i].available) {
            oss << "\n  • " << (fr ? sections[i].frName : sections[i].enName);
            oss << (fr ? " — canaux absents du fichier" : " — channels not present in file");
        }
    }
    return oss.str();
}

// Builds the "Predictions" block for the Summary tab: per-lap consumption/wear rates
// and estimated remaining life, computed from averaged valid-lap LapDelta data.
static std::string buildPredictionsBlock(const LapStats& lap, ReportLanguage lang) {
    const bool fr = (lang == ReportLanguage::French);
    std::ostringstream oss;

    bool anyPrediction = false;

    // --- Fuel consumption (both sims) ---
    if (lap.fuelLevelEnds.valid() && lap.fuelCapacityL.count > 0) {
        const double deltaPct   = -lap.fuelLevelEnds.delta();          // positive % consumed per lap
        const double capacityL  = lap.fuelCapacityL.average();         // litres in tank
        if (deltaPct > 0.0 && std::isfinite(capacityL) && capacityL > 0.0) {
            anyPrediction = true;
            const double lPerLap   = deltaPct / 100.0 * capacityL;     // litres per lap
            const int    maxLaps   = static_cast<int>(100.0 / deltaPct + 0.5);

            oss << "\n\n" << (fr ? "── Prévisions ──\n" : "── Predictions ──\n");
            char buf[96];
            snprintf(buf, sizeof(buf), fr ? "Carburant : %.2f L/tour → ~%d tours max\n"
                                          : "Fuel: %.2f L/lap → ~%d laps max", lPerLap, maxLaps);
            oss << buf;
        }
    }

    // --- Tyre wear (both sims) ---
    {
        struct CornerWear { const char* enName; const char* frName; double ratePct; };
        std::vector<CornerWear> corners = {
            {"FL", "DL", lap.tyreWearLF.valid() ?  lap.tyreWearLF.delta() : 0.0},
            {"FR", "DR", lap.tyreWearRF.valid() ?  lap.tyreWearRF.delta() : 0.0},
            {"RL", "GA", lap.tyreWearLR.valid() ?  lap.tyreWearLR.delta() : 0.0},
            {"RR", "GR", lap.tyreWearRR.valid() ?  lap.tyreWearRR.delta() : 0.0},
        };

        bool anyTyre = false;
        for (auto& c : corners) { if (c.ratePct > 0.0 && std::isfinite(c.ratePct)) { anyTyre = true; break; } }

        if (anyTyre) {
            if (!anyPrediction) oss << "\n\n" << (fr ? "── Prévisions ──\n" : "── Predictions ──\n");
            anyPrediction = true;

            for (auto& c : corners) {
                char buf[96];
                if (!std::isfinite(c.ratePct) || c.ratePct <= 0.0) continue;
                const int lifeLaps = static_cast<int>(100.0 / c.ratePct + 0.5);
                snprintf(buf, sizeof(buf), fr ? "Usure pneus %s : %.2f %%/tour → ~%d tours\n"
                                              : "Tyre wear %s: %.2f %%/lap → ~%d laps",
                         (fr ? c.frName : c.enName), c.ratePct, lifeLaps);
                oss << buf;
            }
        }
    }

    // --- EVO brake pad / disc life remaining (1→0) ---
    {
        struct BrakeLife { const char* enLabel; const char* frLabel; double ratePerLap; };
        std::vector<BrakeLife> pads = {
            {"FL", "DL", lap.padLifeFL.valid() ? -lap.padLifeFL.delta() : 0.0},   // life goes DOWN → negate delta
            {"FR", "DR", lap.padLifeFR.valid() ? -lap.padLifeFR.delta() : 0.0},
            {"RL", "GA", lap.padLifeRL.valid() ? -lap.padLifeRL.delta() : 0.0},
            {"RR", "GR", lap.padLifeRR.valid() ? -lap.padLifeRR.delta() : 0.0},
        };

        bool anyPad = false;
        for (auto& p : pads) { if (p.ratePerLap > 0.0 && std::isfinite(p.ratePerLap)) { anyPad = true; break; } }

        if (anyPad) {
            if (!anyPrediction) oss << "\n\n" << (fr ? "── Prévisions ──\n" : "── Predictions ──\n");
            anyPrediction = true;

            for (auto& p : pads) {
                char buf[96];
                if (!std::isfinite(p.ratePerLap) || p.ratePerLap <= 0.0) continue;
                // rate is in ratio units per lap → convert to %/lap and estimate laps from full (1.0).
                const double pctRate = p.ratePerLap * 100.0;
                const int lifeLaps   = static_cast<int>(1.0 / p.ratePerLap + 0.5);
                snprintf(buf, sizeof(buf), fr ? "Plaquettes %s : %.2f %%/tour → ~%d tours\n"
                                              : "Brake pads %s: %.2f %%/lap → ~%d laps",
                         (fr ? p.frLabel : p.enLabel), pctRate, lifeLaps);
                oss << buf;
            }

            std::vector<BrakeLife> discs = {
                {"FL", "DL", lap.discLifeFL.valid() ? -lap.discLifeFL.delta() : 0.0},
                {"FR", "DR", lap.discLifeFR.valid() ? -lap.discLifeFR.delta() : 0.0},
                {"RL", "GA", lap.discLifeRL.valid() ? -lap.discLifeRL.delta() : 0.0},
                {"RR", "GR", lap.discLifeRR.valid() ? -lap.discLifeRR.delta() : 0.0},
            };

            bool anyDisc = false;
            for (auto& d : discs) { if (d.ratePerLap > 0.0 && std::isfinite(d.ratePerLap)) { anyDisc = true; break; } }

            if (anyDisc) {
                for (auto& d : discs) {
                    char buf[96];
                    if (!std::isfinite(d.ratePerLap) || d.ratePerLap <= 0.0) continue;
                    const double pctRate = d.ratePerLap * 100.0;
                    const int lifeLaps   = static_cast<int>(1.0 / d.ratePerLap + 0.5);
                    snprintf(buf, sizeof(buf), fr ? "Disques %s : %.2f %%/tour → ~%d tours\n"
                                                  : "Brake discs %s: %.2f %%/lap → ~%d laps",
                             (fr ? d.frLabel : d.enLabel), pctRate, lifeLaps);
                    oss << buf;
                }
            }
        }
    }

    // --- AMS2 brake damage (0→up) + clutch wear ---
    {
        struct DmgInfo { const char* enName; const char* frName; double ratePerLap; };
        std::vector<DmgInfo> dmg = {
            {"FL", "DL", lap.brakeDamageLF.valid() ?  lap.brakeDamageLF.delta() : 0.0},   // damage goes UP → positive delta
            {"FR", "DR", lap.brakeDamageRF.valid() ?  lap.brakeDamageRF.delta() : 0.0},
            {"RL", "GA", lap.brakeDamageLR.valid() ?  lap.brakeDamageLR.delta() : 0.0},
            {"RR", "GR", lap.brakeDamageRR.valid() ?  lap.brakeDamageRR.delta() : 0.0},
        };

        bool anyDmg = false;
        for (auto& d : dmg) { if (d.ratePerLap > 0.0 && std::isfinite(d.ratePerLap)) { anyDmg = true; break; } }

        const bool clutchOk = lap.clutchWear.valid() && lap.clutchWear.delta() > 0.0
                              && std::isfinite(lap.clutchWear.delta());

        if (anyDmg || clutchOk) {
            if (!anyPrediction) oss << "\n\n" << (fr ? "── Prévisions ──\n" : "── Predictions ──\n");
            anyPrediction = true;

            for (auto& d : dmg) {
                char buf[96];
                if (!std::isfinite(d.ratePerLap) || d.ratePerLap <= 0.0) continue;
                const double pctRate = d.ratePerLap * 100.0;   // ratio → %/lap
                snprintf(buf, sizeof(buf), fr ? "Dommage freins %s : +%.2f %%/tour\n"
                                              : "Brake damage %s: +%.2f %%/lap",
                         (fr ? d.frName : d.enName), pctRate);
                oss << buf;
            }

            if (clutchOk) {
                char buf[96];
                const double clutchPct = lap.clutchWear.delta() * 100.0;   // ratio → %/lap
                snprintf(buf, sizeof(buf), fr ? "Usure embrayage : +%.2f %%/tour\n"
                                              : "Clutch wear: +%.2f %%/lap", clutchPct);
                oss << buf;
            }
        }
    }

    return anyPrediction ? oss.str() : "";
}

// ============================================================================
// Phase 3 — Diagnostic findings rendering + step-12 recommendation gate
// All helpers are file-local (anonymous namespace). User-facing strings use the
// inline ternary pattern: `fr ? "..." : "..."` consistent with this translation unit.
// ============================================================================

namespace {

/// Best-effort mapping from catalog validation-channel strings to AnalysisAlias values.
/// Returns false when no alias exists — those entries are surfaced in Limitations,
/// never fabricated as concordant evidence (plan §Step 12 gate b).
static bool validationStringToAlias(const std::string& s, AnalysisAlias& out) {
    // Rear axle wheel slip → per-corner aliases; use rear average if available.
    if (s == "rear_wheel_slip")   { out = AnalysisAlias::WheelSlipRearAvg;  return true; }
    if (s == "front_wheel_slip")  { out = AnalysisAlias::WheelSlipFrontAvg; return true; }

    // Yaw rate — direct alias exists.
    if (s == "yaw_rate")          { out = AnalysisAlias::YawRateAvg;        return true; }

    // Lateral G — direct alias exists.
    if (s == "lateral_g" || s == "lat_g")  { out = AnalysisAlias::LatGAvg;   return true; }

    // Suspension travel → use front-left as representative corner.
    if (s == "suspension_travel") { out = AnalysisAlias::SusTravelMeanLF;   return true; }

    // Damper velocity → per-corner susp vel means are the closest proxy.
    if (s == "damper_velocity" || s == "rear_damper_velocity")  { out = AnalysisAlias::SuspVelMeanLR; return true; }
    if (s == "front_damper_velocity")                           { out = AnalysisAlias::SuspVelMeanLF; return true; }

    // Ride height → virtual Rake channel covers front/rear difference.
    if (s == "ride_height" || s == "body_height")  { out = AnalysisAlias::RideHeightFL; return true; }

    // Front wheel speed — direct alias exists.
    if (s == "front_wheel_speed")   { out = AnalysisAlias::WheelSpeedFrontAvg; return true; }
    if (s == "rear_wheel_speed")    { out = AnalysisAlias::WheelSpeedRearAvg;  return true; }

    // Brake bias → AMS2-only channel, but the alias is declared in the enum.
    if (s == "brake_balance" || s == "brake_bias_setting_avg")  { out = AnalysisAlias::BrakeBiasSettingAvg; return true; }

    // Tyre pressure — front-left as representative corner.
    if (s == "front_pressure" || s == "tyre_pressure_front")   { out = AnalysisAlias::TyrePressFL; return true; }
    if (s == "rear_pressure"  || s == "tyre_pressure_rear")    { out = AnalysisAlias::TyrePressRL; return true; }

    // TCS / ABS activity — direct aliases exist.
    if (s == "tcs_active_pct")   { out = AnalysisAlias::TcsActivePct;  return true; }
    if (s == "abs_active_pct")   { out = AnalysisAlias::AbsActivePct;  return true; }

    // Aero balance → EVO-only channel, alias declared in enum.
    if (s == "aero_balance" || s == "downforce_balance")  { out = AnalysisAlias::AeroBalanceAvg; return true; }

    // --- Intentionally unmapped (no AnalysisAlias exists) — surfaced as Limitations: ---
    // exit_speed / entry_speed : no speed alias in the enum
    // front_slip_angle         : slip angle is not computed by any detector
    // body_accel               : vertical acceleration channel not mapped
    // tyre_load                : load transfer estimate not available per-corner
    // front_dive               : derived metric, no direct alias
    // front_brake_temp / rear_brake_temp : brake temps are in LapStats but have no AnalysisAlias

    return false;   // unmapped → unverifiable
}

/// Count how many of the symptom's validation channels resolve to a present alias.
struct ValidationCoverage {
    int totalMapped = 0;      // strings that resolved to an alias (mappable)
    int present     = 0;      // mappable AND provider.hasValue() == true
    std::vector<std::string> unmapped;   // human-readable names with no alias counterpart

    bool anyPresent() const { return present > 0; }
};

static ValidationCoverage checkValidationChannels(const RuleSymptom* sym,
                                                  const DataProvider& provider) {
    ValidationCoverage cov;
    if (!sym) return cov;
    for (const auto& ch : sym->validationChannels) {
        AnalysisAlias alias;
        if (!validationStringToAlias(ch, alias)) {
            cov.unmapped.push_back(ch);   // no alias → unverifiable, listed in Limitations
        } else {
            ++cov.totalMapped;
            if (provider.hasValue(alias)) ++cov.present;
        }
    }
    return cov;
}

/// Gate condition (c): can we name a specific setting for this profile?
/// AC EVO has a full setup menu → true. AMS2 / PMR have no mapping.json entries
/// for the relevant parameters → false, emit "run comparative A/B test" only.
static bool canCategorizeSetting(ChannelProfile profile) {
    return (profile == ChannelProfile::EvoM1);   // AC EVO: full setup space available
}

// --- Human-readable cause label from snake_case id ---------------------------
/// Converts e.g. "rear_rebound_too_high" → "Rear rebound too high".
static std::string humanizeCauseId(const std::string& id) {
    std::string out;
    bool capNext = true;
    for (char c : id) {
        if (c == '_') { capNext = true; continue; }
        out += (capNext && c >= 'a' && c <= 'z') ? char(c - 32) : c;   // toupper via ASCII offset
        capNext = false;
    }
    return out;
}

// --- Render one DiagnosticFinding as a plain-text block ----------------------
/// Returns the formatted text AND sets `emittedCategoricalRec` to true when the
/// step-12 gate was fully satisfied (used for hasRecommendation on the section).
static std::string renderDiagnosticBlock(const DiagnosticFinding& f,
                                         const DataProvider& provider,
                                         ChannelProfile profile,
                                         ReportLanguage language,
                                         bool& emittedCategoricalRec) {
    emittedCategoricalRec = false;

    const bool fr  = (language == ReportLanguage::French);
    std::ostringstream oss;

    // --- Header: symptom name + confidence label -----------------------------
    const RuleSymptom* sym = diag::findSymptom(f.symptomId);
    const char* confLabel  = diagnosticConfidenceLabel(f.confidence);   // "LOW"/"MEDIUM"/"HIGH"
    oss << (fr ? "\n── Diagnostic : " : "\n── Diagnosis: ")
        << humanizeCauseId(f.symptomId)
        << " [" << confLabel << "]";

    // --- When observed -------------------------------------------------------
    if (!f.phaseWindow.empty()) {
        oss << "\n" << (fr ? "Observé lors de : " : "When observed: ") << f.phaseWindow;
    }

    // --- Measurements / evidence ---------------------------------------------
    if (!f.evidence.empty()) {
        oss << "\n" << (fr ? "Mesures :" : "Measurements:") << "\n";
        for (const auto& e : f.evidence)
            oss << "  • " << e << "\n";
    }

    // --- Possible causes (ranked) --------------------------------------------
    if (!f.rankedCauses.empty()) {
        oss << (fr ? "\nCauses possibles (classées) :\n" : "\nPossible causes (ranked):\n");
        int rank = 1;
        for (const auto& c : f.rankedCauses) {
            char wbuf[32];
            std::snprintf(wbuf, sizeof(wbuf), "%.0f%%", c.weight * 100.0);
            oss << "  " << rank++ << ". " << humanizeCauseId(c.cause)
                << (fr ? " (poids " : " (weight ") << wbuf << ")"
                << " → " << c.adjustment;
            if (!c.direction.empty())
                oss << " [" << c.direction << "]";
            oss << "\n";
        }
    }

    // --- Step-12 recommendation gate ------------------------------------------
    const bool gateA = (f.confidence >= g_thresholds.confidenceMedium);   // confidence ≥ MEDIUM
    ValidationCoverage cov = checkValidationChannels(sym, provider);      // gate b: distinguishable?
    const bool gateB = cov.anyPresent();                                  // at least one mappable channel present
    const bool gateC = canCategorizeSetting(profile);                     // profile has setup space

    if (gateA && gateB && gateC) {
        emittedCategoricalRec = true;   // all three gates satisfied → categorical recommendation

        oss << "\n" << (fr ? "Test suggéré :\n" : "Suggested test:\n");

        const CauseEntry& topCause = f.rankedCauses.front();
        oss << "  " << (fr ? "Réglage à tester : " : "Setting to test: ")
            << humanizeCauseId(topCause.adjustment) << "\n";

        if (!topCause.direction.empty()) {
            // Direction label in the report language.
            const char* dirLabel = topCause.direction.c_str();   // raw direction string from catalog
            oss << "  " << (fr ? "Direction : " : "Direction: ") << dirLabel << "\n";
        }

        if (!f.evidence.empty()) {
            oss << "  " << (fr ? "Raison : " : "Reason: ");
            // Use the first evidence item as the primary supporting measurement.
            oss << f.evidence.front() << "\n";
        }

        // Risks / side effects — generic per direction to avoid over-claiming.
        if (!topCause.direction.empty()) {
            const std::string& d = topCause.direction;
            if (d == "softer")
                oss << "  " << (fr ? "Effets secondaires : risque de sur-sollicitation en appui prolongé.\n"
                                   : "Side effects: risk of increased body roll under sustained load.\n");
            else if (d == "stiffer")
                oss << "  " << (fr ? "Effets secondaires : perte d'adhésion sur revêtement irrégulier possible.\n"
                                   : "Side effects: potential loss of grip on irregular surfaces.\n");
        }

    } else {
        // Gate not fully satisfied → comparative A/B framing only.
        oss << "\n" << (fr ? "Test suggéré :\n" : "Suggested test:\n")
            << "  " << (fr ? "Preuve insuffisante pour un réglage catégorique — effectuer un essai comparatif A/B.\n"
                           : "Insufficient evidence for a categorical setting change — run comparative A/B test.\n");

        if (!gateA) {
            oss << "  " << (fr ? "(Confiance trop faible : " : "(Confidence too low: ")
                << confLabel << ")" << "\n";
        }
    }

    // --- Watch next run -------------------------------------------------------
    if (sym && !sym->validationChannels.empty()) {
        oss << "  " << (fr ? "À surveiller au prochain passage : " : "Watch next run: ");
        bool first = true;
        for (const auto& ch : sym->validationChannels) {
            if (!first) oss << ", ";
            oss << ch;
            first = false;
        }
        oss << "\n";

        // Confirm / reject criteria — only when categorical rec was emitted.
        if (emittedCategoricalRec && !f.rankedCauses.empty()) {
            const CauseEntry& topCause = f.rankedCauses.front();
            oss << "  " << (fr ? "Confirmer si : " : "Confirm if: ")
                << humanizeCauseId(topCause.cause)
                << (fr ? " s'améliore après le changement.\n"
                       : " improves after the change.\n");
        }
    }

    // --- Limitations ----------------------------------------------------------
    bool hasLimitation = false;
    if (!f.missingChannels.empty()) {
        oss << "\n" << (fr ? "Limites :\n  • Canaux manquants : " : "Limitations:\n  • Missing channels: ");
        for (std::size_t i = 0; i < f.missingChannels.size(); ++i) {
            if (i > 0) oss << ", ";
            // AnalysisAlias → short name via a minimal switch. We don't have an alias→string table,
            // so we emit the enum ordinal as a stable identifier for traceability.
            char abuf[16];
            std::snprintf(abuf, sizeof(abuf), "alias#%d", static_cast<int>(f.missingChannels[i]));
            oss << abuf;
        }
        hasLimitation = true;
    }

    if (!cov.unmapped.empty()) {
        if (hasLimitation) oss << "\n";
        else               oss << "\n" << (fr ? "Limites :\n" : "Limitations:\n");
        oss << "  • " << (fr ? "Signaux de validation non vérifiables :" 
                             : "Unverifiable validation signals: ");
        for (std::size_t i = 0; i < cov.unmapped.size(); ++i) {
            if (i > 0) oss << ", ";
            oss << cov.unmapped[i];
        }
    }

    return oss.str();
}   // renderDiagnosticBlock

/// Build the full Diagnostics section content from a list of findings.
/// Returns empty string when there are no findings at all (tab stays hidden).
static std::string buildDiagnosticsSection(const std::vector<DiagnosticFinding>& diagFindings,
                                           const DataProvider& provider,
                                           ChannelProfile profile,
                                           ReportLanguage language) {
    if (diagFindings.empty()) return "";

    bool anyCategoricalRec = false;
    std::string out;
    for (const auto& f : diagFindings) {
        bool rec = false;
        std::string block = renderDiagnosticBlock(f, provider, profile, language, rec);
        if (!block.empty()) {
            anyCategoricalRec |= rec;   // track across all findings
            if (!out.empty()) out += "\n";
            out += block;
        }
    }

    (void)anyCategoricalRec;   // hasRecommendation is set by the caller via a separate pass.
    return out;
}

// --- Helper: re-run gate check to determine if ANY finding emitted a categorical rec --
static bool anyFindingHasCategoricalRec(const std::vector<DiagnosticFinding>& diagFindings,
                                        const DataProvider& provider,
                                        ChannelProfile profile) {
    for (const auto& f : diagFindings) {
        bool gateA = (f.confidence >= g_thresholds.confidenceMedium);
        if (!gateA) continue;

        ValidationCoverage cov = checkValidationChannels(diag::findSymptom(f.symptomId), provider);
        if (cov.anyPresent() && canCategorizeSetting(profile)) return true;
    }
    return false;
}   // anyFindingHasCategoricalRec

}   // anonymous namespace — Phase 3 helpers end here


ReportResult LapReportGenerator::generate(const LapStats& lap,
                                          ReportLanguage language, ChannelProfile profile,
                                          const LapSelectionInfo* selection) {
    // Phase 3: profile is now used for the step-12 gate condition (c).

    ReportResult result;
    AnalysisSettings settings = buildDefaultAnalysisSettings();
    settings.language = language;

    // --- Layer 1: per-vehicle threshold override (AMS2 only) ---
    VehicleInfo vehicleRef;
    const bool hasVehicleRef = !lap.vehicleName.empty() && lap.vehicleName != "Unknown vehicle"
        && Ams2VehicleDb::instance().find(lap.vehicleName, vehicleRef);
    if (hasVehicleRef) {
        if (vehicleRef.engine.hasOilWarningTemp)
            settings.engine.oilTempWarnC = vehicleRef.engine.oilWarningTempC;

        // Per-wheel brake fade window from the embedded AMS2 DB. brakes[] index order is
        // {FL, FR, RL, RR} — identical to BrakeThresholds::wheels[]. Only copy a wheel when
        // its section exists AND carries explicit FadeRange temps; otherwise leave has* false
        // so that wheel simply produces no finding (no silent gap).
        for (int w = 0; w < 4; ++w) {
            const WheelBrakeData& wb = vehicleRef.chassis.brakes[w];
            if (!wb.present || !wb.hasFadeTemps)
                continue;
            settings.brakes.wheels[w].hasFadeLow  = true;
            settings.brakes.wheels[w].fadeLowC    = wb.fadeRangeBelowC;
            settings.brakes.wheels[w].hasFadeHigh = true;
            settings.brakes.wheels[w].fadeHighC   = wb.fadeRangeAboveC;
        }
        if (vehicleRef.engine.hasWaterWarningTemp)
            settings.engine.waterTempWarnC = vehicleRef.engine.waterWarningTempC;
    }

    const std::vector<AnalysisResult> findings = evaluateLapWithRules(lap, settings);
    for (std::size_t i = 0; i < findings.size(); ++i) {
        const AnalysisResult& finding = findings[i];
        const ReportSection section = mapAnalysisResultToReportSection(finding);
        SectionData& data = result[section];
        if (!data.content.empty())
            data.content += "\n";
        // First sentence of the tab entry: the logic/expected value that triggered the finding.
        if (!finding.expectedValue.empty()) {
            data.content += (language == ReportLanguage::French ? "Attendu : " : "Expected: ")
                           + finding.expectedValue + "\n";
        }
        data.content += severityLabel(finding.severity, language) + finding.title + "\n";
        if (!finding.description.empty())
            data.content += finding.description + "\n";
        if (!finding.recommendation.empty()) {
            data.content += (language == ReportLanguage::French ? "Recommandation : " : "Recommendation: ");
            data.content += finding.recommendation + "\n";
        }
        data.hasRecommendation = true;
    }

    // --- Layer 1: optimal oil temperature info line (Engine Cooling tab) ---
    // Shows the target AND the alert limits so the user always sees what the
    // thresholds are, even when no finding is triggered. The values here mirror
    // those embedded in individual findings — not a separate "max" display.
    if (hasVehicleRef && vehicleRef.engine.hasOptOilTemp) {
        const bool fr = (language == ReportLanguage::French);
        std::ostringstream oss;
        oss << (fr ? "Temperature d'huile optimale (" : "Optimal oil temperature (")
            << lap.vehicleName << ") : ";
        // Round to integer for display.
        const int optC = static_cast<int>(vehicleRef.engine.optOilTempC + 0.5);
        oss << optC;
        oss << " deg C";   // ASCII-safe (bcc32c encodes narrow literals as CP1252)

        if (vehicleRef.engine.hasOilWarningTemp) {
            const int warnC = static_cast<int>(vehicleRef.engine.oilWarningTempC + 0.5);
            oss << " | " << (fr ? "Alerte huile : " : "Oil alert: ") << warnC << " deg C";
        }
        if (vehicleRef.engine.hasWaterWarningTemp) {
            const int wtrC = static_cast<int>(vehicleRef.engine.waterWarningTempC + 0.5);
            oss << " | " << (fr ? "Alerte eau : " : "Water alert: ") << wtrC << " deg C";
        }

        SectionData& data = result[ReportSection::EngineCooling];
        if (!data.content.empty())
            data.content += "\n\n";
        data.content += oss.str() + "\n";
    }

    // --- Damper-velocity recommendations (Suspension section) ---
    {
        for (int c = 0; c < 4; ++c) {
            const VelocityHistogram& hist = pickDamperHistogram(lap, c);
            const std::string rec = getDamperRecommendation(hist, c, language);
            if (!rec.empty()) {
                SectionData& data = result[ReportSection::Suspension];
                // Blank line between corners so each corner's zone lines stay grouped.
                if (!data.content.empty())
                    data.content += "\n\n";
                data.content += rec + "\n";
                data.hasRecommendation = true;
            }
        }
    }

    // --- Phase 3: Diagnostic findings → Diagnostics section (step-12 gate) ---
    {
        DataProvider diagProvider = buildDataProviderFromLap(lap);
        diagProvider.setChannelMappings(RuleConfig::instance().channelMappings());

        const std::vector<DiagnosticFinding> diagFindings =
            diag::buildDefaultDiagnosticRegistry()
                .evaluateAll(diagProvider, settings);

        if (!diagFindings.empty()) {
            SectionData& ddata = result[ReportSection::Diagnostics];
            ddata.content         = buildDiagnosticsSection(diagFindings, diagProvider, profile, language);
            ddata.hasRecommendation = anyFindingHasCategoricalRec(diagFindings, diagProvider, profile);
        }
    }

    std::ostringstream summary;
    const bool fr = (language == ReportLanguage::French);

    // --- Car & Track ---
    auto displayName = [](const std::string& name) -> const char* {
        if (name.empty() || name == "Unknown vehicle" || name == "Unknown circuit")
            return "N/A";
        return name.c_str();
    };
    // Car name with alias lookup (AMS2 DB): show "Alias*" if found, else raw name.
    {
        std::string carDisp = lap.vehicleName;
        VehicleInfo dbLookup;
        if (!carDisp.empty() && carDisp != "Unknown vehicle"
            && Ams2VehicleDb::instance().find(carDisp, dbLookup) && !dbLookup.alias.empty())
            carDisp = dbLookup.alias + "*";
        summary << (fr ? "Voiture : " : "Car: ") << displayName(carDisp) << "\n";
    }
	summary << (fr ? "Circuit : " : "Track: ") << displayName(lap.trackName) << "\n\n";

    // --- Lap selection breakdown ---
    if (selection && selection->totalLaps > 0) {
        summary << (fr ? "Tours inclus : " : "Laps included: ")
                << selection->includedLaps.size() << " / " << selection->totalLaps;
        if (!selection->includedLaps.empty())
            summary << " (" << formatLapList(selection->includedLaps) << ")";

        if (!selection->excludedLaps.empty()) {
            summary << "\n" << (fr ? "Tours exclus :" : "Excluded laps:");
            for (std::size_t i = 0; i < selection->excludedLaps.size(); ++i) {
                const LapSelectionInfo::ExcludedLap& ex = selection->excludedLaps[i];
                summary << "\n  • " << (fr ? "Tour" : "Lap") << " " << ex.lapNumber;
                if (std::isfinite(ex.duration) && ex.duration > 0.0) {
                    const std::string t = formatLapTime(ex.duration);
                    if (!t.empty())
                        summary << " (" << t << ")";
                }
                summary << " — " << exclusionReasonLabel(ex.reason, language);
            }
        }
    }

    // --- Rule findings line ---
    if (findings.empty()) {
        summary << "\n\n" << (fr ? "Analyse par règles terminée. Aucun constat."
                                 : "Rule analysis complete. No findings.");
    } else {
        summary << "\n\n" << (fr ? "Analyse par règles terminée. Constats : "
                                 : "Rule analysis complete. Findings: ") << findings.size();
    }

    // --- Data availability breakdown ---
    const std::string dataAvail = buildDataAvailabilityText(lap, language);
    if (!dataAvail.empty())
        summary << dataAvail;

    // --- Predictions block (fuel / tyre wear / brake life) ---
    const std::string predictions = buildPredictionsBlock(lap, language);
    if (!predictions.empty())
        summary << predictions;

    result[ReportSection::Summary].content = summary.str();
    result[ReportSection::Summary].hasRecommendation = !findings.empty();
    return result;
}

ReportResult LapReportGenerator::generateAverageLaps(const std::vector<LapStats>& laps,
                                                      ReportLanguage language,
                                                      ChannelProfile profile) {
    const LapSelectionResult sel = selectValidLaps(laps);
    if (sel.valid.size() < 3) {
        ReportResult result;
        result[ReportSection::Summary].content = language == ReportLanguage::French
            ? "Au moins trois tours valides sont nécessaires pour une analyse moyenne."
            : "At least three valid laps are required for an average analysis.";
        return result;
    }

    std::vector<const LapStats*> selected;
    for (std::size_t i = 0; i < sel.valid.size(); ++i)
        selected.push_back(sel.valid[i]);
    const LapSelectionInfo info = refineSelectionInfo(sel, selected);
    return generate(averageSelectedLaps(selected), language, profile, &info);
}

ReportResult LapReportGenerator::generateBestThreeLaps(const std::vector<LapStats>& laps,
                                                        ReportLanguage language,
                                                        ChannelProfile profile) {
    const LapSelectionResult sel = selectValidLaps(laps);
    if (sel.valid.empty()) {
        ReportResult result;
        result[ReportSection::Summary].content = language == ReportLanguage::French
            ? "Aucun tour valide pour une analyse des trois meilleurs tours."
            : "No valid laps for a best-three analysis.";
        return result;
    }

    std::vector<std::pair<double, const LapStats*> > candidates;
    for (std::size_t i = 0; i < sel.valid.size(); ++i) {
        const double lapTime = lapDuration(*sel.valid[i]);
        if (lapTime > 0.0)
            candidates.push_back(std::make_pair(lapTime, sel.valid[i]));
    }
    std::stable_sort(candidates.begin(), candidates.end(), [](const std::pair<double, const LapStats*>& left,
                                                               const std::pair<double, const LapStats*>& right) {
        return left.first < right.first;
    });

    std::vector<const LapStats*> selected;
    const std::size_t selectedCount = std::min<std::size_t>(3, candidates.size());
    for (std::size_t i = 0; i < selectedCount; ++i)
        selected.push_back(candidates[i].second);
    const LapSelectionInfo info = refineSelectionInfo(sel, selected);
    return generate(averageSelectedLaps(selected), language, profile, &info);
}

LapStats LapReportGenerator::getAverageLapData(const std::vector<LapStats>& laps) {
    const LapSelectionResult sel = selectValidLaps(laps);
    if (sel.valid.size() < 2)
        return LapStats();

    std::vector<const LapStats*> selected;
    for (std::size_t i = 0; i < sel.valid.size(); ++i)
        selected.push_back(sel.valid[i]);
    return averageSelectedLaps(selected);
}

std::vector<LapStats> LapReportGenerator::getValidLaps(const std::vector<LapStats>& laps) {
    const LapSelectionResult sel = selectValidLaps(laps);
    std::vector<LapStats> result;
    for (std::size_t i = 0; i < sel.valid.size(); ++i)
        result.push_back(*sel.valid[i]);
    return result;
}