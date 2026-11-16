#pragma once
// LdEncoder — writes MoTeC-compatible binary .ld files + XML .ldx beacon companions.
// Layout is the inverse of DecodeMotecLd() in MotecToCSV.cpp (the project's own decoder).

#include <string>
#include <vector>
#include <cstdint>

struct LdChannelData {
    std::string name;              // channel label, max 32 chars (NUL-terminated)
    std::string unit;              // unit string, max 8 chars (e.g. "km/h", "%")
    int frequencyHz = 60;          // sample rate hint (>0), stored as I16 in block header
    std::vector<float> values;     // float32 LE samples

    bool valid() const { return !name.empty() && !values.empty(); }
};

struct LdExportOptions {
    std::string driverName;        // max 64 chars, written at file offset 158
    std::string vehicleName;       // max 128 chars, written at file offset 222
    std::string trackName;         // max 128 chars, written at file offset 350
};

// Writes a binary .ld file. Returns true on success.
bool WriteLdFile(const std::string& path, const LdExportOptions& opts,
                 const std::vector<LdChannelData>& channels);

// Writes the companion .ldx XML with beacon markers (wall-clock microseconds at lap transitions).
void WriteLdxBeacons(const std::string& ldxPath, const std::vector<int64_t>& beaconTimesUs);

// Creates all intermediate directories of dirPath if they don't exist. No-op on failure.
void EnsurePathExists(const std::string& dirPath);
