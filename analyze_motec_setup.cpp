//---------------------------------------------------------------------------
// analyze_motec_setup.cpp — Cœur de l'analyse de la télémétrie MoTeC.
//
// Ce fichier contient :
//   - Normalisation des noms de canaux et résolution des alias
//     (normalizeChannelName / resolveChannelKey) selon le profil (Evo / AMS2).
//   - Conversion d'unités et routage des canaux (dispatchForChannel /
//     conversionForChannel).
//   - Évaluation des pneus, butées, comportement (sous/dévissage), stabilité,
//     freinage, traction, etc. à partir des seuils g_thresholds.
//   - Génération des rapports (buildLapReport / buildBestThreeLapReport)
//     en anglais ou français.
//   - Analyse des fichiers LD/LDX (parseMotecLd).
//---------------------------------------------------------------------------

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>
#include <codecvt>
#include <locale>

#include "MotecSetupAnalyzer.h"
#include "DiagnosticEngine.h"
#include "MotecToCSV.h"
#include "RuleConfig.h"
#include "LapReportGenerator.h"
#include <IniFiles.hpp>

// Global thresholds instance (loaded from MotecDataAnalyser.thresholds.ini at startup)
Thresholds g_thresholds;



// ===============================
// Symptom weight INI save/load
// Format conservé : section [SymptomWeights], clé "symptôme|cause", valeur 0..1.
// Les règles sont dynamiques (JSON) — on itère la liste courante de RuleConfig.
// ===============================
void saveSymptomWeights(TIniFile* ini) {
    const std::vector<RuleSymptom>& rules = RuleConfig::instance().rules();
    for (std::size_t r = 0; r < rules.size(); ++r) {
        const RuleSymptom& rule = rules[r];
        for (std::size_t i = 0; i < rule.causes.size(); ++i) {
            std::string key = rule.id + "|" + rule.causes[i].cause;
            ini->WriteFloat("SymptomWeights", UTF8String(key.c_str()), rule.causes[i].weight);
        }
    }
}

void loadSymptomWeights(TIniFile* ini) {
    std::vector<RuleSymptom>& rules = RuleConfig::instance().rulesRef();
    for (std::size_t r = 0; r < rules.size(); ++r) {
        RuleSymptom& rule = rules[r];
        for (std::size_t i = 0; i < rule.causes.size(); ++i) {
            std::string key = rule.id + "|" + rule.causes[i].cause;
            double val = ini->ReadFloat("SymptomWeights", UTF8String(key.c_str()), rule.causes[i].weight);
            if (val >= 0.0 && val <= 1.0)
                rule.causes[i].weight = val;
        }
    }
}


// ===============================
// Résolution des canaux (normalisation des noms + alias par profil)
// ===============================
static std::string normalizeChannelName(std::string name) {
    while (!name.empty() && (name.back() == '\0' || name.back() == ' ' || name.back() == '\t' || name.back() == '\r' || name.back() == '\n'))
        name.pop_back();
    while (!name.empty() && (name.front() == ' ' || name.front() == '\t' || name.front() == '\r' || name.front() == '\n'))
        name.erase(name.begin());

    std::string normalized;
    normalized.reserve(name.size());
    for (char c : name) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
            normalized.push_back((char)std::toupper((unsigned char)c));
        else if (c == '_' || c == '-' || c == ' ' || c == '.' || c == '/' || c == '\\')
            normalized.push_back('_');
    }
    while (!normalized.empty() && normalized.back() == '_')
        normalized.pop_back();
    while (!normalized.empty() && normalized.front() == '_')
        normalized.erase(normalized.begin());
    return normalized;
}

std::string resolveChannelKey(const std::string& rawName,
                             ChannelProfile profile)
{
    std::string key = normalizeChannelName(rawName);
    if (key.empty())
        return key;

    // Les alias de canaux ne sont plus des tables statiques : ils vivent dans
    // RuleConfig (valeurs par défaut intégrées + fusion optionnelle depuis le JSON).
    // Ordre identique à l'ancien code : generic d'abord, puis profil EvoM1.
    std::string resolved;
    if (RuleConfig::instance().resolveAlias(profile, key, resolved))
        return resolved;


    return key;
}

