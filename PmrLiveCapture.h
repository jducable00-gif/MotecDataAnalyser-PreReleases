//---------------------------------------------------------------------------
// PmrLiveCapture.h — PMR live capture over UDP (native C++ / Winsock 2).
//
// Third live-capture backend alongside AMS2 and AC EVO. The game emits three
// packet types on a local UDP port (default 7576): RaceInfo, ParticipantRaceState
// and ParticipantVehicleTelemetry. Binary conventions: little-endian primitives;
// dynamic strings = 1-byte length + N bytes UTF-8; float lists / wheel lists are
// framed with a 1-byte count prefix. Authoritative layout reference:
//   data/reference/PMR/udpSample/docs/UDPTelemetryProtocol.h (read-only spec).
//
// The structs below mirror the wire field order exactly so that PmrReader can
// walk them one field at a time; they are decode targets, not raw-memcpy types.
//---------------------------------------------------------------------------

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "MotecSetupAnalyzer.h"   // LapStats (feed target)

// ===============================
// Packet type constants — first byte of every datagram
// ===============================
enum PmrPacketType : unsigned char {
    PMR_PACKET_RACE_INFO = 0,
    PMR_PACKET_PARTICIPANT_STATE = 1,
    PMR_PACKET_VEHICLE_TELEMETRY = 2,
    PMR_PACKET_SESSION_STOPPED = 3
};

// Protocol versions (validated on decode; mismatch -> packet rejected).
static const uint16_t kPmrRaceInfoVersion         = 1;
static const uint16_t kPmrParticipantStateVersion = 1;
static const uint16_t kPmrTelemetryVersion        = 2;

// ===============================
// Primitive blocks (wire order)
// ===============================
struct PmrVec3 { float x, y, z; };                       // 12 bytes
struct PmrQuat { float q[4]; };                          // 16 bytes (x,y,z,w per C# sample reader)

// One wheel of the telemetry packet. Field order == wire order:
// contactMaterialHash | angVel linearSpeed | slideLS forceLS momentLS |
// contactRadius pressure inclination slipRatio slipAngle | tread(3) |
// carcass internalAir wellAir rim brake | springStrain damperVelocity |
// hubTorque hubPower wheelTorque wheelPower.
struct PmrWheelTelemetry {
    int     contactMaterialHash = 0;                     // 4 B
    float   angVel              = 0.f, linearSpeed       = 0.f;      // 8 B
    PmrVec3 slideLS{}, forceLS{}, momentLS{};            // 36 B
    float   contactRadius       = 0.f, pressure          = 0.f,
            inclination         = 0.f, slipRatio         = 0.f,
            slipAngle           = 0.f;                   // 20 B
    PmrVec3 tread{};                                     // 12 B — [inner|middle..] order set by PmrSetTreadOrder() at feed time
    float   carcass             = 0.f, internalAir       = 0.f,
            wellAir             = 0.f, rim               = 0.f,
            brake               = 0.f;                   // 20 B (brake temp in C)
    float   springStrain        = 0.f, damperVelocity    = 0.f;      // 8 B — direct shaft velocity!
    float   hubTorque           = 0.f, hubPower          = 0.f,
            wheelTorque         = 0.f, wheelPower        = 0.f;      // 16 B
};

struct PmrChassisTelemetry {                              // wire order: posWS quat angVelWS angVelLS velWS velLS accWS accLLS speeds
    PmrVec3 posWS{}, angularVelocityWS{}, angularVelocityLS{};
    PmrQuat quat{};                                       // 16 B (between posWS and angularVelocityWS)
    PmrVec3 velocityWS{}, velocityLS{};
    PmrVec3 accelerationWS{}, accelerationLS{};           // m/s^2 in local space: x=lateral, y=vertical?, z=longitudinal — VERIFY on first capture
    float   overallSpeed = 0.f, forwardSpeed = 0.f, sideslip = 0.f;
};

struct PmrGearTelemetry {                                 // per-gear thresholds (8 B each)
    float upshiftRPM = 0.f, downshiftRPM = 0.f;
};

