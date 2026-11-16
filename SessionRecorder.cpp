// ===========================================================================
// SessionRecorder — implementation (see header for the design notes).
// Hot path: feed() does one unordered_map lookup per channel row and appends a
// float; no allocation beyond vector growth. The recorder is only touched from
// the UI thread's live-frame handlers, so it needs no locking of its own.
// ===========================================================================

#include "SessionRecorder.h"
#include <cstddef>                     // size_t (RAD STL: not pulled in by the header's includes)
#include <cmath>                       // std::isfinite
#include "LiveChannelResolver.h"       // LiveChannel::ResolvedChannelTable / ResolvedChannel

void SessionRecorder::enable() {
    m_enabled = true;
    clear();
}

void SessionRecorder::disable() {
    m_enabled = false;
    clear();
}

void SessionRecorder::clear() {
    m_channels.clear();
    m_indexByName.clear();
    m_beacons.clear();
}

bool SessionRecorder::hasSamples() const {
    for (size_t i = 0; i < m_channels.size(); ++i)
        if (!m_channels[i].samples.empty()) return true;
    return false;
}

SessionRecorder::ChannelEntry* SessionRecorder::findOrCreate(const std::string& name) {
    // Fast path: already seen this channel.
    std::unordered_map<std::string, size_t>::iterator it = m_indexByName.find(name);
    if (it != m_indexByName.end()) return &m_channels[it->second];

    // First sample for a new channel — create the entry in insertion order.
    ChannelEntry e;
    e.name  = name;
    e.unit.clear();                    // stamped by feed() from the table row on first append
    m_indexByName[name] = m_channels.size();
    m_channels.push_back(e);
    return &m_channels.back();
}

void SessionRecorder::feed(const LiveChannel::ResolvedChannelTable& table,
                           const std::unordered_map<std::string, double>& raw,
                           double timeSeconds) {
    if (!m_enabled || raw.empty()) return;

    for (size_t i = 0; i < table.channels.size(); ++i) {
        const LiveChannel::ResolvedChannel& rc = table.channels[i];

        std::unordered_map<std::string, double>::const_iterator itRaw = raw.find(rc.rawName);
        if (itRaw == raw.end()) continue;   // channel absent this frame — skip (no gap fill)

        float fv = static_cast<float>(itRaw->second * rc.factor + rc.offset);
        if (!std::isfinite(fv)) fv = 0.0f;  // a single bad frame must not poison the column

        // Label: semantic alias ("Vehicle_Speed") so i2 shows readable names.
        const std::string label = !rc.alias.empty() ? rc.alias : rc.rawName;

        ChannelEntry* e = findOrCreate(label);
        if (e->sampleCount == 0) {
            e->unit      = rc.unit;         // first sample: stamp the unit from mapping.json
            e->timeSum   = 0.0;             // start a fresh frequency window on re-enable
            e->lastTime  = timeSeconds;
        } else if (e->lastTime >= 0.0) {
            const double dt = timeSeconds - e->lastTime;
            if (dt > 0.0) e->timeSum += dt; // monotonic wall clock — negative deltas are dropped
            e->lastTime   = timeSeconds;
        }

        e->samples.push_back(fv);
        ++e->sampleCount;
    }
}