static std::string normalizeUnitName(const std::string& unit) {
    std::string u = unit;
    std::transform(u.begin(), u.end(), u.begin(), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });
    u.erase(std::remove_if(u.begin(), u.end(), [](unsigned char c) {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\0' || c == '%' || c == static_cast<unsigned char>(176);
    }), u.end());
    return u;
}

enum ChannelDispatch {
    CH_NONE = 0,
    CH_STEERANGLE,
    CH_ROTY,
    CH_LAT_G,
    CH_LONG_G,
    CH_BRAKE,
    CH_THROTTLE,
    CH_SUS_TRAVEL_LF,
    CH_SUS_TRAVEL_RF,
    CH_SUS_TRAVEL_LR,
    CH_SUS_TRAVEL_RR,
    CH_SPEED,
    CH_RPMS,
    CH_WHEEL_SPEED_LF,
    CH_WHEEL_SPEED_RF,
    CH_WHEEL_SPEED_LR,
    CH_WHEEL_SPEED_RR,
    CH_BRAKE_TEMP_LF,
    CH_BRAKE_TEMP_RF,
    CH_BRAKE_TEMP_LR,
    CH_BRAKE_TEMP_RR,
    CH_TYRE_PRESS_LF,
    CH_TYRE_PRESS_RF,
    CH_TYRE_PRESS_LR,
    CH_TYRE_PRESS_RR,
    CH_BUMPSTOPUP_TRAVEL_LF,
    CH_BUMPSTOPUP_TRAVEL_RF,
    CH_BUMPSTOPUP_TRAVEL_LR,
    CH_BUMPSTOPUP_TRAVEL_RR,
    CH_AERO_HEIGHT0,
    CH_AERO_HEIGHT1,
    CH_AERO_HEIGHT2,
    CH_AERO_HEIGHT3,
    CH_AERO_BALANCE,
    CH_LSD_LOCKING_TORQUE,
    CH_C_SURF_TEMP_LF,
    CH_C_SURF_TEMP_RF,
    CH_C_SURF_TEMP_LR,
    CH_C_SURF_TEMP_RR,
    CH_TYRE_INNER_TEMP_LF,
    CH_TYRE_CENTRE_TEMP_LF,
    CH_TYRE_OUTER_TEMP_LF,
    CH_TYRE_INNER_TEMP_RF,
    CH_TYRE_CENTRE_TEMP_RF,
    CH_TYRE_OUTER_TEMP_RF,
    CH_TYRE_INNER_TEMP_LR,
    CH_TYRE_CENTRE_TEMP_LR,
    CH_TYRE_OUTER_TEMP_LR,
    CH_TYRE_INNER_TEMP_RR,
    CH_TYRE_CENTRE_TEMP_RR,
    CH_TYRE_OUTER_TEMP_RR,
    CH_ENGINE_OIL_TEMP,
    CH_ENGINE_WATER_TEMP,
    CH_FUEL_LEVEL,
    CH_FUEL_PRES,
    CH_TRACK_TEMP,
    CH_AMBIENT_TEMP,
    CH_BRAKE_TORQUE_LF,
    CH_BRAKE_TORQUE_RF,
    CH_BRAKE_TORQUE_LR,
    CH_BRAKE_TORQUE_RR,
    CH_WHEEL_SR_LF,
    CH_WHEEL_SR_RF,
    CH_WHEEL_SR_LR,
    CH_WHEEL_SR_RR,
    CH_TYRE_TAIR_LF,
    CH_TYRE_TAIR_RF,
    CH_TYRE_TAIR_LR,
    CH_TYRE_TAIR_RR,
    CH_SUSP_VEL_LF,
    CH_SUSP_VEL_RF,
    CH_SUSP_VEL_LR,
    CH_SUSP_VEL_RR,
    CH_BRAKE_BIAS_SETTING,
    CH_TCS_ACTIVE,
    CH_ABS_ACTIVE,
    CH_LAP_BEACON
};

