//---------------------------------------------------------------------------
// PmrLiveCapture.cpp — PMR live capture over UDP (native C++ / Winsock 2).
//
// Phase 1: pure binary decoders for the three packet types. No socket, no VCL;
// everything here is unit-testable in isolation against a hand-crafted datagram.
//
// Wire conventions (verified byte-for-byte against the authoritative C# sample
// decoder at data/reference/PMR/udpSample/SimpleUDPServer/UDPProtocol.cs):
//   - little-endian primitives, read natively on x86 via memcpy;
//   - every bool / u8 flag occupies exactly ONE byte (tight packing, no padding);
//   - DynamicSizeString = 1-byte length + N bytes UTF-8;
//   - float lists and wheel/gear lists are framed with a leading 1-byte count.
//---------------------------------------------------------------------------

// Winsock 2 must be included BEFORE any windows header in this translation unit.
// This file is compiled standalone by the Gate-1 test (no PCH), and inside the VCL
// project it comes before vcl.h's <windows.h>, so winsock2.h always wins first.
#include <winsock2.h>

#pragma comment(lib, "Ws2_32.lib")   // socket/recvfrom/WSAStartup — verify at link time (plan Step 2.5)

#include <cmath>       // isfinite / abs — feed guards + G-force math
#include <cstddef>     // size_t
#include <cstdio>      // snprintf (dict key formatting for per-wheel channels)
#include <cstring>     // memcpy
#include <limits>      // numeric_limits::quiet_NaN (tread temp fallback)
#include <map>         // per-vehicle participant state (Phase 3 metadata)
#include <unordered_map>   // raw channel dictionary fed to LiveChannel table
#include <utility>     // std::move (commit scratch -> out on full success)

#include "PmrLiveCapture.h"
#include "LiveChannelResolver.h"   // JSON-table-driven channel feed (LIVE_CAPTURE_JSON_MIGRATION_PLAN Phase 3)

namespace pmr_detail {

// ---------------------------------------------------------------------------
// PmrReader — bounds-checked, little-endian cursor over one datagram.
// Every accessor returns false (and leaves `pos` unchanged) on underflow so a
// truncated / malformed packet can never read past the buffer end.
// Native little-endian on x86: memcpy straight into the target type; each call is
// a no-op that returns false if fewer than `bytes` remain in the buffer.
// ---------------------------------------------------------------------------
class PmrReader {
public:
    explicit PmrReader(const uint8_t* data, size_t len) : m_data(data), m_len(len), m_pos(0) {}

    bool   ok()        const { return m_pos <= m_len; }
    size_t remaining() const { return (m_pos <= m_len) ? (m_len - m_pos) : 0; }

    bool readU8(uint8_t& out)   { return take(1, &out); }
    bool readI32(int32_t& out)  { int32_t v;      if (!take(4, &v)) return false; out = v; return true; }
    bool readU32(uint32_t& out) { uint32_t v;     if (!take(4, &v)) return false; out = v; return true; }
    bool readF32(float& out)    { float f;        if (!take(4, &f)) return false; out = f; return true; }
    bool readU16(uint16_t& out) { uint16_t v;     if (!take(2, &v)) return false; out = v; return true; }

    // DynamicSizeString: u8 length + N bytes UTF-8. Empty string on underflow.
    bool readString(std::string& out) {
        uint8_t n = 0;
        if (!readU8(n)) return false;
        if (remaining() < static_cast<size_t>(n)) return false;
        out.assign(reinterpret_cast<const char*>(m_data + m_pos), n);
        m_pos += n;
        return true;
    }

private:
    bool take(size_t bytes, void* dst) {
        if (remaining() < bytes) return false;
        std::memcpy(dst, m_data + m_pos, bytes);
        m_pos += bytes;
        return true;
    }

