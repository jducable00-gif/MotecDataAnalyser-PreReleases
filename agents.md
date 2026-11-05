# Agent Instructions: Motec Data Analyser

> **Toolchain, build & debug:** see [`.ai/skills/AGENTS.md`](./.ai/skills/AGENTS.md).
> This file is the **source of truth for architecture and diagnostics**.

## 1. Data Flow (end-to-end)

```
Source file (CSV / LD-LDX) / Live capture (AMS2, AC EVO, PMR UDP)
        |
        v
+------------------------------------------------------+
| PARSE                                                |
|  parseMotecLd() -> DecodeMotecLd() (MotecToCSV.cpp)  |
|  Live: Ams2LiveCapture / AcsEvoLiveCapture           |
|        PmrLiveCapture (UDP, no file import)          |
|    each feed = raw dict + table-driven sink:         |
|      build std::unordered_map<string,double> of     |
|      RAW values -> LiveChannel::FeedLapFromTable()   |
|      applies mapping.json LIVE rows                 |
|      (v = raw*factor+offset) into LapStats          |
|  Output: std::vector<LapStats>                       |
+------------------------------------------------------+
        |
        v
+------------------------------------------------------+
| SELECT + AVERAGE                                     |
|  LapReportGenerator::selectValidLaps()               |
|  LapReportGenerator::averageSelectedLaps()           |
|  Output: averaged LapStats                           |
+------------------------------------------------------+
        |
        v
+------------------------------------------------------+
| ALIAS RESOLUTION + RULE EVALUATION                   |
|  buildDataProviderFromLap(lap)                       |
|    -> DataProvider (alias gateway + virtual channels)|
|  AnalysisRegistry::evaluateAll(provider, settings)   |
|    -> 4 inline rules (see section 3)                 |
|  Output: std::vector<AnalysisResult>                 |
+------------------------------------------------------+
        |
        v
+------------------------------------------------------+
| REPORT GENERATION                                    |
|  mapAnalysisResultToReportSection()                  |
|  LapReportGenerator::generate*()                     |
|  Output: ReportResult (map<ReportSection, SectionData>)|
+------------------------------------------------------+
        |
        v
+------------------------------------------------------+
| UI (MotecSetupForm)                                  |
|  UpdateReportTabs / UpdateTireTemps /                |
|  UpdateBrakeTemps / UpdateSuspension                 |
|  (GDI+ PaintBox rendering, no TChart)               |
+------------------------------------------------------+
```

## 2. Configuration Layer

### Files (all exe-relative)

| File | Purpose | Loaded by |
|------|---------|-----------|
| `MotecDataAnalyser.ini` | UI options: ReportLanguage, ChannelProfile, LastFolder | on start / on change |
| `MotecDataAnalyser.thresholds.ini` | Numeric thresholds (`g_thresholds`) + `[SymptomWeights]` overrides; auto-created if missing | at startup -> **restart required** after edits |
| `MotecDataAnalyser.rules.json` *(optional)* | Custom symptoms / aliases / weights (JSON). Fallback: `buildDefaults()` in code | at startup, before thresholds |
| `MotecDataAnalyser.mapping.json` | Channel alias mappings per profile (EvoM1, AMS2, generic) + virtual channels (Rake) | at startup, after rules |

### Load order (constructor of `TFormMotecSetup`)

```
1. loadRuleConfig()            -> rules.json  (fallback: buildDefaults())
2. loadChannelMappingConfig()  -> mapping.json (fallback: empty mappings)
3. loadThresholds()            -> thresholds.ini + [SymptomWeights] (overrides JSON weights)
```

Priority for cause weights: **INI `[SymptomWeights]` > rules.json > `buildDefaults()`**.

### RuleConfig (`RuleConfig.h/.cpp`) — singleton

- `instance()`, `load(path) -> RuleLoadStatus{Ok, FileNotFound, ParseError}`, `resetToDefaults()`
- `findRule(symptom)`, `rules()` / `rulesRef()`
- `resolveAlias(profile, key, outKey)` — generic first, then profile-specific
- `loadChannelMappings(path)` — parses `mapping.json` into `channelMappings()`
- `buildDefaults()` (line ~642) — 10 built-in symptoms with causes + weights

### DataProvider (`MotecSetupAnalyzer.h`)

- Alias gateway: `getValue(AnalysisAlias)` -> direct value or virtual-channel resolution (recursive, max depth 16)
- Virtual channels: `Rake` = difference of front/rear ride-height averages
- `setChannelMappings()` — injects the mapping table from RuleConfig
- `buildDataProviderFromLap(lap)` fills: water/oil temps, steer/yaw averages, wheel slip (LF->front, LR->rear), 4 pressures, 12 tyre temps, ride heights

## 3. Active Analysis Rules

Defined inline in `MotecSetupAnalyzer.h`, registered by `buildDefaultAnalysisRegistry()`:

| Rule | Category | Logic |
|------|----------|-------|
| `EngineTemperatureRule` | Engine/Cooling | Water/oil temp vs `waterTempMax`/`oilTempMax` -> Critical |
| `TyreTemperatureRule` | Tyre | Inner<=Outer -> camber; Middle vs shoulders -> pressure |
| `TyreSuspensionBalanceRule` | Tyres | Rear outer vs front outer temp diff → rear suspension too stiff (> threshold) or too soft (< 0 °C) |
| `HandlingBalanceRule` | Handling | Wheel slip vs max; steer/yaw combos -> under/oversteer |

Each rule implements `AnalysisRule`:
- `requiredInputs()` -> list of `AnalysisAlias`
- `evaluate(provider, settings)` -> `AnalysisResult{severity, category, title, description, recommendation}` (or empty if insufficient data)

The **only** active evaluation path:

```cpp
// analyze_motec_setup.cpp
evaluateLapWithRules(lap, settings) {
    DataProvider provider = buildDataProviderFromLap(lap);
    provider.setChannelMappings(RuleConfig::instance().channelMappings());
    const AnalysisRegistry registry = buildDefaultAnalysisRegistry();
    return registry.evaluateAll(provider, settings);
}
```

## 4. Damper-Velocity Detection (in `MotecSetupAnalyzer.h`)

Inline in the core analyzer header (previously `RecommendationEngine.h/.cpp`, now merged):

- `detail::evaluateDamperCorner(stats, cornerLabel, ...)` -> detects 3 issues:
  1. Bump/rebound asymmetry > threshold
  2. High-speed share > max
  3. Low-speed share high AND stdDev < max (suspension nearly static)
- `hasDamperIssue(stats)` = shorthand for LF corner

> **Note:** The structured `Recommendation` pipeline (`computeConfidence`, `dataQualityLabel`, `applyRuleToRecommendation`, `findSymptomRule`) was **removed** as dead code. The `CauseEntry` and `Recommendation` structs still exist in `MotecSetupAnalyzer.h` but are **not wired** to the active engine. They will be re-wired when the report engine is extended.

## 5. Source Map

| File | Role |
|------|------|
| `Project1.cpp` | `WinMain`, creates `TFormMotecSetup` |
| `MotecSetupForm.h/.cpp/.dfm` | Main VCL form: file dialogs, progress, report tabs, GDI+ paint boxes (tyre temps, suspension/damper), threshold INI load/save, live-capture wiring |
| `MotecSettingsForm.h/.cpp/.dfm` | Threshold editor dialog (6 tabs: General, Tyre, Handling, Thermal, Suspension, Brake). Reads/writes `g_thresholds` directly |
| `LiveChannelResolver.h/.cpp` | Table-driven live feed: `ResolvedChannelTable::build(profile)` resolves mapping.json LIVE rows into per-alias value sinks (`FeedLapFromTable(lap, rawDict, table, tSeconds)`); damper dual-write for PMR (springStrain → susTravel + damper addSample) attached at build time |
| `MotecSetupAnalyzer.h` | Core types: `LapStats`, `DataProvider`, `AnalysisAlias`, `ChannelMapping`, `AnalysisRule`, `AnalysisRegistry`, `AnalysisResult`, `AnalysisSettings`, `Thresholds` + `g_thresholds`, 4 inline rule classes, damper-velocity detection (`evaluateDamperCorner`, `hasDamperIssue`), `CauseEntry`/`Recommendation` (disabled pipeline) |
| `analyze_motec_setup.cpp` | LD/LDX parsing (`parseMotecLd`), `buildDataProviderFromLap`, `evaluateLapWithRules`, `mapAnalysisResultToReportSection`, `saveSymptomWeights`/`loadSymptomWeights`. File-import path still uses flat alias maps (not migrated — no silent-gap risk, see plan §11 Q6) |
| `RuleConfig.h/.cpp` | Singleton: symptom registry + channel alias mappings + JSON parser. `buildDefaults()`, `resolveAlias()`, `loadChannelMappings()` |
| `VehicleDb.h/.cpp` (+ `VehicleDbRow.h`, `VehicleDbBuckets.h`, 4× `VehicleDbData_*.cpp`) | **Layer 1** embedded AMS2 vehicle DB (507 cars). Plain C++ aggregate rows — one aligned line per car, split by letter range. Source JSON (`data/reference/ams2_vehicles_db.json`) is converted to `.cpp` tables by `scripts/json_to_vehicle_db.py`; no runtime file I/O or JSON parsing. Typed API: `Ams2VehicleDb::instance()` → `find(name, car)` / `vehicleCount()` / `allNames()`, exposing mass/CG + per-wheel brake temp ranges + engine temps |
| `LapReportGenerator.h/.cpp` | Lap selection/averaging, report section generation (`generate*`), `ReportResult` |
| `MotecToCSV.h/.cpp` | Binary LD/LDX decoder (`DecodeMotecLd`), Bresenham resampling, beacon markers, session metadata |
| `Ams2LiveCapture.h/.cpp` | AMS2 shared-memory: binary parse + data-quality guards only; raw dict → table feed (`FeedLapFromTable`). `<Sim>GetChannelTableWarning()` surfaces unresolved mapping rows at startup. Tyre-temp L/R swap stays in C++ (non-linear, by design) |
| `AcsEvoLiveCapture.h/.cpp` | AC EVO shared-memory: same raw-dict + table-feed pattern as AMS2; wheel-speed factor derived from `kWheelRadiusM`, TCS/ABS boolean→% pre-computed into the dict |
| `PmrLiveCapture.h/.cpp` | PMR UDP live capture (non-blocking socket, decodes RaceInfo/ParticipantState/VehicleTelemetry) -> raw dict → table feed. No file import; uses the `"PMR"` mapping.json profile (`ChannelProfile::Pmr`). Player id tracked from first `isPlayer==true` participant state (`PmrGetPlayerId()`) |
| `Ams2SharedMemory.h`, `AcsEvoSharedMemory.h` | Shared-memory layout structs |
| `Project1PCH1.h` | Precompiled header (`vcl.h`, `tchar.h`) |
| `Unit1.h/.cpp/.dfm` | Leftover default form — not the main UI |
| `MotecDataAnalyser.mapping.json` | Channel alias mappings (copied to build output by `Project1.cbproj`) |