// Drivetrain: ICE(13f) | MGU(3f) | gearbox(12f) | flags(7B) | gear list.
struct PmrDrivetrainTelemetry {
    float engineRPM=0, engineRevRatio=0, engineTorque=0, enginePower=0, engineLoad=0;
    float engineTurboRPM=0, engineTurboBoostPressure=0, fuelRemaining=0, fuelUseRate=0;
    float engineOilPressure=0, engineOilTemperature=0, engineCoolantTemperature=0, exhaustGasTemperature=0;
    float motorRPM=0, batteryRemaining=0, batteryUseRate=0;
    float transmissionRPM=0, gearboxInputRPM=0, gearboxOutputRPM=0, gearboxTorque=0, gearboxPower=0;
    float gearboxLoadIn=0, gearboxLoadOut=0, timeSinceShift=0, estDrivenSpeed=0, outputTorque=0;
    float outputPower=0, outputEfficiency=0;
    uint8_t starterActive=0, engineRunning=0, engineFanRunning=0, revLimiterActive=0;
    uint8_t tractionControlActive=0, speedLimiterEnabled=0, speedLimiterActive=0;
    std::vector<PmrGearTelemetry> gearList;               // 1-byte count + N x PmrGearTelemetry
};

struct PmrSuspensionTelemetry {
    std::vector<float> avgLoads;                          // UDPFloatList: 1-byte len + N floats (per-corner average loads)
    float loadBias = 0.f;                                 // 4 B
};

struct PmrInputTelemetry {                                // steering/accelerator/brake/clutch/handbrake normalized, gear int32
    float   steering=0, accelerator=0, brake=0, clutch=0, handbrake=0;
    int32_t gear = 0;                                     // -1 reverse? VERIFY on first capture
};

struct PmrSetupTelemetry {
    float brakeBias=0, frontAntiRollStiffness=0, rearAntiRollStiffness=0, regenLimit=0, deployLimit=0;
    uint8_t absLevel=0, tcsLevel=0;                       // 2 B (after the five floats)
};

struct PmrGeneralTelemetry {                              // CoG vec3 + 7 floats + absActive byte
    PmrVec3 centerOfGravity{};
    float steeringWheelAngle=0, totalMass=0, drivenWheelAngVel=0, nonDrivenWheelAngVel=0;
    float estRollingSpeed=0, estLinearSpeed=0, totalBrakeForce=0;   // VERIFY rad vs deg on first capture
    uint8_t absActive = 0;                                // 1 B (last field)
};

struct PmrConstantTelemetry {                             // sent with every telemetry packet
    PmrVec3 chassisBBMin{}, chassisBBMax{};               // 24 B
    float starterIdleRPM=0, engineTorquePeakRPM=0, enginePowerPeakRPM=0, engineMaxRPM=0;
    float engineMaxTorque=0, engineMaxPower=0, engineMaxBoost=0, fuelCapacity=0, batteryCapacity=0;
    float trackWidthFront=0, trackWidthRear=0, wheelbase=0;
    uint8_t numberOfWheels=0, numberOfForwardGears=0, numberOfReverseGears=0, isHybrid=0;  // 4 B (last)
};

// ===============================
// Top-level decoded packets
// ===============================
struct PmrVehicleTelemetryPacket {                        // type=2, version=2
    int32_t vehicleId = -1;                               // after header
    std::vector<PmrWheelTelemetry> wheels;                // 1-byte count + N x wheel (expect 4: LF RF LR RR — VERIFY order on first capture)
    PmrChassisTelemetry   chassis{};                      // fixed block, no length prefix
    PmrDrivetrainTelemetry drivetrain{};                  // ends with gear list
    PmrSuspensionTelemetry suspension{};                  // avgLoads float-list + loadBias
    PmrInputTelemetry     input{};                        // 5 floats + int32
    PmrSetupTelemetry     setup{};                        // 5 floats + 2 bytes
    PmrGeneralTelemetry   general{};                      // vec3 + 7 floats + byte
    PmrConstantTelemetry  constant{};                     // last block of the packet
};

struct PmrRaceInfoPacket {                                // type=0, version=1 — sent on session start / track change
    std::string track, layout, season, weather, session, gameMode;   // six dynamic strings (wire order)
    float layoutLength=0, duration=0, overtime=0, ambientTemperature=0, trackTemperature=0;  // five floats
    uint8_t isLaps = 0, state = 0, numParticipants = 0;   // three trailing bytes
};