    const uint8_t* m_data;
    size_t         m_len;
    size_t         m_pos;
};

// Fixed-size block helpers (all fields are 4-byte aligned floats / ints).
static bool readVec3(PmrReader& r, PmrVec3& v) { return r.readF32(v.x) && r.readF32(v.y) && r.readF32(v.z); }

// Quat wire order is x,y,z,w (per the C# sample reader).
static bool readQuat(PmrReader& r, PmrQuat& q) { return r.readF32(q.q[0]) && r.readF32(q.q[1]) && r.readF32(q.q[2]) && r.readF32(q.q[3]); }

// UDPFloatList: u8 count + N floats.
static bool readFloatList(PmrReader& r, std::vector<float>& out) {
    uint8_t n = 0;
    if (!r.readU8(n)) return false;
    out.clear();
    for (uint8_t i = 0; i < n; ++i) { float f=0.f; if (!r.readF32(f)) return false; out.push_back(f); }
    return true;
}

// Shared header: u8 type + u16 version (little-endian). Both must match.
static bool readHeader(PmrReader& r, uint8_t expectedType, uint16_t expectedVersion) {
    uint8_t  t = 0;
    uint16_t v = 0;
    if (!r.readU8(t))   return false;
    if (t != expectedType) return false;
    if (!r.readU16(v))  return false;
    return v == expectedVersion;
}

// ---------------------------------------------------------------------------
// Per-block decoders. Each returns false on any underflow, leaving the fields it
// has not yet written in their default state (callers decode into a scratch object).
// ---------------------------------------------------------------------------
static bool decodeWheel(PmrReader& r, PmrWheelTelemetry& w) {
    if (!r.readI32(w.contactMaterialHash)) return false;
    if (!r.readF32(w.angVel))              return false;
    if (!r.readF32(w.linearSpeed))         return false;
    if (!readVec3(r, w.slideLS))           return false;
    if (!readVec3(r, w.forceLS))           return false;
    if (!readVec3(r, w.momentLS))          return false;
    if (!r.readF32(w.contactRadius))       return false;
    if (!r.readF32(w.pressure))            return false;   // bar (convert to PSI at feed time)
    if (!r.readF32(w.inclination))         return false;
    if (!r.readF32(w.slipRatio))           return false;
    if (!r.readF32(w.slipAngle))           return false;   // radians (VERIFY on first capture)
    if (!readVec3(r, w.tread))             return false;   // [inner|middle..] order applied at feed time
    if (!r.readF32(w.carcass))             return false;
    if (!r.readF32(w.internalAir))         return false;
    if (!r.readF32(w.wellAir))             return false;
    if (!r.readF32(w.rim))                 return false;
    if (!r.readF32(w.brake))               return false;   // brake temp in C
    if (!r.readF32(w.springStrain))        return false;
    if (!r.readF32(w.damperVelocity))      return false;   // direct shaft velocity (m/s)
    if (!r.readF32(w.hubTorque))           return false;
    if (!r.readF32(w.hubPower))            return false;
    if (!r.readF32(w.wheelTorque))         return false;
    if (!r.readF32(w.wheelPower))          return false;
    return true;
}

static bool decodeChassis(PmrReader& r, PmrChassisTelemetry& c) {
    if (!readVec3(r, c.posWS))             return false;
    if (!readQuat(r, c.quat))              return false;   // sits between posWS and angularVelocityWS on the wire
    if (!readVec3(r, c.angularVelocityWS)) return false;
    if (!readVec3(r, c.angularVelocityLS)) return false;
    if (!readVec3(r, c.velocityWS))        return false;
    if (!readVec3(r, c.velocityLS))        return false;
    if (!readVec3(r, c.accelerationWS))    return false;   // world space (m/s^2) — convert to g at feed time
    if (!readVec3(r, c.accelerationLS))    return false;   // local space: x=lateral, z=longitudinal (VERIFY on first capture)
    if (!r.readF32(c.overallSpeed))        return false;   // m/s — convert to km/h at feed time
    if (!r.readF32(c.forwardSpeed))        return false;
    if (!r.readF32(c.sideslip))            return false;
    return true;
}

static bool decodeDrivetrain(PmrReader& r, PmrDrivetrainTelemetry& d) {
    float f = 0.f;
    // ICE (13 floats) — engineRPM first.
    if (!r.readF32(f)) return false; d.engineRPM                = f;
    if (!r.readF32(f)) return false; d.engineRevRatio           = f;
    if (!r.readF32(f)) return false; d.engineTorque             = f;
    if (!r.readF32(f)) return false; d.enginePower              = f;
    if (!r.readF32(f)) return false; d.engineLoad               = f;
    if (!r.readF32(f)) return false; d.engineTurboRPM           = f;
    if (!r.readF32(f)) return false; d.engineTurboBoostPressure = f;
    if (!r.readF32(f)) return false; d.fuelRemaining            = f;
    if (!r.readF32(f)) return false; d.fuelUseRate              = f;
    if (!r.readF32(f)) return false; d.engineOilPressure        = f;
    if (!r.readF32(f)) return false; d.engineOilTemperature     = f;   // C (oil temp)
    if (!r.readF32(f)) return false; d.engineCoolantTemperature = f;   // C (water temp)
    if (!r.readF32(f)) return false; d.exhaustGasTemperature    = f;

    // MGU / hybrid (3 floats).
    if (!r.readF32(f)) return false; d.motorRPM         = f;
    if (!r.readF32(f)) return false; d.batteryRemaining = f;
    if (!r.readF32(f)) return false; d.batteryUseRate   = f;

    // Gearbox / drivetrain (12 floats).
    if (!r.readF32(f)) return false; d.transmissionRPM  = f;
    if (!r.readF32(f)) return false; d.gearboxInputRPM  = f;
    if (!r.readF32(f)) return false; d.gearboxOutputRPM = f;
    if (!r.readF32(f)) return false; d.gearboxTorque    = f;
    if (!r.readF32(f)) return false; d.gearboxPower     = f;
    if (!r.readF32(f)) return false; d.gearboxLoadIn    = f;
    if (!r.readF32(f)) return false; d.gearboxLoadOut   = f;
    if (!r.readF32(f)) return false; d.timeSinceShift   = f;
    if (!r.readF32(f)) return false; d.estDrivenSpeed   = f;
    if (!r.readF32(f)) return false; d.outputTorque     = f;
    if (!r.readF32(f)) return false; d.outputPower      = f;
    if (!r.readF32(f)) return false; d.outputEfficiency = f;

    // Seven single-byte flags (tight packed, one byte each).
    uint8_t b = 0;
    if (!r.readU8(b)) return false; d.starterActive         = b ? true : false;
    if (!r.readU8(b)) return false; d.engineRunning         = b ? true : false;
    if (!r.readU8(b)) return false; d.engineFanRunning      = b ? true : false;
    if (!r.readU8(b)) return false; d.revLimiterActive      = b ? true : false;
    if (!r.readU8(b)) return false; d.tractionControlActive = b ? true : false;
    if (!r.readU8(b)) return false; d.speedLimiterEnabled   = b ? true : false;
    if (!r.readU8(b)) return false; d.speedLimiterActive    = b ? true : false;

    // Gear list: u8 count + N x {upshiftRPM, downshiftRPM}.
    uint8_t nGears = 0;
    if (!r.readU8(nGears)) return false;
    d.gearList.clear();
    for (uint8_t i = 0; i < nGears; ++i) {
        PmrGearTelemetry g{};
        float up=0.f, down=0.f;
        if (!r.readF32(up))   return false;
        if (!r.readF32(down)) return false;
        g.upshiftRPM = up; g.downshiftRPM = down;
        d.gearList.push_back(g);
    }
    return true;
}

static bool decodeSuspension(PmrReader& r, PmrSuspensionTelemetry& s) {
    if (!readFloatList(r, s.avgLoads))  return false;   // u8 count + N floats (per-corner average loads)
    if (!r.readF32(s.loadBias))         return false;
    return true;
}

static bool decodeInput(PmrReader& r, PmrInputTelemetry& i) {
    float f = 0.f;
    int32_t g = 0;
    if (!r.readF32(f))   return false; i.steering    = f;      // radians (VERIFY on first capture); convert to deg at feed time
    if (!r.readF32(f))   return false; i.accelerator = f;      // normalized 0..1
    if (!r.readF32(f))   return false; i.brake       = f;      // normalized 0..1
    if (!r.readF32(f))   return false; i.clutch      = f;
    if (!r.readF32(f))   return false; i.handbrake   = f;
    if (!r.readI32(g))   return false; i.gear        = g;      // -1 reverse? VERIFY on first capture
    return true;
}

static bool decodeSetup(PmrReader& r, PmrSetupTelemetry& s) {
    float f = 0.f;
    uint8_t b = 0;
    if (!r.readF32(f))   return false; s.brakeBias              = f;
    if (!r.readF32(f))   return false; s.frontAntiRollStiffness = f;
    if (!r.readF32(f))   return false; s.rearAntiRollStiffness  = f;
    if (!r.readF32(f))   return false; s.regenLimit             = f;
    if (!r.readF32(f))   return false; s.deployLimit            = f;
    if (!r.readU8(b))    return false; s.absLevel               = b;  // one byte each (tight packed)
    if (!r.readU8(b))    return false; s.tcsLevel               = b;
    return true;
}

static bool decodeGeneral(PmrReader& r, PmrGeneralTelemetry& g) {
    float f = 0.f;
    uint8_t b = 0;
    if (!readVec3(r, g.centerOfGravity))   return false;
    if (!r.readF32(f))                     return false; g.steeringWheelAngle     = f; // rad vs deg VERIFY on first capture
    if (!r.readF32(f))                     return false; g.totalMass              = f;
    if (!r.readF32(f))                     return false; g.drivenWheelAngVel      = f;
    if (!r.readF32(f))                     return false; g.nonDrivenWheelAngVel   = f;
    if (!r.readF32(f))                     return false; g.estRollingSpeed        = f;
    if (!r.readF32(f))                     return false; g.estLinearSpeed         = f;
    if (!r.readF32(f))                     return false; g.totalBrakeForce        = f;
    if (!r.readU8(b))                      return false; g.absActive              = b ? true : false; // last field before constant block
    return true;
}

static bool decodeConstant(PmrReader& r, PmrConstantTelemetry& c) {
    float f = 0.f;
    uint8_t b = 0;
    if (!readVec3(r, c.chassisBBMin))      return false;
    if (!readVec3(r, c.chassisBBMax))      return false;
    if (!r.readF32(f))                     return false; c.starterIdleRPM       = f;
    if (!r.readF32(f))                     return false; c.engineTorquePeakRPM  = f;
    if (!r.readF32(f))                     return false; c.enginePowerPeakRPM   = f;
    if (!r.readF32(f))                     return false; c.engineMaxRPM         = f;
    if (!r.readF32(f))                     return false; c.engineMaxTorque      = f;
    if (!r.readF32(f))                     return false; c.engineMaxPower       = f;
    if (!r.readF32(f))                     return false; c.engineMaxBoost       = f;
    if (!r.readF32(f))                     return false; c.fuelCapacity         = f;
    if (!r.readF32(f))                     return false; c.batteryCapacity      = f;
    if (!r.readF32(f))                     return false; c.trackWidthFront      = f;
    if (!r.readF32(f))                     return false; c.trackWidthRear       = f;
    if (!r.readF32(f))                     return false; c.wheelbase            = f;
    if (!r.readU8(b))                      return false; c.numberOfWheels        = b; // four trailing bytes (tight packed)
    if (!r.readU8(b))                      return false; c.numberOfForwardGears  = b;
    if (!r.readU8(b))                      return false; c.numberOfReverseGears  = b;
    if (!r.readU8(b))                      return false; c.isHybrid              = b ? true : false; // last field of the whole packet
    return true;
}

// Tread ordering flag: default true = tread[0]=Inner, [1]=Middle/Centre, [2]=Outer.
static bool s_treadInnerFirst = true;

} // namespace pmr_detail


