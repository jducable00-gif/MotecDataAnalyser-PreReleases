//---------------------------------------------------------------------------
// MotecSetupForm.h — Déclaration de la fenêtre principale (TFormMotecSetup).
//
// Form VCL qui orchestre l'analyse : sélection des fichiers LD/LDX,
// options (rapport complet, langue, profil de canaux), barre de progression
// et affichage du rapport dans un TMemo. Les fonctions d'analyse elles-mêmes
// sont déclarées dans MotecSetupAnalyzer.h.
//---------------------------------------------------------------------------

#pragma once

#include <System.Classes.hpp>
#include <Vcl.Controls.hpp>
#include <Vcl.StdCtrls.hpp>
#include <Vcl.Forms.hpp>
#include <Vcl.ExtCtrls.hpp>
#include <Vcl.Graphics.hpp>

#include "MotecSetupAnalyzer.h"
#include <gdiplus.h>   // Gdiplus::Graphics used by DrawCombinedDamperChart signature
#include <Vcl.ComCtrls.hpp>
#include <Vcl.WinXCtrls.hpp>
#include <Vcl.ExtCtrls.hpp>
#include "Ams2LiveCapture.h"
#include "AcsEvoLiveCapture.h"
#include "PmrLiveCapture.h"
#include "LapReportGenerator.h"
#include "VehicleDb.h"    // Ams2VehicleDb -- per-wheel brake fade thresholds (AMS2 only)
#include "HttpServer.h"   // Phase B: embedded tablet report server
#include "ReleaseInfo.h"  // kAppVersion + GitHub release notes fetch (WinHTTP)
#include <atomic>
#include <mutex>
#include <thread>
#include <map>

// Active live-capture backend.
enum LiveMode { LIVE_AMS2 = 0, LIVE_EVO = 1, LIVE_PMR = 2 };

// [ReleaseInfo] Feeds de self-update : stable par defaut, pre-release en option.
namespace ReleaseFeed
{
	const char kOwner[]          = "jducable00-gif";
	const char kStableRepo[]     = "MotecDataAnalyser-Releases";
	const char kPreReleaseRepo[] = "MotecDataAnalyser-PreReleases";
}

class TFormMotecSetup : public TForm
{
__published:
    TButton *ButtonLoad;
    TButton *ButtonSettings;
    TPageControl *PageControlReport;
    TTabSheet *TabSummary;
    TTabSheet *TabGeneral;
    TTabSheet *TabTyre;
    TTabSheet *TabBrake;
    TTabSheet *TabSuspension;
    TTabSheet *TabEngineCooling;
    TTabSheet *TabHandling;
    TMemo *MemoSummary;
    TMemo *MemoGeneral;
    TMemo *MemoTyre;
    TMemo *MemoBrake;
    TMemo *MemoSuspension;
    TMemo *MemoEngineCooling;
    TMemo *MemoHandling;
	TTabSheet *TabDiagnostics;
	TMemo     *MemoDiagnostics;

	TStatusBar *StatusBar1;
	TButton *Button1;
    TProgressBar *ProgressBarExport;    TComboBox *ChannelProfileCombo;
    TLabel *LabelProfile;
    TLabel *LabelLanguage;
    TPaintBox *TireTempPaintBox;
    TPageControl *DamperLayoutTabs;
    TTabSheet *TabDamperGrid;
    TTabSheet *TabDamperCombined;
    TPaintBox *DamperFLPaintBox;
    TPaintBox *DamperFRPaintBox;
    TPaintBox *DamperRLPaintBox;
    TPaintBox *DamperRRPaintBox;
    TPaintBox *DamperCombinedPaintBox;
    TCheckBox *LiveCaptureCheckBox;
    TLabel *LabelLiveStatus;
    TShape *LiveLed;
    TTimer *LiveTimer;
    TTimer *LiveConnectTimer;
    TPaintBox *BrakeTempPaintBox;
    TComboBox *LanguageCombo;
    TCheckBox *AutoDetectTrackCheckBox;
    TButton *ButtonResetSession;
    TButton *ButtonAbout;
    TTimer  *ReleasePollTimer;   // worker -> UI handoff for the async release fetch (1 s poll)

