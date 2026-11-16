//---------------------------------------------------------------------------
// LiveChannelResolver.cpp — implementation of the JSON-table-driven live feed.
// See docs/LIVE_CAPTURE_JSON_MIGRATION_PLAN.md (Phase 0).
//
// This file is the ONLY place in the codebase that knows which mapping.json alias
// feeds which LapStats member: every sink below appends to exactly one field, and
// build() wires them up from RuleConfig::instance().channelMappings().
//---------------------------------------------------------------------------

#include "LiveChannelResolver.h"
#include "RuleConfig.h"   // singleton access to channelMappings()

#include <unordered_set>

namespace LiveChannel {

using std::string;
using std::vector;
using std::size_t;

// ===========================================================================
// Sink functions — one per LapStats field reachable from a live capture.
// StatSummary sinks: .add(v) (non-finite values are dropped inside add()).
// VelocityHistogram sinks: suspVel* (.add filters |v| > 500 mm/s internally).
// DamperSampleSink pair for PMR dual-write: spring strain feeds BOTH susTravel*
// and damper*.addSample(travel, t) — see ResolvedChannel::damperAddSample.
// ===========================================================================

static void SinkSpeed(LapStats& l, double v)            { l.speed.add(v); }
static void SinkRpm(LapStats& l, double v)              { l.rpm.add(v); }
static void SinkThrottle(LapStats& l, double v)         { l.throttle.add(v); }
static void SinkBrakePedal(LapStats& l, double v)       { l.brakePedal.add(v); }
static void SinkSteerAngle(LapStats& l, double v)       { l.steerAngle.add(v); }
static void SinkYawRate(LapStats& l, double v)          { l.yawRate.add(v); }
static void SinkLatG(LapStats& l, double v)             { l.latG.add(v); }
static void SinkLongG(LapStats& l, double v)            { l.longG.add(v); }

static void SinkWheelSpeedLF(LapStats& l, double v)     { l.wheelSpeedLF.add(v); }
static void SinkWheelSpeedRF(LapStats& l, double v)     { l.wheelSpeedRF.add(v); }
static void SinkWheelSpeedLR(LapStats& l, double v)     { l.wheelSpeedLR.add(v); }
static void SinkWheelSpeedRR(LapStats& l, double v)     { l.wheelSpeedRR.add(v); }

static void SinkBrakeTempLF(LapStats& l, double v)      { l.brakeTempLF.add(v); }
static void SinkBrakeTempRF(LapStats& l, double v)      { l.brakeTempRF.add(v); }
static void SinkBrakeTempLR(LapStats& l, double v)      { l.brakeTempLR.add(v); }
static void SinkBrakeTempRR(LapStats& l, double v)      { l.brakeTempRR.add(v); }

static void SinkPressureLF(LapStats& l, double v)       { l.pressureLF.add(v); }
static void SinkPressureRF(LapStats& l, double v)       { l.pressureRF.add(v); }
static void SinkPressureLR(LapStats& l, double v)       { l.pressureLR.add(v); }
static void SinkPressureRR(LapStats& l, double v)       { l.pressureRR.add(v); }

// Surface (centre tread) temps — fed by EVO and PMR only; AMS2 has no centre-tread
// channel on the wire so its table simply carries no rows for these aliases.
static void SinkSurfTempCFL(LapStats& l, double v)      { l.surfTempCFL.add(v); }
static void SinkSurfTempCRF(LapStats& l, double v)      { l.surfTempCRF.add(v); }
static void SinkSurfTempCLR(LapStats& l, double v)      { l.surfTempCLR.add(v); }
static void SinkSurfTempCRR(LapStats& l, double v)      { l.surfTempCRR.add(v); }

// Tyre shoulder temps. NOTE: the AMS2 left/right tyre-temp swap is NOT handled here —
// mapping.json already encodes it per wheel (Tyre_Temp_FL_Inner -> mTyreTempRight[0],
// Tyre_Temp_FR_Inner -> mTyreTempLeft[1], ...), so every row feeds straight through.
static void SinkTyreInnerLF(LapStats& l, double v)      { l.tyreInnerTempLF.add(v); }
static void SinkTyreCentreLF(LapStats& l, double v)     { l.tyreCentreTempLF.add(v); }
static void SinkTyreOuterLF(LapStats& l, double v)      { l.tyreOuterTempLF.add(v); }
static void SinkTyreInnerRF(LapStats& l, double v)      { l.tyreInnerTempRF.add(v); }
static void SinkTyreCentreRF(LapStats& l, double v)     { l.tyreCentreTempRF.add(v); }
static void SinkTyreOuterRF(LapStats& l, double v)      { l.tyreOuterTempRF.add(v); }
static void SinkTyreInnerLR(LapStats& l, double v)      { l.tyreInnerTempLR.add(v); }
static void SinkTyreCentreLR(LapStats& l, double v)     { l.tyreCentreTempLR.add(v); }
static void SinkTyreOuterLR(LapStats& l, double v)      { l.tyreOuterTempLR.add(v); }
static void SinkTyreInnerRR(LapStats& l, double v)      { l.tyreInnerTempRR.add(v); }
static void SinkTyreCentreRR(LapStats& l, double v)     { l.tyreCentreTempRR.add(v); }
static void SinkTyreOuterRR(LapStats& l, double v)      { l.tyreOuterTempRR.add(v); }

// Tyre air temps — EVO only (TYRE_TAIR_XX).
static void SinkTyreAirLF(LapStats& l, double v)        { l.tyreAirTempLF.add(v); }
static void SinkTyreAirRF(LapStats& l, double v)        { l.tyreAirTempRF.add(v); }
static void SinkTyreAirLR(LapStats& l, double v)        { l.tyreAirTempLR.add(v); }
static void SinkTyreAirRR(LapStats& l, double v)        { l.tyreAirTempRR.add(v); }

// Brake torque — EVO only (BRAKE_TORQUE_XX).
static void SinkBrakeTorqueLF(LapStats& l, double v)    { l.brakeTorqueLF.add(v); }
static void SinkBrakeTorqueRF(LapStats& l, double v)    { l.brakeTorqueRF.add(v); }
static void SinkBrakeTorqueLR(LapStats& l, double v)    { l.brakeTorqueLR.add(v); }
static void SinkBrakeTorqueRR(LapStats& l, double v)    { l.brakeTorqueRR.add(v); }

// Wheel slip — same LapStats field for all sims; semantics differ per sim (AMS2: slip
// SPEED km/h via Tyre_Slip_Speed_* rows, EVO/PMR: ratio % via Wheel_Slip_Ratio_* rows).
static void SinkWheelSlipLF(LapStats& l, double v)      { l.wheelSlipLF.add(v); }
static void SinkWheelSlipRF(LapStats& l, double v)      { l.wheelSlipRF.add(v); }
static void SinkWheelSlipLR(LapStats& l, double v)      { l.wheelSlipLR.add(v); }
static void SinkWheelSlipRR(LapStats& l, double v)      { l.wheelSlipRR.add(v); }

// Direct suspension velocity (mm/s): AMS2 mSuspensionVelocity / PMR damperVelocity.
// Feeds the DIRECT histograms that pickDamperHistogram() prefers over travel-derived.
static void SinkSuspVelLF(LapStats& l, double v)        { l.suspVelLF.add(v); }
static void SinkSuspVelRF(LapStats& l, double v)        { l.suspVelRF.add(v); }
static void SinkSuspVelLR(LapStats& l, double v)        { l.suspVelLR.add(v); }
static void SinkSuspVelRR(LapStats& l, double v)        { l.suspVelRR.add(v); }

// Suspension travel (mm). For PMR the same value also seeds damper*.addSample() via
// ResolvedChannel::damperAddSample — see DamperSink* below. AMS2/EVO keep their own
// post-table addSample calls until Phase 1/2 decide whether to move them here too.
static void SinkSusTravelLF(LapStats& l, double v)      { l.susTravelLF.add(v); }
static void SinkSusTravelRF(LapStats& l, double v)      { l.susTravelRF.add(v); }
static void SinkSusTravelLR(LapStats& l, double v)      { l.susTravelLR.add(v); }
static void SinkSusTravelRR(LapStats& l, double v)      { l.susTravelRR.add(v); }

// Damper dual-write sinks (PMR Phase 3): record travel + timestamp for dTravel/dt.
static void DamperSinkLF(LapStats& l, double v, double t)   { l.damperLF.addSample(v, t); }
static void DamperSinkRF(LapStats& l, double v, double t)   { l.damperRF.addSample(v, t); }
static void DamperSinkLR(LapStats& l, double v, double t)   { l.damperLR.addSample(v, t); }
static void DamperSinkRR(LapStats& l, double v, double t)   { l.damperRR.addSample(v, t); }

// Bumpstop travel — EVO MOTEC rows exist; no LIVE row today (dispatch entry kept so a
// future mapping.json addition works without recompiling).
static void SinkBumpStopLF(LapStats& l, double v)       { l.bumpStopLF.add(v); }
static void SinkBumpStopRF(LapStats& l, double v)       { l.bumpStopRF.add(v); }
static void SinkBumpStopLR(LapStats& l, double v)       { l.bumpStopLR.add(v); }
static void SinkBumpStopRR(LapStats& l, double v)       { l.bumpStopRR.add(v); }

// Brake bias setting (%). The AMS2 "only if mBrakeBias >= 0" guard stays in the sim's
// raw extraction: a negative value simply never lands in the dictionary.
static void SinkBrakeBias(LapStats& l, double v)        { l.brakeBiasSetting.add(v); }

// TCS / ABS active — pre-computed numeric values from bit flags (0/1 AMS2, 0/100 EVO+PMR).
static void SinkTcsActive(LapStats& l, double v)        { l.tcsActive.add(v); }
static void SinkAbsActive(LapStats& l, double v)        { l.absActive.add(v); }

// Thermal / fuel. Fuel_Level_Ratio rows carry factor 1.0 in mapping.json today; the
// ratio -> % conversion is applied by each sim's raw extraction (pre-computed value),
// so this sink stays unit-agnostic either way.
static void SinkOilTemp(LapStats& l, double v)          { l.oilTemp.add(v); }
static void SinkWaterTemp(LapStats& l, double v)        { l.waterTemp.add(v); }
static void SinkFuelLevel(LapStats& l, double v)        { l.fuelLevel.add(v); l.fuelLevelEnds.record(v); }
static void SinkTrackTemp(LapStats& l, double v)        { l.trackTemp.add(v); }
static void SinkAmbientTemp(LapStats& l, double v)      { l.ambientTemp.add(v); }

// --- Prediction sinks: per-lap first/last deltas for rate calculations ---
static void SinkFuelCapacity(LapStats& l, double v)     { l.fuelCapacityL.add(v); }

static void SinkTyreWearLF(LapStats& l, double v)       { l.tyreWearLF.record(v); }
static void SinkTyreWearRF(LapStats& l, double v)       { l.tyreWearRF.record(v); }
static void SinkTyreWearLR(LapStats& l, double v)       { l.tyreWearLR.record(v); }
static void SinkTyreWearRR(LapStats& l, double v)       { l.tyreWearRR.record(v); }

// EVO: pad/disc life remaining (1.0 → 0.0 as they wear down).
static void SinkPadLifeFL(LapStats& l, double v)        { l.padLifeFL.record(v); }
static void SinkPadLifeFR(LapStats& l, double v)        { l.padLifeFR.record(v); }
static void SinkPadLifeRL(LapStats& l, double v)        { l.padLifeRL.record(v); }
static void SinkPadLifeRR(LapStats& l, double v)        { l.padLifeRR.record(v); }

static void SinkDiscLifeFL(LapStats& l, double v)       { l.discLifeFL.record(v); }
static void SinkDiscLifeFR(LapStats& l, double v)       { l.discLifeFR.record(v); }
static void SinkDiscLifeRL(LapStats& l, double v)       { l.discLifeRL.record(v); }
static void SinkDiscLifeRR(LapStats& l, double v)       { l.discLifeRR.record(v); }

// AMS2: brake damage (0 → up as they wear).
static void SinkBrakeDamageLF(LapStats& l, double v)    { l.brakeDamageLF.record(v); }
static void SinkBrakeDamageRF(LapStats& l, double v)    { l.brakeDamageRF.record(v); }
static void SinkBrakeDamageLR(LapStats& l, double v)    { l.brakeDamageLR.record(v); }
static void SinkBrakeDamageRR(LapStats& l, double v)    { l.brakeDamageRR.record(v); }

// AMS2: clutch wear.
static void SinkClutchWear(LapStats& l, double v)       { l.clutchWear.record(v); }

// Ride heights (aeroHeight0..3). AMS2: 4 corners in cm; EVO: front/rear only — its two
// rows map onto aeroHeight0/1 exactly as AcsEvoFeedLap does today. Unit is recorded on
// lap.rideHeightUnit by the sim code, not here (the JSON unit field documents intent).
static void SinkRideHeightFL(LapStats& l, double v)     { l.aeroHeight0.add(v); }
static void SinkRideHeightFR(LapStats& l, double v)     { l.aeroHeight1.add(v); }
static void SinkRideHeightRL(LapStats& l, double v)     { l.aeroHeight2.add(v); }
static void SinkRideHeightRR(LapStats& l, double v)     { l.aeroHeight3.add(v); }

// ===========================================================================
// Alias -> sink dispatch table. Keyed by the EXACT alias strings used in
// mapping.json (see MotecDataAnalyser.mapping.json). Built once per process;
// FeedLapFromTable never touches this map — only build() does, at connect time.
// ===========================================================================

static const std::unordered_map<string, ValueSink>& AliasDispatchMap() {
    static const std::unordered_map<string, ValueSink> m = [] {
        std::unordered_map<string, ValueSink> t;
#define LCR_ALIAS(aliasName) t[#aliasName]
        // Core driving channels
        LCR_ALIAS(Vehicle_Speed)      = SinkSpeed;
        LCR_ALIAS(Engine_RPM)         = SinkRpm;
        LCR_ALIAS(Throttle_Pos)       = SinkThrottle;
        LCR_ALIAS(Brake_Pos)          = SinkBrakePedal;
        LCR_ALIAS(Steering_Angle)     = SinkSteerAngle;
        LCR_ALIAS(Yaw_Rate)           = SinkYawRate;
        LCR_ALIAS(G_Lat)              = SinkLatG;
        LCR_ALIAS(G_Long)             = SinkLongG;

        // Wheel speeds (FL/FR/RL/RR naming follows mapping.json, not LapStats LF..)
        LCR_ALIAS(Wheel_Speed_FL)     = SinkWheelSpeedLF;
        LCR_ALIAS(Wheel_Speed_FR)     = SinkWheelSpeedRF;
        LCR_ALIAS(Wheel_Speed_RL)     = SinkWheelSpeedLR;
        LCR_ALIAS(Wheel_Speed_RR)     = SinkWheelSpeedRR;

        // Brake temps / pressures
        LCR_ALIAS(Brake_Temp_FL)      = SinkBrakeTempLF;
        LCR_ALIAS(Brake_Temp_FR)      = SinkBrakeTempRF;
        LCR_ALIAS(Brake_Temp_RL)      = SinkBrakeTempLR;
        LCR_ALIAS(Brake_Temp_RR)      = SinkBrakeTempRR;
        LCR_ALIAS(Tyre_Pressure_FL)   = SinkPressureLF;
        LCR_ALIAS(Tyre_Pressure_FR)   = SinkPressureRF;
        LCR_ALIAS(Tyre_Pressure_RL)   = SinkPressureLR;
        LCR_ALIAS(Tyre_Pressure_RR)   = SinkPressureRR;

        // Surface (centre tread) temps — EVO / PMR only
        LCR_ALIAS(Surf_Temp_FL)       = SinkSurfTempCFL;
        LCR_ALIAS(Surf_Temp_FR)       = SinkSurfTempCRF;
        LCR_ALIAS(Surf_Temp_RL)       = SinkSurfTempCLR;
        LCR_ALIAS(Surf_Temp_RR)       = SinkSurfTempCRR;

        // Tyre shoulder temps (inner / middle / outer per wheel)
        LCR_ALIAS(Tyre_Temp_FL_Inner)   = SinkTyreInnerLF;
        LCR_ALIAS(Tyre_Temp_FL_Middle)  = SinkTyreCentreLF;
        LCR_ALIAS(Tyre_Temp_FL_Outer)   = SinkTyreOuterLF;
        LCR_ALIAS(Tyre_Temp_FR_Inner)   = SinkTyreInnerRF;
        LCR_ALIAS(Tyre_Temp_FR_Middle)  = SinkTyreCentreRF;
        LCR_ALIAS(Tyre_Temp_FR_Outer)   = SinkTyreOuterRF;
        LCR_ALIAS(Tyre_Temp_RL_Inner)   = SinkTyreInnerLR;
        LCR_ALIAS(Tyre_Temp_RL_Middle)  = SinkTyreCentreLR;
        LCR_ALIAS(Tyre_Temp_RL_Outer)   = SinkTyreOuterLR;
        LCR_ALIAS(Tyre_Temp_RR_Inner)   = SinkTyreInnerRR;
        LCR_ALIAS(Tyre_Temp_RR_Middle)  = SinkTyreCentreRR;
        LCR_ALIAS(Tyre_Temp_RR_Outer)   = SinkTyreOuterRR;

        // Tyre air temps — EVO only
        LCR_ALIAS(Tyre_Air_Temp_FL)     = SinkTyreAirLF;
        LCR_ALIAS(Tyre_Air_Temp_FR)     = SinkTyreAirRF;
        LCR_ALIAS(Tyre_Air_Temp_RL)     = SinkTyreAirLR;
        LCR_ALIAS(Tyre_Air_Temp_RR)     = SinkTyreAirRR;

        // Brake torque — EVO only
        LCR_ALIAS(Brake_Torque_FL)      = SinkBrakeTorqueLF;
        LCR_ALIAS(Brake_Torque_FR)      = SinkBrakeTorqueRF;
        LCR_ALIAS(Brake_Torque_RL)      = SinkBrakeTorqueLR;
        LCR_ALIAS(Brake_Torque_RR)      = SinkBrakeTorqueRR;

        // Wheel slip: ratio rows (EVO/PMR %) and speed rows (AMS2 km/h) share sinks.
        LCR_ALIAS(Wheel_Slip_Ratio_FL)  = SinkWheelSlipLF;
        LCR_ALIAS(Wheel_Slip_Ratio_FR)  = SinkWheelSlipRF;
        LCR_ALIAS(Wheel_Slip_Ratio_RL)  = SinkWheelSlipLR;
        LCR_ALIAS(Wheel_Slip_Ratio_RR)  = SinkWheelSlipRR;
        LCR_ALIAS(Tyre_Slip_Speed_FL)   = SinkWheelSlipLF;
        LCR_ALIAS(Tyre_Slip_Speed_FR)   = SinkWheelSlipRF;
        LCR_ALIAS(Tyre_Slip_Speed_RL)   = SinkWheelSlipLR;
        LCR_ALIAS(Tyre_Slip_Speed_RR)   = SinkWheelSlipRR;

        // Suspension: direct velocity (AMS2/PMR), travel (+ PMR damper dual-write).
        LCR_ALIAS(Damper_Velocity_FL)   = SinkSuspVelLF;
        LCR_ALIAS(Damper_Velocity_FR)   = SinkSuspVelRF;
        LCR_ALIAS(Damper_Velocity_RL)   = SinkSuspVelLR;
        LCR_ALIAS(Damper_Velocity_RR)   = SinkSuspVelRR;

        // Brake bias / TCS / ABS (numeric values pre-computed by the sim code).
        LCR_ALIAS(Brake_Bias_Setting)   = SinkBrakeBias;
        LCR_ALIAS(TCS_Active)           = SinkTcsActive;
        LCR_ALIAS(ABS_Active)           = SinkAbsActive;

        // Thermal / fuel (Water_Temp lives in the extra map below).
        LCR_ALIAS(Engine_Oil_Temp)      = SinkOilTemp;
#undef LCR_ALIAS
        return t;
    }();
    return m;
}

// Second dispatch table — keeps each initializer small and readable. mapping.json's LIVE
// rows use the alias "Engine_Water_Temp" for coolant temperature (the *_Percent row is a
// different, unused channel).
static const std::unordered_map<string, ValueSink>& AliasDispatchMapExtra() {
    static const std::unordered_map<string, ValueSink> m = [] {
        std::unordered_map<string, ValueSink> t;
#define LCR_ALIAS(aliasName) t[#aliasName]
        LCR_ALIAS(Engine_Water_Temp)   = SinkWaterTemp;
        LCR_ALIAS(Fuel_Level_Ratio)     = SinkFuelLevel;
        LCR_ALIAS(Track_Temp)           = SinkTrackTemp;
        LCR_ALIAS(Ambient_Temp)         = SinkAmbientTemp;

        // Prediction: fuel capacity + wear/damage deltas.
        LCR_ALIAS(Fuel_Capacity)        = SinkFuelCapacity;
        LCR_ALIAS(Tyre_Wear_FL)         = SinkTyreWearLF;
        LCR_ALIAS(Tyre_Wear_FR)         = SinkTyreWearRF;
        LCR_ALIAS(Tyre_Wear_RL)         = SinkTyreWearLR;
        LCR_ALIAS(Tyre_Wear_RR)         = SinkTyreWearRR;

        // EVO: pad / disc life remaining.
        LCR_ALIAS(Brake_Pad_Life_FL)    = SinkPadLifeFL;
        LCR_ALIAS(Brake_Pad_Life_FR)    = SinkPadLifeFR;
        LCR_ALIAS(Brake_Pad_Life_RL)    = SinkPadLifeRL;
        LCR_ALIAS(Brake_Pad_Life_RR)    = SinkPadLifeRR;
        LCR_ALIAS(Brake_Disc_Life_FL)   = SinkDiscLifeFL;
        LCR_ALIAS(Brake_Disc_Life_FR)   = SinkDiscLifeFR;
        LCR_ALIAS(Brake_Disc_Life_RL)   = SinkDiscLifeRL;
        LCR_ALIAS(Brake_Disc_Life_RR)   = SinkDiscLifeRR;

        // AMS2: brake damage (increases with use).
        LCR_ALIAS(Brake_Damage_FL)      = SinkBrakeDamageLF;
        LCR_ALIAS(Brake_Damage_FR)      = SinkBrakeDamageRF;
        LCR_ALIAS(Brake_Damage_RL)      = SinkBrakeDamageLR;
        LCR_ALIAS(Brake_Damage_RR)      = SinkBrakeDamageRR;

        // AMS2: clutch wear.
        LCR_ALIAS(Clutch_Wear)          = SinkClutchWear;

        // Ride heights (aeroHeight0..3). EVO's front/rear-only rows map onto 0/1.
        LCR_ALIAS(Ride_Height_FL)       = SinkRideHeightFL;
        LCR_ALIAS(Ride_Height_FR)       = SinkRideHeightFR;
        LCR_ALIAS(Ride_Height_RL)       = SinkRideHeightRL;
        LCR_ALIAS(Ride_Height_RR)       = SinkRideHeightRR;

        // Bumpstop travel (no LIVE rows today — kept for forward compatibility).
        LCR_ALIAS(Bumpstop_Travel_FL)   = SinkBumpStopLF;
        LCR_ALIAS(Bumpstop_Travel_FR)   = SinkBumpStopRF;
        LCR_ALIAS(Bumpstop_Travel_RL)   = SinkBumpStopLR;
        LCR_ALIAS(Bumpstop_Travel_RR)   = SinkBumpStopRR;

        // Suspension travel. The PMR table build attaches the damper dual-write sink to
        // these entries (see ResolvedChannel::damperAddSample); AMS2/EVO leave it null.
        t["Suspension_Travel_FL"]       = SinkSusTravelLF;
        t["Suspension_Travel_FR"]       = SinkSusTravelRF;
        t["Suspension_Travel_RL"]       = SinkSusTravelLR;
        t["Suspension_Travel_RR"]       = SinkSusTravelRR;

        // PMR-only aliases (no mapping.json rows exist yet — Phase 3 adds them). The
        // dispatch entries are in place so the JSON additions need no recompile.
        LCR_ALIAS(Pmr_Damper_Velocity_FL)   = SinkSuspVelLF;    // reserved, unused today
#undef LCR_ALIAS
        return t;
    }();
    return m;
}

// Human-readable profile name for warning messages (no shared helper exists yet).
static const char* ProfileName(ChannelProfile p) {
    switch (p) {
        case ChannelProfile::EvoM1:   return "EvoM1";
        case ChannelProfile::AMS2:    return "AMS2";  // Generic == AMS2
        case ChannelProfile::Pmr:     return "Pmr";
        default:                      return "?";
    }
}

// ===========================================================================
// ResolvedChannelTable::build — runs once at connect time.
// Keeps LIVE raw rows for the requested profile (Generic rows apply to all),
// resolves each alias through the dispatch map, and records aliases that have
// no sink so validate() can surface them as a visible warning.
// ===========================================================================

void ResolvedChannelTable::build(ChannelProfile p) {
    channels.clear();
    missingAliases.clear();
    profile = p;

    const std::unordered_map<string, ValueSink>& dispatch  = AliasDispatchMap();
    const std::unordered_map<string, ValueSink>& extra     = AliasDispatchMapExtra();
    const vector<ChannelMapping>& mappings = RuleConfig::instance().channelMappings();

    // PMR dual-write: suspension travel also seeds the damper histograms. Only attached
    // when building for the Pmr profile — AMS2/EVO keep their own post-table addSample()
    // calls (their tables carry no Suspension_Travel rows with a second sink).
    static const std::unordered_map<string, DamperSampleSink> kPmrDamperSinks = [] {
        std::unordered_map<string, DamperSampleSink> d;
        d["Suspension_Travel_FL"] = DamperSinkLF;
        d["Suspension_Travel_FR"] = DamperSinkRF;
        d["Suspension_Travel_RL"] = DamperSinkLR;
        d["Suspension_Travel_RR"] = DamperSinkRR;
        return d;
    }();

    std::unordered_set<string> seenKeys;   // dedupe: first row wins per (rawChannel, alias) pair

    for (size_t i = 0; i < mappings.size(); ++i) {
        const ChannelMapping& cm = mappings[i];

        if (cm.type != ChannelMappingType::Raw) continue;                 // virtual rows resolve in DataProvider
        if (cm.source != MappingSource::LiveSharedMemory) continue;       // file-import rows are out of scope here
        if (cm.profile != p && cm.profile != ChannelProfile::Generic) continue;

        ValueSink sink = 0;
        std::unordered_map<string, ValueSink>::const_iterator it1 = dispatch.find(cm.alias);
        if (it1 != dispatch.end()) {
            sink = it1->second;
        } else {
            std::unordered_map<string, ValueSink>::const_iterator it2 = extra.find(cm.alias);
            if (it2 != extra.end())
                sink = it2->second;
        }

        ResolvedChannel rc;
        rc.rawName  = cm.channel.empty() ? cm.alias : cm.channel;   // dict key: raw channel name, alias as fallback
        rc.addValue = sink;                                          // null when the alias has no LapStats field — reported below
        rc.factor   = cm.factor;
        rc.offset   = cm.offset;
        rc.alias    = cm.alias;                 // semantic label for the LD export (i2 channel name)
        rc.unit     = cm.standardUnit;          // unit string written into the .ld header block

        // PMR dual-write: the four Suspension_Travel_* aliases also seed damper*.addSample().
        if (p == ChannelProfile::Pmr && sink != 0) {
            std::unordered_map<string, DamperSampleSink>::const_iterator itD = kPmrDamperSinks.find(cm.alias);
            if (itD != kPmrDamperSinks.end())
                rc.damperAddSample = itD->second;
        }

        // Report aliases with no dispatch sink ONCE each.
        if (sink == 0) {
            bool alreadyReported = false;
            for (size_t k = 0; k < missingAliases.size(); ++k)
                if (missingAliases[k] == cm.alias) { alreadyReported = true; break; }
            if (!alreadyReported)
                missingAliases.push_back(cm.alias);
        }

        // Skip exact duplicate (rawChannel, alias) pairs — multiple aliases MAY share one raw channel.
        const string dedupeKey = rc.rawName + "\x01" + cm.alias;
        if (!seenKeys.insert(dedupeKey).second) continue;

        channels.push_back(rc);
    }
}

// ===========================================================================
// ResolvedChannelTable::validate — visible-warning helper, never fatal.
// With requiredNames: every name must appear as a rawName in the table (the sim's raw
// extraction promises to provide it). Without: just checks that the table is non-empty.
// ===========================================================================

bool ResolvedChannelTable::validate(const vector<string>& requiredNames, string& missingReport) const {
    missingReport.clear();

    if (channels.empty()) {
        missingReport = std::string("Live channel table for profile '") + ProfileName(profile) +
                        "' is empty - no LIVE mapping rows resolved.";
        return false;
    }

    std::unordered_set<string> present;
    for (size_t i = 0; i < channels.size(); ++i)
        present.insert(channels[i].rawName);

    bool ok = true;
    if (!requiredNames.empty()) {
        vector<string> absent;
        for (size_t i = 0; i < requiredNames.size(); ++i) {
            if (present.find(requiredNames[i]) == present.end())
                absent.push_back(requiredNames[i]);
        }
        if (!absent.empty()) {
            ok = false;
            missingReport += "Required raw channels not in table: ";
            for (size_t i = 0; i < absent.size(); ++i) {
                if (i != 0) missingReport += ", ";
                missingReport += absent[i];
            }
        }
    }

    // Aliases present in mapping.json but with no LapStats sink — informational only,
    // never fatal (the table is still usable for every alias that DOES have a sink).
    if (!missingAliases.empty()) {
        if (!missingReport.empty()) missingReport += "; ";
        missingReport += "no live dispatch target for aliases: ";
        for (size_t k = 0; k < missingAliases.size(); ++k) {
            if (k != 0) missingReport += ", ";
            missingReport += "'" + missingAliases[k] + "'";
        }
    }

    return ok;   // false only when the table is empty or a required raw channel is absent
}

// ===========================================================================
// FeedLapFromTable — THE generic per-frame feed (hot path, zero allocation).
// For each table entry: dict lookup -> skip if missing this frame -> convert with the
// pre-resolved factor/offset -> sink. The optional damperAddSample runs second so a PMR
// spring-strain value lands in both susTravel* and damper*.addSample(travel, t).
// ===========================================================================

void FeedLapFromTable(LapStats& lap,
                      const std::unordered_map<string, double>& rawChannels,
                      const ResolvedChannelTable& table,
                      double timeSeconds) {
    for (size_t i = 0; i < table.channels.size(); ++i) {
        const ResolvedChannel& rc = table.channels[i];

        if (!rc.addValue && !rc.damperAddSample) continue;   // alias with no sink — nothing to do per frame

        std::unordered_map<string, double>::const_iterator it = rawChannels.find(rc.rawName);
        if (it == rawChannels.end()) continue;               // channel not present this frame — skip silently

        const double v = it->second * rc.factor + rc.offset;

        if (rc.addValue)       rc.addValue(lap, v);
        if (rc.damperAddSample) rc.damperAddSample(lap, v, timeSeconds);
    }
}

} // namespace LiveChannel