//---------------------------------------------------------------------------
// Public API — Phase 1 (pure decode)
//---------------------------------------------------------------------------

void PmrSetTreadOrder(bool innerFirst) { pmr_detail::s_treadInnerFirst = innerFirst; }
bool PmrGetTreadInnerFirst()           { return pmr_detail::s_treadInnerFirst; }


// ---------------------------------------------------------------------------
// RaceInfo (type=0, version=1): six strings + five floats + three trailing bytes.
// Decodes into a scratch object and commits to `out` only on full success so a
// malformed packet never leaves `out` half-populated.
// ---------------------------------------------------------------------------
bool PmrDecodeRaceInfo(const uint8_t* data, size_t len, PmrRaceInfoPacket& out) {
    if (!data || len < 3) return false;

    pmr_detail::PmrReader r(data, len);
    if (!pmr_detail::readHeader(r, PMR_PACKET_RACE_INFO, kPmrRaceInfoVersion)) return false;

    PmrRaceInfoPacket tmp{};   // scratch so `out` is untouched on failure

    if (!r.readString(tmp.track)            ) return false;
    if (!r.readString(tmp.layout)           ) return false;
    if (!r.readString(tmp.season)           ) return false;
    if (!r.readString(tmp.weather)          ) return false;
    if (!r.readString(tmp.session)          ) return false;
    if (!r.readString(tmp.gameMode)         ) return false;

    // Five floats in wire order.
    if (!r.readF32(tmp.layoutLength)        ) return false;
    if (!r.readF32(tmp.duration)            ) return false;
    if (!r.readF32(tmp.overtime)            ) return false;
    if (!r.readF32(tmp.ambientTemperature)  ) return false;
    if (!r.readF32(tmp.trackTemperature)    ) return false;

    // Three trailing single-byte fields (tight packed, one byte each).
    if (!r.readU8(tmp.isLaps)               ) return false;
    if (!r.readU8(tmp.state)                ) return false;
    if (!r.readU8(tmp.numParticipants)      ) return false;

    out = std::move(tmp);   // commit only on full success
    return true;
}