    void __fastcall ButtonLoadClick(TObject *Sender);
	void __fastcall Button1Click(TObject *Sender);
    void __fastcall ButtonSettingsClick(TObject *Sender);
    void __fastcall LanguageComboChange(TObject *Sender);
    void __fastcall ChannelProfileComboChange(TObject *Sender);
    void __fastcall FormResize(TObject *Sender);
    void __fastcall TireTempPaintBoxPaint(TObject *Sender);
    void __fastcall DamperFLPaintBoxPaint(TObject *Sender);
    void __fastcall DamperFRPaintBoxPaint(TObject *Sender);
    void __fastcall DamperRLPaintBoxPaint(TObject *Sender);
    void __fastcall DamperRRPaintBoxPaint(TObject *Sender);
    void __fastcall DamperCombinedPaintBoxPaint(TObject *Sender);
    void __fastcall DamperLayoutTabsChange(TObject *Sender);
    void __fastcall BrakeTempPaintBoxPaint(TObject *Sender);
    void __fastcall LiveCaptureCheckBoxClick(TObject *Sender);
    void __fastcall LiveTimerTimer(TObject *Sender);
    void __fastcall LiveConnectTimerTimer(TObject *Sender);

    // DFM-referenced handlers must stay in __published so VCL streaming can resolve them by name.
    void __fastcall ButtonResetSessionClick(TObject *Sender);
    void __fastcall AutoDetectTrackCheckBoxClick(TObject *Sender);
    void __fastcall ButtonAboutClick(TObject *Sender);
    void __fastcall ReleasePollTimerTimer(TObject *Sender);

    // Phase 4 — Before/After comparison mode (DFM-referenced, keep in __published).



public:
    struct SuspensionCornerData {
        double bins[41] = {0}; // normalized 0-1 damper velocity distribution
        std::size_t totalCount = 0; // total sample count for this corner (for avg computation)
        double travel = 0.0;   // average suspension travel (mm)
        double bumpstop = 0.0; // max bumpstop engagement (mm)
        bool issue = false;    // damper evaluation issue
        bool valid = false;    // has data
    };

    __fastcall TFormMotecSetup(TComponent* Owner);
    ~TFormMotecSetup();

    // Internal helpers (NOT VCL event handlers — must stay out of __published).
    void captureThreadFunc();          // dedicated capture thread worker
    void __fastcall resetLiveSession();// stop + clear live state, ready for a fresh session
    void __fastcall updateResetButtonState();  // enable/disable ButtonResetSession based on liveActive
    void __fastcall TranslateUI();     // apply language to all static labels
    // OnClose handler: performs all resource cleanup (live capture, tablet server)
    // while child controls are still alive. The destructor itself must NOT touch
    // any control because VCL destroys components before the dtor body runs.
    void __fastcall DoFormClose(TObject *Sender, TCloseAction &Action);

    static ReportLanguage ReadReportLanguageSetting();
    bool __fastcall AnalyzeSelectedLdFile(const std::string& filePath, std::string& errorMessage);
    void __fastcall resetAnalysisGraphics();
    void __fastcall stopLiveCapture(bool isShuttingDown = false);
    void __fastcall processLiveFrame(const Ams2SharedMemory& frame);
    // physUsable=false: the physics page failed plausibility this tick — skip feeding its values into lap data only; all gfx-based detection (pit/track state, line crossing) runs normally.
    void __fastcall processLiveFrameEVO(const SPageFilePhysics& phys, const SPageFileGraphicEvo& gfx, bool physUsable = true);
    void __fastcall processLiveFramePmir();

    // Frame-sync adaptive poll interval + FPS surfacing (plan §6). Call once per accepted NewFrame.
    // simFpsHint: authoritative sim-reported fps when available (>0), else 0 to use measured arrival rate only.
    void __fastcall updateLiveAdaptiveInterval(double simFpsHint);

    // packetId jump diagnostic (AC EVO): reset tokens/counters on connect + session reset; call once per accepted NewFrame with each page's token.
    void resetEvoPacketJumpTracking();
    void checkEvoPacketJump(const char* label, int id, bool isPhys);
    void __fastcall finalizeLiveLap();
    void __fastcall generateLiveReport();
    void __fastcall UpdateReportTabs(const ReportResult& result);
    void __fastcall DrawModernHistogram(TCanvas* canvas, int x, int y, int width, int height, const SuspensionCornerData& stats, const String& label);
    void __fastcall DrawCombinedDamperChart(Gdiplus::Graphics& graphics, int width, int height);

    // Internal update helpers (NOT VCL event handlers — must stay out of __published).
    void __fastcall UpdateTireTemps(const std::vector<LapStats>& laps);
    void __fastcall UpdateBrakeTemps(const std::vector<LapStats>& laps);
    void __fastcall UpdateSuspension(std::vector<LapStats>& laps);