static ChannelDispatch dispatchForChannel(const std::string& key, ChannelProfile profile) {
    // Check standard raw channel names first (Motec LD / EVO / AMS2 native names).
    static const std::unordered_map<std::string, ChannelDispatch> dispatchMap = {
        {"ROTY", CH_ROTY}, {"LAT_G", CH_LAT_G}, {"LONG_G", CH_LONG_G}, {"BRAKE", CH_BRAKE},
        {"THROTTLE", CH_THROTTLE}, {"SUS_TRAVEL_LF", CH_SUS_TRAVEL_LF}, {"SUS_TRAVEL_RF", CH_SUS_TRAVEL_RF},
        {"SUS_TRAVEL_LR", CH_SUS_TRAVEL_LR}, {"SUS_TRAVEL_RR", CH_SUS_TRAVEL_RR}, {"SPEED", CH_SPEED},
        {"RPMS", CH_RPMS}, {"WHEEL_SPEED_LF", CH_WHEEL_SPEED_LF}, {"WHEEL_SPEED_RF", CH_WHEEL_SPEED_RF},
        {"WHEEL_SPEED_LR", CH_WHEEL_SPEED_LR}, {"WHEEL_SPEED_RR", CH_WHEEL_SPEED_RR}, {"BRAKE_TEMP_LF", CH_BRAKE_TEMP_LF},
        {"BRAKE_TEMP_RF", CH_BRAKE_TEMP_RF}, {"BRAKE_TEMP_LR", CH_BRAKE_TEMP_LR}, {"BRAKE_TEMP_RR", CH_BRAKE_TEMP_RR},
        {"TYRE_PRESS_LF", CH_TYRE_PRESS_LF}, {"TYRE_PRESS_RF", CH_TYRE_PRESS_RF}, {"TYRE_PRESS_LR", CH_TYRE_PRESS_LR},
        {"TYRE_PRESS_RR", CH_TYRE_PRESS_RR}, {"BUMPSTOPUP_TRAVEL_LF", CH_BUMPSTOPUP_TRAVEL_LF},
        {"BUMPSTOPUP_TRAVEL_RF", CH_BUMPSTOPUP_TRAVEL_RF}, {"BUMPSTOPUP_TRAVEL_LR", CH_BUMPSTOPUP_TRAVEL_LR},
        {"BUMPSTOPUP_TRAVEL_RR", CH_BUMPSTOPUP_TRAVEL_RR}, {"AERO_HEIGHT0", CH_AERO_HEIGHT0},
        {"AERO_HEIGHT1", CH_AERO_HEIGHT1}, {"AERO_HEIGHT2", CH_AERO_HEIGHT2}, {"AERO_HEIGHT3", CH_AERO_HEIGHT3},
        {"AERO_BALANCE", CH_AERO_BALANCE}, {"LSD_LOCKING_TORQUE", CH_LSD_LOCKING_TORQUE}, {"C_SURF_TEMP_LF", CH_C_SURF_TEMP_LF},
        {"C_SURF_TEMP_RF", CH_C_SURF_TEMP_RF}, {"C_SURF_TEMP_LR", CH_C_SURF_TEMP_LR}, {"C_SURF_TEMP_RR", CH_C_SURF_TEMP_RR},
        {"TYRE_INNER_TEMP_LF", CH_TYRE_INNER_TEMP_LF}, {"TYRE_CENTRE_TEMP_LF", CH_TYRE_CENTRE_TEMP_LF},
        {"TYRE_OUTER_TEMP_LF", CH_TYRE_OUTER_TEMP_LF}, {"TYRE_INNER_TEMP_RF", CH_TYRE_INNER_TEMP_RF},
        {"TYRE_CENTRE_TEMP_RF", CH_TYRE_CENTRE_TEMP_RF}, {"TYRE_OUTER_TEMP_RF", CH_TYRE_OUTER_TEMP_RF},
        {"TYRE_INNER_TEMP_LR", CH_TYRE_INNER_TEMP_LR}, {"TYRE_CENTRE_TEMP_LR", CH_TYRE_CENTRE_TEMP_LR},
        {"TYRE_OUTER_TEMP_LR", CH_TYRE_OUTER_TEMP_LR}, {"TYRE_INNER_TEMP_RR", CH_TYRE_INNER_TEMP_RR},
        {"TYRE_CENTRE_TEMP_RR", CH_TYRE_CENTRE_TEMP_RR}, {"TYRE_OUTER_TEMP_RR", CH_TYRE_OUTER_TEMP_RR},
        {"ENGINE_OIL_TEMP", CH_ENGINE_OIL_TEMP}, {"ENGINE_WATER_TEMP", CH_ENGINE_WATER_TEMP},
        {"FUEL_LEVEL", CH_FUEL_LEVEL}, {"FUEL_PRES", CH_FUEL_PRES}, {"TRACK_TEMP", CH_TRACK_TEMP},
        {"AMBIENT_TEMP", CH_AMBIENT_TEMP}, {"BRAKE_TORQUE_LF", CH_BRAKE_TORQUE_LF}, {"BRAKE_TORQUE_RF", CH_BRAKE_TORQUE_RF},
        {"BRAKE_TORQUE_LR", CH_BRAKE_TORQUE_LR}, {"BRAKE_TORQUE_RR", CH_BRAKE_TORQUE_RR}, {"WHEEL_SR_LF", CH_WHEEL_SR_LF},
        {"WHEEL_SR_RF", CH_WHEEL_SR_RF}, {"WHEEL_SR_LR", CH_WHEEL_SR_LR}, {"WHEEL_SR_RR", CH_WHEEL_SR_RR},
        {"TYRE_TAIR_LF", CH_TYRE_TAIR_LF}, {"TYRE_TAIR_RF", CH_TYRE_TAIR_RF}, {"TYRE_TAIR_LR", CH_TYRE_TAIR_LR},
        {"TYRE_TAIR_RR", CH_TYRE_TAIR_RR}, {"SUSP_VEL_LF", CH_SUSP_VEL_LF}, {"SUSP_VEL_RF", CH_SUSP_VEL_RF},
        {"SUSP_VEL_LR", CH_SUSP_VEL_LR}, {"SUSP_VEL_RR", CH_SUSP_VEL_RR}, {"BRAKE_BIAS_SETTING", CH_BRAKE_BIAS_SETTING},
        {"TCS_ACTIVE", CH_TCS_ACTIVE}, {"ABS_ACTIVE", CH_ABS_ACTIVE}, {"LAP_BEACON", CH_LAP_BEACON}
    };

    auto it = dispatchMap.find(key);
    if (it != dispatchMap.end()) return it->second;

    return CH_NONE;
}