// ---------------------------------------------------------------------------
// ParticipantState (type=1, version=1): vehicleId(i32), isPlayer(u8), four names,
// two ints, three floats, one int sector, two float lists, three status bytes, u32 flags.
// NOTE: per the authoritative C# decoder, vehicleId precedes isPlayer on the wire.
// ---------------------------------------------------------------------------
bool PmrDecodeParticipantState(const uint8_t* data, size_t len, PmrParticipantStatePacket& out) {
    if (!data || len < 3) return false;

    pmr_detail::PmrReader r(data, len);
    if (!pmr_detail::readHeader(r, PMR_PACKET_PARTICIPANT_STATE, kPmrParticipantStateVersion)) return false;

    PmrParticipantStatePacket tmp{};   // scratch so `out` is untouched on failure

    int32_t id = -1;
    uint8_t  b = 0;
    if (!r.readI32(id)                  ) return false;  tmp.vehicleId = id;      // FIRST after header+version (per C# decoder: id then isPlayer)
    if (!r.readU8(b)                    ) return false;  tmp.isPlayer  = b ? true : false;   // SECOND byte on the wire

    // Four dynamic strings in wire order.
    if (!r.readString(tmp.vehicleName)  ) return false;
    if (!r.readString(tmp.driverName)   ) return false;
    if (!r.readString(tmp.liveryId)     ) return false;
    if (!r.readString(tmp.vehicleClass) ) return false;

    // Two ints, three floats.
    int32_t pos = 0, lap = 0;
    float f = 0.f;
    if (!r.readI32(pos)                 ) return false;  tmp.racePos      = pos;
    if (!r.readI32(lap)                 ) return false;  tmp.currentLap   = lap;
    if (!r.readF32(f)                   ) return false;  tmp.currentLapTime = f;
    if (!r.readF32(f)                   ) return false;  tmp.bestLapTime      = f;
    if (!r.readF32(f)                   ) return false;  tmp.lapProgress      = f;

    // One int (lap-detection key), two UDPFloatLists.
    int32_t sector = -1;
    if (!r.readI32(sector)              ) return false;  tmp.currentSector   = sector;
    if (!pmr_detail::readFloatList(r, tmp.currentSectorTimes)) return false;
    if (!pmr_detail::readFloatList(r, tmp.bestSectorTimes   )) return false;

    // Three trailing status bytes (tight packed) + u32 flag bitmask.
    uint32_t flags = 0;
    if (!r.readU8(b)                    ) return false;  tmp.inPits          = b ? true : false;
    if (!r.readU8(b)                    ) return false;  tmp.sessionFinished = b ? true : false;
    if (!r.readU8(b)                    ) return false;  tmp.dq              = b ? true : false;
    if (!r.readU32(flags)               ) return false;  tmp.flags           = flags;

    out = std::move(tmp);   // commit only on full success
    return true;
}


// ---------------------------------------------------------------------------
// VehicleTelemetry (type=2, version=2): vehicleId(i32), wheel list, then fixed blocks.
// Decodes into a scratch object and commits to `out` only on full success so a
// malformed packet never leaves `out` half-populated.
// ---------------------------------------------------------------------------
bool PmrDecodeVehicleTelemetry(const uint8_t* data, size_t len, PmrVehicleTelemetryPacket& out) {
    if (!data || len < 7) return false;

    pmr_detail::PmrReader r(data, len);
    if (!pmr_detail::readHeader(r, PMR_PACKET_VEHICLE_TELEMETRY, kPmrTelemetryVersion)) return false;

    PmrVehicleTelemetryPacket tmp{};   // scratch so `out` is untouched on failure

    int32_t id = -1;
    if (!r.readI32(id)                  ) return false;  tmp.vehicleId = id;      // after header+version

    // Wheel list: u8 count + N x wheel block (expect 4: LF RF LR RR — VERIFY order on first capture).
    uint8_t nWheels = 0;
    if (!r.readU8(nWheels)              ) return false;
    tmp.wheels.clear();
    for (uint8_t i = 0; i < nWheels; ++i) {
        PmrWheelTelemetry w{};
        if (!pmr_detail::decodeWheel(r, w)) return false;
        tmp.wheels.push_back(std::move(w));
    }

    // Fixed blocks in wire order. Each returns false on any underflow so `out` stays untouched.
    if (!pmr_detail::decodeChassis   (r, tmp.chassis)     ) return false;
    if (!pmr_detail::decodeDrivetrain(r, tmp.drivetrain)  ) return false;
    if (!pmr_detail::decodeSuspension(r, tmp.suspension)  ) return false;
    if (!pmr_detail::decodeInput     (r, tmp.input)       ) return false;
    if (!pmr_detail::decodeSetup     (r, tmp.setup)       ) return false;
    if (!pmr_detail::decodeGeneral   (r, tmp.general)     ) return false;
    if (!pmr_detail::decodeConstant  (r, tmp.constant)    ) return false;

    out = std::move(tmp);   // commit only on full success
    return true;
}


// ===========================================================================
// Phase 3 — feed: convert one decoded telemetry frame into LapStats.
// Mirrors AcsEvoFeedLap() field-for-field so the report pipeline sees PMR data
// exactly like AMS2 / AC EVO (plan Step 3.1–3.7). Unit conversions follow plan
// Appendix A; items marked VERIFY are assumptions to confirm on first real capture.
// ===========================================================================

