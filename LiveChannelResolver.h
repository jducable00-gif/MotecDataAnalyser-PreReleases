//---------------------------------------------------------------------------
// LiveChannelResolver.h — JSON-table-driven channel resolution for live capture.
//
// Part of the LIVE_CAPTURE_JSON_MIGRATION_PLAN (docs/LIVE_CAPTURE_JSON_MIGRATION_PLAN.md).
// Replaces the per-sim unit-conversion + field-assignment code inside
// Ams2FeedLap / AcsEvoFeedLap / PmrFeedLap with ONE generic feed function driven
// by a pre-resolved table built from mapping.json (RuleConfig singleton), filtered
// on profile + source == LiveSharedMemory.
//
// Design:
//   - Each capture keeps ONLY binary-layout parsing and produces, per frame, a flat
//     dictionary of RAW named channel values (no unit conversion in the sim code).
//   - ResolvedChannelTable::build() runs ONCE at connect time; it turns every LIVE
//     mapping row into {rawName -> sink function + factor/offset}. The alias string
//     is resolved to a LapStats field through a static dispatch table (the only
//     place in the codebase that knows which JSON alias feeds which LapStats member).
//   - FeedLapFromTable() runs per frame: dict lookup, v = raw * factor + offset,
//     sink(lap, v). Zero allocation on the hot path (~60 hash lookups ≈ 1 µs/frame).
//
// What deliberately does NOT go through the table (stays in sim C++):
//   - Non-linear conversions: TCS/ABS bit-flag -> numeric, AMS2 tyre-temp L/R swap
//     per wheel position, PMR tread innerFirst resolution. These are pre-computed as
//     raw dictionary values by the capture code before FeedLapFromTable() runs.
//   - Multi-value derived samples (addHandlingSample / addSteerYaw) — they consume
//     several converted channels at once and stay in per-sim post-table code.
//---------------------------------------------------------------------------

#pragma once

#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

#include "MotecSetupAnalyzer.h"   // LapStats, ChannelProfile

namespace LiveChannel {

// Sink for a single converted value: appends v to the matching StatSummary or
// VelocityHistogram member of lap. Stateless — convertible to function pointer.
typedef void (*ValueSink)(LapStats& lap, double v);

// Optional second sink used by PMR (Phase 3): records suspension travel with its
// timestamp so DamperCornerStats can derive shaft velocity (dTravel/dt). Null for
// every other channel and simulator; the feed loop calls it only when present.
typedef void (*DamperSampleSink)(LapStats& lap, double v, double timeSeconds);

struct ResolvedChannel {
    std::string rawName;                 // key into the per-frame RAW value dictionary
    ValueSink addValue = 0;              // primary sink (StatSummary / VelocityHistogram)
    DamperSampleSink damperAddSample = 0;// optional dual-write for suspension travel
    double factor = 1.0;                 // v_std = raw * factor + offset
    double offset = 0.0;
    std::string alias;                   // semantic name ("Vehicle_Speed") — LD export channel label
    std::string unit;                    // standardUnit from mapping.json — written into the .ld header for i2 display
};

// Pre-resolved table, built once per capture session (at connect time).
struct ResolvedChannelTable {
    std::vector<ResolvedChannel> channels;   // one entry per LIVE mapping row for the profile
    ChannelProfile profile = ChannelProfile::Generic;
    std::vector<std::string> missingAliases; // aliases with no dispatch-table sink — report these

    // Builds from RuleConfig::instance().channelMappings(): keeps rows whose source is
    // LiveSharedMemory and whose profile matches (or is Generic, which applies to all).
    // Virtual-type rows are skipped here — virtual channels resolve in DataProvider.
    void build(ChannelProfile p);

    // If requiredNames is non-empty: reports every name not present as a rawName in the
    // table into missingReport and returns false when anything is absent (visible warning,
    // never fatal). With an empty list it only checks that the table itself has entries.
    bool validate(const std::vector<std::string>& requiredNames, std::string& missingReport) const;

    size_t count() const { return channels.size(); }
};

// The ONE generic per-frame feed (replaces the conversion/assignment blocks of all three
// FeedLap functions). rawChannels maps RAW channel names to unconverted values for this
// frame. Channels absent from the dictionary are skipped silently — a sim simply does not
// expose every alias, and missing coverage is surfaced once at build time via validate().
void FeedLapFromTable(LapStats& lap,
                      const std::unordered_map<std::string, double>& rawChannels,
                      const ResolvedChannelTable& table,
                      double timeSeconds);

} // namespace LiveChannel