static void conversionForChannel(const std::string& key, const std::string& unit,
                                 double& factor, double& offset) {
    factor = 1.0;
    offset = 0.0;
    std::string u = normalizeUnitName(unit);

    if (key == "SPEED" || key.find("SPEED") != std::string::npos) {
        if (u == "MPS" || u == "M/S") factor = 3.6;
        return;
    }

    if (key == "ROTY" || key.find("ROTY") != std::string::npos) {
        if (u == "RAD/S" || u == "RADS") factor = 57.295779513;
        return;
    }

    if (key == "LAT_G" || key == "LONG_G" ||
        key == "G_LAT" || key == "G_LON" ||
        key == "LATERAL_G" || key == "LONGITUDINAL_G") {
        if (u == "M/S2" || u == "MPS2") factor = 1.0 / 9.80665;
        return;
    }

    if (key.find("WHEEL_SPEED") != std::string::npos ||
        key.find("WHEELSPEED") != std::string::npos) {
        if (u == "MPS" || u == "M/S") factor = 3.6;
        return;
    }

    if (key.find("SUSP_VEL") != std::string::npos) {
        if (u == "M/S" || u == "MPS") factor = 1000.0;
        return;
    }

    if (key.find("PRESS") != std::string::npos || key == "FUEL_PRES") {
        if (u == "KPA") factor = 1.0 / 6.894757;
        else if (u == "BAR") factor = 14.503774;
        else if (u == "PSI") factor = 1.0;
        return;
    }

    if (key.find("TEMP") != std::string::npos ||
        key == "TRACK_TEMP" || key == "AMBIENT_TEMP" ||
        key == "ENGINE_OIL_TEMP" || key == "ENGINE_WATER_TEMP") {
        if (u == "F" || u == "FAHRENHEIT") {
            factor = 5.0 / 9.0;
            offset = -32.0 * 5.0 / 9.0;
        }
        return;
    }
}