namespace pmr_feed {

// Unit conversions that stay in C++ (data-quality decisions + full-precision math):
//   - isfinite guards on throttle / brake pedal / steer angle / yaw rate / G-forces;
//   - rad->deg and m/s^2 -> g use FULL-PRECISION constants so live values are bit-identical
//     to the pre-migration code (the JSON rows for these channels carry factor 1.0);
//   - slip ratio x100, damper velocity x1000, spring strain x1000 arrive as raw dict values;
//   - fuel is only emitted when capacity > 0 and remaining is finite (dict-skip == old skip).
// Speed / wheel speed (x3.6) and tyre pressure bar->psi (x14.5038) are applied by the table,
// so they arrive here as RAW m/s and bar values respectively.

static const double kDegPerRad = 180.0 / 3.14159265358979323846;   // rad -> deg (full precision)
static const double kMToMm     = 1000.0;                          // m -> mm (spring strain, damper velocity)
static const double kGravity   = 9.81;                            // m/s^2 — G-force normalization

} // namespace pmr_feed


std::string PmrGetChannelTableWarning();   // defined below
// Resolved channel table for PMR LIVE rows (mapping.json), built ONCE on first use.
// Unit conversions live in mapping.json, not here: this function only extracts RAW values
// from the decoded telemetry frame and lets FeedLapFromTable() apply factor/offset per row.
std::string g_pmrChannelTableWarning;
static LiveChannel::ResolvedChannelTable& PmrGetChannelTable()
{
    static LiveChannel::ResolvedChannelTable table = [] {
        LiveChannel::ResolvedChannelTable t;
        t.build(ChannelProfile::Pmr);   // PMR + Generic rows (PMR damper dual-write attached here)
        return t;
    }();
    // One-time validation: surface (never fatal) any mapping row that failed to resolve.
    static bool validated = false;
    if (!validated) {
        std::string report;
        const std::vector<std::string> required = [] {
            std::vector<std::string> v;
            v.push_back("chassis.overallSpeed");
            v.push_back("drivetrain.engineRPM");
            v.push_back("input.acceleratorPct");
            v.push_back("input.brakePedalPct");
            v.push_back("general.steeringWheelAngleDeg");
            v.push_back("chassis.angularVelocityLS.yDps");
            v.push_back("chassis.accelerationLS.xG");
            v.push_back("chassis.accelerationLS.zG");
            for (int i = 0; i < 4; ++i) {   // wheel order: LF=0, RF=1, LR=2, RR=3
                char key[64];
                snprintf(key, sizeof(key), "wheels[%d].linearSpeed",       i); v.push_back(key);
                snprintf(key, sizeof(key), "wheels[%d].brakeTempC",        i); v.push_back(key);
                snprintf(key, sizeof(key), "wheels[%d].pressureBar",       i); v.push_back(key);
                snprintf(key, sizeof(key), "wheels[%d].treadInnerC",       i); v.push_back(key);
                snprintf(key, sizeof(key), "wheels[%d].treadMiddleC",      i); v.push_back(key);
                snprintf(key, sizeof(key), "wheels[%d].treadOuterC",       i); v.push_back(key);
                snprintf(key, sizeof(key), "wheels[%d].slipRatioPct",      i); v.push_back(key);
                snprintf(key, sizeof(key), "wheels[%d].damperVelocityMmS", i); v.push_back(key);
                snprintf(key, sizeof(key), "wheels[%d].springStrainMm",    i); v.push_back(key);
            }
            v.push_back("setup.brakeBiasPct");
            v.push_back("drivetrain.tractionControlActivePct");
            v.push_back("general.absActivePct");
            v.push_back("drivetrain.engineOilTemperatureC");
            v.push_back("drivetrain.engineCoolantTemperatureC");
            // fuelLevelPct is conditional (only emitted when capacity > 0) — not required.
            return v;
        }();
        table.validate(required, report);
        g_pmrChannelTableWarning = report.empty() ? "" : "PMR live channel table: " + report;
        validated = true;
    }
    return table;
}