    // --- Release info (version + GitHub release notes) ---------------------
    // The async fetch runs on a detached worker thread that only writes module-
    // static state inside ReleaseInfo.cpp. UI updates are picked up by
    // ReleasePollTimer (1 s, main thread) via takePendingRelease(). No mutex or
    // atomic is needed here: m_latestRelease is touched from the UI thread only.
    AppRelease  m_latestRelease{};      // last known release (INI cache, then network)
    bool        m_hasCachedRelease = false;   // true once INI or network provided a release

    TForm*  m_aboutForm = nullptr;      // open About dialog (to refresh in place), else null
    TMemo*  m_aboutMemo = nullptr;
	TButton* m_aboutUpdateBtn = nullptr;

	// Self-update state — worker thread <-> UI via atomics only.
	std::atomic<int> m_updateState{0};   // 0 idle / 1 downloading / 2 restarting / 3 failed
	std::atomic<long long> m_downloadReceived{0};
	std::atomic<long long> m_downloadTotal{-1};

    void __fastcall loadReleaseInfoCache();   // [ReleaseInfo] INI section -> m_latestRelease
    void __fastcall applyReleaseFeedChange(bool preReleaseNow);   // [ReleaseInfo] re-fetch apres changement de feed dans les parametres
    void __fastcall saveReleaseInfoCache(const AppRelease& rel);
    String __fastcall buildAboutText() const;  // FR/EN formatted version + notes (UI thread only)
    void __fastcall refreshAboutFormIfOpen();  // re-render memo if the About dialog is visible
	void __fastcall startSelfUpdate(TButton* progressBtn = NULL);  // telechargement + echange exe (bouton ou popup)
	void __fastcall promptForAvailableUpdate();   // Yes/No au demarrage si une release .exe plus recente existe
	void __fastcall UpdateButtonClick(TObject *Sender);
	void __fastcall AboutDialogClosing(TObject *Sender, System::Uitypes::TCloseAction &Action);

	// Anti-spam du popup : tag deja propose ce lancement + tags refuses (persiste INI).
	std::string m_updatePromptTag;      // vide = pas encore propose cette session
	std::string m_lastFailedTag;        // anti-spam de l alerte d echec : une seule boite par release en echec
	std::string m_declinedUpdateTags[8];   // versions refusees par l utilisateur (INI [ReleaseInfo] Declined*)

    // Phase B5: push the freshly generated report to any connected tablet. No-op when server is off.
    void __fastcall publishToTabletServer(const ReportResult& result, const std::vector<LapStats>& laps);
void __fastcall pushLiveStatusToTablet(const std::string& vehicleName,
                                   const std::string& trackLocation, int lapCount);

    // Phase D3: live start/stop/restart the tablet web server from Settings dialog (no app restart).
    void ApplyWebServerSettings();

    struct TireTempData {
        double inner = 0.0;
        double middle = 0.0;
        double outer = 0.0;
        double pressure = 0.0;
        bool pressureValid = false;
        bool valid = false;
    };
    TireTempData tireTemps[4]; // 0=FL, 1=FR, 2=RL, 3=RR
    bool hasTireTempData = false;

    struct BrakeTempData {
        double avg = 0.0;
        double max = 0.0;
        bool valid = false;
    };
    BrakeTempData brakeTemps[4]; // 0=FL, 1=FR, 2=RL, 3=RR
    bool hasBrakeTempData = false;

    // Per-wheel fade threshold (deg C) from the embedded AMS2 vehicle DB.
    // valid[i] == true only when the current car is known AND its wheel entry
    // carries a FadeRangeAbove value. Filled in UpdateBrakeTemps().
    double brakeFadeThresholdC[4];   // 0=FL, 1=FR, 2=RL, 3=RR (kWheelFl..kWheelRr order)
    bool brakeFadeValid[4] = { false, false, false, false };

    struct FluidTempData {
        double avg = 0.0;
        double max = 0.0;
        bool valid = false;
    };
    FluidTempData waterTemp;
    FluidTempData oilTemp;

    double aeroBalanceValue = 0.0;
    bool aeroBalanceValid = false;

    SuspensionCornerData suspensionData[4]; // 0=LF, 1=RF, 2=LR, 3=RR
    double suspensionMaxBin = 0.0;          // shared Y-axis max (highest bin across all 4 corners)

    // --- Live capture state ---
    bool liveActive = false;
    bool liveConnected = false;
    bool liveInPit = false;

