# Implementation Plan: Live Data Export to MoTeC LD/LDX Format

## Objective
Extend the existing live telemetry capture system to not only calculate lap statistics but also buffer high-frequency raw samples. These buffered samples will be exported into binary `.ld` (data) and `.ldx` (metadata/beacons) files when a report is generated, allowing users to open their captured sessions in official MoTeC i2 software for deeper analysis.

## Architecture Overview
The current system captures raw data from simulators via `LiveCapture` classes, resolves them through a mapping table into `LapStats`, and generates reports. To support LD/LDX export:
1. **Data Accumulation**: Instead of only keeping the latest lap stats, we must buffer high-frequency samples for all mapped channels during the session.
2. **Binary Encoding**: Implement an encoder that reverses the logic found in `MotecToCSV.cpp` to write binary blocks and headers.
3. **File Organization**: Save files into profile-specific subfolders (e.g., `/Exports/AMS2/`, `/Exports/PMR/`).

## Detailed Technical Steps

## Detailed Technical Steps

### 1. Data Buffering Layer (The Session Recorder)
Currently, `LiveChannelResolver` uses "Sinks" (e.g., `SinkSpeed`) to feed values into `LapStats` members via `.add(v)`. This process is statistical—it accumulates data for averaging and reporting but discards the individual high-frequency samples.

To support LD export, implement a **SessionRecorder** singleton or persistent member in `TFormMotecSetup`:
- **Buffer Structure**: A map of channel aliases to vectors: `std::map<std::string, std::vector<double>>`.
- **Integration Point**: Modify the Sinks in `LiveChannelResolver.cpp` (or wrap them) so that every value passed to a sink is also pushed into the `SessionRecorder` buffer if "Generate MoTeC File" is enabled.
- **Lifecycle**: 
    - **Start**: Clear buffers when a new session starts or recording is toggled ON.
    - **Capture**: Append samples continuously across all laps in the session.
    - **Export**: Trigger binary write during report generation, then clear/reset for next use.

### 2. LD Binary Encoder Implementation (The Blueprint)
Based on `MotecToCSV.cpp`, the encoder must follow this exact layout:

#### A. The Header (Fixed Offsets)
Create a buffer of at least $1636$ bytes. Write strings as NUL-terminated sequences:
- **Offset 158**: Driver Name (max $64$ chars).
- **Offset 222**: Vehicle Name (max $128$ chars).
- **Offset 350**: Track Name (max $128$ chars).

#### B. The Block Structure
For each channel in the `SessionRecorder` buffers, create a block:
1. **Block Pointer**: A list of pointers at the start of the file pointing to these blocks.
2. **Block Header** ($32+$ bytes):
    - Offset $0$: Next Block Pointer (U32).
    - Offset $4$: Data Start offset (U32).
    - Offset $8$: Sample Count (U32).
    - Offset $12$: Frequency (I16) - *Use the actual sampling rate of the sim feed*.
    - Offset $18$: Type (U16) - *Type 7 for Float, others for scaled integers*.
    - Offset $20$: Entry Size (U16) - *$2$ bytes for I16, $4$ bytes for U32/Float*.
    - Offset $30$: Decimal Shift (I16).
    - Offset $32$: Channel Name ($32$ chars, NUL-terminated).
    - Offset $72$: Unit ($8$ chars, NUL-terminated).

#### C. Data Payload
Write the `std::vector<double>` values using the specified `EntrySize`. If Type is not 7 (Float), multiply by $10^{\text{decimalShift}}$ and cast to integer before writing.

### 3. LDX XML Generation
The `.ldx` file must be a valid XML containing beacon markers for lap transitions:
- **Format**: `<Beacons><Beacon Time="[timestamp_in_microseconds]" /></Beacons>`
- **Source**: Use the timestamps recorded during `FeedLapFromTable` when a new lap is detected.

### 4. Export Trigger & File System
The export should trigger when `LapReportGenerator` is called while in Live Mode:
- **Path Logic**: Root folder `/Exports/`.
- **Subfolders**: Create subdirectories based on the active `ChannelProfile` (e.g., `/Exports/AMS2/`).
- **Naming Convention**: `[Vehicle]_[Track]_[SessionType]_YYYYMMDD_HHMM.ld` and `.ldx`.

### 4. Integration with UI (`MotecSetupForm`)
- Add a checkbox or toggle in the main UI (MotecSetupForm) labeled **"Generate MoTeC File"**. 
- When enabled, the system will activate the Session Recorder to buffer high-frequency samples. If disabled, only statistical accumulation for reports occurs, saving memory and CPU.
- Provide visual feedback (e.g., a small recording icon) when data is being buffered to avoid unexpected memory growth during very long sessions.

## Verification Plan
1. **Binary Integrity**: Generate an LD file and attempt to load it using `DecodeMotecLd` in the existing codebase; verify that values match original captures.
2. **MoTeC i2 Compatibility**: Open the generated `.ld`/`.ldx` pair in MoTeC i2 software to ensure headers and channels are recognized correctly.
3. **Performance Impact**: Monitor CPU/Memory usage during live capture with recording enabled to ensure no frame drops or lag in the simulator feed.

## Potential Risks & Mitigations
- **Memory Usage**: Based on typical MoTeC data density ($\sim 20\text{ MB}$ per $35$-$40$ minutes of recording), a full $45$-minute race will likely consume less than $100\text{ MB}$ of RAM. This is well within acceptable limits for modern systems, so periodic flushing to disk is not required.
- **Frequency Mismatch**: Simulators provide data at varying rates.
    - *Mitigation*: Use the `frequency` field in the LD block header to accurately describe the sampled rate of each channel.