void PmrFeedLap(LapStats& lap, const PmrVehicleTelemetryPacket& pkt, double timeSeconds) {
    using namespace pmr_feed;

    if (pkt.wheels.size() != 4) return;   // defensive: feed expects LF RF LR RR in that order — VERIFY on first capture

    LiveChannel::ResolvedChannelTable& table = PmrGetChannelTable();

    const PmrWheelTelemetry* wheels[4] = { &pkt.wheels[0], &pkt.wheels[1], &pkt.wheels[2], &pkt.wheels[3] };   // LF RF LR RR

    lap.rideHeightUnit = RideHeightUnit::Millimeters;   // PMR has no ride-height channel — unit is moot, set for consistency

    // ---- Guarded / pre-computed channels (full-precision constants in C++) ---
    const double throttlePct   = std::isfinite(static_cast<double>(pkt.input.accelerator)) ? pkt.input.accelerator * 100.0 : 0.0;
    const double brakePedalPct = std::isfinite(static_cast<double>(pkt.input.brake))            ? pkt.input.brake        * 100.0 : 0.0;

    // Steering: general.steeringWheelAngle — VERIFY radians vs degrees on first capture (plan default: rad).
    const double steerDeg = std::isfinite(static_cast<double>(pkt.general.steeringWheelAngle)) ? pkt.general.steeringWheelAngle * kDegPerRad : 0.0;

    // Yaw rate: local-space angular velocity, y axis — VERIFY sign convention on first capture (plan default).
    const double yawRateDps = std::isfinite(pkt.chassis.angularVelocityLS.y) ? pkt.chassis.angularVelocityLS.y * kDegPerRad : 0.0;

    // G-forces: local-space acceleration x=lateral, z=longitudinal — VERIFY axis convention on first capture (plan default).
    const double latG  = std::isfinite(pkt.chassis.accelerationLS.x) ? pkt.chassis.accelerationLS.x / kGravity : 0.0;
    const double longG = std::isfinite(pkt.chassis.accelerationLS.z) ? pkt.chassis.accelerationLS.z / kGravity : 0.0;

    // ---- Tread temps: resolve inner/middle/outer per PmrGetTreadInnerFirst() ---
    const bool innerFirst = PmrGetTreadInnerFirst();   // default true: [inner=0, middle=1, outer=2] — VERIFY on first capture (plan Step 3.3)

    auto treadTemp = [](const PmrWheelTelemetry& w, int shoulder /*0=inner 1=middle 2=outer*/) -> double {
        const float* t = &w.tread.x;   // x,y,z == [0],[1],[2] on the wire
        if (shoulder < 3) return static_cast<double>(t[shoulder]);
        return std::numeric_limits<double>::quiet_NaN();
    };

    const int iInner = innerFirst ? 0 : 2;   // shoulder index for "inner"
    const int iOuter = innerFirst ? 2 : 0;   // shoulder index for "outer"

    // ---- Raw channel dictionary (RAW values only - no unit conversion here). ---
    // Keys MUST match the "channel" field of mapping.json PMR LIVE rows EXACTLY.
    std::unordered_map<std::string, double> raw;
    raw["chassis.overallSpeed"]            = pkt.chassis.overallSpeed;                 // m/s -> km/h x3.6 via table (Vehicle_Speed)
    raw["drivetrain.engineRPM"]            = pkt.drivetrain.engineRPM;                  // RPM direct
    raw["input.acceleratorPct"]            = throttlePct;                               // pre-computed % (f=1, isfinite-guarded in C++)
    raw["input.brakePedalPct"]             = brakePedalPct;                             // pre-computed % (f=1)
    raw["general.steeringWheelAngleDeg"]   = steerDeg;                                  // pre-computed deg (f=1, full-precision rad->deg in C++)
    raw["chassis.angularVelocityLS.yDps"]  = yawRateDps;                                // pre-computed deg/s (f=1)
    raw["chassis.accelerationLS.xG"]       = latG;                                      // pre-computed g (f=1, /9.81 in C++)
    raw["chassis.accelerationLS.zG"]       = longG;                                     // pre-computed g (f=1)

    for (int i = 0; i < 4; ++i) {   // wheel order: LF=0, RF=1, LR=2, RR=3
        char key[64];
        snprintf(key, sizeof(key), "wheels[%d].linearSpeed",       i); raw[key] = wheels[i]->linearSpeed;           // m/s -> km/h x3.6 via table (Wheel_Speed_*)
        snprintf(key, sizeof(key), "wheels[%d].brakeTempC",        i); raw[key] = wheels[i]->brake;                  // C direct (Brake_Temp_*)
        snprintf(key, sizeof(key), "wheels[%d].pressureBar",       i); raw[key] = wheels[i]->pressure;               // bar -> psi x14.5038 via table (Tyre_Pressure_*)
        snprintf(key, sizeof(key), "wheels[%d].treadInnerC",       i); raw[key] = treadTemp(*wheels[i], iInner);     // C inner  (Tyre_Temp_*_Inner)
        snprintf(key, sizeof(key), "wheels[%d].treadMiddleC",      i); raw[key] = treadTemp(*wheels[i], 1);          // C middle (Tyre_Temp_*_Middle + Surf_Temp_*)
        snprintf(key, sizeof(key), "wheels[%d].treadOuterC",       i); raw[key] = treadTemp(*wheels[i], iOuter);     // C outer  (Tyre_Temp_*_Outer)
        snprintf(key, sizeof(key), "wheels[%d].slipRatioPct",      i); raw[key] = wheels[i]->slipRatio * 100.0;      // ratio -> % x100 in C++ (Wheel_Slip_Ratio_*)
        snprintf(key, sizeof(key), "wheels[%d].damperVelocityMmS", i); raw[key] = wheels[i]->damperVelocity * kMToMm;// m/s -> mm/s x1000 in C++ (Damper_Velocity_* histograms)
        snprintf(key, sizeof(key), "wheels[%d].springStrainMm",    i); raw[key] = wheels[i]->springStrain   * kMToMm;// m -> mm x1000 in C++ (Suspension_Travel_*, + damper dual-write)
    }

    // Brake bias: normalized 0..1 ratio, default 50/50 when non-finite. Pre-computed % so the f=1 row lands in percent.
    raw["setup.brakeBiasPct"] = std::isfinite(static_cast<double>(pkt.setup.brakeBias)) ? pkt.setup.brakeBias * 100.0 : 50.0;

    // TCS / ABS: on/off flags -> % (matches EVO TC/ABS channel semantics).
    raw["drivetrain.tractionControlActivePct"] = pkt.drivetrain.tractionControlActive != 0 ? 100.0 : 0.0;   // pre-computed % (TCS_Active, f=1)
    raw["general.absActivePct"]                = pkt.general.absActive                 != 0 ? 100.0 : 0.0;   // pre-computed % (ABS_Active, f=1)

    // Thermal: C direct.
    raw["drivetrain.engineOilTemperatureC"]     = pkt.drivetrain.engineOilTemperature;      // C (oil temp lives on the drivetrain block)
    raw["drivetrain.engineCoolantTemperatureC"] = pkt.drivetrain.engineCoolantTemperature;  // C

    // Fuel level: remaining / capacity -> %. Capacity comes from the constant block, which is sent with every frame.
    // Emitted ONLY when valid — a missing key makes FeedLapFromTable() skip it, matching old per-frame guard exactly.
    const double fuelCap = pkt.constant.fuelCapacity;                 // litres (VERIFY on first capture)
    if (fuelCap > 0.0 && std::isfinite(static_cast<double>(pkt.drivetrain.fuelRemaining))) {
        raw["fuelLevelPct"] = static_cast<double>(pkt.drivetrain.fuelRemaining) / fuelCap * 100.0;   // % - pre-computed (f=1, conditional key)
    }

    // trackTemp / ambientTemp: PMR sends these only in RaceInfo (session start), not per frame — left empty by design (plan Step 3.5).
    // aeroHeight*: no ride-height channel on the wire -> Rake virtual channel won't resolve for PMR laps; acceptable degradation (plan Step 3.7).

    // ---- Generic table-driven feed: v = raw * factor + offset -> LapStats sink. ---
    LiveChannel::FeedLapFromTable(lap, raw, table, timeSeconds);

    // Suspension travel dual-write into the damper corner stats is handled by the resolver's
    // PMR-specific DamperSampleSink (attached to the springStrainMm rows at build()), so no
    // post-table addSample() calls are needed here — unlike AMS2 / AC EVO.

    // ---- Handling evidence block --------------------------------------------
    const double trLF = wheels[0]->springStrain * kMToMm;   // front-left travel, mm (for asymmetry only)
    const double trRF = wheels[1]->springStrain * kMToMm;   // front-right travel, mm
    const double suspAsymmetry = (trLF - trRF) * 0.5;       // front axle travel asymmetry, mm — mirrors AcsEvoFeedLap()

    const double speedKmh = static_cast<double>(pkt.chassis.overallSpeed) * 3.6;   // m/s -> km/h (same conversion as the table row above)
    lap.addHandlingSample(steerDeg, yawRateDps, latG, longG, brakePedalPct, throttlePct, suspAsymmetry, speedKmh);
    lap.addSteerYaw(steerDeg, yawRateDps);
    ++lap.rowCount;
}