    // --- Dedicated Capture Thread (Performance Fix) ---
    std::thread* captureThread = nullptr;
    std::atomic<bool> stopCaptureThread{false};
    std::mutex frameMutex;
    struct CapturedFrame {
        union {
            struct {   // AC EVO (LIVE_EVO)
                SPageFilePhysics phys;
                SPageFileGraphicEvo gfx;
                bool physUsable;
            } evo;
            Ams2SharedMemory ams;  // AMS2 (LIVE_AMS2) — POD, copied whole from shared memory
        };
        double timestampMs = 0.0;

        // SPageFilePhysics has NSDMIs → non-trivial default ctor, so the union's implicit
        // default constructor is deleted (bcc32c: "variant field 'evo' has a non-trivial
        // default constructor"). A user-provided body suppresses that rule. Both capture
        // branches assign every member before push_back, so an empty body is safe — we just
        // need the type to be default-constructible for std::vector's reallocation/copy path.
        CapturedFrame() {}
    };
    std::vector<CapturedFrame> frameBuffer; // Queue of frames to be processed by UI thread

    LiveMode liveMode = LIVE_AMS2;
    bool liveLapInProgress = false;
    LapStats liveCurrentLap;
    std::vector<LapStats> liveLaps;

    int liveLapNumber = 0;
    double liveLapStartTime = 0.0;
    bool liveWasOnTrack = false;
    std::string liveTrackName;

    // --- PMR State Tracking (for event detection) ---
    int pmrPrevSector = -1;         // last known sector for the player vehicle (-1 = unknown / no state yet)

    // --- AC EVO State Tracking (for event detection) ---
    float liveEvoPrevNPos = -1.0f;  // previous npos (normalized track position, wraps ~0->1 per lap); -1 = not on track yet
    bool  liveEvoPitReportDone = false; // prevents repeated generateLiveReport() while parked in pits
    int32_t prevCurrentLapEvo = -1;   // previous gfx.current_lap for restart detection

    // --- AMS2 State Tracking (for event detection) ---
    int lastPitMode = -1;           // To detect pit exit and teleport transitions
    unsigned int lastCompletedLaps = 0; // The value from the game's mCompletedLaps field
    unsigned int sessionLapCounter = 0;  // Our internal counter that resets on pit entry/exit
	float previous_mCurrentTime = 0.0;
    bool firstLapStarted = false;

    // --- Frame-sync adaptive interval + FPS display (plan §6) ---
    double    liveLastNewFrameMs   = 0.0; // GetTickCount64() ms of last accepted NewFrame (for Hz measurement); 0 = none yet
    double    liveMeasuredHz       = 0.0; // EMA-smoothed capture rate in frames/sec (alpha=0.1)
    int32_t   liveEvoFpsAvg        = 0;   // last-seen AC EVO player_fps_avg for status display / comparison
    int       liveAdaptiveCounter  = 0;   // accepted-frame counter used to throttle interval updates (~every 64 frames)
    __int64     liveEvoTornReads      = 0;   // diagnostic: count of frames discarded due to torn reads

    // --- packetId jump diagnostic (AC EVO): a token advancing by >1 between two ACCEPTED reads means at least one sim write was overwritten before we sampled it (= missed frame on our side). Steady +1 steps prove the reader keeps up and any slowness is in the sim. Messages throttled to 1/s; counters feed the session summary line.
    int       liveEvoLastPhysId    = -1;   // last accepted physics packetId (-1 = none yet this connection)
    int       liveEvoLastGfxId     = -1;   // last accepted graphics packetId
    __int64   liveEvoPhysJumps     = 0;    // total missed physics writes since connect/reset
    __int64   liveEvoGfxJumps      = 0;    // total missed graphics writes since connect/reset
    double    liveEvoJumpMsgMs     = 0.0;  // GetTickCount64() ms of last jump message (throttle)

    // --- Live session counters (plan §10a: "Collection rate" summary line) ---
    __int64   liveFrameCount       = 0;     // total accepted NewFrames since the current collection started
    double    liveSessionStartMs   = 0.0;   // GetTickCount64() ms when this collection's first frame was seen (for "over S s"); 0 = none yet

    // --- Tablet Report Server (Phase B) ---
    TabletReportServer _tabletServer{8765};   // default port; overridden by INI at startup
    int tabletVersion_ = 1;                   // incremented per analysis pass, embedded in JSON version field
};

extern PACKAGE TFormMotecSetup *FormMotecSetup;
