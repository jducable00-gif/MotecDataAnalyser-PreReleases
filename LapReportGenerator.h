#ifndef LAPREPORTGENERATOR_H
#define LAPREPORTGENERATOR_H

// ===============================
// Rule-only report output routed to the existing report sections.
// ===============================

#include <string>
#include <vector>
#include <map>

// Forward declarations to avoid circular dependency with MotecSetupAnalyzer.h
struct LapStats;
enum class ReportLanguage;
enum class ChannelProfile;

enum class ReportSection {
    Summary,
    General,
    Tyre,
    Brake,
    Suspension,
    EngineCooling,
    Handling,
    Diagnostics   // Phase 3: diagnostic findings + controlled-test recommendations
};

struct SectionData {
    std::string content;
    bool hasRecommendation = false;
};

using ReportResult = std::map<ReportSection, SectionData>;

// Describes which laps were used for the averaged report and why the others
// were dropped. Surfaced in the Summary tab so the user can see exactly how
// many / which laps were taken into account.
struct LapSelectionInfo {
    int totalLaps = 0;                 // every lap detected in the file
    std::vector<int> includedLaps;     // lap numbers used for the average
    double sessionBestLap = -1.0;      // fastest valid lap duration found during selection

    struct ExcludedLap {
        int lapNumber = 0;
        double duration = 0.0;         // seconds (may be non-finite)
        enum class Reason { NonFinite, TooShort, TooLong, ValidButUnused } reason = Reason::TooShort;
    };
    std::vector<ExcludedLap> excludedLaps;
};

class LapReportGenerator {
public:
    // Per-lap report (header + recommendations + data quality summary).
    // When `selection` is provided, the Summary tab also shows which laps were
    // included/excluded and a per-section data-availability breakdown.
    static ReportResult generate(const LapStats& lap,
                                 ReportLanguage language, ChannelProfile profile,
                                 const LapSelectionInfo* selection = nullptr);

    // Best-three-laps average report (selection list + per-lap avg report + fastest-vs-slowest comparison)
    static ReportResult generateBestThreeLaps(const std::vector<LapStats>& laps,
                                              ReportLanguage language, ChannelProfile profile);

    // Averaged lap report (all laps except the last one)
    static ReportResult generateAverageLaps(const std::vector<LapStats>& laps,
                                             ReportLanguage language, ChannelProfile profile);

    // Returns the averaged LapStats object for all but the last lap. 
    // Requires at least 2 laps to be valid (returns empty if < 2).
    static LapStats getAverageLapData(const std::vector<LapStats>& laps);

    // Returns only the laps that pass the validity window, so UI graphics
    // (brake temps, tyre temps, suspension) stay consistent with the report.
    static std::vector<LapStats> getValidLaps(const std::vector<LapStats>& laps);

};

#endif // LAPREPORTGENERATOR_H