std::string PmrGetChannelTableWarning() { return g_pmrChannelTableWarning; }   // Phase 3 accessor (declared in header)

// ===========================================================================
// Phase 2 — socket lifecycle + non-blocking receive (Winsock 2, loopback UDP).
// Runs on the VCL main-thread timer: must NEVER block. All state is file-static,
// matching the AMS2 / AC EVO module pattern. Single-socket design; re-opening a
// new port closes any previous socket first.
// ===========================================================================

namespace pmr_net {

static SOCKET  s_sock = INVALID_SOCKET;   // bound UDP receive socket (INVALID when closed)
static bool    s_wsaUp = false;           // WSAStartup done / WSACleanup pending pair guard
static PmrVehicleTelemetryPacket s_lastTelemetry{};
static bool    s_haveLastTelemetry = false;

// Session metadata, populated as a side-effect of decoding (plan Step 3.8). The
// accessors at the bottom of this file read these directly — kept here so the
// decode path has one obvious home for "last seen" state from packet type 0/1.
static PmrRaceInfoPacket s_lastRaceInfo{};

// Per-vehicle participant state, keyed by vehicleId with update-in-place semantics
// (mirrors DataStore.cs in the C# reference). Cleared on RaceInfo receipt — a new
// session starts fresh and stale lap/sector data from the previous one must not leak.
static std::map<int32_t, PmrParticipantStatePacket> s_participants;

// Player vehicle id: set when a PARTICIPANT_STATE packet with isPlayer==true arrives.
// Reset to -1 on RaceInfo (new session) so stale ids from the previous run don't leak.
static int s_playerVehicleId = -1;

// One-shot WSAStartup with a paired WSACleanup guard. Returns true on success or if
// the stack is already up for this process lifetime (WSA ref-counts per call, so we
// deliberately do NOT pair each open/close — see shutdownWsa() note).
bool ensureWsa(std::string& err) {
    if (s_wsaUp) return true;
    WSADATA wsa{};
    const int rc = WSAStartup(MAKEWORD(2, 2), &wsa);
    if (rc != 0) {
        err = "WSAStartup failed with error code ";
        err += std::to_string(rc);
        return false;
    }
    s_wsaUp = true;
    return true;
}

void shutdownWsa() {
    // Deliberately NOT calling WSACleanup here: the VCL app process outlives any single
    // capture session, and re-opening after cleanup would require a second WSAStartup.
    // The OS cleans up at process exit (we hold at most one socket per port). Kept as an
    // explicit hook in case that policy changes later — plan Step 2.1 asks for the pair.
}

void closeSocket() {
    if (s_sock != INVALID_SOCKET) {
        closesocket(s_sock);
        s_sock = INVALID_SOCKET;
    }
    shutdownWsa();   // no-op today, see above — documents the intended pairing point
}

// Receive one datagram without blocking. Returns false immediately when nothing is
// pending (WSAEWOULDBLOCK) or on any socket error — never sleeps, never retries.
bool recvOne(uint8_t* buf, size_t cap, int& outLen) {
    if (s_sock == INVALID_SOCKET) return false;

    // UDP datagrams are at most 65507 payload bytes; PMR telemetry is ~1 KB so a fixed
    // buffer with headroom covers every realistic frame. An oversized datagram would be
    // dropped by the OS anyway under SOCK_DGRAM semantics (no partial delivery).
    static uint8_t s_buf[2048];   // file-static: avoids 2 KB of stack per timer tick

    const int n = recvfrom(s_sock, reinterpret_cast<char*>(s_buf),
                           static_cast<int>(cap > sizeof(s_buf) ? sizeof(s_buf) : cap),
                           0, nullptr, nullptr);
    if (n <= 0) return false;     // WSAEWOULDBLOCK / socket closed — no data this tick

    std::memcpy(buf, s_buf, static_cast<size_t>(n));
    outLen = n;
    return true;
}

} // namespace pmr_net


//---------------------------------------------------------------------------
// Public API — Phase 2 (socket lifecycle + non-blocking receive)
//---------------------------------------------------------------------------

// Overload without explicit address — binds to INADDR_ANY (all interfaces).
bool PmrOpenSocket(int port, std::string& err) {
    return PmrOpenSocket(std::string(), port, err);
}

