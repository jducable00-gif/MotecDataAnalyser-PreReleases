# Implementation Plan — RaceRoom (R3E) Shared-Memory Live-Capture Profile

> **Status:** PLAN ONLY. No code written yet. This document is self-contained so a fresh agent can implement it without prior context. Review and enhance before starting Phase 1.
>
> **Source of truth for the R3E wire format:** [kwstudios-sweden/r3e-api](https://github.com/kwstudios-sweden/r3e-api) — `sample-c/src/r3e.h` (struct version **major=3, minor=6**) and `sample.c` (official polling idiom).
>
> **App architecture reference:** [agents.md](../.ai/skills/AGENTS.md), [`docs/DATA_MAPPING_SPEC.md`](./DATA_MAPPING_SPEC.md), the existing AMS2 / AC EVO / PMR live-capture pairs, and [`MotecDataAnalyser.mapping.json`](../MotecDataAnalyser.mapping.json).

---

## 1. Goal

Add **RaceRoom Racing Experience (RRRE)** as a fourth live-telemetry source alongside AMS2, Assetto Corsa Evo M1, and Project Motor Race (PMR), using R3E's official `"$R3E"` named Windows shared-memory object. The goal is full parity with the other profiles:

- Live lap capture → averaged `LapStats`
- All semantic aliases resolved through [`MotecDataAnalyser.mapping.json`](../MotecDataAnalyser.mapping.json) (no new analysis rules required — R3E reuses every existing rule via alias mapping).
- Same UI flow: profile combo box, connect button, live paint boxes, lap-boundary detection + report generation.

**Non-goals:** file import for R3E (live-only, like PMR); vehicle-specific reference data (the embedded `Ams2VehicleDb` is AMS2-only — guard with the existing "no DB" path).

---

## 2. Why this fits cleanly (architecture)

The app isolates per-simulator differences in exactly **two** places; everything downstream of them is simulator-agnostic:

1. A `<Sim>LiveCapture.h/.cpp` pair that opens the sim's data source, reads frames with a torn-read guard, and emits **RAW values only** (no unit conversion) into `std::unordered_map<std::string,double>` via one function per profile (`<Sim>FeedLap`).
2. [`MotecDataAnalyser.mapping.json`](../MotecDataAnalyser.mapping.json) LIVE rows — `{alias, profile:"R3E", source:"LIVE", channel:<rawKey>, unit, sourceUnit, factor, offset}` where `value = raw * factor + offset`. These are consumed by the single generic [`LiveChannel::FeedLapFromTable()`](../LiveChannelResolver.cpp) and its sink dispatch map in [LiveChannelResolver.h](../LiveChannelResolver.h).

R3E slots into both. The **only** C++-specific (non-linear / multi-value derived) logic stays inside `RaceRoomLiveCapture.cpp`, exactly as AMS2 keeps its tyre-temp L/R swap there:
- Tyre temp position mapping (`LEFT/CENTER/RIGHT` enum → app's `_Outer/_Middle/_Inner`) — non-linear, per-wheel-side.
- Torn-read guard (ticks double-check).
- Post-table `addHandlingSample()` / `addSteerYaw()` multi-value derived samples; optionally the travel-derived damper stats from deflection deltas (§6.6 note — direct velocity is table-routed like every other sim).

**Result:** no changes to rules, registry, report generator, or UI rendering logic — only a new capture pair + form wiring + mapping rows.

---

## 3. R3E shared-memory facts (from `r3e.h` v3.6)

| Fact | Value / note |
|------|--------------|
| Shared-mem object name | **`"$R3E"`** (`R3E_SHARED_MEMORY_NAME`) — a *named* Windows file mapping, opened read-only: `OpenFileMappingW(FILE_MAP_READ, FALSE, L"$R3E")` → `MapViewOfFile(..., sizeof(r3e_shared))`. No handle passed from the sim. |
| Process name (is-running check) | **`RRRE.exe`** |
| Struct packing | `#pragma pack(push,1)` — no padding; offsets are stable within a major version. |
| Versioning fields | Top of struct: `version_major`(=3), `version_minor`(=6). Layout grows over versions → validate major and read only 3.x-present fields (see §9 Q4). |
| Player identification | High-detail section is **player-only**: `r3e_playerdata player`. Top-level `vehicle_info` (`user_id`, `slot_id`, `model_id`) identifies the car. We always feed from `player.*`. |
| Frame token / time source | No explicit sequence number. Use `player.game_simulation_ticks` (1 tick = 1/400 s) for change detection + torn-read guard, and `game_simulation_time` (f64 seconds) as the wall-clock-ish timestamp passed to sinks. |

### Key telemetry fields & native units
| Data | R3E field(s) (`player.` unless noted) | Native unit → app target |
|------|----------------------------------------|--------------------------|
| Speed | `car_speed` | m/s → km/h (×3.6 via table, like AMS2 `mSpeed`) |
| Engine RPM | `engine_rps`, `max_engine_rps`; playerdata also has `engine_torque`(f64) | rad/s; r3e.h defines `RPS_TO_RPM` (= 95.4929…). Emit raw RPS, convert in table row (×95.4929 → RPM). **VERIFY** whether app's `Engine_Rpm` alias expects RPM — it does for AMS2 (`mRpm`). |
| Gear | `gear`, `num_gears` | int; −2 = N/A, −1 = reverse |
| Pedals / steer | `throttle`,`brake`,`clutch`(0–1); `steer_input_raw`(−1..1), `steer_lock_degrees`, `steer_wheel_range_degrees`; playerdata `steering_force_percentage` (±1.0 FFB) | 0–1 → % via table; steer −1..1 → deg ×360 like AMS2, **or** compute true angle = `steer_input_raw * steer_lock_degrees`. Prefer the latter for a real steering-angle value (§9 Q5). |
| Yaw / G-forces | playerdata f64 vec3: `local_angular_velocity[x,y,z]` (rad/s), `local_g_force[x,y,z]` local to car (**+X left, +Y up, +Z back** per struct comment) | yaw = `[1]` rad/s → deg/s ×57.2958; latG/longG from g-force components / 9.81 — **VERIFY axis/sign on first capture (§9 Q3)**. Note R3E gives G-forces directly (no need to divide acceleration by gravity). |
| Engine temps | `engine_temp`, `engine_oil_temp` | °C direct → water/oil temp aliases |
| Tyre pressure | `tire_pressure[4]` (**kPa**) | kPa → psi ×0.1450377 (identical factor AMS2 uses) — wheel order 0=FL,1=FR,2=RL,3=RR |
| Tyres per wheel (`i`=0 FL..3 RR) | `tire_rps`, `tire_speed`(m/s), `tire_grip`(0–1), `tire_wear`(0–1); temps: **`r3e_tire_temp tire_temp[4].current_temp[3]`** with enum LEFT=0 / CENTER=1 / RIGHT=2 (+ optimal/cold/hot per wheel) | °C direct. L/C/R → Inner/Middle/Outer is position-dependent (non-linear, stays in C++). Also emit a 3-point average for `Tyre_Temp_Avg_*`. |
| Brakes | `r3e_brake_temp brake_temp[4].current_temp` (°C); `brake_bias`(0–1 rear bias) | °C direct; bias ×100 → % via table |
| Suspension / dampers | playerdata f64: **`suspension_deflection[4]`, `suspension_velocity[4]`, `camber[4]`, `ride_height[4]`** (all meters) | ride height m → mm ×1000; damper velocity directly available in m/s — routed via mapping rows exactly like AMS2's `mSuspensionVelocity`. Wheel order 0=FL..3=RR. |
| Fuel / mass | `total_mass`(kg), `fuel_left`/`capacity`/`per_lap`(L); electric: virtual_energy_*(MJ), battery_soc(0–100) | fuel % = left/capacity×100 (pre-compute in C++ like PMR, conditional key when capacity>0) |
| Session / laps | `track_name[64]`, `layout_name[64]` UTF-8; `layout_length`(m); sector_start_factors(fractions); session_type/phase enums; **`lap_time_current/previous/best_self`** + `[3] sectors each`; **`completed_laps`**; `in_pitlane`,`pit_state`; flags struct; position, num_cars | Use `completed_laps` increment as the primary lap boundary (§6.5). |
| Aids / electronics | `aid_settings{abs,tc,esp,...}` int enums; traction_control_setting/percent, engine_map_setting, abs_setting | map to TCS_Active / ABS_Active 0/1 flags like AMS2's synthetic bit-flag channels (optional) |

> **Wheel index convention:** R3E uses `0=FL, 1=FR, 2=RL, 3=RR` — same as the app's internal order. No reordering needed for indexed keys; only the tyre-temp L/C/R shoulder mapping is non-linear (§6.4).

---

## 4. Files to create / modify (summary)

| File | Action |
|------|--------|
| `r3e.h` (repo root, next to `Ams2SharedMemory.h`) | **Create** — vendor from r3e-api (`sample-c/src/r3e.h`). Self-contained packed struct; no runtime I/O. Pin the exact revision (§9 Q4). |
| `RaceRoomLiveCapture.h/.cpp` (repo root) | **Create** — new capture pair mirroring AMS2's API surface (§5, §6). |
| [`MotecSetupAnalyzer.h`](../MotecSetupAnalyzer.h) | **Modify** — add `ChannelProfile::R3e = 3`. |
| [`MotecSetupForm.h`](../MotecSetupForm.h) / [`.cpp`](../MotecSetupForm.cpp) | **Modify** — `LiveMode` enum, profile↔mode mapping, INI validation + combo item, connect/close dispatch, capture-thread branch, timer drain, new lap-boundary handler (§6). |
| [`MotecDataAnalyser.mapping.json`](../MotecDataAnalyser.mapping.json) | **Modify** — add `"profile":"R3E"` LIVE rows (§7). |
| `Project1.cbproj` | **Modify** — compile `RaceRoomLiveCapture.cpp`; ensure repo root on include path for `r3e.h`. (mapping.json is already copied to output by an existing rule.) |
| `scripts/test_r3e_feed.cpp` (+ build entry) | **Create** — unit test mirroring [`scripts/test_pmr_feed.cpp`](../scripts). |

---

## 5. New capture pair: `RaceRoomLiveCapture.h/.cpp`

Mirror the AMS2 API surface exactly (see [Ams2LiveCapture.h](../Ams2LiveCapture.h)). All state is file-static; thread-safety matches the existing modules. Prefix all public symbols with **`R3e`**.

| Function | R3E implementation notes |
|----------|---------------------------|
| `bool R3eOpenSharedMemory(std::string& err)` | `OpenFileMappingW(FILE_MAP_READ, FALSE, L"$R3E")`; on failure set a clear message (sim not running / no map). Then `MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, sizeof(r3e_shared))`. Validate `version_major == 3` — on mismatch record a warning string but **do not hard-fail** (§9 Q4). Store handle + mapped pointer. |
| `FrameReadStatus R3eReadFrame(r3e_shared& out)` → `{NoNewData, TornRetry, NewFrame}` | **Torn-read guard (adapt AMS2's sequence double-check):** read `player.game_simulation_ticks` twice; if they differ → return `TornRetry`. Else memcpy the whole packed struct into a local POD and copy to `out`, returning `NewFrame`. Return `NoNewData` when ticks are unchanged since the last accepted frame. |
| `void R3eResetFrameTracking()` | Reset the "last seen ticks" baseline (called on connect, like AMS2). |
| `void R3eCloseSharedMemory()` | Unmap + close handle; idempotent and thread-safe (AMS2 pattern). Safe to call when not open. |
| `std::string R3eGetChannelTableWarning()` | Return the one-time table-validation report (§6.1), empty if clean. |
| `bool R3eIsOnTrack(const r3e_shared&)` / `R3eIsInPits(...)` | On-track = not in pitlane; pits from `in_pitlane`/`pit_state`. (Exact field semantics to confirm on first capture.) |
| `void R3eFeedLap(LapStats& lap, const r3e_shared&, double timeSeconds)` | Build the raw dict + call one generic `FeedLapFromTable(...)`, then post-table derived samples (§6.4). **No unit conversion in this function** — table rows do it. |
| (process check) | Reuse the existing "is process running" helper pattern used by AMS2/PMR, with name `"RRRE.exe"` (`R3eIsProcessRunning()` wrapper is fine for form symmetry). |

---

## 6. Form wiring ([`MotecSetupForm.h`](../MotecSetupForm.h) / [`.cpp`](../MotecSetupForm.cpp))

> Line numbers below are approximate anchors from the current source; **anchor on function names + unique snippets**, not exact lines, since they shift as you edit top-to-bottom.

### 6.1 Profile enum & INI
- [`ChannelProfile::R3e = 3`](../MotecSetupAnalyzer.h) — add after `Pmr=2`. The INI stores the profile as an int (`ReadInteger("General","ChannelProfile",...)`), so no migration is needed for existing users.
- In [`MotecSetupForm.cpp`](../MotecSetupForm.cpp), `ReadChannelProfileSetting()`: extend validation to accept `(int)ChannelProfile::R3e`; add a combo-box item labeled **"RaceRoom"**.

### 6.2 LiveMode + mapping
- [`enum LiveMode`](../MotecSetupForm.h): add **`LIVE_R3E = 3`** (after `LIVE_PMR`).
- [`MotecSetupForm.cpp`](../MotecSetupForm.cpp), `ProfileToLiveMode()` (~line 185, currently: Pmr→PMR, EvoM1→EVO, else AMS2): insert **before the fallback**:
  ```cpp
  if (profile == ChannelProfile::R3e) return LIVE_R3E;
  ```

### 6.3 Connect / close dispatch — `LiveConnectTimerTimer` (~line 2890+) and `stopLiveCapture()`
- In **connect** (`LiveConnectTimerTimer`): add an R3E branch mirroring the AMS2/EVO branches:
  - process check → if not running, show "start RRRE" message; else
  - open via `R3eOpenSharedMemory(err)` (on failure surface err); on success spawn/ensure the capture thread and post a "connected to RaceRoom" status + append `R3eGetChannelTableWarning()` to any existing warning text.
- The **capture-thread spawn condition** is currently `(mode == LIVE_EVO || mode == LIVE_AMS2)` — extend it to include `LIVE_R3E` (PMR stays on the timer; R3E, like AMS2/EVO, runs its own capture thread).
- In **close** (`stopLiveCapture()`): add an R3E branch calling `R3eCloseSharedMemory()`. This appears in **both** the reconnect path and the shutdown path — update both.

### 6.4 Frame union + capture thread + timer drain
- [`CapturedFrame`](../MotecSetupForm.h) (~line 145): a union `{ evo{...}, ams }` with an empty user-provided ctor (bcc32c non-trivial-default-ctor workaround). Add a **POD member** `r3e_shared r3e;`. Keep the existing empty-ctor pattern.
- [`MotecSetupForm.cpp`](../MotecSetupForm.cpp), `captureThreadFunc()` (~line 3199): add an R3E branch mirroring AMS2 — loop: `R3eReadFrame(local)` → on `NewFrame` copy into `frame.r3e`, set timestamp, push to the bounded queue (cap ~1000), sleep ~8 ms; on `TornRetry`/`NoNewData` just continue.
- [`MotecSetupForm.cpp`](../MotecSetupForm.cpp), `LiveTimerTimer` (~line 3269): add R3E handling — process-check → stop if gone (like AMS2); drain the queue and call a new **`processLiveFrameR3e(frame.r3e)`** per frame.

### 6.5 Lap-boundary handler: `processLiveFrameR3e(const r3e_shared&)`
Mirror the existing AMS2 lap logic, with one improvement — R3E exposes an explicit counter:
- **Primary boundary:** detect a change in `completed_laps`; on increment → finalize current lap + generate live report (same as PIT_MODE transitions do today). This is cleaner than AMS2's time-delta heuristic.
- Keep the existing pause / pit→track start-with-memo-wipe behavior, driven by `in_pitlane`/`pit_state`.
- Feed only when on-track and a lap is in progress; pass **time = `game_simulation_time`** (f64 s) to `R3eFeedLap(...)`, using the same high-res-clock fallback pattern AMS2 uses if needed.

### 6.6 Raw-dict keys emitted by `R3eFeedLap`
Keys MUST match the `"channel"` field of the R3E mapping rows **exactly** (§7). Emit RAW values only:
- Scalars: `"car_speed"`, `"engine_rps"`, `"gear"`, `"throttle"`,`"brake"`,`"clutch"`, `"steer_input_raw"`, `"steer_lock_degrees"`, `"steer_wheel_range_degrees"`, `"angVelY"` (=`local_angular_velocity[1]`), latG/longG keys, `"engine_temp"`, `"engine_oil_temp"`.
- Indexed (`snprintf(key,"...[%d]",i)`, i=0 FL..3 RR): `"tire_pressure[i]"`(kPa; **omit key when value < 0** — r3e.h uses −1 = N/A), tyre-temp position-mapped keys (§6.3 above + §7, same −1 sentinel guard per point), `"brake_temp[i]"`(°C, −1 guard), `"ride_height[i]"`(m), **`"damperVelocity[i]"`(m/s)** — direct from `suspension_velocity[4]`, consumed by the R3E `Damper_Velocity_*` mapping rows (§7).
- Indexed (continued): **"wheelSpeed[i]"** = `tire_speed[4]`(m/s) for the `Wheel_Speed_*` rows; **`"suspensionDeflection[i]"`** = `suspension_deflection[4]`(m) for the `Suspension_Travel_*` rows; `"tireWear[i]"` = `tire_wear[4]`(0–1, −1 guard).
- C++ pre-computed scalars (multi-value derivations that a single factor/offset row cannot express — same pattern as PMR's fuel/TCS handling): **`"fuelLevelRatio"`** = `fuel_left / fuel_capacity`, emitted only when `capacity > 0`; `"tcsActive"` / `"absActive"` from `aid_settings.tc/.abs`: value == 5 ("currently active") → emit the app-convention number, else 0 (see §9 Q7 for % vs bool).
- **Tyre temp mapping (stays in C++, non-linear):** R3E enum `LEFT=0/CENTER=1/RIGHT=2`. Map to the app's `_Outer/_Middle/_Inner` per wheel side, mirroring AMS2's convention where front wheels have LEFT→outer and rear wheels mirror it. Emit both the three shoulder keys **and** a `"tireAvgTemp[i]"` = mean of the 3 for `Tyre_Temp_Avg_*`.
- Post-table (multi-value derived samples — NOT table sinks), mirroring AMS2: compute steer deg, yaw deg/s, latG/longG g, brake/throttle %, front travel asymmetry; call `lap.addHandlingSample(...)` and `lap.addSteerYaw(...)`, then `++lap.rowCount`. Set `lap.rideHeightUnit = RideHeightUnit::Millimeters` (table converts m→mm).
- **Keys with no mapping row are harmless** — the table feed only warns about *rows* whose alias has no sink, never about dict keys without rows. So `"gear"`, `"clutch"` etc. may stay in the dict for future use; just don't add LIVE rows for them (§7 coverage policy).

> **Damper velocity — resolved, no decision needed:** damper velocity is a normal table sink for every sim: AMS2 emits `"mSuspensionVelocity[i]"`(m/s) + mapping rows ×1000 → `SinkSuspVel*`; PMR pre-computes mm/s in C++ + f=1 rows. R3E follows the same pattern — emit raw m/s keys and add 4 `Damper_Velocity_*` rows with factor **1000** (§7). The separate post-table travel-derivation block AMS2 has (`addSample(travel, time)` from suspension-deflection deltas) is a *secondary* fallback that feeds different accumulators; it is not an alternative routing for the velocity alias. R3E may optionally replicate that block using `suspension_deflection[4]` if parity with AMS2's travel stats is wanted — but the direct-velocity path above fully covers the damper analysis rules either way.

---

## 7. mapping.json rows (`"profile":"R3E"`, `"source":"LIVE"`)

Add LIVE rows per alias reusing the file's existing factor conventions (see the AMS2/EVO/PMR rows for each). `value = raw * factor + offset`. Wheel order in indexed keys: 0=FL,1=FR,2=RL,3=RR.

| Alias | R3E channel key | unit ← sourceUnit | factor | offset |
|-------|-----------------|--------------------|--------|--------|
| `Vehicle_Speed` (speed) | `"car_speed"` | km/h ← m/s | 3.6 | 0 |
| `Wheel_Speed_FL..RR` | `"wheelSpeed[0]"` … `[3]` = raw `tire_speed[i]`(m/s), emitted in C++ (§6.6) | km/h ← m/s | **3.6** (identical to the AMS2/EVO/PMR rows; negative-in-reverse is expected, same as other profiles) | 0 |
| Engine RPM alias (`Engine_Rpm`) | `"engine_rps"` | rpm ← rad/s(RPS) | **95.4929** (= r3e.h `RPS_TO_RPM`; VERIFY target unit §9 Q1b) | 0 |
| `Throttle_Pos` | `"throttle"` | % ← 0-1 | 100 | 0 |
| Brake pedal alias (`Brake_Pos`) | `"brake"` | % ← 0-1 | 100 | 0 |
| `Brake_Bias_Setting` | `"brakeBiasPct"` — **C++ pre-computed** from raw `brake_bias`: R3E's value is a *REAR*-bias fraction (r3e.h comment: "0.3 = 30% rear"), app convention is % FRONT → emit `(1 − brake_bias) × 100`; when raw < 0 (N/A), default **50** like PMR does for non-finite bias | % ← pre-computed | 1 | 0 — see §9 Q6; do NOT use a plain factor-100 row on the raw field, that would invert front/rear semantics |
| Steering angle alias (`Steering_Angle`) | `"steer_input_raw"` (−1..1; optionally pre-computed to degrees in C++ using `steer_lock_degrees` per §6.3) | deg ← … | see §9 Q5 | — |
| `Yaw_Rate` | `"angVelY"` | deg/s ← rad/s | 57.2958 | 0 (flip to −57.2958 if sign is inverted, §9 Q3) |
| Lat/Long G aliases (`G_Lat`,`G_Long`) | latG / longG keys (§6.4) | g ← … | see §9 Q3 axis mapping | — |
| Water/oil temp aliases | `"engine_temp"` / `"engine_oil_temp"` | C ← C | 1 | 0 |
| `Tyre_Pressure_FL..RR` | `"tire_pressure[0..3]"` | psi ← kPa | **0.1450377** (same as AMS2) | 0 |
| `Brake_Temp_*_FL..RR` | `"brake_temp[0..3]"` | C ← C | 1 | 0 |
| `Tyre_Temp_Avg_FL..RR` | `"tireAvgTemp[i]"` (C++-computed mean) | C ← C | 1 | 0 |
| `Tyre_Temp_{FL,FR,RL,RR}_{Inner,Middle,Outer}` | position-mapped keys per the tyre-temp mapping note in §6.3 (LEFT=outer front / inner rear) | C ← C | 1 | 0 |
| Ride height aliases (`Ride_Height_FL..RR`) | `"ride_height[0..3]"` | mm ← **m** | **1000** (AMS2 uses cm→mm ×10; R3E is meters) | 0 |
| `Damper_Velocity_FL..RR` | `"damperVelocity[0..3]"` | mm/s ← m/s | **1000** (identical pattern to AMS2's `mSuspensionVelocity` rows, mapping.json ~line 189; PMR uses f=1 because it pre-computes in C++) | 0 |
| `Fuel_Level_Ratio` | `"fuelLevelRatio"` — **C++ pre-computed** = `fuel_left / fuel_capacity`, emitted only when capacity > 0 (PMR pattern) | ratio ← pre-computed | 1 | 0 |
| `Tyre_Wear_FL..RR` | `"tireWear[0]"` … `[3]` = raw `tire_wear[i]`(0–1; key omitted on −1 N/A, §6.6) | native (ratio) ← native — same "native" sourceUnit convention as the AMS2/EVO wear rows | 1 | 0 |
| `Suspension_Travel_FL..RR` | `"suspensionDeflection[0]"` … `[3]` = raw `suspension_deflection[i]`(m) | mm ← m (AMS2/EVO convention; PMR pre-computes in C++) | **1000** | 0 — note: the R3E profile does NOT get a PMR-style damper dual-write (`kPmrDamperSinks` is `profile==Pmr` only); travel and velocity stay separate aliases, as for AMS2/EVO |
| `TCS_Active` / `ABS_Active` | `"tcsActive"` / `"absActive"` — **C++ pre-computed** from `aid_settings.tc/.abs`: r3e.h encodes 5 = "currently active" → emit the app-convention number (0/1 bool like AMS2, or 0/100 % like EVO+PMR), else 0 | see §9 Q7 for which convention to match | per Q7 | 0 — R3E has **no** runtime TCS/ABS state field beyond these aid-setting enums; the "5 = active" value is the only live signal available. If a first capture shows it never reaches 5, drop both rows (see coverage policy) and flag in PR |

> **Per-profile source-unit note:** AMS2 ride-height rows use `factor:10.0` because its raw unit is centimeters. R3E exposes **meters**, so the row must say `"sourceUnit":"m"`, `"unit":"mm"`, `"factor":1000`. This per-profile difference in source units is exactly what the table layer exists for — no code change, just correct rows.

### 7.1 Coverage policy — aliases intentionally NOT mapped (and why)
The goal is to map **every alias that has both a live sink and an R3E shared-memory source**. The following are deliberately excluded; adding LIVE rows for them would produce startup warnings (`validate()`) or fake data:

| Alias | Reason no R3E row |
|-------|-------------------|
| `Gear` (and any gear alias) | No live sink exists in the resolver dispatch maps — a row would warn at startup. Raw `"gear"` key is still emitted (§6.6) for future use. |
| `Clutch_Pos`, `Handbrake_Pos` | Same: no sinks exist today; raw keys may be kept dict-only if useful later (handbrake has **no** R3E field — don't emit it). |
| `Camber_FL..RR` | No live sink. Raw `camber[4]`(rad) is available in shared memory and can be emitted as a dict key for future use, but no row now. |
| Race/lap position aliases (`Race_Position`, etc.) | No sinks; R3E does expose `position`/`completed_laps` — keep out of the table until a sink exists (lap counter is consumed directly by §6.5). |
| `Engine_Oil_Pressure`, fuel/turbo pressure aliases | **No sinks exist** for any engine/fuel/turbo pressure alias in the resolver maps, even though R3E exposes kPa/Bar values — nothing to map into today. Revisit if a sink is added later. |
| `Track_Temp` / `Ambient_Temp` (sinks DO exist) | **No source field** anywhere in r3e.h v3.6 shared memory (verified: the only temperature fields are tyre, brake and engine/oil). Cannot map honestly — leave unmapped; these aliases simply stay empty for R3E laps, same as any profile missing a channel today. |
| `Bumpstop_Travel_FL..RR` | Sink exists but no EVO/AMS2/R3E LIVE rows exist anywhere in mapping.json (forward-compat sink only); R3E has no bumpstop-contact field to feed it anyway. |

Everything else with an existing live sink is covered by the table above: speed, wheel speeds ×4, RPM, throttle/brake %, steering, yaw, latG/longG, water/oil temp, tyre pressure/temp (avg + 12 shoulders) ×4 sets, brake temps ×4, ride heights ×4, damper velocities ×4, suspension travel ×4, fuel ratio, tyre wear ×4, TCS/ABS active (§9 Q7), and brake bias.

---

## 8. Build + test
- **`Project1.cbproj`:** add `RaceRoomLiveCapture.cpp` to DCCCompile; confirm repo root (where `r3e.h` lives) is on the include path. mapping.json copy rule already exists — no change needed there.
- **New [`scripts/test_r3e_feed.cpp`](../scripts)** mirroring [test_pmr_feed.cpp](../scripts): construct a synthetic `r3e_shared`, run it through the R3E table + sinks, and assert:
  - yaw conversion (−0.1 rad/s → −5.7296 deg/s),
  - tyre pressure kPa→psi (×0.1450377),
  - ride height m→mm (×1000),
  - tyre-temp L/C/R mapping lands on the correct `_Inner/_Middle/_Outer` aliases for each wheel side,
  - speed m/s→km/h (car + all four `Wheel_Speed_*`) and throttle/brake % scaling,
  - suspension travel m→mm (×1000) via `"suspensionDeflection[i]"`,
  - fuel ratio: capacity > 0 → `fuel_left/capacity` lands in the alias; capacity == 0 → key absent, no crash/no NaN sink value,
  - tyre wear pass-through with −1 sentinel (key omitted → alias untouched),
  - brake bias conversion: raw rear fraction 0.35 → **65** (% front); raw < 0 → default 50;
  - TCS/ABS active flag mapping per the §9 Q7 decision (aid value 5 → on-convention number, other values incl. −1 N/A → off).

---

## 9. Open questions / risks — resolve during implementation (flag in PR)

- **Q1a Torn-read robustness:** R3E has no sequence number; the ticks double-check is a heuristic (400 Hz tick can wrap between reads). Acceptable for telemetry, but weaker than AMS2's `mSequenceNumber`. Mitigation: sanity-bound key values (`car_speed`, temps) and drop frames that fail bounds checks.
- **Q1b Engine RPM unit:** confirm the app's engine-RPM alias expects true RPM (AMS2 feeds raw `mRpm`). If so, table factor = 95.4929 from RPS; if it wants rad/s elsewhere, adjust. Verify against how AMS2/EVO/PMR populate that alias.
- ~~**Q2 Damper velocity routing:**~~ **RESOLVED — not a decision.** The damper-velocity alias is table-routed for every sim (AMS2 `mSuspensionVelocity` rows ×1000; PMR pre-computed mm/s f=1). R3E does the same: raw `"damperVelocity[i]"`(m/s) keys + 4 mapping rows factor **1000** (§7, §6.6). AMS2's post-table `addSample(travel,time)` block is a separate travel-derived stat (deflection deltas over wall-clock), not an alternative velocity source; replicate it optionally from R3E `suspension_deflection[4]` for parity with AMS2's travel stats only if desired.
- **Q3 Yaw / G-force sign & axis conventions:** R3E local frame is +X left, +Y up, +Z back; yaw = angular_velocity.y. Verify positive-direction expectations against the app's understeer/oversteer logic on first real capture (PMR had the same VERIFY note). Plan default: use as-is; flip factor to −1 if a live session shows inverted behavior.
- **Q4 Version drift / struct size:** layout is forward-growing and we memcpy `sizeof(r3e_shared)`, so compile-time size must match runtime → pin r3e.h to the exact revision used (v3.6). Guard with `version_major==3` at open; read only 3.x-present fields so future minor bumps don't break behavior even if a newer header is vendored later.
- **Q5 Steering angle source:** use raw normalized `steer_input_raw`(−1..1) ×360 (AMS2-style), or compute true wheel angle = `steer_input_raw * steer_lock_degrees`? The latter yields a physically meaningful degree value; prefer it if the alias is used for correlation magnitude. Confirm which downstream consumers expect.
- **Q6 Brake-bias convention:** r3e.h documents `brake_bias` as a **rear** fraction (0–1, −1 N/A), while every other profile feeds this alias in % front. The plan therefore pre-computes `(1−raw)×100` in C++ with default 50 on N/A — verify against the first real capture that R3E really reports rear bias as documented (a mislabeled field would show a mirrored value, e.g. ~65 where you expect ~35).
- **Q7 TCS/ABS active numeric convention:** sinks accept pre-computed numbers ("0/1 AMS2, 0/100 EVO+PMR" per the resolver comment) and R3E's only live signal is `aid_settings.abs/tc == 5` (currently active). Decide at implementation time which scale to emit — check what consumes these aliases downstream; if a first capture shows value 5 never appears, drop both rows entirely.

---

## 10. Suggested sequencing & acceptance criteria
1. **Vendor header + capture-pair skeleton** (Open/Close/process-check) compiling against `Project1.cbproj`. *Accept: builds clean, no sim needed.*
2. **Raw-dict FeedLap + mapping.json rows**; get `test_r3e_feed.cpp` green at unit level (no sim running). *Accept: all §8 assertions pass.*
3. **Form wiring** — enum/LiveMode/connect/close/capture-thread/timer/lap-boundary; end-to-end with RRRE.exe open and a car on track. *Accept: live paint boxes update, laps finalize + report generates at each `completed_laps` increment.*
4. **Live verification pass:** confirm yaw/latG signs, tyre-temp L/C/R mapping, pressure & ride-height units against real telemetry; flip factors / axis mappings if needed and record the final values in §7. *Accept: a full session produces sane averaged lap stats with no torn-frame artifacts.*
