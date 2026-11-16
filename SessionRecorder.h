// ===========================================================================
// SessionRecorder — in-memory buffer of high-frequency live samples for LD export.
// Active only while the "Generate MoTeC File" toggle is on: every frame, each
// resolved channel's standard-unit value (raw * factor + offset) is appended to a
// per-channel sample vector keyed by its semantic alias ("Vehicle_Speed", ...).
// Beacon timestamps (wall-clock µs at pit→track transitions) are collected in the
// same buffer and written into the companion .ldx file. Data accumulates across
// laps for the whole session; it is flushed to disk during report generation, then
// cleared. Memory-only by design (~100 MB per 45-minute race at ~60 Hz).
// ===========================================================================

#pragma once

#include <cstddef>                     // size_t
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace LiveChannel { struct ResolvedChannelTable; }   // forward decl (LiveChannelResolver.h)

class SessionRecorder {
public:
    struct ChannelEntry {
        std::string name;              // alias label written into the .ld header block
        std::string unit;              // standardUnit from mapping.json (i2 display)
        double timeSum = 0.0;          // sum of inter-sample deltas -> estimated frequency at export
        size_t sampleCount = 0;
        double lastTime = -1.0;        // wall-clock seconds of the previous sample (-1 == none yet)
        std::vector<float> samples;    // float32, matches the .ld type-7/entrySize-4 layout

        int estimatedFrequencyHz() const {   // 60 Hz default when too few deltas to estimate
            if (sampleCount < 8 || timeSum <= 1e-9) return 60;
            double hz = static_cast<double>(sampleCount - 1) / timeSum;
            int f = static_cast<int>(hz + 0.5);
            if (f < 1)   f = 1;
            if (f > 32767) f = 32767;        // .ld frequency field is a signed 16-bit value
            return f;
        }
    };

    void enable();                     // start recording (clears any previous buffer)
    void disable();                    // stop + clear
    bool enabled() const { return m_enabled; }
    void clear();                      // drop all samples/beacons, keep the enabled flag

    // Per-frame feed: iterates table.channels × rawChannels and appends v = raw*factor+offset.
    // Channel order follows first-seen insertion (stable — driven by mapping.json row order).
    void feed(const LiveChannel::ResolvedChannelTable& table, const std::unordered_map<std::string, double>& raw,
              double timeSeconds);

    void recordBeacon(int64_t timeUs) { m_beacons.push_back(timeUs); }

    size_t channelCount()  const { return m_channels.size(); }
    bool   hasSamples()    const;      // true when at least one sample was recorded this session

private:
    // Returns the entry for `name`, creating it (in insertion order) on first sight.
    ChannelEntry* findOrCreate(const std::string& name);

    bool                                        m_enabled = false;
    std::vector<ChannelEntry>                   m_channels;          // insertion order == export order
    std::unordered_map<std::string, size_t>     m_indexByName;       // fast per-frame lookup (hot path)
    std::vector<int64_t>                        m_beacons;           // wall-clock µs at lap transitions

public:
    const std::vector<ChannelEntry>& channels()  const { return m_channels; }
    const std::vector<int64_t>&      beacons()   const { return m_beacons; }
};