// Full overload: bind to a specific IP or all interfaces.
bool PmrOpenSocket(const std::string& bindAddress, int port, std::string& err) {
    using namespace pmr_net;

    closeSocket();   // idempotent: re-opening on a new address/port drops the old binding first

    if (!ensureWsa(err)) return false;

    s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_sock == INVALID_SOCKET) {
        err = "socket() failed with error code ";
        err += std::to_string(WSAGetLastError());
        return false;
    }

    // Non-blocking: recvfrom returns WSAEWOULDBLOCK immediately when no datagram is
    // pending — the core guarantee that keeps the 16 ms VCL timer responsive.
    u_long mode = 1;
    if (ioctlsocket(s_sock, FIONBIO, &mode) != 0) {
        err = "ioctlsocket(FIONBIO) failed with error code ";
        err += std::to_string(WSAGetLastError());
        closeSocket();
        return false;
    }

    // Resolve bind address: empty string → INADDR_ANY (all interfaces).
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(static_cast<uint16_t>(port));

    if (bindAddress.empty()) {
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
    } else {
        unsigned long ipVal = inet_addr(bindAddress.c_str());
        if (ipVal == INADDR_NONE) {
            err = "Invalid bind address: ";
            err += bindAddress;
            closeSocket();
            return false;
        }
        addr.sin_addr.s_addr = ipVal;   // inet_addr already returns network byte order
    }

    if (bind(s_sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        err = "bind() to ";
        err += bindAddress.empty() ? std::string("*") : bindAddress;
        err += ":" + std::to_string(port);
        err += " failed with error code ";
        err += std::to_string(WSAGetLastError());
        closeSocket();
        return false;
    }

    s_haveLastTelemetry = false;   // fresh session: no stale telemetry from a prior run
    return true;
}

void PmrCloseSocket() { pmr_net::closeSocket(); }

bool PmrIsConnected() { return pmr_net::s_sock != INVALID_SOCKET; }

const PmrVehicleTelemetryPacket* PmrLastTelemetry() {
    if (!pmr_net::s_haveLastTelemetry) return nullptr;
    return &pmr_net::s_lastTelemetry;
}

bool PmrReadPacket(uint8_t& outType) {
    using namespace pmr_net;

    uint8_t buf[2048];   // matches recvOne's internal buffer cap (plan Step 2.3)
    int n = 0;
    if (!recvOne(buf, sizeof(buf), n)) return false;

    outType = buf[0];    // first byte is the packet type on every datagram

    switch (outType) {
        case PMR_PACKET_RACE_INFO: {
            // A RaceInfo marks the start of a new session — reset both cached race info and
            // per-vehicle state so lap/sector numbers from the previous session can't leak.
            s_lastRaceInfo = PmrRaceInfoPacket{};   // reset before decode so a partial failure clears stale data
            if (!PmrDecodeRaceInfo(buf, static_cast<size_t>(n), s_lastRaceInfo)) return false;
            s_participants.clear();                 // mirrors DataStore.cs: fresh session starts empty
            s_playerVehicleId = -1;                  // new session — no player state yet
            break;
        }

        case PMR_PACKET_PARTICIPANT_STATE: {
            PmrParticipantStatePacket ps{};         // update-in-place keyed by vehicleId (plan Step 3.8)
            if (!PmrDecodeParticipantState(buf, static_cast<size_t>(n), ps)) return false;
            s_participants[ps.vehicleId] = std::move(ps);
            if (s_playerVehicleId == -1 && ps.isPlayer != 0)   // first player state seen this session
                s_playerVehicleId = static_cast<int>(ps.vehicleId);
            break;
        }

        case PMR_PACKET_VEHICLE_TELEMETRY: {
            PmrVehicleTelemetryPacket tmp{};
            if (!PmrDecodeVehicleTelemetry(buf, static_cast<size_t>(n), tmp)) return false;
            s_lastTelemetry = std::move(tmp);       // commit-on-success mirrors the decode layer's policy
            s_haveLastTelemetry = true;
            break;
        }

        default:   // PMR_PACKET_SESSION_STOPPED or unknown type — no Phase-1 decoder exists for it.
                   // The datagram WAS received and consumed (no re-read loop); we just don't model it yet.
            return false;
    }
    return true;
}

int PmrDrainAllPackets() {
    int count = 0;
    uint8_t type = 0;
    while (PmrReadPacket(type)) ++count;   // exits immediately on WSAEWOULDBLOCK — bounded by datagrams in flight
    return count;
}

// ===============================
// Phase 3 — session metadata accessors (plan Step 3.8)
// Read the file-static state populated as a side-effect of PmrReadPacket above.
// All are safe to call before any packet has been received: they fall back to
// sensible defaults ("", -1, false). No locking needed — single-threaded VCL pump.
// ===============================

std::string PmrGetTrackName() {
    return pmr_net::s_lastRaceInfo.track;   // "" until the first RaceInfo arrives
}

std::string PmrGetDriverName(int vehicleId) {
    auto it = pmr_net::s_participants.find(static_cast<int32_t>(vehicleId));
    if (it == pmr_net::s_participants.end()) return "";   // unknown id — no state yet for this car
    return it->second.driverName;                          // "" is also the wire default when PMR omits the name field
}

int PmrGetCurrentSector(int vehicleId) {
    auto it = pmr_net::s_participants.find(static_cast<int32_t>(vehicleId));
    if (it == pmr_net::s_participants.end()) return -1;   // unknown id / no state yet — matches the wire default (-1)
    return static_cast<int>(it->second.currentSector);     // 0..N on a valid lap, -1 between laps per C# reference
}

bool PmrIsInPits(int vehicleId) {
    auto it = pmr_net::s_participants.find(static_cast<int32_t>(vehicleId));
    if (it == pmr_net::s_participants.end()) return false; // unknown id — treat as on track, not in pits
    return it->second.inPits != 0;                          // single status byte: non-zero = pit box occupied
}

std::string PmrGetGameMode() {
    return pmr_net::s_lastRaceInfo.gameMode;   // "" until the first RaceInfo arrives (free text, e.g. "Race" / "Time Attack")
}

bool PmrIsSessionFinished(int vehicleId) {
    auto it = pmr_net::s_participants.find(static_cast<int32_t>(vehicleId));
    if (it == pmr_net::s_participants.end()) return false; // unknown id — no state yet for this car
    return it->second.sessionFinished != 0;                // single status byte: non-zero = session over for that participant
}

int PmrGetPlayerId() {
    return pmr_net::s_playerVehicleId;   // -1 until the first isPlayer==true participant state arrives
}