## 6. Key Design Constraints

- **Simulator-agnostic**: rules use semantic aliases via `DataProvider`, never raw channel names. New simulators require only a new mapping profile in `mapping.json`.
- **Vehicle DB is AMS2-only** (`Ams2VehicleDb`): the embedded Layer 1 reference data covers Assetto Corsa Mod 2 vehicles exclusively. AC EVO / PMR have no equivalent table — code must guard with `has*` flags or skip vehicle-specific logic for those profiles.
- **Data-driven rules**: symptoms/aliases/weights come from `RuleConfig` (code defaults + optional JSON). No recompile to add a symptom.
- **Exe-relative paths**: all file I/O anchored to the executable directory (`AppDirPath()`), never absolute.
- **GDI+ rendering**: all charts are custom GDI+ in PaintBox. No TChart components.
- **`profile`** is accepted by `LapReportGenerator::generate()` but currently **ignored** — the report content is the same regardless.

## 7. Operational Guidelines

### When implementing a NEW analysis rule:
1. **Verify Aliases**: Check `RuleConfig.cpp` / `mapping.json` to ensure all required channels are mapped as semantic aliases. If not, add them first.
2. **Create Rule Class**: Inherit from `AnalysisRule` in `MotecSetupAnalyzer.h`. Implement `requiredInputs()` and `evaluate()`.
3. **Register**: Add the rule to `buildDefaultAnalysisRegistry()`.
4. **Ensure Localization**: Wrap all user-facing strings in `tr()` for English/French support.

### When debugging existing rules:
1. Trace from the **rule class** back to its `evaluate()` logic.
2. Verify that the `DataProvider` is receiving the correct alias values (check `buildDataProviderFromLap`).
3. Check if the threshold in `AnalysisSettings` matches the value in `g_thresholds` / `thresholds.ini`.

### When writing Unit Tests:
- Focus on **Alias Resolution** (ensuring mapping works via `DataProvider::getValue`).
- Focus on **Rule Logic** (verifying thresholds and `AnalysisResult` output).
- Focus on **Registry Integrity** (ensuring new rules don't break existing ones).

## 8. Reference Documents

| Document | Purpose |
|----------|---------|
| [`docs/DATA_MAPPING_SPEC.md`](./docs/DATA_MAPPING_SPEC.md) | Technical specification of the mapping layer and data flow |
| [`docs/NEW_ALERT_PROCEDURE.md`](./docs/NEW_ALERT_PROCEDURE.md) | Step-by-step guide for creating new diagnostic rules |
| [`docs/TUNING_RECOMMENDATIONS.md`](./docs/TUNING_RECOMMENDATIONS.md) | Telemetry-based setup checks & warnings (decision aid, not universal values) |
| [`docs/channels/Channel_Correspondence.md`](./docs/channels/Channel_Correspondence.md) | Cross-reference table AC EVO <-> AMS2 channel names (+ per-sim channel inventories in the same folder) |
| [`docs/RELEASE_PROCEDURE.md`](./docs/RELEASE_PROCEDURE.md) | Agent-facing release procedure: version bump → build → two-file dist commit (private `release` + public `-Releases`) → publish GitHub Release with exe asset on the **public** repo only → unauthenticated feed verification. The app's self-update reads ONLY releases of the public `jducable00-gif/MotecDataAnalyser-Releases` |
| [`.ai/skills/AGENTS.md`](./.ai/skills/AGENTS.md) | Toolchain, build & debug reference |