struct PmrParticipantStatePacket {                        // type=1, version=1 — one per participant (player first)
    int32_t vehicleId = -1;                               // FIRST after header+version (per C# decoder: id then isPlayer)
    uint8_t isPlayer  = 0;                                // SECOND byte on the wire (after vehicleId, before names)
    std::string vehicleName, driverName, liveryId, vehicleClass;     // four dynamic strings (wire order)
    int32_t racePos=0, currentLap=0;                      // two ints
    float   currentLapTime=0, bestLapTime=0, lapProgress=0;          // three floats
    int32_t currentSector = -1;                           // one int (lap detection key)
    std::vector<float> currentSectorTimes, bestSectorTimes;          // two UDPFloatLists
    uint8_t inPits=0, sessionFinished=0, dq=0;            // three trailing status bytes
    uint32_t flags = 0;                                   // flag bitmask (last field)
};

// ===============================
// Phase 1 — pure decode API (no socket, no VCL). Unit-testable in isolation.
// Each function returns false on malformed input or version mismatch without
// touching `out` partially beyond what was safely parsed.
// ===============================
bool PmrDecodeRaceInfo(const uint8_t* data, size_t len, PmrRaceInfoPacket& out);
bool PmrDecodeParticipantState(const uint8_t* data, size_t len, PmrParticipantStatePacket& out);
bool PmrDecodeVehicleTelemetry(const uint8_t* data, size_t len, PmrVehicleTelemetryPacket& out);

// Tread ordering flag: default true = tread[0]=Inner, [1]=Middle/Centre, [2]=Outer.
// Swappable if real PMR output shows a different order (see plan Step 3.3).
void PmrSetTreadOrder(bool innerFirst);
bool PmrGetTreadInnerFirst();

// ===============================
// Phase 2 — socket lifecycle + non-blocking receive (Winsock 2, loopback UDP)
// Runs on the VCL main-thread timer: must never block.
// ===============================
bool PmrOpenSocket(int port, std::string& err);           // WSAStartup + bind INADDR_ANY; false with message in `err` on failure
bool PmrOpenSocket(const std::string& bindAddress, int port, std::string& err);
    // Bind to a specific IP (e.g. "192.168.1.5"). Empty string = INADDR_ANY.
void PmrCloseSocket();                                    // closes socket (idempotent)

// Non-blocking single-packet read: receives one datagram if pending and decodes it.
// Decoded results are cached internally; access via the getters below.
bool PmrReadPacket(uint8_t& outType);                     // true = received AND decoded a packet this call
int  PmrDrainAllPackets();                                // loops until no data left, returns packets processed

const PmrVehicleTelemetryPacket* PmrLastTelemetry();      // last telemetry frame (null if none yet)
bool PmrIsConnected();                                    // socket open and bound

// ===============================
// Phase 3 — feed + session metadata accessors
// ===============================
void PmrFeedLap(LapStats& lap, const PmrVehicleTelemetryPacket& pkt, double timeSeconds);

// Returns a non-empty warning string if any PMR LIVE mapping row failed to resolve at startup
// (empty table or missing raw channel names). Empty = OK. Mirrors AcsEvoGetChannelTableWarning().
std::string PmrGetChannelTableWarning();

std::string PmrGetTrackName();                            // from last RaceInfo packet ("" if none)
std::string PmrGetDriverName(int vehicleId);              // "" for unknown id
int   PmrGetCurrentSector(int vehicleId);                 // -1 for unknown id / no state yet
bool  PmrIsInPits(int vehicleId);                         // false for unknown id
std::string PmrGetGameMode();                             // free-text game mode from last RaceInfo ("" if none) — e.g. "Race" / "Time Attack"; VERIFY exact values on first capture
bool  PmrIsSessionFinished(int vehicleId);                // participant's sessionFinished byte; false for unknown id

// Player vehicle id: set when the first PARTICIPANT_STATE with isPlayer==true arrives.
// Returns -1 until then (or after a RaceInfo reset). Use this to drive lap detection —
// it identifies which participant's sector/pit state belongs to the local player.
int   PmrGetPlayerId();                                    // -1 if no player state yet