// Returns the averaged data for all but the last lap. 
// Requires at least 2 laps to be valid (returns empty if < 2).
LapStats buildAverageLapData(const std::vector<LapStats>& laps) {
    return LapReportGenerator::getAverageLapData(laps);
}

ReportResult buildAverageLapReport(const std::vector<LapStats>& laps,
                                  ReportLanguage language, ChannelProfile profile) {
    return LapReportGenerator::generateAverageLaps(laps, language, profile);
}

std::vector<AnalysisResult> evaluateLapWithRules(const LapStats& lap,
                                                const AnalysisSettings& settings) {
    DataProvider provider = buildDataProviderFromLap(lap);
    provider.setChannelMappings(RuleConfig::instance().channelMappings());
    const AnalysisRegistry registry = buildDefaultAnalysisRegistry();
    return registry.evaluateAll(provider, settings);
}

// ===============================
// Analyse des fichiers LD/LDX (décodage binaire puis agrégation par tour)
// ===============================
bool parseMotecLd(const std::string& path,
                  std::vector<LapStats>& laps,
                  std::string& errorMessage,
                  AnalysisProgressCallback progressCallback,
                  ChannelProfile profile)
{
    laps.clear();
    std::vector<MotecColumnInfo> columns;
    std::vector<double> beaconMarkers;
    std::string decodeError;
    if (!DecodeMotecLd(path, columns, beaconMarkers, decodeError, progressCallback)) {
        errorMessage = decodeError;
        return false;
    }

    int masterFrequency = 1;
    double duration = 0.0;
    for (size_t i = 0; i < columns.size(); ++i) {
        if (columns[i].frequency > 0) {
            masterFrequency = std::max(masterFrequency, columns[i].frequency);
            duration = std::max(duration, (double)columns[i].values.size() / columns[i].frequency);
        }
    }
    if (masterFrequency <= 0 || duration <= 0.0) {
        errorMessage = "No usable sample timing information was found in the LD/LDX file.";
        return false;
    }

    std::size_t sampleCount = static_cast<std::size_t>(duration * masterFrequency);
    size_t nextBeaconMarker = 0;
    double previousTime = 0.0;
    double previousBeacon = -1.0;
    int lapCounter = 1;
    LapStats currentLap;
    currentLap.lapNumber = lapCounter;
    currentLap.rideHeightUnit = profile == ChannelProfile::AMS2
        ? RideHeightUnit::Centimeters : RideHeightUnit::Millimeters;

    // Precompute per-column dispatch and unit conversion (constant per column)
    std::vector<ChannelDispatch> colDispatch(columns.size(), CH_NONE);
    std::vector<double> colFactor(columns.size(), 1.0);
    std::vector<double> colOffset(columns.size(), 0.0);
    for (size_t i = 0; i < columns.size(); ++i) {
        const std::string key = resolveChannelKey(columns[i].name, profile);
        colDispatch[i] = dispatchForChannel(key, profile);
        conversionForChannel(key, columns[i].unit, colFactor[i], colOffset[i]);
    }

    // Integer resampling cursors: advance each column's sample index with exact
    // integer arithmetic (Bresenham) instead of a float multiply per cell.
    std::vector<std::size_t> colCursor(columns.size(), 0);   // current sample index per column
    std::vector<int> colAccumulator(columns.size(), 0);      // Bresenham accumulator (master-Freq units)

    for (std::size_t row = 0; row < sampleCount; ++row) {
        double timeValue = static_cast<double>(row) / static_cast<double>(masterFrequency);
        double beacon = 0.0;
        bool newLap = false;

        if (progressCallback && sampleCount > 0 && row % std::max<size_t>(1, sampleCount / 100) == 0) {
            int percent = static_cast<int>((double)(row + 1) / (double)sampleCount * 100.0);
            if (percent > 0 && percent <= 100)
                progressCallback(percent);
        }

        if (nextBeaconMarker < beaconMarkers.size() && timeValue >= beaconMarkers[nextBeaconMarker]) {
            newLap = true;
            ++nextBeaconMarker;
        }

        if (newLap && currentLap.rowCount > 0) {
            currentLap.endTime = previousTime;
            laps.push_back(currentLap);
            ++lapCounter;
            currentLap = LapStats();
            currentLap.lapNumber = lapCounter;
            currentLap.rideHeightUnit = profile == ChannelProfile::AMS2
                ? RideHeightUnit::Centimeters : RideHeightUnit::Millimeters;
        }

        if (currentLap.rowCount == 0)
            currentLap.startTime = timeValue;

        currentLap.endTime = timeValue;
        currentLap.rowCount++;

        double sampleSteer = std::numeric_limits<double>::quiet_NaN();
        double sampleYaw = std::numeric_limits<double>::quiet_NaN();
        double sampleLatG = std::numeric_limits<double>::quiet_NaN();
        double sampleLongG = std::numeric_limits<double>::quiet_NaN();
        double sampleBrake = 0.0;
        double sampleThrottle = 0.0;
        double sampleSpeed = std::numeric_limits<double>::quiet_NaN();
        double suspensionFrontLeft = std::numeric_limits<double>::quiet_NaN();
        double suspensionFrontRight = std::numeric_limits<double>::quiet_NaN();
        double suspensionRearLeft = std::numeric_limits<double>::quiet_NaN();
        double suspensionRearRight = std::numeric_limits<double>::quiet_NaN();

        for (size_t i = 0; i < columns.size(); ++i) {
            const auto& values = columns[i].values;
            if (values.empty())
                continue;

            // Advance integer resampling cursor (exact nearest-sample stepping,
            // no floating point). Runs at most once per row since each column's
            // frequency is <= masterFrequency.
            colAccumulator[i] += columns[i].frequency;
            while (colAccumulator[i] >= masterFrequency) {
                colAccumulator[i] -= masterFrequency;
                if (colCursor[i] + 1 < values.size()) ++colCursor[i];
            }
            std::size_t sample = colCursor[i];
            if (sample >= values.size())   // tail rows beyond this column's data
                continue;

            double value = values[sample] * colFactor[i] + colOffset[i];

            switch (colDispatch[i]) {
                case CH_STEERANGLE: sampleSteer = value; currentLap.steerAngle.add(value); break;
                case CH_ROTY: sampleYaw = value; currentLap.yawRate.add(value); break;
                case CH_LAT_G: sampleLatG = value; currentLap.latG.add(value); break;
                case CH_LONG_G: sampleLongG = value; currentLap.longG.add(value); break;
                case CH_BRAKE: sampleBrake = value; currentLap.brakePedal.add(value); break;
                case CH_THROTTLE: sampleThrottle = value; currentLap.throttle.add(value); break;
                case CH_SUS_TRAVEL_LF: suspensionFrontLeft = value; currentLap.susTravelLF.add(value); currentLap.damperLF.addSample(value, timeValue); break;
                case CH_SUS_TRAVEL_RF: suspensionFrontRight = value; currentLap.susTravelRF.add(value); currentLap.damperRF.addSample(value, timeValue); break;
                case CH_SUS_TRAVEL_LR: suspensionRearLeft = value; currentLap.susTravelLR.add(value); currentLap.damperLR.addSample(value, timeValue); break;
                case CH_SUS_TRAVEL_RR: suspensionRearRight = value; currentLap.susTravelRR.add(value); currentLap.damperRR.addSample(value, timeValue); break;
                case CH_SPEED: sampleSpeed = value; currentLap.speed.add(value); break;
                case CH_RPMS: currentLap.rpm.add(value); break;
                case CH_WHEEL_SPEED_LF: currentLap.wheelSpeedLF.add(value); break;
                case CH_WHEEL_SPEED_RF: currentLap.wheelSpeedRF.add(value); break;
                case CH_WHEEL_SPEED_LR: currentLap.wheelSpeedLR.add(value); break;
                case CH_WHEEL_SPEED_RR: currentLap.wheelSpeedRR.add(value); break;
                case CH_BRAKE_TEMP_LF: currentLap.brakeTempLF.add(value); break;
                case CH_BRAKE_TEMP_RF: currentLap.brakeTempRF.add(value); break;
                case CH_BRAKE_TEMP_LR: currentLap.brakeTempLR.add(value); break;
                case CH_BRAKE_TEMP_RR: currentLap.brakeTempRR.add(value); break;
                case CH_TYRE_PRESS_LF: currentLap.pressureLF.add(value); break;
                case CH_TYRE_PRESS_RF: currentLap.pressureRF.add(value); break;
                case CH_TYRE_PRESS_LR: currentLap.pressureLR.add(value); break;
                case CH_TYRE_PRESS_RR: currentLap.pressureRR.add(value); break;
                case CH_BUMPSTOPUP_TRAVEL_LF: currentLap.bumpStopLF.add(value); break;
                case CH_BUMPSTOPUP_TRAVEL_RF: currentLap.bumpStopRF.add(value); break;
                case CH_BUMPSTOPUP_TRAVEL_LR: currentLap.bumpStopLR.add(value); break;
                case CH_BUMPSTOPUP_TRAVEL_RR: currentLap.bumpStopRR.add(value); break;
                case CH_AERO_HEIGHT0: currentLap.aeroHeight0.add(value); break;
                case CH_AERO_HEIGHT1: currentLap.aeroHeight1.add(value); break;
                case CH_AERO_HEIGHT2: currentLap.aeroHeight2.add(value); break;
                case CH_AERO_HEIGHT3: currentLap.aeroHeight3.add(value); break;
                case CH_AERO_BALANCE: currentLap.aeroBalance.add(value); break;
                case CH_LSD_LOCKING_TORQUE: currentLap.lsdTorque.add(value); break;
                case CH_C_SURF_TEMP_LF: currentLap.surfTempCFL.add(value); break;
                case CH_C_SURF_TEMP_RF: currentLap.surfTempCRF.add(value); break;
                case CH_C_SURF_TEMP_LR: currentLap.surfTempCLR.add(value); break;
                case CH_C_SURF_TEMP_RR: currentLap.surfTempCRR.add(value); break;
                case CH_TYRE_INNER_TEMP_LF: currentLap.tyreInnerTempLF.add(value); break;
                case CH_TYRE_CENTRE_TEMP_LF: currentLap.tyreCentreTempLF.add(value); break;
                case CH_TYRE_OUTER_TEMP_LF: currentLap.tyreOuterTempLF.add(value); break;
                case CH_TYRE_INNER_TEMP_RF: currentLap.tyreInnerTempRF.add(value); break;
                case CH_TYRE_CENTRE_TEMP_RF: currentLap.tyreCentreTempRF.add(value); break;
                case CH_TYRE_OUTER_TEMP_RF: currentLap.tyreOuterTempRF.add(value); break;
                case CH_TYRE_INNER_TEMP_LR: currentLap.tyreInnerTempLR.add(value); break;
                case CH_TYRE_CENTRE_TEMP_LR: currentLap.tyreCentreTempLR.add(value); break;
                case CH_TYRE_OUTER_TEMP_LR: currentLap.tyreOuterTempLR.add(value); break;
                case CH_TYRE_INNER_TEMP_RR: currentLap.tyreInnerTempRR.add(value); break;
                case CH_TYRE_CENTRE_TEMP_RR: currentLap.tyreCentreTempRR.add(value); break;
                case CH_TYRE_OUTER_TEMP_RR: currentLap.tyreOuterTempRR.add(value); break;
				case CH_ENGINE_OIL_TEMP: currentLap.oilTemp.add(value); break;
                case CH_ENGINE_WATER_TEMP: currentLap.waterTemp.add(value); break;
                case CH_FUEL_LEVEL: currentLap.fuelLevel.add(value); break;
                case CH_FUEL_PRES: currentLap.fuelPressure.add(value); break;
                case CH_TRACK_TEMP: currentLap.trackTemp.add(value); break;
                case CH_AMBIENT_TEMP: currentLap.ambientTemp.add(value); break;
                case CH_BRAKE_TORQUE_LF: currentLap.brakeTorqueLF.add(value); break;
                case CH_BRAKE_TORQUE_RF: currentLap.brakeTorqueRF.add(value); break;
                case CH_BRAKE_TORQUE_LR: currentLap.brakeTorqueLR.add(value); break;
                case CH_BRAKE_TORQUE_RR: currentLap.brakeTorqueRR.add(value); break;
                case CH_WHEEL_SR_LF: currentLap.wheelSlipLF.add(value); break;
                case CH_WHEEL_SR_RF: currentLap.wheelSlipRF.add(value); break;
                case CH_WHEEL_SR_LR: currentLap.wheelSlipLR.add(value); break;
                case CH_WHEEL_SR_RR: currentLap.wheelSlipRR.add(value); break;
                case CH_TYRE_TAIR_LF: currentLap.tyreAirTempLF.add(value); break;
                case CH_TYRE_TAIR_RF: currentLap.tyreAirTempRF.add(value); break;
                case CH_TYRE_TAIR_LR: currentLap.tyreAirTempLR.add(value); break;
                case CH_TYRE_TAIR_RR: currentLap.tyreAirTempRR.add(value); break;
                case CH_SUSP_VEL_LF: currentLap.suspVelLF.add(value); break;
                case CH_SUSP_VEL_RF: currentLap.suspVelRF.add(value); break;
                case CH_SUSP_VEL_LR: currentLap.suspVelLR.add(value); break;
                case CH_SUSP_VEL_RR: currentLap.suspVelRR.add(value); break;
                case CH_BRAKE_BIAS_SETTING: currentLap.brakeBiasSetting.add(value); break;
                case CH_TCS_ACTIVE: currentLap.tcsActive.add(value); break;
                case CH_ABS_ACTIVE: currentLap.absActive.add(value); break;
                case CH_LAP_BEACON: beacon = value; break;
                case CH_NONE:
                default: break;
            }
        }

        double suspensionAsymmetry = 0.0;
        if (std::isfinite(suspensionFrontLeft) && std::isfinite(suspensionFrontRight) &&
            std::isfinite(suspensionRearLeft) && std::isfinite(suspensionRearRight)) {
            suspensionAsymmetry = std::max(
                std::abs(suspensionFrontLeft - suspensionFrontRight),
                std::abs(suspensionRearLeft - suspensionRearRight));
        }
        currentLap.addHandlingSample(sampleSteer, sampleYaw, sampleLatG, sampleLongG,
                                     sampleBrake, sampleThrottle, suspensionAsymmetry, sampleSpeed);

        previousTime = timeValue;
        previousBeacon = beacon;
    }

    if (currentLap.rowCount > 0) {
        currentLap.endTime = previousTime;
        laps.push_back(currentLap);
    }

    if (laps.empty()) {
        errorMessage = "No valid lap data was found in the LD/LDX file.";
        return false;
    }

    // Session metadata was already extracted from the header during decode
    // (no second full-file read). Take it from the first non-empty column.
    for (const MotecColumnInfo& col : columns) {
        if (!col.values.empty()) {
            for (LapStats& lap : laps) {
                lap.driverName  = col.driverName.empty()  ? "Unknown driver"  : col.driverName;
                lap.vehicleName = col.vehicleName.empty() ? "Unknown vehicle" : col.vehicleName;
                lap.trackName   = col.trackName.empty()   ? "Unknown circuit" : col.trackName;
            }
            break;
        }

    }
    return true;
}
