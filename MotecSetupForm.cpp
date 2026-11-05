//---------------------------------------------------------------------------
// MotecSetupForm.cpp — Implémentation de la fenêtre principale.
//
// Contient :
//   - Persistance des réglages dans MotecDataAnalyser.ini (dossiers récents,
//     rapport complet, langue, profil de canaux) via TIniFile.
//   - Chargement / création des seuils d'analyse dans
//     MotecDataAnalyser.thresholds.ini (loadThresholds / saveDefaultThresholds).
//   - Callbacks de progression (UpdateProgress / AnalysisProgress / ExportProgress).
//   - Gestion des boutons : Button1 (LD/LDX), qui appelle
//     parseMotecLd puis buildAverageLapReport.
//---------------------------------------------------------------------------

#include <vcl.h>
#pragma hdrstop

#include <System.UITypes.hpp>
#include <winsock2.h>

#include <System.SysUtils.hpp>
#include <IniFiles.hpp>
#include <shlobj.h>
#include <shobjidl.h>
#include <userenv.h>
#pragma comment(lib, "userenv")
#include <string>
#include <vector>

#include <thread>
#include "MotecSetupForm.h"
#include "MotecToCSV.h"
#include "RuleConfig.h"
#include "ReportJsonWriter.h"
#include "MotecSettingsForm.h"
#include <Vcl.Dialogs.hpp>   // TThread (worker -> UI marshalling)
#include <gdiplus.h>


#pragma comment(lib, "gdiplus.lib")

// NOTE: The GDI+ token and its startup/shutdown now live in _tWinMain() (Project1.cpp),
// not here. Starting/stopping GDI+ inside this form's lifetime caused an access violation
// on exit ("Abnormal program termination").

// ===============================

// Symptom weight INI persistence (defined in analyze_motec_setup.cpp)
void saveSymptomWeights(TIniFile* ini);
void loadSymptomWeights(TIniFile* ini);


#pragma package(smart_init)
#pragma resource "*.dfm"

TFormMotecSetup *FormMotecSetup;

// ===============================
// High-resolution wall clock for live-feed timestamps.
// GetTickCount64() has only ~15 ms resolution (Windows 64 Hz timer tick), which
// collapses multiple game frames into one timestamp and compresses derived
// velocities toward zero in the damper histograms. QueryPerformanceCounter gives
// microsecond precision so each poll gets a unique, accurate Î”t matching the true
// per-frame interval (~3–5 ms at 270 Hz sim rate).
// ===============================

static double HighResClockSeconds()
{
    static LARGE_INTEGER freq = {0};
    if (freq.QuadPart == 0)
        QueryPerformanceFrequency(&freq);
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (double)now.QuadPart / (double)freq.QuadPart;
}

// ===============================
// Chemins de base (Étape 2 — portabilité).
// Tous les fichiers d'application sont résolus relativement à l'exécutable,
// pour que le dossier portable fonctionne n'importe où il est décompressé.
// ===============================

String AppDirPath()
{
	return IncludeTrailingPathDelimiter(ExtractFilePath(Application->ExeName));
}

String DataPath()
{
	// Dossiers d'échantillons : <exedir>data\ (paquet portable) sinon le dossier courant.
	String p = AppDirPath() + "data\\";
	if (!DirectoryExists(p))
		p = ExtractFilePath(Application->ExeName);
	return IncludeTrailingPathDelimiter(p);
}

String SettingsFilePath()
{
	return AppDirPath() + "MotecDataAnalyser.ini";
}

// ===============================
// Release info helpers (version + GitHub release notes).
// The async fetch callback runs on a worker thread: it only touches the
// mutex/atomic-guarded members, never VCL controls. UI updates are picked up
// by ReleasePollTimer (1 s) which marshals them onto the main thread.
// ===============================
// Convertit un std::string UTF-8 en String VCL (UTF-16).
// Routine native du RTL — System.hpp ligne 1674 :
//   UnicodeString __fastcall UTF8ArrayToString(const char *S, const NativeInt S_High)
static String Utf8ToWideString(const std::string& utf8)
{
	if (utf8.empty()) return "";
	// S_High = index du dernier octet (0-based), soit size()-1.
	return UTF8ArrayToString(utf8.data(), static_cast<System::NativeInt>(utf8.size() - 1));
}

// Convertit un String VCL (UTF-16) en std::string UTF-8.
// Routine native du RTL — System.hpp ligne 1658 :
//   RawByteString __fastcall UTF8Encode(const UnicodeString US)
static std::string WideStringToUtf8(const String& w)
{
	if (w.IsEmpty()) return "";
	RawByteString utf8 = UTF8Encode(w);   // bytes UTF-8 ; le buffer interne vit tant que utf8 est vivant
	const char* p = static_cast<const char*>(utf8.c_str());
	return std::string(p, static_cast<std::size_t>(utf8.Length()));
}

String DefaultMotecFolder()
{
	// "Saved Games" is a known folder: Windows returns the localized name
	// ("Saved Games" / "Jeux enregistrés" / ...) for the current user.
	String savedGames;
	PWSTR path = NULL;
	if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_SavedGames, 0, NULL, &path)))
	{
		savedGames = String(path);
		CoTaskMemFree(path);
	}
	if (savedGames.IsEmpty())
	{
		// Fallback: user profile directory + literal "Saved Games".
		DWORD size = 0;
		if (GetUserProfileDirectory(0, NULL, &size) && size > 0)
		{
			std::vector<WCHAR> buffer(size);
			if (GetUserProfileDirectory(0, &buffer[0], &size))
				savedGames = String(&buffer[0]) + "\\Saved Games";
		}
	}
	if (savedGames.IsEmpty())
		return "";
	String folder = savedGames + "\\ACE\\MoTec";
	return DirectoryExists(folder) ? folder : "";
}

String ReadLastFolder(const String& section, const String& fallbackSection = "")
{
	TIniFile* settings = new TIniFile(SettingsFilePath());
	try {
		String folder = settings->ReadString(section, "LastFolder", "");
		if (!folder.IsEmpty() && DirectoryExists(folder))
			return folder;
		if (!fallbackSection.IsEmpty()) {
			folder = settings->ReadString(fallbackSection, "LastFolder", "");
			if (!folder.IsEmpty() && DirectoryExists(folder))
				return folder;
		}
		String def = DefaultMotecFolder();
		if (!def.IsEmpty())
			return def;
	} __finally {
		delete settings;
	}
	return ExtractFilePath(Application->ExeName);
}

String LdSectionForProfile(ChannelProfile profile)
{
	return (profile == ChannelProfile::EvoM1) ? "LdImport_Evo" : "LdImport_AMS2";
}

// Map a channel-profile selection to the matching live-capture backend.
LiveMode ProfileToLiveMode(ChannelProfile profile)
{
	if (profile == ChannelProfile::Pmr)   return LIVE_PMR;
	if (profile == ChannelProfile::EvoM1) return LIVE_EVO;
	return LIVE_AMS2; // AMS2 and Generic both use the AMS2 shared-memory backend
}

void SaveLastFolder(const String& section, const String& folder)
{
	TIniFile* settings = new TIniFile(SettingsFilePath());
	try {
		settings->WriteString(section, "LastFolder", folder);
	} __finally {
		delete settings;
	}
}

ReportLanguage TFormMotecSetup::ReadReportLanguageSetting()
{
	TIniFile* settings = new TIniFile(SettingsFilePath());
	try {
		int value = settings->ReadInteger("General", "ReportLanguage", (int)ReportLanguage::English);
		if (value == (int)ReportLanguage::French)
			return ReportLanguage::French;
		return ReportLanguage::English;
	} __finally {
		delete settings;
	}
}

void SaveReportLanguageSetting(ReportLanguage language)
{
	TIniFile* settings = new TIniFile(SettingsFilePath());
	try {
		settings->WriteInteger("General", "ReportLanguage", (int)language);
	} __finally {
		delete settings;
	}
}

ChannelProfile ReadChannelProfileSetting()
{
	TIniFile* settings = new TIniFile(SettingsFilePath());
	try {
		int value = settings->ReadInteger("General", "ChannelProfile", (int)ChannelProfile::EvoM1);
		if (value == (int)ChannelProfile::Generic)
			return ChannelProfile::Generic;
		if (value == (int)ChannelProfile::Pmr)
			return ChannelProfile::Pmr;
		return ChannelProfile::EvoM1;
	} __finally {
		delete settings;
	}
}


// Convert a UTF-8 byte string (simulator shared memory / UDP) to VCL UnicodeString.
static String liveNameUtf8(const std::string& s)
{
	if (s.empty())
		return String("");
	wchar_t* wide = new wchar_t[s.size() + 1];
	try {
		int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), wide, (int)(s.size() + 1));
		if (n <= 0)
			return String(s.data()); // fallback: raw bytes as best effort
		wide[n] = L'\u0000';
		return String(wide);
	} __finally {
		delete[] wide;
	}
}

// Localized labels for the live-summary name lines.
static String liveNameLabel(bool french, bool vehicle)
{
	if (french)
		return vehicle ? String(L"V\u00e9hicule : ") : String("Circuit : ");
	return vehicle ? String("Vehicle: ") : String("Circuit: ");
}
// PMR UDP port: persisted as [PMR] Port, default 7576 (the game's documented default).
int ReadPmrPortSetting()
{
	TIniFile* settings = new TIniFile(SettingsFilePath());
	try {
		int value = settings->ReadInteger("PMR", "Port", 7576);
		if (value < 1 || value > 65535)
			value = 7576;
		return value;
	} __finally {
		delete settings;
	}
}

void SavePmrPortSetting(int port)
{
	TIniFile* settings = new TIniFile(SettingsFilePath());
	try {
		settings->WriteInteger("PMR", "Port", port);
	} __finally {
		delete settings;
	}
}

// Tablet web server settings: persisted as [WebServer] Enabled/Port in the main
// UI-options INI (same file/location pattern as PMR). Default port 8765.
int ReadWebServerPort()
{
	TIniFile* settings = new TIniFile(SettingsFilePath());
	try {
		int value = settings->ReadInteger("WebServer", "Port", 8765);
		if (value < 1024 || value > 65535)
			value = 8765;
		return value;
	} __finally {
		delete settings;
	}
}

void SaveWebServerPort(int port)
{
	TIniFile* settings = new TIniFile(SettingsFilePath());
	try {
		settings->WriteInteger("WebServer", "Port", port);
	} __finally {
		delete settings;
	}
}

bool ReadWebServerEnabled()
{
	TIniFile* settings = new TIniFile(SettingsFilePath());
	try {
		return settings->ReadInteger("WebServer", "Enabled", 0) == 1;
	} __finally {
		delete settings;
	}
}

void SaveWebServerEnabled(bool enabled)
{
	TIniFile* settings = new TIniFile(SettingsFilePath());
	try {
		settings->WriteInteger("WebServer", "Enabled", enabled ? 1 : 0);
	} __finally {
		delete settings;
	}
}

// Main window geometry: persisted as [Window] Left/Top/Width/Height so the app
// reopens at its previous size and on the same monitor in multi-screen setups.
void SaveWindowGeometry(TForm* form)
{
	if (form == NULL || form->BorderStyle != bsSizeable || form->WindowState != wsNormal) // skip maximized/minimized states
		return;
	TIniFile* settings = new TIniFile(SettingsFilePath());
	try {
		settings->WriteInteger("Window", "Left",   form->Left);
		settings->WriteInteger("Window", "Top",    form->Top);
		settings->WriteInteger("Window", "Width",  form->Width);
		settings->WriteInteger("Window", "Height", form->Height);
	} __finally {
		delete settings;
	}
}

void RestoreWindowGeometry(TForm* form)
{
	if (form == NULL || form->BorderStyle != bsSizeable)
		return;
	TIniFile* settings = new TIniFile(SettingsFilePath());
	int left, top, width, height;
	try {
		left   = settings->ReadInteger("Window", "Left", 0);
		top    = settings->ReadInteger("Window", "Top", 0);
		width  = settings->ReadInteger("Window", "Width", 0);
		height = settings->ReadInteger("Window", "Height", 0);
	} __finally {
		delete settings;
	}
	if (width < 400 || height < 300) // missing/corrupt entry -> keep poScreenCenter default
		return;

	TRect rect(left, top, left + width, top + height);

	// Check the saved position against connected monitors via Win32. The VCL indexed
	// Monitors property does not compile cleanly in BCC, so use MonitorFromPoint with
	// MONITOR_DEFAULTTONULL on the window center: it returns NULL when that point is
	// outside every physical monitor (e.g. a secondary screen was disconnected), which
	// triggers primary-screen re-centering below.
	bool onVisibleMonitor = true; // default trust if MonitorFromPoint fails for any reason
	HMONITOR hMon = MonitorFromPoint(POINT{left + width / 2, top + height / 2}, MONITOR_DEFAULTTONULL);
	if (hMon == NULL)
		onVisibleMonitor = false;
	if (onVisibleMonitor) {
		form->Left   = left;
		form->Top    = top;
		form->Width  = width;
		form->Height = height;
	} else {
		// Saved monitor was disconnected -> fall back to centering on the primary screen.
		Vcl::Forms::TMonitor* prim = Screen->PrimaryMonitor; // indexed property in this VCL version
		TRect workArea = prim ? prim->WorkareaRect : TRect(0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN));
		form->Left = (workArea.Right - workArea.Left - width) / 2 + workArea.Left;
		form->Top  = (workArea.Bottom - workArea.Top - height) / 2 + workArea.Top;
	}
}

// Best-effort primary LAN IPv4 address for the status-bar URL (B4).
static String getLocalLanIp()
{
	char namebuf[256];
	if (::gethostname(namebuf, sizeof(namebuf)) != 0)
		return L"localhost";

	struct hostent* he = ::gethostbyname(namebuf);
	if (he == NULL || he->h_addr_list[0] == NULL)
		return L"localhost";

	char buf[INET_ADDRSTRLEN];
	const struct in_addr addr = *(reinterpret_cast<const struct in_addr*>(he->h_addr_list[0]));
	// Windows WinSock inet_ntop() returns PCSTR (NULL on failure), unlike POSIX int.
	if (::inet_ntop(AF_INET, &addr, buf, sizeof(buf)) != NULL && strcmp(buf, "127.0.0.1") != 0)
		return String(buf);

	return L"localhost";
}

// PMR UDP bind address: persisted as [PMR] BindAddress. Empty string = INADDR_ANY (all interfaces).
String ReadPmrBindAddressSetting()
{
	TIniFile* settings = new TIniFile(SettingsFilePath());
	try {
		return settings->ReadString("PMR", "BindAddress", "");
	} __finally {
		delete settings;
	}
}

void SavePmrBindAddressSetting(const String& addr)
{
	String trimmed = addr.Trim();
	TIniFile* settings = new TIniFile(SettingsFilePath());
	try {
		settings->WriteString("PMR", "BindAddress", trimmed);
	} __finally {
		delete settings;
	}
}

void SaveChannelProfileSetting(ChannelProfile profile)
{
	TIniFile* settings = new TIniFile(SettingsFilePath());
	try {
		settings->WriteInteger("General", "ChannelProfile", (int)profile);
	} __finally {
		delete settings;
	}
}

// Damper histogram layout: 0=Grid, 1=Combined. Persisted as [General] HistogramLayout.
int ReadHistogramLayoutSetting()
{
	TIniFile* settings = new TIniFile(SettingsFilePath());
	try {
		return settings->ReadInteger("General", "HistogramLayout", 0);
	} __finally {
		delete settings;
	}
}

void SaveHistogramLayoutSetting(int index)
{
	TIniFile* settings = new TIniFile(SettingsFilePath());
	try {
		settings->WriteInteger("General", "HistogramLayout", index);
	} __finally {
		delete settings;
	}
}

void SaveAutoDetectTrackSetting(bool enabled)
{
	TIniFile* settings = new TIniFile(SettingsFilePath());
	try {
		settings->WriteInteger("General", "AutoDetectTrack", enabled ? 1 : 0);
	} __finally {
		delete settings;
	}
}

bool ReadAutoDetectTrackSetting()
{
	TIniFile* settings = new TIniFile(SettingsFilePath());
	try {
		return settings->ReadInteger("General", "AutoDetectTrack", 1) != 0;   // default: checked
	} __finally {
		delete settings;
	}
}

bool ReadLiveCaptureSetting()
{
	TIniFile* settings = new TIniFile(SettingsFilePath());
	try {
		// Default: off (0) — do not auto-start capture on launch unless previously enabled
		return settings->ReadInteger("General", "LiveCapture", 0) != 0;
	} __finally {
		delete settings;
	}
}

void SaveLiveCaptureSetting(bool enabled)
{
	TIniFile* settings = new TIniFile(SettingsFilePath());
	try {
		settings->WriteInteger("General", "LiveCapture", enabled ? 1 : 0);
	} __finally {
		delete settings;
	}
}
// ===============================
// Thresholds configuration
// ===============================

// ---------------------------------------------------------------------------
// OnClose handler — the ONLY place where resource cleanup happens.
// VCL calls this while every child control is still alive (before DestroyComponents).
// The destructor below must NOT touch any control because by then they are freed.
// ---------------------------------------------------------------------------
void __fastcall TFormMotecSetup::DoFormClose(TObject *Sender, TCloseAction &Action)
{
	// Stop the release-info poll timer before teardown. The detached fetch worker
	// only writes module-static state in ReleaseInfo.cpp, so it is safe to let it
	// finish on its own without joining here.
	if (ReleasePollTimer != NULL) ReleasePollTimer->Enabled = false;

	// Persist window geometry before anything else so the next launch restores it.
	SaveWindowGeometry(this);

    // 0. Detach all PaintBox OnPaint handlers BEFORE any cleanup begins.
    // If a timer fires mid-teardown and triggers WM_PAINT on a control that is
    // being destroyed, the handler dereferences freed memory → AV / terminate().
    if (TireTempPaintBox)        TireTempPaintBox->OnPaint = NULL;
    if (BrakeTempPaintBox)       BrakeTempPaintBox->OnPaint = NULL;
    if (DamperFLPaintBox)        DamperFLPaintBox->OnPaint = NULL;
    if (DamperFRPaintBox)        DamperFRPaintBox->OnPaint = NULL;
    if (DamperRLPaintBox)        DamperRLPaintBox->OnPaint = NULL;
    if (DamperRRPaintBox)        DamperRRPaintBox->OnPaint = NULL;
    if (DamperCombinedPaintBox)  DamperCombinedPaintBox->OnPaint = NULL;

    // 1. Signal capture thread and stop timers to prevent new UI events or data pushes
    stopCaptureThread = true;
    if (LiveTimer != NULL) LiveTimer->Enabled = false;
    if (LiveConnectTimer != NULL) LiveConnectTimer->Enabled = false;

    // 2. Perform a strict sequential shutdown of capture resources
    stopLiveCapture(true);
    // 3. Stop the tablet server (HTTP + Winsock) while VCL is still fully alive.
    if (_tabletServer.isRunning()) _tabletServer.stop();
}

// ---------------------------------------------------------------------------
// Destructor — intentionally minimal.
// By the time this runs, VCL has already called DestroyComponents() and every
// child control (timers, buttons, memos) is freed. Touching them here caused an
// access violation ("Abnormal program termination") on exit. All real cleanup
// lives in DoFormClose above. We only set a flag so any stray thread can see it.
// ---------------------------------------------------------------------------
TFormMotecSetup::~TFormMotecSetup()
{
	// CRITICAL: Do NOT access ANY member variables in this destructor body.
	// By the time VCL calls our C++ destructor, DestroyComponents() has already
	// run and a pending WM_TIMER can corrupt atomic members mid-destruction.
	// All real cleanup (thread join, timer disable, tablet server stop) is done
	// in DoFormClose above — which runs while the form object is fully alive.
}

String ThresholdsFilePath()
{
	return AppDirPath() + "MotecDataAnalyser.thresholds.ini";
}

// ===============================
// Rule configuration (JSON) — Étape 1 du plan de développement.
// Les règles de symptômes et les alias de canaux vivent dans RuleConfig ;
// un fichier JSON optionnel permet de les personnaliser sans recompiler.
// ===============================

String RulesFilePath()
{
	return AppDirPath() + "MotecDataAnalyser.rules.json";
}

void loadRuleConfig()
{
	String appDir = AppDirPath(); // avec séparateur final

	// 1) À côté de l'exécutable.
	String path = appDir + "MotecDataAnalyser.rules.json";
	if (!FileExists(path)) {
		// 2) Sous-répertoire config\ (paquet portable).
		path = appDir + "config\\MotecDataAnalyser.rules.json";
	}

	RuleLoadStatus status;
	if (FileExists(path)) {
		UTF8String utf8 = path;
		std::string stdPath = utf8.c_str();
		status = RuleConfig::instance().load(stdPath);
		// FileNotFound / ParseError : les valeurs par défaut intégrées restent en place.
	} else {
		RuleConfig::instance().resetToDefaults();
	}
}

void loadChannelMappingConfig()
{
    String appDir = AppDirPath();
    String path = appDir + "MotecDataAnalyser.mapping.json";
    if (!FileExists(path))
        path = appDir + "config\\MotecDataAnalyser.mapping.json";

    if (FileExists(path)) {
        UTF8String utf8 = path;
        RuleConfig::instance().loadChannelMappings(utf8.c_str());
    } else {
        RuleConfig::instance().loadChannelMappings("");
    }
}

void saveDefaultThresholds()
{
	TIniFile* ini = new TIniFile(ThresholdsFilePath());
	try {
		// General
		ini->WriteInteger("General", "MinDamperSamples", g_thresholds.minDamperSamples);
		ini->WriteInteger("General", "MinHandlingSamples", g_thresholds.minHandlingSamples);
		ini->WriteInteger("General", "MinSteerYawSamples", g_thresholds.minSteerYawSamples);
		ini->WriteInteger("General", "HandlingConfidenceSamples", g_thresholds.handlingConfidenceSamples);

		// Pressure (psi)
        ini->WriteFloat("Pressure", "Low", g_thresholds.pressureLowPsi);
        ini->WriteFloat("Pressure", "High", g_thresholds.pressureHighPsi);
		ini->WriteFloat("Pressure", "SideDiffMax", g_thresholds.pressureSideDiffMax);

		// Tyre temp spread (C) — per axle
		ini->WriteFloat("TyreTemp", "CamberMinSpreadFront", g_thresholds.camberMinSpreadFront);
		ini->WriteFloat("TyreTemp", "CamberMaxSpreadFront", g_thresholds.camberMaxSpreadFront);
		ini->WriteFloat("TyreTemp", "CamberMinSpreadRear",  g_thresholds.camberMinSpreadRear);
		ini->WriteFloat("TyreTemp", "CamberMaxSpreadRear",  g_thresholds.camberMaxSpreadRear);
		ini->WriteFloat("TyreTemp", "CentreLrDiffMax", g_thresholds.tyreCentreTempLrDiffMax);
		ini->WriteFloat("TyreTemp", "AirLrDiffMax", g_thresholds.tyreAirTempLrDiffMax);
          ini->WriteFloat("TyreTemp", "RearOuterVsFrontStiffC", g_thresholds.rearOuterVsFrontStiffC);

		// LSD
		ini->WriteFloat("LSD", "HighEvo", g_thresholds.lsdHighEvo);
		ini->WriteFloat("LSD", "LowEvo", g_thresholds.lsdLowEvo);
		ini->WriteFloat("LSD", "HighAms2", g_thresholds.lsdHighAms2);
		ini->WriteFloat("LSD", "LowAms2", g_thresholds.lsdLowAms2);

		// Damper
		ini->WriteFloat("Damper", "LowSpeedThreshold", g_thresholds.damperLowSpeedThreshold);
		ini->WriteFloat("Damper", "AsymmetryThreshold", g_thresholds.damperAsymmetryThreshold);
		ini->WriteFloat("Damper", "HighSpeedShareMax", g_thresholds.damperHighSpeedShareMax);
		ini->WriteFloat("Damper", "LowSpeedShareMin", g_thresholds.damperLowSpeedShareMin);
		ini->WriteFloat("Damper", "LowSpeedStdDevMax", g_thresholds.damperLowSpeedStdDevMax);
		ini->WriteFloat("Damper", "HistogramYOffset", g_thresholds.histogramYOffset);
		ini->WriteFloat("Damper", "HistogramMinYPct", g_thresholds.histogramMinYPct);

		// Handling
		ini->WriteFloat("Handling", "BrakePedalMin", g_thresholds.brakePedalMin);
		ini->WriteFloat("Handling", "ThrottleMaxBraking", g_thresholds.throttleMaxBraking);
		ini->WriteFloat("Handling", "ThrottleMinAccelerating", g_thresholds.throttleMinAccelerating);
		ini->WriteFloat("Handling", "SteerMinCornering", g_thresholds.steerMinCornering);
		ini->WriteFloat("Handling", "LatGMinCornering", g_thresholds.latGMinCornering);
		ini->WriteFloat("Handling", "SteerChangeMin", g_thresholds.steerChangeMin);
		ini->WriteFloat("Handling", "YawChangeMin", g_thresholds.yawChangeMin);
		ini->WriteFloat("Handling", "LatGMaxUndersteer", g_thresholds.latGMaxUndersteer);
		ini->WriteFloat("Handling", "YawMinOversteer", g_thresholds.yawMinOversteer);
		ini->WriteFloat("Handling", "SteerChangeMaxOversteer", g_thresholds.steerChangeMaxOversteer);
		ini->WriteFloat("Handling", "UndersteerRatioMin", g_thresholds.understeerRatioMin);
		ini->WriteFloat("Handling", "OversteerRatioMin", g_thresholds.oversteerRatioMin);

		// PhaseSegmentation
		ini->WriteFloat("PhaseSegmentation", "phaseLatGMaxStraight", g_thresholds.phaseLatGMaxStraight);
		ini->WriteFloat("PhaseSegmentation", "phaseLongGMinAccel", g_thresholds.phaseLongGMinAccel);
		ini->WriteFloat("PhaseSegmentation", "phaseSteerStableMax", g_thresholds.phaseSteerStableMax);
		ini->WriteFloat("PhaseSegmentation", "phaseLatGMidMin", g_thresholds.phaseLatGMidMin);
		ini->WriteFloat("PhaseSegmentation", "phaseExitSteerDrop", g_thresholds.phaseExitSteerDrop);
		ini->WriteFloat("PhaseSegmentation", "phaseExitLatGMax", g_thresholds.phaseExitLatGMax);
		ini->WriteFloat("PhaseSegmentation", "phaseBrakeDecelPeakMin", g_thresholds.phaseBrakeDecelPeakMin);
		ini->WriteFloat("PhaseSegmentation", "phaseSpeedDropPerSec", g_thresholds.phaseSpeedDropPerSec);
		ini->WriteFloat("PhaseSegmentation", "phaseLayoutBiasMinRatio", g_thresholds.phaseLayoutBiasMinRatio);

		// Thermal
		ini->WriteFloat("Thermal", "WaterTempMax", g_thresholds.waterTempMax);
		ini->WriteFloat("Thermal", "OilTempMax", g_thresholds.oilTempMax);

		// Suspension
		ini->WriteFloat("Suspension", "TravelMax", g_thresholds.suspensionTravelMax);
		ini->WriteFloat("Suspension", "RideHeightDiffMax", g_thresholds.rideHeightDiffMax);
		ini->WriteFloat("Suspension", "VelocityMax", g_thresholds.suspensionVelocityMax);
		ini->WriteFloat("Suspension", "AvgTravelFrontMax", g_thresholds.suspensionAvgTravelFrontMax);
		ini->WriteFloat("Suspension", "AvgTravelRearMax", g_thresholds.suspensionAvgTravelRearMax);

		// Brake
		ini->WriteFloat("Brake", "TempDiffMax", g_thresholds.brakeTempDiffMax);
		ini->WriteFloat("Brake", "BiasMax", g_thresholds.brakeBiasMax);
		ini->WriteFloat("Brake", "BiasFrontAvgMin", g_thresholds.brakeBiasFrontAvgMin);
		ini->WriteFloat("Brake", "TorqueMin", g_thresholds.brakeTorqueMin);
		ini->WriteFloat("Brake", "ChannelMismatchMax", g_thresholds.brakeChannelMismatchMax);
		ini->WriteFloat("Brake", "TorqueFrontShareMax", g_thresholds.brakeTorqueFrontShareMax);
		ini->WriteFloat("Brake", "TorqueFrontShareMin", g_thresholds.brakeTorqueFrontShareMin);
		ini->WriteFloat("Brake", "TorqueLrDiffRatio", g_thresholds.brakeTorqueLrDiffRatio);
		ini->WriteFloat("Brake", "BiasSettingFrontMax", g_thresholds.brakeBiasSettingFrontMax);
		ini->WriteFloat("Brake", "BiasSettingRearMin", g_thresholds.brakeBiasSettingRearMin);

		// Channel mismatch
		ini->WriteFloat("ChannelMismatch", "PressureMax", g_thresholds.pressureChannelMismatchMax);
		ini->WriteFloat("ChannelMismatch", "PressureMin", g_thresholds.pressureChannelMismatchMin);
		ini->WriteFloat("ChannelMismatch", "SuspensionMax", g_thresholds.suspensionChannelMismatchMax);
		ini->WriteFloat("ChannelMismatch", "SuspensionMin", g_thresholds.suspensionChannelMismatchMin);

		// TCS / ABS
		ini->WriteFloat("TCS", "ActiveMax", g_thresholds.tcsActiveMax);
		ini->WriteFloat("ABS", "ActiveMax", g_thresholds.absActiveMax);

		// Wheel slip
		ini->WriteFloat("WheelSlip", "Max", g_thresholds.wheelSlipMax);

		// Wheel speed imbalance
		ini->WriteFloat("WheelSpeed", "FrontDiffMax", g_thresholds.wheelSpeedFrontDiffMax);
		ini->WriteFloat("WheelSpeed", "RearDiffMax", g_thresholds.wheelSpeedRearDiffMax);
		ini->WriteFloat("WheelSpeed", "TempDiffMax", g_thresholds.wheelSpeedTempDiffMax);

		// Traction loss
		ini->WriteFloat("Traction", "RearDiffMax", g_thresholds.tractionRearDiffMax);
		ini->WriteFloat("Traction", "RearTempDiffMax", g_thresholds.tractionRearTempDiffMax);
		ini->WriteFloat("Traction", "LongGMax", g_thresholds.tractionLongGMax);
		ini->WriteFloat("Traction", "FrontDiffMax", g_thresholds.tractionFrontDiffMax);
		ini->WriteFloat("Traction", "RearDiffMin", g_thresholds.tractionRearDiffMin);
		ini->WriteFloat("Traction", "RearTempSpreadMax", g_thresholds.tractionRearTempSpreadMax);
		ini->WriteFloat("Traction", "RearPressureSpreadMax", g_thresholds.tractionRearPressureSpreadMax);

		// Bumpstop
		ini->WriteFloat("Bumpstop", "Min", g_thresholds.bumpStopMin);

		// Aero
		ini->WriteFloat("Aero", "BalanceMax", g_thresholds.aeroBalanceMax);
		ini->WriteFloat("Aero", "HeightDiffMax", g_thresholds.aeroHeightDiffMax);
		ini->WriteFloat("Aero", "SpeedMin", g_thresholds.aeroSpeedMin);

		// Steering
		ini->WriteFloat("Steering", "CorrUndersteer", g_thresholds.steerYawCorrUndersteer);
		ini->WriteFloat("Steering", "CorrOversteer", g_thresholds.steerYawCorrOversteer);
		ini->WriteFloat("Steering", "SpeedLowMax", g_thresholds.speedLowMax);
		ini->WriteFloat("Steering", "SpeedMediumMax", g_thresholds.speedMediumMax);

		// Stability
		ini->WriteFloat("Stability", "ScoreMin", g_thresholds.stabilityScoreMin);
		ini->WriteFloat("Stability", "CorrUndersteer", g_thresholds.stabilityCorrUndersteer);
		ini->WriteFloat("Stability", "CorrOversteer", g_thresholds.stabilityCorrOversteer);
		ini->WriteFloat("Stability", "WheelSpeedDiffMax", g_thresholds.stabilityWheelSpeedDiffMax);
		ini->WriteFloat("Stability", "PressureDiffMax", g_thresholds.stabilityPressureDiffMax);
		ini->WriteFloat("Stability", "TempDiffMax", g_thresholds.stabilityTempDiffMax);

		// Lap
		ini->WriteFloat("Lap", "TimeGapMax", g_thresholds.lapTimeGapMax);
		ini->WriteFloat("Lap", "MinValidTimeMult", g_thresholds.minValidLapTimeMultiplier);
		ini->WriteFloat("Lap", "MaxValidTimeMult", g_thresholds.maxValidLapTimeMultiplier);

		// Confidence scoring
		ini->WriteFloat("Confidence", "Low", g_thresholds.confidenceLow);
		ini->WriteFloat("Confidence", "Medium", g_thresholds.confidenceMedium);
		ini->WriteFloat("Confidence", "High", g_thresholds.confidenceHigh);
		ini->WriteInteger("Confidence", "MinLapsForReproducibility", g_thresholds.minLapsForReproducibility);

		// Symptom cause weights (Phase F)
		saveSymptomWeights(ini);
	} __finally {
		delete ini;
	}
}

void loadThresholdsPart1(TIniFile* ini)
{
	// General
	g_thresholds.minDamperSamples = ini->ReadInteger("General", "MinDamperSamples", g_thresholds.minDamperSamples);
	g_thresholds.minHandlingSamples = ini->ReadInteger("General", "MinHandlingSamples", g_thresholds.minHandlingSamples);
	g_thresholds.minSteerYawSamples = ini->ReadInteger("General", "MinSteerYawSamples", g_thresholds.minSteerYawSamples);
	g_thresholds.handlingConfidenceSamples = ini->ReadInteger("General", "HandlingConfidenceSamples", g_thresholds.handlingConfidenceSamples);

	// Pressure (psi)
    g_thresholds.pressureLowPsi = ini->ReadFloat("Pressure", "Low",
        ini->ReadFloat("Pressure", "LowEvo", ini->ReadFloat("Pressure", "LowAms2", g_thresholds.pressureLowPsi)));
    g_thresholds.pressureHighPsi = ini->ReadFloat("Pressure", "High",
        ini->ReadFloat("Pressure", "HighEvo", ini->ReadFloat("Pressure", "HighAms2", g_thresholds.pressureHighPsi)));
	g_thresholds.pressureSideDiffMax = ini->ReadFloat("Pressure", "SideDiffMax", g_thresholds.pressureSideDiffMax);

	// Tyre temp spread (C) — per axle
	g_thresholds.camberMinSpreadFront = ini->ReadFloat("TyreTemp", "CamberMinSpreadFront", g_thresholds.camberMinSpreadFront);
	g_thresholds.camberMaxSpreadFront = ini->ReadFloat("TyreTemp", "CamberMaxSpreadFront", g_thresholds.camberMaxSpreadFront);
	g_thresholds.camberMinSpreadRear  = ini->ReadFloat("TyreTemp", "CamberMinSpreadRear",  g_thresholds.camberMinSpreadRear);
	g_thresholds.camberMaxSpreadRear  = ini->ReadFloat("TyreTemp", "CamberMaxSpreadRear",  g_thresholds.camberMaxSpreadRear);
	g_thresholds.tyreCentreTempLrDiffMax = ini->ReadFloat("TyreTemp", "CentreLrDiffMax", g_thresholds.tyreCentreTempLrDiffMax);
	g_thresholds.tyreAirTempLrDiffMax = ini->ReadFloat("TyreTemp", "AirLrDiffMax", g_thresholds.tyreAirTempLrDiffMax);
  g_thresholds.rearOuterVsFrontStiffC = ini->ReadFloat("TyreTemp", "RearOuterVsFrontStiffC", g_thresholds.rearOuterVsFrontStiffC);

	// LSD
	g_thresholds.lsdHighEvo = ini->ReadFloat("LSD", "HighEvo", g_thresholds.lsdHighEvo);
	g_thresholds.lsdLowEvo = ini->ReadFloat("LSD", "LowEvo", g_thresholds.lsdLowEvo);
	g_thresholds.lsdHighAms2 = ini->ReadFloat("LSD", "HighAms2", g_thresholds.lsdHighAms2);
	g_thresholds.lsdLowAms2 = ini->ReadFloat("LSD", "LowAms2", g_thresholds.lsdLowAms2);

	// Damper
	g_thresholds.damperLowSpeedThreshold = ini->ReadFloat("Damper", "LowSpeedThreshold", g_thresholds.damperLowSpeedThreshold);
	g_thresholds.damperAsymmetryThreshold = ini->ReadFloat("Damper", "AsymmetryThreshold", g_thresholds.damperAsymmetryThreshold);
	g_thresholds.damperHighSpeedShareMax = ini->ReadFloat("Damper", "HighSpeedShareMax", g_thresholds.damperHighSpeedShareMax);
	g_thresholds.damperLowSpeedShareMin = ini->ReadFloat("Damper", "LowSpeedShareMin", g_thresholds.damperLowSpeedShareMin);
	g_thresholds.damperLowSpeedStdDevMax = ini->ReadFloat("Damper", "LowSpeedStdDevMax", g_thresholds.damperLowSpeedStdDevMax);
	g_thresholds.histogramYOffset = ini->ReadFloat("Damper", "HistogramYOffset", g_thresholds.histogramYOffset);
	g_thresholds.histogramMinYPct   = ini->ReadFloat("Damper", "HistogramMinYPct", g_thresholds.histogramMinYPct);
	g_thresholds.damperSlowSpeedRange = ini->ReadFloat("Damper", "SlowSpeedRange", g_thresholds.damperSlowSpeedRange);

    // Damper 4-zone targets (%): single target per zone (band = target +/- tolerance).
    // Migrate from legacy Min/Max keys by taking their midpoint when the new key is absent.
    auto readZoneTarget = [&](const char* key, const char* minKey, const char* maxKey, double& field) {
        if (!ini->ValueExists("DamperZones", key)) {
            double mn = ini->ReadFloat("DamperZones", minKey, -1.0);
            double mx = ini->ReadFloat("DamperZones", maxKey, -1.0);
            field = (mn >= 0.0 && mx > mn) ? (mn + mx) / 2.0 : field;
        } else {
            field = ini->ReadFloat("DamperZones", key, field);
        }
    };
    readZoneTarget("HscTarget", "HscTargetMin", "HscTargetMax", g_thresholds.damperHscTarget);
    readZoneTarget("LscTarget", "LscTargetMin", "LscTargetMax", g_thresholds.damperLscTarget);
    readZoneTarget("LsrTarget", "LsrTargetMin", "LsrTargetMax", g_thresholds.damperLsrTarget);
    readZoneTarget("HsrTarget", "HsrTargetMin", "HsrTargetMax", g_thresholds.damperHsrTarget);
        // Per-zone tolerances (fallback to legacy ModerateBand for backward compat)
        g_thresholds.damperZoneToleranceHsc = ini->ValueExists("DamperZones", "ToleranceHsc") ? ini->ReadFloat("DamperZones", "ToleranceHsc", 5.0) : ini->ReadFloat("DamperZones", "ModerateBand", g_thresholds.damperZoneToleranceHsc);
        g_thresholds.damperZoneToleranceLsc = ini->ValueExists("DamperZones", "ToleranceLsc") ? ini->ReadFloat("DamperZones", "ToleranceLsc", 5.0) : ini->ReadFloat("DamperZones", "ModerateBand", g_thresholds.damperZoneToleranceLsc);
        g_thresholds.damperZoneToleranceLsr = ini->ValueExists("DamperZones", "ToleranceLsr") ? ini->ReadFloat("DamperZones", "ToleranceLsr", 5.0) : ini->ReadFloat("DamperZones", "ModerateBand", g_thresholds.damperZoneToleranceLsr);
        g_thresholds.damperZoneToleranceHsr = ini->ValueExists("DamperZones", "ToleranceHsr") ? ini->ReadFloat("DamperZones", "ToleranceHsr", 5.0) : ini->ReadFloat("DamperZones", "ModerateBand", g_thresholds.damperZoneToleranceHsr);

	// Handling
	g_thresholds.brakePedalMin = ini->ReadFloat("Handling", "BrakePedalMin", g_thresholds.brakePedalMin);
	g_thresholds.throttleMaxBraking = ini->ReadFloat("Handling", "ThrottleMaxBraking", g_thresholds.throttleMaxBraking);
	g_thresholds.throttleMinAccelerating = ini->ReadFloat("Handling", "ThrottleMinAccelerating", g_thresholds.throttleMinAccelerating);
	g_thresholds.steerMinCornering = ini->ReadFloat("Handling", "SteerMinCornering", g_thresholds.steerMinCornering);
	g_thresholds.latGMinCornering = ini->ReadFloat("Handling", "LatGMinCornering", g_thresholds.latGMinCornering);
	g_thresholds.steerChangeMin = ini->ReadFloat("Handling", "SteerChangeMin", g_thresholds.steerChangeMin);
	g_thresholds.yawChangeMin = ini->ReadFloat("Handling", "YawChangeMin", g_thresholds.yawChangeMin);
	g_thresholds.latGMaxUndersteer = ini->ReadFloat("Handling", "LatGMaxUndersteer", g_thresholds.latGMaxUndersteer);
	g_thresholds.yawMinOversteer = ini->ReadFloat("Handling", "YawMinOversteer", g_thresholds.yawMinOversteer);
	g_thresholds.steerChangeMaxOversteer = ini->ReadFloat("Handling", "SteerChangeMaxOversteer", g_thresholds.steerChangeMaxOversteer);
	g_thresholds.understeerRatioMin = ini->ReadFloat("Handling", "UndersteerRatioMin", g_thresholds.understeerRatioMin);
	g_thresholds.oversteerRatioMin = ini->ReadFloat("Handling", "OversteerRatioMin", g_thresholds.oversteerRatioMin);

	// PhaseSegmentation
	g_thresholds.phaseLatGMaxStraight   = ini->ReadFloat("PhaseSegmentation", "phaseLatGMaxStraight",   g_thresholds.phaseLatGMaxStraight);
	g_thresholds.phaseLongGMinAccel     = ini->ReadFloat("PhaseSegmentation", "phaseLongGMinAccel",     g_thresholds.phaseLongGMinAccel);
	g_thresholds.phaseSteerStableMax    = ini->ReadFloat("PhaseSegmentation", "phaseSteerStableMax",    g_thresholds.phaseSteerStableMax);
	g_thresholds.phaseLatGMidMin        = ini->ReadFloat("PhaseSegmentation", "phaseLatGMidMin",        g_thresholds.phaseLatGMidMin);
	g_thresholds.phaseExitSteerDrop     = ini->ReadFloat("PhaseSegmentation", "phaseExitSteerDrop",     g_thresholds.phaseExitSteerDrop);
	g_thresholds.phaseExitLatGMax       = ini->ReadFloat("PhaseSegmentation", "phaseExitLatGMax",       g_thresholds.phaseExitLatGMax);
	g_thresholds.phaseBrakeDecelPeakMin = ini->ReadFloat("PhaseSegmentation", "phaseBrakeDecelPeakMin", g_thresholds.phaseBrakeDecelPeakMin);
	g_thresholds.phaseSpeedDropPerSec   = ini->ReadFloat("PhaseSegmentation", "phaseSpeedDropPerSec",   g_thresholds.phaseSpeedDropPerSec);
	g_thresholds.phaseLayoutBiasMinRatio= ini->ReadFloat("PhaseSegmentation", "phaseLayoutBiasMinRatio",g_thresholds.phaseLayoutBiasMinRatio);
}

void loadThresholdsPart2(TIniFile* ini)
{
	// Thermal
	g_thresholds.waterTempMax = ini->ReadFloat("Thermal", "WaterTempMax", g_thresholds.waterTempMax);
	g_thresholds.oilTempMax = ini->ReadFloat("Thermal", "OilTempMax", g_thresholds.oilTempMax);

	// Suspension
	g_thresholds.suspensionTravelMax = ini->ReadFloat("Suspension", "TravelMax", g_thresholds.suspensionTravelMax);
	g_thresholds.rideHeightDiffMax = ini->ReadFloat("Suspension", "RideHeightDiffMax", g_thresholds.rideHeightDiffMax);
	g_thresholds.suspensionVelocityMax = ini->ReadFloat("Suspension", "VelocityMax", g_thresholds.suspensionVelocityMax);
	g_thresholds.suspensionAvgTravelFrontMax = ini->ReadFloat("Suspension", "AvgTravelFrontMax", g_thresholds.suspensionAvgTravelFrontMax);
	g_thresholds.suspensionAvgTravelRearMax = ini->ReadFloat("Suspension", "AvgTravelRearMax", g_thresholds.suspensionAvgTravelRearMax);

	// Brake
	g_thresholds.brakeTempDiffMax = ini->ReadFloat("Brake", "TempDiffMax", g_thresholds.brakeTempDiffMax);
	g_thresholds.brakeBiasMax = ini->ReadFloat("Brake", "BiasMax", g_thresholds.brakeBiasMax);
	g_thresholds.brakeBiasFrontAvgMin = ini->ReadFloat("Brake", "BiasFrontAvgMin", g_thresholds.brakeBiasFrontAvgMin);
	g_thresholds.brakeTorqueMin = ini->ReadFloat("Brake", "TorqueMin", g_thresholds.brakeTorqueMin);
	g_thresholds.brakeChannelMismatchMax = ini->ReadFloat("Brake", "ChannelMismatchMax", g_thresholds.brakeChannelMismatchMax);
	g_thresholds.brakeTorqueFrontShareMax = ini->ReadFloat("Brake", "TorqueFrontShareMax", g_thresholds.brakeTorqueFrontShareMax);
	g_thresholds.brakeTorqueFrontShareMin = ini->ReadFloat("Brake", "TorqueFrontShareMin", g_thresholds.brakeTorqueFrontShareMin);
	g_thresholds.brakeTorqueLrDiffRatio = ini->ReadFloat("Brake", "TorqueLrDiffRatio", g_thresholds.brakeTorqueLrDiffRatio);
	g_thresholds.brakeBiasSettingFrontMax = ini->ReadFloat("Brake", "BiasSettingFrontMax", g_thresholds.brakeBiasSettingFrontMax);
	g_thresholds.brakeBiasSettingRearMin = ini->ReadFloat("Brake", "BiasSettingRearMin", g_thresholds.brakeBiasSettingRearMin);

	// Channel mismatch
	g_thresholds.pressureChannelMismatchMax = ini->ReadFloat("ChannelMismatch", "PressureMax", g_thresholds.pressureChannelMismatchMax);
	g_thresholds.pressureChannelMismatchMin = ini->ReadFloat("ChannelMismatch", "PressureMin", g_thresholds.pressureChannelMismatchMin);
	g_thresholds.suspensionChannelMismatchMax = ini->ReadFloat("ChannelMismatch", "SuspensionMax", g_thresholds.suspensionChannelMismatchMax);
	g_thresholds.suspensionChannelMismatchMin = ini->ReadFloat("ChannelMismatch", "SuspensionMin", g_thresholds.suspensionChannelMismatchMin);

	// TCS / ABS
	g_thresholds.tcsActiveMax = ini->ReadFloat("TCS", "ActiveMax", g_thresholds.tcsActiveMax);
	g_thresholds.absActiveMax = ini->ReadFloat("ABS", "ActiveMax", g_thresholds.absActiveMax);

	// Wheel slip
	g_thresholds.wheelSlipMax = ini->ReadFloat("WheelSlip", "Max", g_thresholds.wheelSlipMax);

	// Wheel speed imbalance
	g_thresholds.wheelSpeedFrontDiffMax = ini->ReadFloat("WheelSpeed", "FrontDiffMax", g_thresholds.wheelSpeedFrontDiffMax);
	g_thresholds.wheelSpeedRearDiffMax = ini->ReadFloat("WheelSpeed", "RearDiffMax", g_thresholds.wheelSpeedRearDiffMax);
	g_thresholds.wheelSpeedTempDiffMax = ini->ReadFloat("WheelSpeed", "TempDiffMax", g_thresholds.wheelSpeedTempDiffMax);

	// Traction loss
	g_thresholds.tractionRearDiffMax = ini->ReadFloat("Traction", "RearDiffMax", g_thresholds.tractionRearDiffMax);
	g_thresholds.tractionRearTempDiffMax = ini->ReadFloat("Traction", "RearTempDiffMax", g_thresholds.tractionRearTempDiffMax);
	g_thresholds.tractionLongGMax = ini->ReadFloat("Traction", "LongGMax", g_thresholds.tractionLongGMax);
	g_thresholds.tractionFrontDiffMax = ini->ReadFloat("Traction", "FrontDiffMax", g_thresholds.tractionFrontDiffMax);
	g_thresholds.tractionRearDiffMin = ini->ReadFloat("Traction", "RearDiffMin", g_thresholds.tractionRearDiffMin);
	g_thresholds.tractionRearTempSpreadMax = ini->ReadFloat("Traction", "RearTempSpreadMax", g_thresholds.tractionRearTempSpreadMax);
	g_thresholds.tractionRearPressureSpreadMax = ini->ReadFloat("Traction", "RearPressureSpreadMax", g_thresholds.tractionRearPressureSpreadMax);

	// Bumpstop
	g_thresholds.bumpStopMin = ini->ReadFloat("Bumpstop", "Min", g_thresholds.bumpStopMin);

	// Aero
	g_thresholds.aeroBalanceMax = ini->ReadFloat("Aero", "BalanceMax", g_thresholds.aeroBalanceMax);
	g_thresholds.aeroHeightDiffMax = ini->ReadFloat("Aero", "HeightDiffMax", g_thresholds.aeroHeightDiffMax);
	g_thresholds.aeroSpeedMin = ini->ReadFloat("Aero", "SpeedMin", g_thresholds.aeroSpeedMin);

	// Steering
	g_thresholds.steerYawCorrUndersteer = ini->ReadFloat("Steering", "CorrUndersteer", g_thresholds.steerYawCorrUndersteer);
	g_thresholds.steerYawCorrOversteer = ini->ReadFloat("Steering", "CorrOversteer", g_thresholds.steerYawCorrOversteer);
	g_thresholds.speedLowMax = ini->ReadFloat("Steering", "SpeedLowMax", g_thresholds.speedLowMax);
	g_thresholds.speedMediumMax = ini->ReadFloat("Steering", "SpeedMediumMax", g_thresholds.speedMediumMax);

	// Stability
	g_thresholds.stabilityScoreMin = ini->ReadFloat("Stability", "ScoreMin", g_thresholds.stabilityScoreMin);
	g_thresholds.stabilityCorrUndersteer = ini->ReadFloat("Stability", "CorrUndersteer", g_thresholds.stabilityCorrUndersteer);
	g_thresholds.stabilityCorrOversteer = ini->ReadFloat("Stability", "CorrOversteer", g_thresholds.stabilityCorrOversteer);
	g_thresholds.stabilityWheelSpeedDiffMax = ini->ReadFloat("Stability", "WheelSpeedDiffMax", g_thresholds.stabilityWheelSpeedDiffMax);
	g_thresholds.stabilityPressureDiffMax = ini->ReadFloat("Stability", "PressureDiffMax", g_thresholds.stabilityPressureDiffMax);
	g_thresholds.stabilityTempDiffMax = ini->ReadFloat("Stability", "TempDiffMax", g_thresholds.stabilityTempDiffMax);

	// Lap
	g_thresholds.lapTimeGapMax = ini->ReadFloat("Lap", "TimeGapMax", g_thresholds.lapTimeGapMax);
	g_thresholds.minValidLapTimeMultiplier = ini->ReadFloat("Lap", "MinValidTimeMult", g_thresholds.minValidLapTimeMultiplier);
	g_thresholds.maxValidLapTimeMultiplier = ini->ReadFloat("Lap", "MaxValidTimeMult", g_thresholds.maxValidLapTimeMultiplier);

	// Confidence scoring
	g_thresholds.confidenceLow = ini->ReadFloat("Confidence", "Low", g_thresholds.confidenceLow);
	g_thresholds.confidenceMedium = ini->ReadFloat("Confidence", "Medium", g_thresholds.confidenceMedium);
	g_thresholds.confidenceHigh = ini->ReadFloat("Confidence", "High", g_thresholds.confidenceHigh);
	g_thresholds.minLapsForReproducibility = ini->ReadInteger("Confidence", "MinLapsForReproducibility", g_thresholds.minLapsForReproducibility);

}

void loadThresholds()
{
	String path = ThresholdsFilePath();
	if (!FileExists(path)) {
		saveDefaultThresholds();
		return;
	}

	TIniFile* ini = new TIniFile(path);
	try {
		loadThresholdsPart1(ini);
		loadThresholdsPart2(ini);
		loadSymptomWeights(ini);
	} __finally {
		delete ini;
	}
}


void UpdateProgress(int percent, const String& message)
{
	static int lastPercent = -1;
	if (percent == lastPercent)
		return;
	lastPercent = percent;

	if (FormMotecSetup == NULL)
		return;

	FormMotecSetup->ProgressBarExport->Position = percent;
	//FormMotecSetup->StatusBar1->SimpleText = message + " " + IntToStr(percent) + "%";
	FormMotecSetup->ProgressBarExport->Update();
	//FormMotecSetup->StatusBar1->Update();

	if (percent % 5 == 0)
		Application->ProcessMessages();
}

void AnalysisProgress(int percent)
{
	if (percent < 50)
		UpdateProgress(percent, "Analyzing data...");
	else
		UpdateProgress(percent, "Building lap summary...");
}

 void DetectMotecInstallation(void);

__fastcall TFormMotecSetup::TFormMotecSetup(TComponent* Owner)
    : TForm(Owner)
{
	// GDI+ is started in _tWinMain() before this form is created, and shut down there
	// after Application->Run() returns. Do NOT start/stop it here — a double-startup
	// (or shutdown while other VCL objects are alive) caused an access violation on exit.

	// Load rule configuration from MotecDataAnalyser.rules.json (optional).
	// Do this BEFORE loadThresholds() so the [SymptomWeights] INI values can
	// override any weights that came from the JSON file.

	loadRuleConfig();

    loadChannelMappingConfig();


	// Load analysis thresholds from MotecDataAnalyser.thresholds.ini
	// (creates the file with defaults if it does not exist yet).
	loadThresholds();


	// Tablet viewer: auto-start the embedded HTTP server when enabled in INI.
	if (ReadWebServerEnabled()) {
		const int webPort = ReadWebServerPort();
		_tabletServer.setPort(webPort);
		if (!_tabletServer.isRunning() && _tabletServer.start()) {
			StatusBar1->SimpleText = "Tablet viewer active — http://" + getLocalLanIp()
				+ ":" + IntToStr(_tabletServer.port());
		}
	}

	if (LanguageCombo != NULL) {
		LanguageCombo->Items->Clear();
		LanguageCombo->Items->Add("English");
		LanguageCombo->Items->Add("Français");
		LanguageCombo->ItemIndex = (int)ReadReportLanguageSetting();
		LanguageCombo->OnChange = LanguageComboChange;
	}

	if (ChannelProfileCombo != NULL) {
		ChannelProfileCombo->Items->Clear();
		ChannelProfileCombo->Items->Add("Evo");
		ChannelProfileCombo->Items->Add("AMS2");
		ChannelProfileCombo->Items->Add("PMR");
		int profileIndex = (int)ReadChannelProfileSetting();
		if (profileIndex < 0 || profileIndex > (int)ChannelProfile::Pmr)
			profileIndex = 0; // guard against stale INI values from older builds
		ChannelProfileCombo->ItemIndex = profileIndex;
		ChannelProfileCombo->OnChange = ChannelProfileComboChange;
    }
	if (DamperLayoutTabs != NULL) {
		int layoutIndex = ReadHistogramLayoutSetting();
		const int kTabCount = 2; // Grid, Combined — matches DFM tab count
		if (layoutIndex < 0 || layoutIndex >= kTabCount)
			layoutIndex = 0;
		DamperLayoutTabs->ActivePageIndex = layoutIndex;
	}

	if (AutoDetectTrackCheckBox != NULL)
		AutoDetectTrackCheckBox->Checked = ReadAutoDetectTrackSetting();

	if (LiveCaptureCheckBox != NULL && ReadLiveCaptureSetting()) {
		// Restore previous state and run the normal start path (timers, connection search)
		LiveCaptureCheckBox->Checked = true;
		LiveCaptureCheckBoxClick(this);
	} else if (LiveCaptureCheckBox != NULL) {
        LiveCaptureCheckBox->Checked = false;
    }

	// --- Release info : version dynamique + notes de release GitHub ---------
	if (StatusBar1 && StatusBar1->Panels->Count >= 2)
		StatusBar1->Panels->Items[1]->Text = kAppVersion;   // ex: "V5.6"

	loadReleaseInfoCache();   // [ReleaseInfo] INI : dernières notes connues (hors-ligne OK)

	promptForAvailableUpdate();// popup immediat si le cache contient deja une version plus recente

	// Fetch asynchrone du dernier release GitHub. Repo privé sans token :
	// WinHTTP délègue les credentials Windows de l'utilisateur connecté.
	// Le worker écrit dans l'état global de ReleaseInfo ; ReleasePollTimer (1 s)
	// récupère via takePendingRelease() et met à jour UI + cache INI.
	fetchLatestReleaseAsync("jducable00-gif", "MotecDataAnalyser-Releases");

	if (ReleasePollTimer != NULL) {
		ReleasePollTimer->Interval = 1000;   // poll de l'état global worker -> UI
		ReleasePollTimer->OnTimer = ReleasePollTimerTimer;
		ReleasePollTimer->Enabled = true;
	}

	TranslateUI();


	// Plan Â§10b — default to the Summary tab at startup so live collection-rate /
	// report content is visible immediately. Runs once here (not in TranslateUI) so a
	// mid-session language switch doesn't yank the user off their current report tab.
	if (PageControlReport != NULL && TabSummary != NULL) {
		PageControlReport->ActivePage = TabSummary;
	}

	updateResetButtonState();


	// Wire up OnClose so all resource cleanup (live capture, tablet server) happens
	// while child controls are still alive. The destructor runs AFTER VCL has already
	// destroyed every component — touching any control there causes an access violation.
	OnClose = DoFormClose;

	// Restore saved window size/position ([Window] in the INI). Runs after DFM load so it
	// overrides poScreenCenter, and falls back to centering if no monitor matches (e.g. a
	// disconnected secondary screen from last session) or no entry exists yet.
	RestoreWindowGeometry(this);
}

void __fastcall TFormMotecSetup::TranslateUI()
{
    ReportLanguage lang = ReadReportLanguageSetting();
    bool isFr = (lang == ReportLanguage::French);

    this->Caption = isFr ? "Analyseur de Setup MoTeC" : "MoTeC Setup Analyzer";
	LabelProfile->Caption = isFr ? "Profil :" : "Profile:";
    LabelLanguage->Caption = isFr ? "Langue :" : "Language:";
    ButtonLoad->Caption = isFr ? "Importer Données Motec" : "Import Motec Data";
    Button1->Caption = isFr ? "Importer Données Motec" : "Import Motec Data";
    ButtonSettings->Caption = isFr ? "Réglages" : "Settings";
    if (ButtonAbout != NULL) ButtonAbout->Caption = isFr ? "À propos" : "About";
    LiveCaptureCheckBox->Caption = isFr ? "Capture Live" : "Live Capture";
    ButtonResetSession->Caption = isFr ? "Réinitialiser" : "Reset";
    AutoDetectTrackCheckBox->Caption = isFr ? "Auto" : "Auto";

    TabSummary->Caption = isFr ? "Résumé" : "Summary";
    TabGeneral->Caption = isFr ? "Général" : "General";
    TabTyre->Caption = isFr ? "Pneus" : "Tyre";
    TabBrake->Caption = isFr ? "Freins" : "Brake";
    TabSuspension->Caption = isFr ? "Suspensions" : "Suspension";
    TabEngineCooling->Caption = isFr ? "Moteur & Refroidissement" : "Engine & Cooling";
    TabHandling->Caption = isFr ? "Comportement" : "Handling";
    TabDiagnostics->Caption = isFr ? "Diagnostic" : "Diagnostics";

    // Re-apply â— indicator for tabs that have content (preserves dots across language switch)
    auto applyDot = [](TTabSheet* tab, TMemo* memo) {
        if (tab == NULL || memo == NULL) return;
        if (!memo->Text.IsEmpty())
            tab->Caption = String((System::WideChar)0x25CF) + " " + tab->Caption;
    };
    applyDot(TabGeneral, MemoGeneral);
    applyDot(TabTyre, MemoTyre);
    applyDot(TabBrake, MemoBrake);
    applyDot(TabSuspension, MemoSuspension);
    applyDot(TabEngineCooling, MemoEngineCooling);
    applyDot(TabHandling, MemoHandling);

    // Re-paint the GDI+ paint boxes: their captions (corner names, "Rebound"/"Bump",
    // "No tyre temp data", ...) are drawn in code and must be re-rendered with the new language.
    if (TireTempPaintBox != NULL)     TireTempPaintBox->Invalidate();
    if (BrakeTempPaintBox != NULL)    BrakeTempPaintBox->Invalidate();
    if (DamperFLPaintBox != NULL)     DamperFLPaintBox->Invalidate();
    if (DamperFRPaintBox != NULL)     DamperFRPaintBox->Invalidate();
    if (DamperRLPaintBox != NULL)     DamperRLPaintBox->Invalidate();
    if (DamperRRPaintBox != NULL)     DamperRRPaintBox->Invalidate();
    if (DamperCombinedPaintBox != NULL) DamperCombinedPaintBox->Invalidate();

    // Combo boxes items are usually set in DFM or constructor, 
    // but we can update them here if they were populated.


	OnResize = FormResize;
	FormResize(this);

	// DetectMotecInstallation();
}

void __fastcall TFormMotecSetup::ButtonSettingsClick(TObject *Sender)
{
	TFormMotecSettings *settingsForm = new TFormMotecSettings(this);
	try {
		if (settingsForm->ShowModal() == mrOk) {
			// Thresholds are already updated in g_thresholds and the .ini file 
			// by ButtonSaveClick in MotecSettingsForm.cpp
		}
	} __finally {
		delete settingsForm;
	}
}

void __fastcall TFormMotecSetup::LanguageComboChange(TObject *Sender)
{
	if (LanguageCombo == NULL)
		return;

	ReportLanguage language = (ReportLanguage)LanguageCombo->ItemIndex;
	SaveReportLanguageSetting(language);
	TranslateUI();
	MemoSummary->Lines->Add(language == ReportLanguage::French ? "Langue changée en français." : "Language changed to English.");
}

void __fastcall TFormMotecSetup::ChannelProfileComboChange(TObject *Sender)
{
	if (ChannelProfileCombo == NULL)
		return;

	ChannelProfile profile = (ChannelProfile)ChannelProfileCombo->ItemIndex;
	SaveChannelProfileSetting(profile);

	const char* profileName = "AMS2";
	if (profile == ChannelProfile::EvoM1)      profileName = "Evo";
	else if (profile == ChannelProfile::Pmr)   profileName = "PMR";

	// Wipe all previously displayed data — the old profile's results no longer apply.
	resetAnalysisGraphics();
	if (MemoSummary != NULL)       MemoSummary->Lines->Clear();
	if (MemoGeneral != NULL)       MemoGeneral->Lines->Clear();
	if (MemoTyre != NULL)          MemoTyre->Lines->Clear();
	if (MemoBrake != NULL)         MemoBrake->Lines->Clear();
	if (MemoSuspension != NULL)    MemoSuspension->Lines->Clear();
	if (MemoEngineCooling != NULL) MemoEngineCooling->Lines->Clear();
	if (MemoHandling != NULL)      MemoHandling->Lines->Clear();

	MemoSummary->Lines->Add("Channel profile changed to " + String(profileName) + ".");

    // Reactive Live Capture logic: If live is active, reconnect to the new profile immediately.
    if (LiveCaptureCheckBox != NULL && LiveCaptureCheckBox->Checked) {
        liveActive = true;
        liveConnected = false;
        resetLiveSession();

        // Update mode based on the newly selected profile
        liveMode = ProfileToLiveMode(profile);

        // LED red = not connected yet (reconnecting)
        LiveLed->Brush->Color = clRed;
        const bool isFrConn = ReadReportLanguageSetting() == ReportLanguage::French;
        LabelLiveStatus->Caption = isFrConn ? String(L"Connexion...") : String("Connecting...");

        // Start the connection timer to begin looking for shared memory/process
        LiveConnectTimer->Enabled = true;
    }
}


void __fastcall TFormMotecSetup::FormResize(TObject *Sender)
{
    if (PageControlReport == NULL)
        return;

    // Match the IDE design: boxes sit just below ProgressBarExport (ends at y=82).
    const int top = 96;              // DFM: TireTempPaintBox Top = 100, BrakeTop = 100
    const int gap = 10;
    const int tireBoxWidth = 280;
    const int tireBoxHeight = 358;   // DFM design heights (tire ~362 / brake 358)

    // 1. Tire Temp Box - Positioned on the Left
    if (TireTempPaintBox != NULL) {
        TireTempPaintBox->Left = 20;
        TireTempPaintBox->Top = top;
        TireTempPaintBox->Width = tireBoxWidth;
        TireTempPaintBox->Height = tireBoxHeight;
    }

    // 1.5 Brake Temp Box - Positioned between Tire Temps and Dampers
    if (BrakeTempPaintBox != NULL) {
        int areaLeft = tireBoxWidth + 30;
        int brakeBoxWidth = 300; // Wide enough for brake discs + water/oil gauges on the right
        int brakeBoxHeight = tireBoxHeight;

        BrakeTempPaintBox->Left = areaLeft;
        BrakeTempPaintBox->Top = top;
        BrakeTempPaintBox->Width = brakeBoxWidth;
        BrakeTempPaintBox->Height = brakeBoxHeight;

        // Shift Damper Histograms to the right of Brake Temp Box
        int damperAreaLeft = areaLeft + brakeBoxWidth + 10;
        int availableDamperWidth = ClientWidth - damperAreaLeft - 20;
        int chartWidth = (availableDamperWidth - gap) / 2;
        int chartHeight = (tireBoxHeight - gap) / 2;

        // The four grid paintboxes live inside TabDamperGrid (a child of
        // DamperLayoutTabs). Position the tab control in form coordinates and the boxes
        // relative to it so they stay visible when the window resizes.
        if (DamperLayoutTabs != NULL) {
            const int damperTabsAnchor =
                (BrakeTempPaintBox != NULL) ? damperAreaLeft : tireBoxWidth + 30;
            const int tabsTopY = top - 8;              // DFM: DamperLayoutTabs Top=88, boxes at 96/100
            DamperLayoutTabs->Left = damperTabsAnchor; // right of the brake temp box
            DamperLayoutTabs->Top = tabsTopY;
            DamperLayoutTabs->Width = ClientWidth - damperTabsAnchor - 20;
            // Bottom edge flush with the tire/brake boxes so all panels align.
            const int panelBottomY = top + tireBoxHeight;      // e.g., 96+358=454
            DamperLayoutTabs->Height = panelBottomY - tabsTopY;
            // Grid rows fill the content area below the tab strip, ending at that same bottom edge.
            const int kTabHeaderH = 40;                // VCL tab-strip headroom inside page control
            chartHeight = (DamperLayoutTabs->Height - kTabHeaderH - gap) / 2;
        }

        if (DamperFLPaintBox != NULL && DamperFRPaintBox != NULL && DamperRLPaintBox != NULL && DamperRRPaintBox != NULL) {
            int areaTopDampers = top - 4;
            // Convert form-space coordinates to tab-sheet-relative ones.
            const int tabsLeftPx = (DamperLayoutTabs != NULL) ? DamperLayoutTabs->Left : tireBoxWidth + 30;
            const int tabsTopPx = (DamperLayoutTabs != NULL) ? DamperLayoutTabs->Top : top - 4;

            DamperFLPaintBox->Left = 0; // tab control is anchored at the grid's form-space origin
            DamperFLPaintBox->Top = areaTopDampers - tabsTopPx;
            DamperFLPaintBox->Width = chartWidth;
            DamperFLPaintBox->Height = chartHeight;

            DamperFRPaintBox->Left = chartWidth + gap;
            DamperFRPaintBox->Top = areaTopDampers - tabsTopPx;
            DamperFRPaintBox->Width = chartWidth;
            DamperFRPaintBox->Height = chartHeight;

            DamperRLPaintBox->Left = 0; // tab control is anchored at the grid's form-space origin
            DamperRLPaintBox->Top = areaTopDampers - tabsTopPx + chartHeight + gap;
            DamperRLPaintBox->Width = chartWidth;
            DamperRLPaintBox->Height = chartHeight;

            DamperRRPaintBox->Left = chartWidth + gap;
            DamperRRPaintBox->Top = areaTopDampers - tabsTopPx + chartHeight + gap;
            DamperRRPaintBox->Width = chartWidth;
            DamperRRPaintBox->Height = chartHeight;
        }
    } else {
        // Fallback if BrakeTempPaintBox is missing: use original layout
        if (DamperFLPaintBox != NULL && DamperFRPaintBox != NULL && DamperRLPaintBox != NULL && DamperRRPaintBox != NULL) {
            const int tabsLeftPx = (DamperLayoutTabs != NULL) ? DamperLayoutTabs->Left : tireBoxWidth + 30;
            const int tabsTopPx = (DamperLayoutTabs != NULL) ? DamperLayoutTabs->Top : top - 4;
            int areaLeft = tireBoxWidth + 30;
            int areaTop = top;
            int availableWidth = ClientWidth - areaLeft - 20;
            int chartWidth = (availableWidth - gap) / 2;
            int chartHeight = (tireBoxHeight - gap) / 2;

            DamperFLPaintBox->Left = areaLeft - tabsLeftPx;
            DamperFLPaintBox->Top = areaTop - tabsTopPx;
            DamperFLPaintBox->Width = chartWidth;
            DamperFLPaintBox->Height = chartHeight;

            DamperFRPaintBox->Left = areaLeft - tabsLeftPx + chartWidth + gap;
            DamperFRPaintBox->Top = areaTop - tabsTopPx;
            DamperFRPaintBox->Width = chartWidth;
            DamperFRPaintBox->Height = chartHeight;

            DamperRLPaintBox->Left = areaLeft - tabsLeftPx;
            DamperRLPaintBox->Top = areaTop - tabsTopPx + chartHeight + gap;
            DamperRLPaintBox->Width = chartWidth;
            DamperRLPaintBox->Height = chartHeight;

            DamperRRPaintBox->Left = areaLeft - tabsLeftPx + chartWidth + gap;
            DamperRRPaintBox->Top = areaTop - tabsTopPx + chartHeight + gap;
            DamperRRPaintBox->Width = chartWidth;
            DamperRRPaintBox->Height = chartHeight;
        }
    }

    // 3. Report Area - Positioned Below the Panels, spanning full width.
    // Start below whichever is taller: the temp boxes or the damper tab control.
    int reportTopY = top + tireBoxHeight + 20;
    if (DamperLayoutTabs != NULL) {
        const int tabsBottomY = DamperLayoutTabs->Top + DamperLayoutTabs->Height + 14;
        if (tabsBottomY > reportTopY)
            reportTopY = tabsBottomY;
    }
    PageControlReport->Left = 20;
    PageControlReport->Top = reportTopY;
    PageControlReport->Width = ClientWidth - 40;
    PageControlReport->Height = ClientHeight - PageControlReport->Top - 40;
}

// Remove this method entirely as it is no longer used
void __fastcall TFormMotecSetup::BrakeTempPaintBoxPaint(TObject *Sender)
{
    if (BrakeTempPaintBox == NULL) return;

    const bool isFr = ReadReportLanguageSetting() == ReportLanguage::French;

    TCanvas* canvas = BrakeTempPaintBox->Canvas;
    int width = BrakeTempPaintBox->Width;
    int height = BrakeTempPaintBox->Height;

    // Initialize GDI+ Graphics from VCL Canvas
    Gdiplus::Graphics graphics(canvas->Handle);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);

    // 1. Background: Deep Charcoal
    Gdiplus::SolidBrush bgBrush(Gdiplus::Color(255, 26, 26, 26));
    graphics.FillRectangle(&bgBrush, 0, 0, width, height);

    if (!hasBrakeTempData) {
        // Draw placeholder text using GDI+ (avoids mixing VCL canvas after Graphics init).
        Gdiplus::Font phFont(L"Segoe UI", 10.0f);
        Gdiplus::SolidBrush phTextBrush(Gdiplus::Color(255, 180, 180, 180)); // Light grey on charcoal
        Gdiplus::StringFormat phFmt;
        phFmt.SetAlignment(Gdiplus::StringAlignmentCenter);
        phFmt.SetLineAlignment(Gdiplus::StringAlignmentCenter);
        Gdiplus::RectF layoutRect(0.0f, 0.0f, (Gdiplus::REAL)width, (Gdiplus::REAL)height);
        graphics.DrawString(isFr ? L"Aucune donn\u00e9e de freins" : L"No brake data", -1, &phFont, layoutRect, &phFmt, &phTextBrush);
        return;
    }

    // Layout constants. Box is ~240 wide x 310 tall (set in FormResize).
    const int iconSize = 46;      // disc circle diameter
    const int colGap   = 18;      // horizontal gap between the two columns
    const int leftPad  = 20;      // left margin before first column

    // Vertical budget per row: label(20) + gap + caliper/rotor(46) + Avg/Max lines(~32).
    // We give each row a fixed pitch so temps of one row never collide with the next.
    const int labelH   = 20;      // corner-label height above its disc (extra clearance for caliper)
    const int tempBlock= 34;      // space below disc for Avg + Max lines
    const int rowPitch = labelH + iconSize + tempBlock + 30; // ~136px per row (incl. inter-row gap)

    int colW   = iconSize + colGap;        // pitch per column
    int topY0  = 48;                       // y of first row's disc (leaves room for label above caliper)
    int topY1  = topY0 + rowPitch;         // y of second row's disc

    struct IconPos { int x, y; String label; };
    IconPos pos[4] = {
        { leftPad,          topY0, isFr ? "AV G" : "FL" }, // 0: FL (top-left)
        { leftPad + colW,   topY0, isFr ? "AV D" : "FR" }, // 1: FR (top-right)
        { leftPad,          topY1, isFr ? "AR G" : "RL" }, // 2: RL (bottom-left)
        { leftPad + colW,   topY1, isFr ? "AR D" : "RR" }  // 3: RR (bottom-right)
    };

    // --- Disk-brake rendering brushes/pens (shared across the four corners) ---
    Gdiplus::SolidBrush rotorFill(Gdiplus::Color(255, 96, 100, 108));   // steel-grey disc face
    Gdiplus::Pen       rimPen     (Gdiplus::Color(255, 175, 180, 188), 2.0f); // bright outer edge
    Gdiplus::Pen       groovePen  (Gdiplus::Color(90, 60, 63, 68),      1.0f); // faint concentric grooves
    Gdiplus::SolidBrush hubFill   (Gdiplus::Color(255, 70, 74, 80));     // darker central hub
    Gdiplus::Pen       hubPen     (Gdiplus::Color(255, 130, 134, 140), 1.0f);
    Gdiplus::SolidBrush boltFill  (Gdiplus::Color(255, 200, 204, 210)); // small hub bolts
    Gdiplus::SolidBrush caliperFill(Gdiplus::Color(235, 214, 68, 52));   // red brake caliper

    // Draw each disc with its corner label ABOVE it and Avg/Max temps BELOW it.
    Gdiplus::Font labelFont(L"Segoe UI", 9.5f, Gdiplus::FontStyleBold);
    Gdiplus::SolidBrush labelBrush(Gdiplus::Color(255, 230, 230, 230)); // Off-white corner name
    Gdiplus::Font tempFont(L"Segoe UI", 8.0f);
    Gdiplus::SolidBrush avgBrush(Gdiplus::Color(255, 100, 200, 255));   // Light Blue for Avg
    Gdiplus::SolidBrush maxBrush(Gdiplus::Color(255, 255, 130, 130));   // Soft Red for Max

    Gdiplus::StringFormat centerFmt;
    centerFmt.SetAlignment(Gdiplus::StringAlignmentCenter);
    centerFmt.SetLineAlignment(Gdiplus::StringAlignmentNear);

    for (int i = 0; i < 4; ++i) {
        // Corner label ABOVE the disc+caliper, centered on it. Sits well clear of the red caliper.
        Gdiplus::RectF labelRect((Gdiplus::REAL)(pos[i].x - 12), (Gdiplus::REAL)(pos[i].y - labelH - iconSize * 0.34f + 2), (Gdiplus::REAL)(iconSize + 24), labelH);
        graphics.DrawString(pos[i].label.c_str(), -1, &labelFont, labelRect, &centerFmt, &labelBrush);

        // --- Draw a disk brake: rotor + grooves + hub/bolts + caliper ---
        float dx = (float)pos[i].x;
        float dy = (float)pos[i].y;
        float d  = (float)iconSize;
        float cx = dx + d / 2.0f;   // disc centre x
        float cy = dy + d / 2.0f;   // disc centre y

        // Rotor face (filled steel-grey disc).
        graphics.FillEllipse(&rotorFill, dx, dy, d, d);
        // Bright outer rim to define the edge.
        graphics.DrawEllipse(&rimPen, dx, dy, d, d);

        // Concentric grooves suggesting a slotted/drilled rotor face (3 rings).
        for (int g = 1; g <= 3; ++g) {
            float inset = d * (0.28f + 0.16f * g);   // shrink inward each ring
            graphics.DrawEllipse(&groovePen, cx - inset / 2.0f, cy - inset / 2.0f, inset, inset);
        }

        // Central hub with a few bolt dots around it (wheel/rotor centre).
        float hubD = d * 0.34f;
        graphics.FillEllipse(&hubFill, cx - hubD / 2.0f, cy - hubD / 2.0f, hubD, hubD);
        graphics.DrawEllipse(&hubPen,   cx - hubD / 2.0f, cy - hubD / 2.0f, hubD, hubD);

        float boltOrbit = d * 0.15f;                 // radius of the bolt circle
        float boltR     = d * 0.035f;                // each bolt dot diameter
        const double PI = 3.14159265358979323846;
        for (int b = 0; b < 4; ++b) {
            double ang = PI / 2.0 + (PI / 2.0) * b; // N, E, S, W
            float bx = cx + (float)(boltOrbit * std::cos(ang)) - boltR / 2.0f;
            float by = cy + (float)(boltOrbit * std::sin(ang)) - boltR / 2.0f;
            graphics.FillEllipse(&boltFill, bx, by, boltR, boltR);
        }

        // Caliper: a red block straddling the top of the disc. Kept in a tight band just above
        // the rim so it never reaches up into the corner-label zone drawn higher up.
        float calW = d * 0.58f;
        float calH = d * 0.26f;
        Gdiplus::RectF caliper(dx + (d - calW) / 2.0f, dy - calH * 0.30f, calW, calH);
        graphics.FillRectangle(&caliperFill, caliper);

        // Avg / Max temps BELOW the disc, centered on it. Build wide buffers via swprintf.
        wchar_t avgBuf[32];
        wchar_t maxBuf[32];
        swprintf(avgBuf, 32, isFr ? L"Moy.: %.0f\u00b0C" : L"Avg: %.0fC", brakeTemps[i].avg);
        swprintf(maxBuf, 32, L"Max: %.0fC", (double)brakeTemps[i].max);

        Gdiplus::RectF avgRect((Gdiplus::REAL)(pos[i].x - 16), (Gdiplus::REAL)(pos[i].y + iconSize + 3), (Gdiplus::REAL)(iconSize + 32), 15);
        graphics.DrawString(avgBuf, -1, &tempFont, avgRect, &centerFmt, &avgBrush);

        Gdiplus::RectF maxRect((Gdiplus::REAL)(pos[i].x - 16), (Gdiplus::REAL)(pos[i].y + iconSize + 19), (Gdiplus::REAL)(iconSize + 32), 15);
        graphics.DrawString(maxBuf, -1, &tempFont, maxRect, &centerFmt, &maxBrush);
    }

    // --- Temperature Gauge rendering (Water & Oil) in third column ---
    const int gaugeX = leftPad + 2 * (iconSize + colGap) + 30; // x of gauge column (pushed to the right)
    const float gaugeD = 46.0f;                            // same diameter as brake discs

    // isFr captured from outer scope for localized Avg/Max value labels inside the gauge.
    auto drawTempGauge = [&](float gx, float gy, double tempValue, double maxTempValue, bool dataValid, double warnThreshold, const wchar_t* caption) {
        float cx = gx + gaugeD / 2.0f;
        float cy = gy + gaugeD / 2.0f;

        // Gauge background circle (dark)
        Gdiplus::SolidBrush gaugeBg(Gdiplus::Color(255, 40, 42, 48));
        graphics.FillEllipse(&gaugeBg, gx, gy, gaugeD, gaugeD);
        Gdiplus::Pen gaugeRim(Gdiplus::Color(255, 100, 104, 112), 1.5f);
        graphics.DrawEllipse(&gaugeRim, gx, gy, gaugeD, gaugeD);

        // Colored arc segments (blue -> yellow -> red) over top half
        const int segCount = 24;
        float arcInset = gaugeD * 0.12f;
        float arcX = gx + arcInset;
        float arcY = gy + arcInset;
        float arcW = gaugeD - 2.0f * arcInset;

        for (int s = 0; s < segCount; ++s) {
            float t = (float)s / (segCount - 1);
            int r, g, b;
            if (t < 0.5f) {
                float f = t * 2.0f;
                r = (int)(60 + f * 195);   // 60 -> 255
                g = (int)(140 + f * 80);   // 140 -> 220
                b = (int)(255 - f * 195);  // 255 -> 60
            } else {
                float f = (t - 0.5f) * 2.0f;
                r = 255;
                g = (int)(220 - f * 150);   // 220 -> 70
                b = (int)(60 - f * 10);     // 60 -> 50
            }
            float startAng = 180.0f + t * 180.0f / segCount;
            float sweepAng = 180.0f / segCount + 0.5f;
            Gdiplus::Pen segPen(Gdiplus::Color(255, r, g, b), 3.0f);
            graphics.DrawArc(&segPen, arcX, arcY, arcW, arcW, startAng, sweepAng);
        }

        // Red warning zone: ~30Â° arc starting at the threshold temperature
        {
            const double tMin = 40.0, tMax = 150.0;
            float threshFrac = (float)((warnThreshold - tMin) / (tMax - tMin));
            if (threshFrac < 0.0f) threshFrac = 0.0f;
            if (threshFrac > 1.0f) threshFrac = 1.0f;
            float warnStart = 180.0f + threshFrac * 180.0f;
            float warnSweep = 30.0f;
            if (warnStart + warnSweep > 360.0f) warnSweep = 360.0f - warnStart;
            Gdiplus::Pen warnPen(Gdiplus::Color(255, 220, 40, 40), 5.0f);
            graphics.DrawArc(&warnPen, arcX, arcY, arcW, arcW, warnStart, warnSweep);
        }

        // Needle
        if (dataValid) {
            const double tMin = 40.0, tMax = 150.0;
            double clamped = tempValue < tMin ? tMin : (tempValue > tMax ? tMax : tempValue);
            float frac = (float)((clamped - tMin) / (tMax - tMin));
            float needleAngRad = (float)(3.14159265 * (1.0 - frac)); // PI(left) to 0(right)
            float needleLen = gaugeD * 0.38f;
            float nx = cx + needleLen * cosf(needleAngRad);
            float ny = cy - needleLen * sinf(needleAngRad);
            // Needle turns red when the temperature exceeds the warning threshold
            Gdiplus::Color needleCol = (tempValue > warnThreshold)
                ? Gdiplus::Color(255, 230, 60, 50)   // alert red
                : Gdiplus::Color(255, 240, 240, 240); // normal white
            Gdiplus::Pen needlePen(needleCol, 2.0f);
            graphics.DrawLine(&needlePen, cx, cy, nx, ny);
        } else {
            // No data: short grey needle pointing down
            Gdiplus::Pen needlePen(Gdiplus::Color(255, 120, 120, 120), 2.0f);
            graphics.DrawLine(&needlePen, cx, cy, cx, cy + gaugeD * 0.25f);
        }

        // Center hub dot
        Gdiplus::SolidBrush hubDot(Gdiplus::Color(255, 200, 204, 210));
        float hubR = 3.0f;
        graphics.FillEllipse(&hubDot, cx - hubR, cy - hubR, hubR * 2.0f, hubR * 2.0f);

        // Caption below gauge
        Gdiplus::Font capFont(L"Segoe UI", 8.5f, Gdiplus::FontStyleBold);
        Gdiplus::SolidBrush capBrush(Gdiplus::Color(255, 230, 230, 230));
        Gdiplus::RectF capRect(gx - 6.0f, gy + gaugeD + 2.0f, gaugeD + 12.0f, 14.0f);
        graphics.DrawString(caption, -1, &capFont, capRect, &centerFmt, &capBrush);

        // Value text
        wchar_t valBuf[16];
        wchar_t maxBuf[16];
        Gdiplus::RectF valRect(gx - 6.0f, gy + gaugeD + 17.0f, gaugeD + 12.0f, 14.0f);
        Gdiplus::RectF maxRect(gx - 6.0f, gy + gaugeD + 31.0f, gaugeD + 12.0f, 14.0f);
        if (dataValid) {
            swprintf(valBuf, 16, isFr ? L"Moy.: %.0f\u00b0C" : L"Avg: %.0fC", tempValue);
            Gdiplus::SolidBrush valBrush(Gdiplus::Color(255, 100, 200, 255));
            graphics.DrawString(valBuf, -1, &tempFont, valRect, &centerFmt, &valBrush);

            swprintf(maxBuf, 16, L"Max: %.0fC", maxTempValue);
            Gdiplus::SolidBrush maxBrakeBrush(Gdiplus::Color(255, 255, 130, 130));
            graphics.DrawString(maxBuf, -1, &tempFont, maxRect, &centerFmt, &maxBrakeBrush);
        } else {
            Gdiplus::SolidBrush noDataBrush(Gdiplus::Color(255, 140, 140, 140));
            graphics.DrawString(L"--", -1, &tempFont, valRect, &centerFmt, &noDataBrush);
            graphics.DrawString(L"--", -1, &tempFont, maxRect, &centerFmt, &noDataBrush);
        }
    };

    // Water gauge (front row) and Oil gauge (rear row)
    // Needle turns red above the configured warning thresholds
    drawTempGauge((float)gaugeX, (float)topY0, waterTemp.avg, waterTemp.max, waterTemp.valid, g_thresholds.waterTempMax,
                  isFr ? L"Eau" : L"Water");
    drawTempGauge((float)gaugeX, (float)topY1, oilTemp.avg, oilTemp.max, oilTemp.valid, g_thresholds.oilTempMax,
                  isFr ? L"Huile" : L"Oil");
}

void __fastcall TFormMotecSetup::DrawModernHistogram(TCanvas* canvas, int x, int y, int width, int height, const SuspensionCornerData& stats, const String& label)
{
    if (!canvas) return;

    // Initialize GDI+ Graphics from VCL Canvas
    Gdiplus::Graphics graphics(canvas->Handle);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);

    // 1. Background: Deep Charcoal
    Gdiplus::SolidBrush bgBrush(Gdiplus::Color(255, 26, 26, 26));
    graphics.FillRectangle(&bgBrush, 0, 0, width, height);

    int marginLeft = 35;
    int marginBottom = 20;
    int chartWidth = width - marginLeft;
    int chartHeight = height - marginBottom;

    // Highlight Slow Damper Section (Symmetrical range around 0)
    double slowRange = g_thresholds.damperSlowSpeedRange;
    if (slowRange > 0) {
        float xCenter = marginLeft + chartWidth / 2.0f;
        float pixelsPerUnit = chartWidth / 400.0f; // Total range is -200 to 200
        float slowHalfWidth = (float)(slowRange * pixelsPerUnit);

        Gdiplus::SolidBrush slowBgBrush(Gdiplus::Color(100, 60, 60, 100)); // Enhanced contrast for slow damper section
        graphics.FillRectangle(&slowBgBrush, Gdiplus::RectF(xCenter - slowHalfWidth, 0, slowHalfWidth * 2, (float)height));
    }

    // 2. Grid Lines (Subtle Dark Grey)
    Gdiplus::Pen gridPen(Gdiplus::Color(80, 60, 60, 60), 1.0f);
    
    // Vertical center line (0 velocity)
    graphics.DrawLine(&gridPen, marginLeft + chartWidth / 2, 0, marginLeft + chartWidth / 2, chartHeight);
    
    // Horizontal grid lines (e.g., every 25% of height)
    for (int i = 1; i < 4; ++i) {
        int gy = (chartHeight * i) / 4;
        graphics.DrawLine(&gridPen, marginLeft, gy, width, gy);
    }

    // Side labels: rebound side is negative velocity (left), bump/compression positive (right).
    const bool isFr = ReadReportLanguageSetting() == ReportLanguage::French;
    {
        Gdiplus::Font lblFont(L"Segoe UI", 8.0f, Gdiplus::FontStyleBold);
        Gdiplus::SolidBrush sideLbl(Gdiplus::Color(215, 168, 170, 178));

        const wchar_t* rebTxt = isFr ? L"D\u00e9tente" : L"Rebound";      // v < 0
        Gdiplus::RectF rb; graphics.MeasureString(rebTxt, -1, &lblFont, Gdiplus::PointF(0, 0), &rb);
        float rebX = marginLeft + chartWidth * 0.25f - rb.Width / 2.0f;   // center of left half
        if (rebX < marginLeft) rebX = marginLeft;

        const wchar_t* bumpTxt = isFr ? L"Compression" : L"Bump";         // v > 0
        Gdiplus::RectF bb; graphics.MeasureString(bumpTxt, -1, &lblFont, Gdiplus::PointF(0, 0), &bb);
        float bumpX = marginLeft + chartWidth * 0.75f - bb.Width / 2.0f;  // center of right half
        if (bumpX > width - bb.Width) bumpX = width - bb.Width;

        graphics.DrawString(rebTxt, -1, &lblFont, Gdiplus::PointF(rebX, 3), &sideLbl);
        graphics.DrawString(bumpTxt, -1, &lblFont, Gdiplus::PointF(bumpX, 3), &sideLbl);
    }

    // 3. Draw Bars
    // Use the shared Y-axis maximum so all four histograms share the same scale.
    // Apply a configurable floor so small data sets aren't stretched to fill the full chart height.
    double maxBin = suspensionMaxBin;
    double displayMax = std::max(maxBin * (1.0 + g_thresholds.histogramYOffset / 100.0),
                                 g_thresholds.histogramMinYPct / 100.0);

    if (maxBin > 0) {
        // Gradient for bars: Electric Blue to Darker Blue
        Gdiplus::LinearGradientBrush barBrush(
            Gdiplus::Rect(0, 0, width, height),
            Gdiplus::Color(255, 0, 180, 255), // Top: Bright Cyan/Blue
            Gdiplus::Color(255, 0, 60, 120),   // Bottom: Deep Blue
            Gdiplus::LinearGradientModeVertical
        );

        float binWidth = (float)chartWidth / 41.0f;
        float scaleY = (float)chartHeight / displayMax;

        for (int i = 0; i < VelocityHistogram::kBinCount; ++i) {
            float barHeight = stats.bins[i] * scaleY;
            float bx = marginLeft + i * binWidth;
            float by = chartHeight - barHeight;
            
            graphics.FillRectangle(&barBrush, bx, by, binWidth, barHeight);
        }
    }

    // 4. Labels & Axis
    Gdiplus::Font font(L"Segoe UI", 8, Gdiplus::FontStyleRegular);
    Gdiplus::SolidBrush textBrush(Gdiplus::Color(255, 180, 180, 180));

    // Corner Label (e.g., "FL")
    graphics.DrawString(label.c_str(), -1, &font, Gdiplus::PointF(5, 5), &textBrush);
    
    // Y-Axis Labels
    if (maxBin > 0) {
        wchar_t labelBuf[32];
        
        // Top Label (Max)
        
        for (int i = 1; i < 4; ++i) {
            int gy = (chartHeight * i) / 4;
            double val = displayMax * (1.0 - (i / 4.0)) * 100.0; // bins are fractions -> percent
            swprintf(labelBuf, 32, L"%.1f%%", val);
            graphics.DrawString(labelBuf, -1, &font, Gdiplus::PointF(5, gy - 7), &textBrush);
        }
        graphics.DrawString(L"0.0%", -1, &font, Gdiplus::PointF(5, chartHeight - 7), &textBrush);
    }

    // X-Axis Labels (Every 50 mm/s within -200 to +200 range)
    for (int val = -200; val <= 200; val += 50) {
        wchar_t xLabelBuf[16];
        swprintf(xLabelBuf, 16, L"%d", val);
        float xPos = marginLeft + ((val + 205.0f) / 410.0f) * chartWidth;
        Gdiplus::RectF labelBounds;
        graphics.MeasureString(xLabelBuf, -1, &font, Gdiplus::PointF(0, 0), &labelBounds);
        float labelX = xPos - labelBounds.Width / 2.0f;
        labelX = std::max(0.0f, std::min(labelX, width - labelBounds.Width));
        graphics.DrawString(xLabelBuf, -1, &font, Gdiplus::PointF(labelX, height - 15), &textBrush);
    }
}

// Localized corner names for the four damper histogram paint handlers.
static String DamperCornerLabel(int index) {
    const bool isFr = TFormMotecSetup::ReadReportLanguageSetting() == ReportLanguage::French;
    switch (index) {
        case 0: return isFr ? L"Avant Gauche" : L"Front Left";
        case 1: return isFr ? L"Avant Droit" : L"Front Right";
        case 2: return isFr ? String(L"Arri\u00e8re Gauche") : L"Rear Left";
        default:return isFr ? String(L"Arri\u00e8re Droit")  : L"Rear Right";
    }
}

void __fastcall TFormMotecSetup::DamperFLPaintBoxPaint(TObject *Sender) {
    TPaintBox* pb = (TPaintBox*)Sender;
    DrawModernHistogram(pb->Canvas, pb->Left, pb->Top, pb->Width, pb->Height, suspensionData[0], DamperCornerLabel(0));
}

void __fastcall TFormMotecSetup::DamperFRPaintBoxPaint(TObject *Sender) {
    TPaintBox* pb = (TPaintBox*)Sender;
    DrawModernHistogram(pb->Canvas, pb->Left, pb->Top, pb->Width, pb->Height, suspensionData[1], DamperCornerLabel(1));
}

void __fastcall TFormMotecSetup::DamperRLPaintBoxPaint(TObject *Sender) {
    TPaintBox* pb = (TPaintBox*)Sender;
    DrawModernHistogram(pb->Canvas, pb->Left, pb->Top, pb->Width, pb->Height, suspensionData[2], DamperCornerLabel(2));
}

void __fastcall TFormMotecSetup::DamperRRPaintBoxPaint(TObject *Sender) {
    TPaintBox* pb = (TPaintBox*)Sender;
    DrawModernHistogram(pb->Canvas, pb->Left, pb->Top, pb->Width, pb->Height, suspensionData[3], DamperCornerLabel(3));
}

// --- Combined damper chart: all four corners overlaid on a single axis set. ---

namespace {
struct CornerVelStats {
    double reboundSidePct = -1;   // total % of all samples on the rebound side
    double reboundHiPct = -1;     // % of samples with |v| > slowRange (rebound side) — fast
    double reboundLoPct = -1;     // % of samples with |v| <= slowRange (rebound side) — slow
    double bumpSidePct  = -1;     // total % of all samples on the bump side
    double bumpHiPct    = -1;     // % of samples with v  > slowRange (bump side) — fast
    double bumpLoPct    = -1;     // % of samples with |v| <= slowRange (bump side) — slow
    double avgReboundMs = -1e308;  // mean velocity on rebound side, mm/s (unused in legend)
    double avgBumpMs    = -1e308;  // mean velocity on bump side,   mm/s (unused in legend)
    double reboundAvgPct = -1.0; // (Fast % + Slow %) / 2 on the rebound side
    double bumpAvgPct    = -1.0; // (Slow % + Fast %) / 2 on the bump side

    bool hasAny() const { return reboundHiPct >= 0 || bumpHiPct >= 0; }
};

// Per-corner stats reconstructed from the normalized bins[] (41 fractions).
CornerVelStats computeCombinedCornerStats(const TFormMotecSetup::SuspensionCornerData& corner, double slowRange) {
    CornerVelStats s;
    if (!corner.valid || corner.totalCount == 0) return s; // all fields stay sentinel

    const int kBin = VelocityHistogram::kBinCount;
    double rebHiF = 0.0, rebLoF = 0.0, bumpHiF = 0.0, bumpLoF = 0.0;
    double rebSideF = 0.0, bumpSideF = 0.0, rebWV = 0.0, bumpWV = 0.0;

    for (int i = 0; i < kBin; ++i) {
        const double f = corner.bins[i]; // normalized fraction (~sums to 1 across all bins)
        if (f <= 0.0) continue;
        double binCenter = -VelocityHistogram::rangeMax() + (i + 0.5) * VelocityHistogram::binWidth();

        bool slow = std::abs(binCenter) <= slowRange + 1e-9;

        if (binCenter < 0.0) {           // rebound side
            rebSideF += f;
            rebWV    += f * binCenter;   // weighted velocity for per-side average
            if (!slow)      rebHiF += f;
            else            rebLoF += f;
        } else if (binCenter > 0.0) {    // bump side
            bumpSideF += f;
            bumpWV    += f * binCenter;
            if (!slow)      bumpHiF += f;
            else            bumpLoF += f;
        }
    }

    s.reboundSidePct = rebSideF  * 100.0;
    s.bumpSidePct    = bumpSideF * 100.0;
    s.reboundHiPct   = rebHiF  * 100.0;
    s.reboundLoPct   = rebLoF  * 100.0;
    s.bumpHiPct      = bumpHiF * 100.0;
    s.bumpLoPct      = bumpLoF * 100.0;
    if (rebSideF > 1e-9)  s.avgReboundMs = rebWV / rebSideF;
    if (bumpSideF > 1e-9) s.avgBumpMs    = bumpWV / bumpSideF;
    // Average % per side: simple mean of the Fast and Slow percentages.
    if (rebHiF + rebLoF  > 1e-9) s.reboundAvgPct = (rebHiF  + rebLoF)  * 50.0;
    if (bumpHiF + bumpLoF > 1e-9) s.bumpAvgPct   = (bumpHiF + bumpLoF) * 50.0;

    return s;
}
} // namespace

// Step 9: paint handler for the combined chart tab.
void __fastcall TFormMotecSetup::DamperCombinedPaintBoxPaint(TObject *Sender) {
    TPaintBox* pb = (TPaintBox*)Sender;
    Gdiplus::Graphics graphics(pb->Canvas->Handle);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    DrawCombinedDamperChart(graphics, pb->Width, pb->Height);
}

void __fastcall TFormMotecSetup::DrawCombinedDamperChart(Gdiplus::Graphics& graphics, int width, int height) {
    if (width < 80 || height < 120) return; // too small to be meaningful

    const bool isFr = ReadReportLanguageSetting() == ReportLanguage::French;

    // --- Step 8a: background + layout zones ---------------------------------
    Gdiplus::SolidBrush bg(Gdiplus::Color(255, 30, 30, 34));
    graphics.FillRectangle(&bg, 0, 0, width, height);

    const int marginLeft = 46;
    const int marginRight = 12;
    // Thin top band: the corner legend is now drawn as an overlay on top of the plot,
    // so most vertical space goes to the Y axis / curves.
    const int headerHeight = 30;
    const int xAxisHeight = 22;
    const float chartLeft   = (float)marginLeft;
    const float chartRight  = (float)(width - marginRight);
    const float chartTop    = (float)headerHeight;
    const float chartBottom = (float)(height - xAxisHeight);
    const float chartWidth  = chartRight - chartLeft;
    const float chartHeight = chartBottom - chartTop;

    // --- Step 8c: per-corner stats ------------------------------------------
    CornerVelStats corner[4];
    for (int i = 0; i < 4; ++i)
        corner[i] = computeCombinedCornerStats(suspensionData[i], g_thresholds.damperSlowSpeedRange);

    // --- Step 8b: header / legend block -------------------------------------
    Gdiplus::Font fontReg(L"Segoe UI", 9.0f, Gdiplus::FontStyleRegular);
    Gdiplus::Font fontBold(L"Segoe UI", 9.0f, Gdiplus::FontStyleBold);

    const float colLabelX = chartLeft;                 // corner name column (left-aligned)
    {
        // Legend overlay: drawn as a translucent strip across the TOP of the plot area so it sits
        // "over" the chart (curves show through faintly) and leaves full vertical room for Y axis.
        const int   kValCols  = 6;
        const float cellW     = std::max(52.0f, (chartRight - colLabelX) / (kValCols * 3));
                                                         // uniform spacing inside AND between groups
        const float labelW    = 136.0f;                 // "Damper Vel FL" caption width (kept clear of values)
        const float valStartX = colLabelX + labelW;     // rebound group: as far LEFT as possible after names
        const float bumpStartX = chartRight - 3.0f * cellW; // bump group: as far RIGHT as possible

        std::size_t totalSamples = 0;
        for (int i = 0; i < 4; ++i) if (suspensionData[i].valid) totalSamples += suspensionData[i].totalCount;

        const float panelTop   = chartTop + 6.0f;                  // top of overlay strip
        const float rowStep    = 17.0f;                            // vertical spacing between rows
        const float samplesY   = panelTop;                         // "<N> samples" line
        const float groupHdrY  = panelTop + rowStep * 1.35f;       // "Rebound"/"Bump" bold headers
        const float subCapY    = panelTop + rowStep * 2.60f;       // Total/Slow/Fast captions line
        const float firstRowY  = panelTop + rowStep * 4.10f;       // top of first corner data row

        Gdiplus::SolidBrush overlayBg(Gdiplus::Color(208, 24, 24, 28)); // mostly-opaque dark
        const float panelH = (firstRowY - chartTop) + 3.9f * rowStep;   // cover all lines above + 4 rows
        graphics.FillRectangle(&overlayBg, (int)chartLeft, (int)(panelTop - 5),
                               (int)(chartRight - chartLeft), (int)panelH);

        Gdiplus::SolidBrush greyText(Gdiplus::Color(255, 176, 178, 184));
        wchar_t sampleBuf[32];
        swprintf(sampleBuf, 32, L"%llu %ls", (unsigned long long)totalSamples, isFr ? L"\u00e9chantillons" : L"samples");
        graphics.DrawString(sampleBuf, -1, &fontReg, Gdiplus::PointF(colLabelX + 4.0f, samplesY), &greyText);

        // Group headers centered over their three value columns each.
        Gdiplus::SolidBrush whiteText(Gdiplus::Color(255, 240, 240, 244));
        {
            const wchar_t* grpReb = isFr ? L"D\u00e9tente" : L"Rebound";
            Gdiplus::RectF gb; graphics.MeasureString(grpReb, -1, &fontBold, Gdiplus::PointF(0, 0), &gb);
            float rebCenter = valStartX + 1.5f * cellW;
            graphics.DrawString(grpReb, -1, &fontBold, Gdiplus::PointF(rebCenter - gb.Width / 2.0f, groupHdrY), &whiteText);

            const wchar_t* grpBump = isFr ? L"Compression" : L"Bump";

            Gdiplus::RectF bb; graphics.MeasureString(grpBump, -1, &fontBold, Gdiplus::PointF(0, 0), &bb);
            float bumpCenter = bumpStartX + 1.5f * cellW;
            graphics.DrawString(grpBump, -1, &fontBold, Gdiplus::PointF(bumpCenter - bb.Width / 2.0f, groupHdrY), &whiteText);
        }

        // Sub-captions per side: rebound = Total/Fast/Slow; bump = Slow/Fast/Total.
        {
            const wchar_t* avgCap  = isFr ? L"Moyenne %" : L"Average %";
            const wchar_t* capsReb[3]  = { avgCap, isFr ? L"Rapide %" : L"Fast %",
                                           isFr ? L"Lent %" : L"Slow %" };
            const wchar_t* capsBump[3] = { isFr ? L"Lent %" : L"Slow %", isFr ? L"Rapide %" : L"Fast %",
                                           avgCap };

            for (int c = 0; c < 3; ++c) {
                Gdiplus::RectF cb; graphics.MeasureString(capsReb[c], -1, &fontReg, Gdiplus::PointF(0, 0), &cb);
                float cx = valStartX + (float)c * cellW;
                graphics.DrawString(capsReb[c], -1, &fontReg,
                    Gdiplus::PointF(cx + (cellW - cb.Width) / 2.0f, subCapY), &greyText);
            }

            for (int c = 0; c < 3; ++c) {
                Gdiplus::RectF cb; graphics.MeasureString(capsBump[c], -1, &fontReg, Gdiplus::PointF(0, 0), &cb);
                float cx = bumpStartX + (float)c * cellW;
                graphics.DrawString(capsBump[c], -1, &fontReg,
                    Gdiplus::PointF(cx + (cellW - cb.Width) / 2.0f, subCapY), &greyText);
            }
        }

        // Four data rows: colored corner label + six % values; "--" when invalid.
        const wchar_t* cornerNames[4] = { L"Damper Vel FL", L"Damper Vel FR", L"Damper Vel RL", L"Damper Vel RR" };
        Gdiplus::Color colors[4] = {
            Gdiplus::Color(255, 237, 64, 59),   // FL — red
            Gdiplus::Color(255, 255, 193, 7),   // FR — amber/yellow
            Gdiplus::Color(255, 80, 220, 104),  // RL — green
            Gdiplus::Color(255, 64, 156, 255)   // RR — blue/cyan
        };

        for (int i = 0; i < 4; ++i) {
            Gdiplus::SolidBrush cb(colors[i]);
            float y = firstRowY + (float)i * rowStep;

            graphics.DrawString(cornerNames[i], -1, &fontReg, Gdiplus::PointF(colLabelX + 4.0f, y), &cb);

            wchar_t vals[6][16];
            if (!corner[i].hasAny()) {
                for (int c = 0; c < kValCols; ++c) wcscpy_s(vals[c], 16, L"--");
            } else {
                if (corner[i].reboundAvgPct >= 0.0)
                    swprintf(vals[0], 16, L"%.1f%%", corner[i].reboundAvgPct); // Reb avg % = (Fast+Slow)/2
                else wcscpy_s(vals[0], 16, L"--");
                swprintf(vals[1], 16, L"%.1f%%", corner[i].reboundHiPct);     // Reb fast %
                swprintf(vals[2], 16, L"%.1f%%", corner[i].reboundLoPct);     // Reb slow %
                swprintf(vals[3], 16, L"%.1f%%", corner[i].bumpLoPct);        // Bump slow %
                swprintf(vals[4], 16, L"%.1f%%", corner[i].bumpHiPct);        // Bump fast %
                if (corner[i].bumpAvgPct >= 0.0)
                    swprintf(vals[5], 16, L"%.1f%%", corner[i].bumpAvgPct);    // Bump avg % = (Slow+Fast)/2
                else wcscpy_s(vals[5], 16, L"--");
            }

            for (int c = 0; c < kValCols; ++c) {   // centered in each value cell
                Gdiplus::RectF vb; graphics.MeasureString(vals[c], -1, &fontReg, Gdiplus::PointF(0, 0), &vb);
                float cx = (c < 3 ? valStartX : bumpStartX) + (float)(c % 3) * cellW;
                graphics.DrawString(vals[c], -1, &fontReg, Gdiplus::PointF(cx + (cellW - vb.Width) / 2.0f, y), &cb);
            }
        }
    }

    // --- Step 8d: grid + axes -------------------------------------------------
    double maxBin = suspensionMaxBin;
    if (!(maxBin > 0.0)) {
        for (int i = 0; i < 4; ++i)
            for (int b = 0; b < VelocityHistogram::kBinCount; ++b)
                if (suspensionData[i].valid && suspensionData[i].bins[b] > maxBin)
                    maxBin = suspensionData[i].bins[b];
    }

    double minFloorPct = std::max(g_thresholds.histogramMinYPct, 1.0) / 100.0; // floor even with no data
    double displayMax = minFloorPct;
    bool haveScale = false;
    if (maxBin > 0.0) {
        displayMax = std::max(maxBin * (1.0 + g_thresholds.histogramYOffset / 100.0),
                              g_thresholds.histogramMinYPct / 100.0);
        haveScale = true;
    }

    Gdiplus::Pen gridPen(Gdiplus::Color(60, 70, 70, 70), 1.0f);
    Gdiplus::SolidBrush axisText(Gdiplus::Color(255, 148, 150, 156));

    if (haveScale) {
        // Y ticks: ~5 values from 0 to displayMax*100 (%).
        double maxPct = displayMax * 100.0;
        int tickCount = 4; // -> 5 lines including zero baseline handled separately below
        for (int t = 0; t <= tickCount; ++t) {
            double pctVal = maxPct * ((double)t / (double)tickCount);
            float yPix = chartBottom - (float)(pctVal / 100.0f) * (chartHeight / displayMax);

            if (t > 0) // skip baseline duplicate at t=0; drawn as x-axis line below
                graphics.DrawLine(&gridPen, chartLeft, yPix, chartRight, yPix);

            wchar_t lbl[16];
            swprintf(lbl, 16, L"%.0f%%", pctVal);
            Gdiplus::RectF lb;
            graphics.MeasureString(lbl, -1, &fontReg, Gdiplus::PointF(0, 0), &lb);
            graphics.DrawString(lbl, -1, &fontReg,
                Gdiplus::PointF(chartLeft - lb.Width - 6.0f, yPix - lb.Height / 2.0f), &axisText);
        }

        // X ticks: fixed −200 … +200 step 50 mm/s; vertical grid lines at each tick.
        for (int val = -200; val <= 200; val += 50) {
            float xPix = chartLeft + ((float)(val + 205) / 410.0f) * chartWidth;

            if (val != 0) // zero line drawn separately, brighter/dashed below
                graphics.DrawLine(&gridPen, xPix, chartTop, xPix, chartBottom);

            wchar_t xl[8];
            swprintf(xl, 8, L"%d", val);
            Gdiplus::RectF xb;
            graphics.MeasureString(xl, -1, &fontReg, Gdiplus::PointF(0, 0), &xb);
            float lx = std::max(chartLeft, std::min(xPix - xb.Width / 2.0f, chartRight - xb.Width));
            graphics.DrawString(xl, -1, &fontReg, Gdiplus::PointF(lx, chartBottom + 4.0f), &axisText);
        }

        // Zero-velocity line: brighter dashed vertical at x=0.
        float zeroX = chartLeft + (205.0f / 410.0f) * chartWidth;
        Gdiplus::Pen zeroPen(Gdiplus::Color(160, 130, 132, 138), 1.0f);
        const float dashPattern[2] = { 5.0f, 4.0f };
        zeroPen.SetDashPattern(dashPattern, 2);
        graphics.DrawLine(&zeroPen, zeroX, chartTop, zeroX, chartBottom);

        // Baseline (y=0) axis line across the bottom of the plot area.
        Gdiplus::Pen basePen(Gdiplus::Color(140, 96, 98, 104), 1.0f);
        graphics.DrawLine(&basePen, chartLeft, chartBottom, chartRight, chartBottom);

        // Side labels just above the baseline: rebound (v < 0) left half, bump/compression right half.
        {
            Gdiplus::Font sideLbl(L"Segoe UI", 8.0f, Gdiplus::FontStyleBold);
            Gdiplus::SolidBrush sideTxt(Gdiplus::Color(215, 168, 170, 178));

            const wchar_t* rebTxt = isFr ? L"D\u00e9tente" : L"Rebound";      // v < 0
            Gdiplus::RectF rb; graphics.MeasureString(rebTxt, -1, &sideLbl, Gdiplus::PointF(0, 0), &rb);
            float rebX = chartLeft + (zeroX - chartLeft) / 2.0f - rb.Width / 2.0f; // center of left half

            const wchar_t* bumpTxt = isFr ? L"Compression" : L"Bump";         // v > 0
            Gdiplus::RectF bb; graphics.MeasureString(bumpTxt, -1, &sideLbl, Gdiplus::PointF(0, 0), &bb);
            float bumpX = zeroX + (chartRight - zeroX) / 2.0f - bb.Width / 2.0f; // center of right half

            graphics.DrawString(rebTxt, -1, &sideLbl, Gdiplus::PointF(rebX, chartBottom - rb.Height - 3), &sideTxt);
            graphics.DrawString(bumpTxt, -1, &sideLbl, Gdiplus::PointF(bumpX, chartBottom - bb.Height - 3), &sideTxt);
        }

        // --- Step 8e: slow-speed band shading ---------------------------------
        double slowRange = g_thresholds.damperSlowSpeedRange;
        if (slowRange > 0) {
            float xLo = chartLeft + ((float)((int)-slowRange + 205) / 410.0f) * chartWidth;
            float xHi = chartLeft + ((float)((int)+slowRange + 205) / 410.0f) * chartWidth;
            if (xLo < chartLeft) xLo = chartLeft;
            if (xHi > chartRight) xHi = chartRight;
            Gdiplus::SolidBrush slowBand(Gdiplus::Color(50, 120, 140, 255)); // light blue tint
            graphics.FillRectangle(&slowBand, xLo, chartTop, std::max(0.0f, xHi - xLo), chartHeight);
        }

        // --- Step 8f: per-corner curves (area fill + polyline) -----------------
        const Gdiplus::Color colors[4] = {
            Gdiplus::Color(255, 237, 64, 59),   // FL — red
            Gdiplus::Color(255, 255, 193, 7),   // FR — amber/yellow
            Gdiplus::Color(255, 80, 220, 104),  // RL — green
            Gdiplus::Color(255, 64, 156, 255)   // RR — blue/cyan
        };

        for (int i = 0; i < 4; ++i) {
            if (!suspensionData[i].valid) continue;

            const int kBin = VelocityHistogram::kBinCount;
            Gdiplus::PointF pts[kBin];
            float scaleY = (float)(chartHeight / displayMax); // fraction -> pixels

            for (int b = 0; b < kBin; ++b) {
                double binCenter = -VelocityHistogram::rangeMax() + (b + 0.5) * VelocityHistogram::binWidth();
                float xPix = chartLeft + ((float)(binCenter + 205.0f) / 410.0f) * chartWidth;
                float yPix = chartBottom - (float)suspensionData[i].bins[b] * scaleY;
                pts[b].X = xPix;
                pts[b].Y = std::max(chartTop, yPix); // clamp to plot area top
            }

            Gdiplus::Color fillColor(50, colors[i].GetR(), colors[i].GetG(), colors[i].GetB());
            Gdiplus::SolidBrush fill(fillColor);  // translucent area under curve (alpha=50)
            {
                Gdiplus::PointF poly[kBin + 2];
                for (int b = 0; b < kBin; ++b) poly[b] = pts[b];
                poly[kBin].X   = pts[kBin - 1].X;     // close along baseline to bottom-right corner
                poly[kBin].Y   = chartBottom;
                poly[kBin + 1].X = pts[0].X;          // back up the left edge
                poly[kBin + 1].Y = chartBottom;
                graphics.FillPolygon(&fill, poly, kBin + 2);
            }

            Gdiplus::Pen line(colors[i], 2.0f);       // full-opacity curve on top
            graphics.DrawLines(&line, pts, kBin);
        }
    } else {
        // No data at all: draw a minimal axis frame so the tab doesn't look broken/empty-black.
        Gdiplus::Pen basePen(Gdiplus::Color(140, 96, 98, 104), 1.0f);
        graphics.DrawLine(&basePen, chartLeft, chartBottom, chartRight, chartBottom);

        for (int val = -200; val <= 200; val += 50) {
            float xPix = chartLeft + ((float)(val + 205) / 410.0f) * chartWidth;
            wchar_t xl[8];
            swprintf(xl, 8, L"%d", val);
            Gdiplus::RectF xb;
            graphics.MeasureString(xl, -1, &fontReg, Gdiplus::PointF(0, 0), &xb);
            float lx = std::max(chartLeft, std::min(xPix - xb.Width / 2.0f, chartRight - xb.Width));
            graphics.DrawString(xl, -1, &fontReg, Gdiplus::PointF(lx, chartBottom + 4.0f), &axisText);
        }

        const wchar_t* noData = isFr ? L"Aucune donn\u00e9e" : L"No data";
        Gdiplus::RectF nd;
        graphics.MeasureString(noData, -1, &fontReg, Gdiplus::PointF(0, 0), &nd);
        graphics.DrawString(noData, -1, &fontReg,
            Gdiplus::PointF(chartLeft + (chartWidth - nd.Width) / 2.0f, chartTop + (chartHeight - nd.Height) / 2.0f),
            &axisText);
    }
}

// Step 7c: persist the active damper layout tab to [General] HistogramLayout.
void __fastcall TFormMotecSetup::DamperLayoutTabsChange(TObject *Sender) {
    (void)Sender;
    if (DamperLayoutTabs != NULL)
        SaveHistogramLayoutSetting(DamperLayoutTabs->ActivePageIndex);
}

void __fastcall TFormMotecSetup::resetAnalysisGraphics()
{
    for (int i = 0; i < 4; ++i) {
        tireTemps[i].inner = 0.0;
        tireTemps[i].middle = 0.0;
        tireTemps[i].outer = 0.0;
        tireTemps[i].pressure = 0.0;
        tireTemps[i].pressureValid = false;
        tireTemps[i].valid = false;

        suspensionData[i].travel = 0.0;
        suspensionData[i].bumpstop = 0.0;
        suspensionData[i].issue = false;
        suspensionData[i].valid = false;
        suspensionData[i].totalCount = 0;
        for (int b = 0; b < VelocityHistogram::kBinCount; ++b)
            suspensionData[i].bins[b] = 0.0;
    }

    suspensionMaxBin = 0.0;

    hasTireTempData = false;
    aeroBalanceValue = 0.0;
    aeroBalanceValid = false;

    for (int i = 0; i < 4; ++i) {
        brakeTemps[i].avg = 0.0;
        brakeTemps[i].max = 0.0;
        brakeTemps[i].valid = false;
    }
    hasBrakeTempData = false;

    if (TireTempPaintBox != NULL)
        TireTempPaintBox->Invalidate();
    if (BrakeTempPaintBox != NULL)
        BrakeTempPaintBox->Invalidate();
    if (DamperFLPaintBox != NULL)
        DamperFLPaintBox->Invalidate();
    if (DamperFRPaintBox != NULL)
        DamperFRPaintBox->Invalidate();
    if (DamperRLPaintBox != NULL)
        DamperRLPaintBox->Invalidate();
    if (DamperRRPaintBox != NULL)
        DamperRRPaintBox->Invalidate();
    if (DamperCombinedPaintBox != NULL)
        DamperCombinedPaintBox->Invalidate();
}

void __fastcall TFormMotecSetup::UpdateTireTemps(const std::vector<LapStats>& laps)
{
    if (laps.empty()) {
        hasTireTempData = false;
        if (TireTempPaintBox != NULL)
            TireTempPaintBox->Invalidate();
        return;
    }

    LapStats avg = buildAverageLapData(laps);

    tireTemps[0].inner  = avg.tyreInnerTempLF.average();
    tireTemps[0].middle = avg.tyreCentreTempLF.average();
    tireTemps[0].outer  = avg.tyreOuterTempLF.average();
    tireTemps[0].valid  = (avg.tyreInnerTempLF.count > 0 && avg.tyreCentreTempLF.count > 0 && avg.tyreOuterTempLF.count > 0);
    tireTemps[0].pressure = avg.pressureLF.average();
    tireTemps[0].pressureValid = (avg.pressureLF.count > 0);

    tireTemps[1].inner  = avg.tyreInnerTempRF.average();
    tireTemps[1].middle = avg.tyreCentreTempRF.average();
    tireTemps[1].outer  = avg.tyreOuterTempRF.average();
    tireTemps[1].valid  = (avg.tyreInnerTempRF.count > 0 && avg.tyreCentreTempRF.count > 0 && avg.tyreOuterTempRF.count > 0);
    tireTemps[1].pressure = avg.pressureRF.average();
    tireTemps[1].pressureValid = (avg.pressureRF.count > 0);

    tireTemps[2].inner  = avg.tyreInnerTempLR.average();
    tireTemps[2].middle = avg.tyreCentreTempLR.average();
    tireTemps[2].outer  = avg.tyreOuterTempLR.average();
    tireTemps[2].valid  = (avg.tyreInnerTempLR.count > 0 && avg.tyreCentreTempLR.count > 0 && avg.tyreOuterTempLR.count > 0);
    tireTemps[2].pressure = avg.pressureLR.average();
    tireTemps[2].pressureValid = (avg.pressureLR.count > 0);

    tireTemps[3].inner  = avg.tyreInnerTempRR.average();
    tireTemps[3].middle = avg.tyreCentreTempRR.average();
    tireTemps[3].outer  = avg.tyreOuterTempRR.average();
    tireTemps[3].valid  = (avg.tyreInnerTempRR.count > 0 && avg.tyreCentreTempRR.count > 0 && avg.tyreOuterTempRR.count > 0);
    tireTemps[3].pressure = avg.pressureRR.average();
    tireTemps[3].pressureValid = (avg.pressureRR.count > 0);

    aeroBalanceValue = avg.aeroBalance.average();
    aeroBalanceValid = (avg.aeroBalance.count > 0);

    hasTireTempData = tireTemps[0].valid || tireTemps[1].valid || tireTemps[2].valid || tireTemps[3].valid;
    if (TireTempPaintBox != NULL)
        TireTempPaintBox->Invalidate();
}

// Normalized vehicle name: lowercase alphanumerics only (spaces/underscores/dots dropped).
static std::string normalizeVehicleName(const std::string& s) {
    std::string out;
    for (std::size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
            out += (char)(c | 32);   // tolower for ASCII letters
        } else if (c >= '0' && c <= '9') {
            out += c;
        }
    }
    return out;
}

// DB lookup with a normalized-name fallback: AMS2 shared memory may report the car name
// in a slightly different form than the embedded-DB key ("Alpine A110 GT4 Evo" vs "Alpine_A110_GT4_Evo").
static bool findVehicleInDb(const std::string& rawName, VehicleInfo& out) {
    if (Ams2VehicleDb::instance().find(rawName, out)) return true;

    const Ams2VehicleDb& db = Ams2VehicleDb::instance();
    if (!db.isValid()) return false;
    std::vector<std::string> names;
    db.allNames(names);
    const std::string normTarget = normalizeVehicleName(rawName);
    for (std::size_t i = 0; i < names.size() && !normTarget.empty(); ++i) {
        if (normalizeVehicleName(names[i]) == normTarget) {
            return db.find(names[i], out);   // exact key -> full parse guaranteed
        }
    }
    return false;
}

// Display vehicle name for Summary tab: prefer DB alias over raw car name.
// Appends '*' to mark that the displayed name is an in-game alias, not the
// technical model identifier. Non-AMS2 names (AC EVO / PMR) never match the
// embedded DB and pass through unchanged.
static String displayVehicleName(const std::string& rawName)
{
    VehicleInfo info;
    if (!rawName.empty() && findVehicleInDb(rawName, info) && !info.alias.empty())
        return liveNameUtf8(info.alias) + L"*";
    return liveNameUtf8(rawName);
}

void __fastcall TFormMotecSetup::UpdateBrakeTemps(const std::vector<LapStats>& laps)
{
    if (laps.empty()) {
        hasBrakeTempData = false;
        for (int i = 0; i < 4; ++i) brakeFadeValid[i] = false;
        if (BrakeTempPaintBox) BrakeTempPaintBox->Invalidate();
        return;
    }

    // Use averaged data for brake and engine temperatures across all valid laps.
    // NOTE: buildAverageLapData() returns an EMPTY LapStats when fewer than 2 valid
    // laps exist (single-lap live sessions) -- its vehicleName is then empty, so the
    // car name must fall back to any lap that carries one for the fade lookup below.
    LapStats avg = buildAverageLapData(laps);

    std::string carName = avg.vehicleName;
    if (carName.empty() || carName == "Unknown vehicle") {
        for (std::size_t i = 0; i < laps.size(); ++i) {
            const std::string& n = laps[i].vehicleName;
            if (!n.empty() && n != "Unknown vehicle") { carName = n; break; }
        }
    }

    // Per-wheel fade threshold from the embedded AMS2 vehicle DB (AMS2 only).
    // Unknown car / missing wheel entry -> flags stay false, no Fade line drawn.
    for (int i = 0; i < 4; ++i) brakeFadeValid[i] = false;
    if (!carName.empty()) {
        VehicleInfo fadeCar;
        if (findVehicleInDb(carName, fadeCar)) {
            for (int i = 0; i < 4; ++i) {
                const WheelBrakeData& wb = fadeCar.chassis.brakes[i];
                if (wb.present && wb.hasFadeTemps) {
                    brakeFadeThresholdC[i] = wb.fadeRangeAboveC;
                    brakeFadeValid[i] = true;
                }
            }
        }
    }

    for (int i = 0; i < 4; ++i) {
        const StatSummary* summary = nullptr;
        switch (i) {
            case 0: summary = &avg.brakeTempLF; break; // FL
            case 1: summary = &avg.brakeTempRF; break; // FR
            case 2: summary = &avg.brakeTempLR; break; // RL
            case 3: summary = &avg.brakeTempRR; break; // RR
        }

        if (summary && summary->count > 0) {
            brakeTemps[i].avg = summary->average(); 
            brakeTemps[i].max = summary->max;
            brakeTemps[i].valid = true;
        } else {
            brakeTemps[i].valid = false;
        }
    }

    // Water & Oil temperatures
    if (avg.waterTemp.count > 0) {
        waterTemp.avg = avg.waterTemp.average();
        waterTemp.max = avg.waterTemp.max;
        waterTemp.valid = true;
    } else {
        waterTemp.valid = false;
    }

    if (avg.oilTemp.count > 0) {
        oilTemp.avg = avg.oilTemp.average();
        oilTemp.max = avg.oilTemp.max;
        oilTemp.valid = true;
    } else {
        oilTemp.valid = false;
    }

    // Append per-wheel fade threshold text to MemoBrake (from the AMS2 vehicle DB).
    if (MemoBrake != NULL) {
        bool anyFade = false;
        for (int i = 0; i < 4; ++i) if (brakeFadeValid[i]) { anyFade = true; break; }
        if (anyFade && !carName.empty()) {
            const bool isFr2 = ReadReportLanguageSetting() == ReportLanguage::French;
            MemoBrake->Lines->Add(String(L""));  // blank separator line

            String carNameWide(carName.c_str());   // narrow -> wide (ANSI codepage)
			MemoBrake->Lines->Add((isFr2 ? L"Seuil d'éfficacité température Max: " : L"Max Temp Fade threshold : ") + carNameWide);

            const wchar_t* cornerNames[4] = {
                isFr2 ? L"AV G" : L"FL",
                isFr2 ? L"AV D" : L"FR",
                isFr2 ? L"AR G" : L"RL",
                isFr2 ? L"AR D" : L"RR"
            };
            for (int i = 0; i < 4; ++i) {
                if (!brakeFadeValid[i]) continue;
                wchar_t lineBuf[64];
                swprintf(lineBuf, 64, L"  %s: %.0f\u00b0C", cornerNames[i], brakeFadeThresholdC[i]);
                MemoBrake->Lines->Add(String(lineBuf));
            }
        }
    }

    hasBrakeTempData = true;
    if (BrakeTempPaintBox) BrakeTempPaintBox->Invalidate();
}

void __fastcall TFormMotecSetup::TireTempPaintBoxPaint(TObject *Sender)
{
    // Initialize GDI+ Graphics from VCL Canvas
    Gdiplus::Graphics graphics(TireTempPaintBox->Canvas->Handle);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);

    TCanvas* canvas = TireTempPaintBox->Canvas;
    canvas->Brush->Color = clWhite;
    canvas->FillRect(TRect(0, 0, TireTempPaintBox->Width, TireTempPaintBox->Height));

    const bool isFr = ReadReportLanguageSetting() == ReportLanguage::French;

    if (!hasTireTempData) {
        canvas->Font->Color = clGray;
        canvas->Font->Name = "Segoe UI";
        canvas->Font->Size = 10;
        canvas->Font->Style = TFontStyles();
        String msg = isFr ? String(L"Aucune donn\u00e9e de temp\u00e9rature pneu") : L"No tyre temp data";
        int tw = canvas->TextWidth(msg);
        canvas->TextOut((TireTempPaintBox->Width - tw) / 2, TireTempPaintBox->Height / 2 - 10, msg);
        return;
    }

    // FL/FR/RL/RR are universal in English; use AV G / AV D / AR G / AR D (avant/arri\u00e8re + gauche/droite) in French.
    const String tireNames[4] = {
        isFr ? "AV G" : "FL",
        isFr ? "AV D" : "FR",
        isFr ? "AR G" : "RL",
        isFr ? "AR D" : "RR"
    };
    const int boxW = TireTempPaintBox->Width;
    const int boxH = TireTempPaintBox->Height;
    const int tireW = 70;
    const int tireH = 90;
    const int pressureH = 18;
    const int labelH = 16;
    const int tempH = 14;
    const int contentH = labelH + pressureH + tireH + tempH;
    const int rowGap = 20;
    const int totalBlockH = 2 * contentH + rowGap;
    int startY = 5;
    const int aeroW = 70;
    const int tireAreaW = boxW - aeroW;
    const int cellW = tireAreaW / 2;

    for (int t = 0; t < 4; t++) {
        int col = t % 2;
        int row = t / 2;
        int cellX = col * cellW;
        int cellY = startY + row * (contentH + rowGap);
        int centerX = cellX + cellW / 2;

        // Tire name label
        canvas->Font->Color = clBlack;
        canvas->Font->Name = "Segoe UI";
        canvas->Font->Size = 9;
        TFontStyles boldStyle;
        boldStyle << fsBold;
        canvas->Font->Style = boldStyle;
        String nameStr = tireNames[t];
        int nameW = canvas->TextWidth(nameStr);
        canvas->TextOut(centerX - nameW / 2, cellY + 2, nameStr);

        // Pressure
        int yCursor = cellY + labelH;
        canvas->Font->Size = 8;
        canvas->Font->Style = TFontStyles();
        if (tireTemps[t].pressureValid) {
            String pressStr = FloatToStrF(tireTemps[t].pressure, ffFixed, 5, 1) + " psi";
            int pressW = canvas->TextWidth(pressStr);
            canvas->Font->Color = TColor(RGB(0, 100, 0));
            canvas->TextOut(centerX - pressW / 2, yCursor, pressStr);
        } else {
            canvas->Font->Color = clGray;
            canvas->TextOut(centerX - 5, yCursor, "-");
        }
        yCursor += pressureH;

        // Tire graphic
        int tireX = centerX - tireW / 2;
        int tireY = yCursor;
        bool isLeftSide = (t == 0 || t == 2);

        double inner = tireTemps[t].inner;
        double middle = tireTemps[t].middle;
        double outer = tireTemps[t].outer;

        bool middleBetween = (middle > inner && middle < outer) || (middle < inner && middle > outer);
        TColor middleColor = middleBetween ? TColor(RGB(0, 200, 0)) : TColor(RGB(255, 165, 0));
        TColor highColor = TColor(RGB(0, 128, 0));
        TColor lowColor = TColor(RGB(144, 238, 144));
        TColor innerColor = (inner >= outer) ? highColor : lowColor;
        TColor outerColor = (outer >= inner) ? highColor : lowColor;

        TColor colors[3];
        if (isLeftSide) {
            colors[0] = outerColor;
            colors[1] = middleColor;
            colors[2] = innerColor;
        } else {
            colors[0] = innerColor;
            colors[1] = middleColor;
            colors[2] = outerColor;
        }

        // Draw tire outline
        canvas->Pen->Color = TColor(RGB(80, 80, 80));
        canvas->Pen->Width = 2;
        canvas->Brush->Color = clWhite;
        canvas->Rectangle(tireX, tireY, tireX + tireW, tireY + tireH);
        canvas->Pen->Width = 1;

        // Draw 3 vertical bands inside tire
        int innerPad = 4;
        int bandStartX = tireX + innerPad;
        int bandStartY = tireY + innerPad;
        int bandDrawW = (tireW - 2 * innerPad - 4) / 3;
        int bandDrawH = tireH - 2 * innerPad;

        for (int b = 0; b < 3; b++) {
            int bx = bandStartX + b * (bandDrawW + 2);
            canvas->Brush->Color = colors[b];
            canvas->Pen->Color = TColor(RGB(100, 100, 100));
            canvas->Pen->Width = 1;
            canvas->Rectangle(bx, bandStartY, bx + bandDrawW, bandStartY + bandDrawH);
        }

        // Temperature values below tire
        int tempY = tireY + tireH + 4;
        canvas->Brush->Color = clWhite;
        canvas->Font->Color = clBlack;
        canvas->Font->Size = 7;
        canvas->Font->Style = TFontStyles();
        String tI = FloatToStrF(tireTemps[t].inner, ffFixed, 4, 1) + L"\u00B0";
        String tM = FloatToStrF(tireTemps[t].middle, ffFixed, 4, 1) + L"\u00B0";
        String tO = FloatToStrF(tireTemps[t].outer, ffFixed, 4, 1) + L"\u00B0";

        // Order: Left side (FL/RL) -> Outer, Middle, Inner | Right side (FR/RR) -> Inner, Middle, Outer
        String s1, s2, s3;
        if (isLeftSide) {
            s1 = tO; s2 = tM; s3 = tI;
        } else {
            s1 = tI; s2 = tM; s3 = tO;
        }

        // Center the three values as a group under the tire, with enough gap to keep 3-digit temps readable.
        const int tempGap = 12;   // px between temperature strings (was 4 — too tight for "127.9Â°")
        int totalW = canvas->TextWidth(s1) + canvas->TextWidth(s2) + canvas->TextWidth(s3) + 2 * tempGap;
        int tx = centerX - totalW / 2;
        canvas->TextOut(tx, tempY, s1);
        tx += canvas->TextWidth(s1) + tempGap;
        canvas->TextOut(tx, tempY, s2);
        tx += canvas->TextWidth(s2) + tempGap;
        canvas->TextOut(tx, tempY, s3);

    }

    // --- Aero balance indicator (car icon + line + triangle) in the right column ---
    if (aeroBalanceValid) {
        int aeroCX = tireAreaW + aeroW / 2;
        int blockMidY = startY + totalBlockH / 2;

        // Title above the indicator
        canvas->Brush->Color = clWhite;
        canvas->Font->Name = "Segoe UI";
        canvas->Font->Size = 7;
        TFontStyles titleBold;
        titleBold << fsBold;
        canvas->Font->Style = titleBold;
        canvas->Font->Color = clBlack;
        String titleStr = "Aero Balance";
        int titleW = canvas->TextWidth(titleStr);
        int titleY = blockMidY - 60;
        canvas->TextOut(aeroCX - titleW / 2, titleY, titleStr);
        canvas->Font->Style = TFontStyles();

        // Draw car as emoji icon
        canvas->Brush->Color = clWhite;
        canvas->Font->Name = "Segoe UI Emoji";
        canvas->Font->Size = 34;
        canvas->Font->Color = clBlack;
        String carIcon = L"\U0001F3CE";
        int iconW = canvas->TextWidth(carIcon);
        int iconH = canvas->TextHeight(carIcon);
        int iconX = aeroCX - iconW / 2;
        int iconY = blockMidY - iconH / 2 - 4;
        canvas->TextOut(iconX, iconY, carIcon);

        // Balance line below car (same width as icon)
        int lineY = iconY + iconH + 4;
        int lineX1 = iconX;
        int lineX2 = iconX + iconW;
        canvas->Pen->Color = clBlack;
        canvas->Pen->Width = 2;
        canvas->MoveTo(lineX1, lineY);
        canvas->LineTo(lineX2, lineY);
        canvas->Pen->Width = 1;

        // Legend: "Front" at left end, "Rear" at right end
        canvas->Brush->Color = clWhite;
        canvas->Font->Name = "Segoe UI";
        canvas->Font->Size = 7;
        canvas->Font->Color = TColor(RGB(100, 100, 100));
        canvas->Font->Style = TFontStyles();
        String frontLbl = "Front";
        String rearLbl = "Rear";
        int frontW = canvas->TextWidth(frontLbl);
        int rearW = canvas->TextWidth(rearLbl);
        int lblY = lineY + 28;
        canvas->TextOut(lineX1 - frontW / 2, lblY, frontLbl);
        canvas->TextOut(lineX2 - rearW / 2, lblY, rearLbl);

        // Triangle marker (position based on aeroBalanceValue: 0%=front, 100%=rear)
        double frac = aeroBalanceValue / 100.0;
        if (frac < 0.0) frac = 0.0;
        if (frac > 1.0) frac = 1.0;
        int triX = lineX1 + (int)(frac * (lineX2 - lineX1));
        TColor markerColor = TColor(RGB(0, 100, 0));

        // Draw triangle using MoveTo/LineTo (pointing down from the line)
        int triSize = 8;
        canvas->Pen->Color = markerColor;
        canvas->Pen->Width = 2;
        canvas->MoveTo(triX - triSize, lineY + 1);
        canvas->LineTo(triX + triSize, lineY + 1);
        canvas->LineTo(triX, lineY + 1 + triSize);
        canvas->LineTo(triX - triSize, lineY + 1);
        canvas->Pen->Width = 1;

        int triBot = lineY + 1 + triSize;

        // Numeric value
        String valStr = FloatToStrF(aeroBalanceValue, ffFixed, 5, 1) + "%";
        canvas->Brush->Color = clWhite;
        canvas->Font->Color = clBlack;
        canvas->Font->Name = "Segoe UI";
        canvas->Font->Size = 9;
        TFontStyles boldStyle;
        boldStyle << fsBold;
        canvas->Font->Style = boldStyle;
        int valW = canvas->TextWidth(valStr);
        canvas->TextOut(triX - valW / 2, triBot + 2, valStr);
        canvas->Font->Style = TFontStyles();
    }
}

// Removed legacy SuspensionPaintBoxPaint implementation as it is replaced by TChart components.



bool __fastcall TFormMotecSetup::AnalyzeSelectedLdFile(const std::string& filePath, std::string& errorMessage)
{
    resetAnalysisGraphics();
    Button1->Enabled = false;
    UpdateProgress(0, "Analyzing LD/LDX file...");
    std::vector<LapStats> laps;
    ChannelProfile profile = ChannelProfile::EvoM1;
    if (ChannelProfileCombo != NULL)
        profile = (ChannelProfile)ChannelProfileCombo->ItemIndex;

    if (!parseMotecLd(filePath, laps, errorMessage, AnalysisProgress, profile)) {
        Button1->Enabled = true;
		//StatusBar1->SimpleText = "LD/LDX analysis failed";
		return false;
    }

    UpdateProgress(50, "Generating report...");
    ReportLanguage language = (LanguageCombo != NULL) ? (ReportLanguage)LanguageCombo->ItemIndex : ReadReportLanguageSetting();
	ReportResult result = buildAverageLapReport(laps, language, profile);

    UpdateReportTabs(result);
    publishToTabletServer(result, laps);
    std::vector<LapStats> validLaps = LapReportGenerator::getValidLaps(laps);
    UpdateTireTemps(validLaps);
    UpdateBrakeTemps(validLaps);
    UpdateSuspension(validLaps);
    if (TabSummary != NULL && TabSummary->Visible)
        PageControlReport->ActivePage = TabSummary;
    Button1->Enabled = true;
    ProgressBarExport->Position = 100;
	//StatusBar1->SimpleText = "LD/LDX analysis complete";
	return true;
}

void __fastcall TFormMotecSetup::ButtonLoadClick(TObject *Sender)
{
	TOpenDialog* dialog = new TOpenDialog(nullptr);
	try {
		ChannelProfile profile2 = ChannelProfile::EvoM1;
		if (ChannelProfileCombo != NULL)
			profile2 = (ChannelProfile)ChannelProfileCombo->ItemIndex;
		String section2 = LdSectionForProfile(profile2);
		dialog->InitialDir = ReadLastFolder(section2, "LdImport");
		dialog->Filter = L"LD/LDX files (*.ld;*.ldx)|*.ld;*.ldx|All files (*.*)|*.*";
		dialog->Options << ofFileMustExist;

		if (dialog->Execute()) {
			UnicodeString uPath = dialog->FileName;
			UTF8String utf8 = uPath;
			std::string filePath = utf8.c_str();
			SaveLastFolder(section2, ExtractFileDir(uPath));
			//MemoReport->Lines->Add("PATH = " + String(filePath.c_str()));
			std::string errorMessage;
			if (!AnalyzeSelectedLdFile(filePath, errorMessage))
				MemoSummary->Text = "LD/LDX analysis failed.\n" + String(errorMessage.c_str());
		}

	}
	__finally {
		delete dialog;
	}
}


void __fastcall TFormMotecSetup::Button1Click(TObject *Sender)
{
	TOpenDialog* dialog = new TOpenDialog(nullptr);
	try {
		ChannelProfile profile = ChannelProfile::EvoM1;
		if (ChannelProfileCombo != NULL)
			profile = (ChannelProfile)ChannelProfileCombo->ItemIndex;
		String section = LdSectionForProfile(profile);
		dialog->InitialDir = ReadLastFolder(section, "LdImport");
		dialog->Filter = L"LD/LDX files (*.ld)|*.ld|All files (*.*)|*.*";
		dialog->Options << ofFileMustExist;

		if (dialog->Execute()) {
			UnicodeString uPath = dialog->FileName;
			UTF8String utf8 = uPath;
			std::string filePath = utf8.c_str();
			SaveLastFolder(section, ExtractFileDir(uPath));
			std::string errorMessage;
			if (!AnalyzeSelectedLdFile(filePath, errorMessage))
				MemoSummary->Text = "LD/LDX analysis failed.\n" + String(errorMessage.c_str());
		}

	}
	__finally {
		delete dialog;
	}

}




//---------------------------------------------------------------------------
// Live capture from AMS2 shared memory
//---------------------------------------------------------------------------

void __fastcall TFormMotecSetup::resetLiveSession()
{
	liveInPit = true;
	liveLapInProgress = false;
	liveLaps.clear();
	liveLapNumber = 0;
	sessionLapCounter = 0;
	lastCompletedLaps = 0;
	liveCurrentLap = LapStats();
	liveWasOnTrack = false;
	firstLapStarted = false;
	previous_mCurrentTime = 0.0f;
	lastPitMode = -1;

	pmrPrevSector = -1;      // PMR: no sector state until the first participant packet arrives
	liveEvoPrevNPos = -1.0f;  // AC EVO: no npos history until back on track
	liveEvoPitReportDone = false;

	// Frame-sync change-detection resets (plan Â§4/Â§5): after a session reset the
	// first real frame from each sim must count as NewFrame again. Cheap no-ops
	// for whichever source is not currently connected, so both are safe to call.
	Ams2ResetFrameTracking();
	AcsEvoResetFrameTracking();

	// Drain buffer: frames queued by the capture thread belong to the OLD profile's
	// union member — draining them under a new liveMode would read garbage. The
	// idling capture thread only appends when connected, so clearing here is safe.
	{
	    std::lock_guard<std::mutex> lock(frameMutex);
	    frameBuffer.clear();
	}

	// Frame-sync adaptive-interval / FPS state reset (plan Â§6): start fresh measurement on the next session.
	liveLastNewFrameMs   = 0.0;
	liveMeasuredHz       = 0.0;
	liveEvoFpsAvg        = 0;
	liveAdaptiveCounter  = 0;

	// Session counters reset (plan Â§10a): collection rate is measured from the next session's first frame.
	liveFrameCount       = 0;
	liveSessionStartMs   = 0.0;

	// packetId jump diagnostic resets with the session so stale tokens can't cause false positives on reconnect.
	resetEvoPacketJumpTracking();
}

void __fastcall TFormMotecSetup::updateResetButtonState()
{
	if (ButtonResetSession == NULL) return;
	bool autoChecked = (AutoDetectTrackCheckBox != NULL && AutoDetectTrackCheckBox->Checked);
	ButtonResetSession->Enabled = liveActive && !autoChecked;
}

void __fastcall TFormMotecSetup::ButtonResetSessionClick(TObject *Sender)
{
	if (!liveActive) return;
	resetLiveSession();

	// Clear all report memos so no stale text remains from the previous session.
	MemoSummary->Lines->Clear();
	MemoGeneral->Lines->Clear();
	MemoTyre->Lines->Clear();
	MemoBrake->Lines->Clear();
	MemoSuspension->Lines->Clear();
	MemoEngineCooling->Lines->Clear();
	MemoHandling->Lines->Clear();
    const bool isFrSum = ReadReportLanguageSetting() == ReportLanguage::French;
	MemoSummary->Lines->Add(isFrSum ? "Nouvelle session - prêt pour la collecte" : "Session reset — ready for new collection.");

	// Hide all data tabs (they'll reappear when the next report is generated).
	if (TabGeneral != NULL) TabGeneral->Visible = false;
	if (TabTyre != NULL) TabTyre->Visible = false;
	if (TabBrake != NULL) TabBrake->Visible = false;
	if (TabSuspension != NULL) TabSuspension->Visible = false;
	if (TabEngineCooling != NULL) TabEngineCooling->Visible = false;
	if (TabHandling != NULL) TabHandling->Visible = false;

	// Invalidate paint boxes so they redraw in their "no data" state.
	std::vector<LapStats> emptyLaps;
	UpdateTireTemps(emptyLaps);
	UpdateBrakeTemps(emptyLaps);
	UpdateSuspension(emptyLaps);
}

void __fastcall TFormMotecSetup::AutoDetectTrackCheckBoxClick(TObject *Sender)
{
	if (AutoDetectTrackCheckBox != NULL)
		SaveAutoDetectTrackSetting(AutoDetectTrackCheckBox->Checked);
	updateResetButtonState();
}

void __fastcall TFormMotecSetup::LiveCaptureCheckBoxClick(TObject *Sender)
{
	bool isChecked = (LiveCaptureCheckBox != NULL && LiveCaptureCheckBox->Checked);
	SaveLiveCaptureSetting(isChecked);

    // Mutual exclusivity with import buttons
	if (isChecked) {
		if (ButtonLoad != NULL) ButtonLoad->Enabled = false;
		if (Button1 != NULL) Button1->Enabled = false;

		liveActive = true;
		liveConnected = false;

		// Determine mode based on current profile selection
		ChannelProfile profile = ChannelProfile::AMS2; // Default fallback
		if (ChannelProfileCombo != NULL) {
			profile = (ChannelProfile)ChannelProfileCombo->ItemIndex;
		}
		liveMode = ProfileToLiveMode(profile);

        MemoSummary->Lines->Clear();
        const bool isFrSum = ReadReportLanguageSetting() == ReportLanguage::French;
        MemoSummary->Lines->Add(isFrSum ? "Capture en direct activée. Recherche de la source..." : "Live capture enabled. Searching for source...");

		// Reset state for reconnection
		resetLiveSession();

		// LED red = not connected yet (reconnecting)
		const bool isFrConn2 = ReadReportLanguageSetting() == ReportLanguage::French;
		LiveLed->Brush->Color = clRed;
		LabelLiveStatus->Caption = isFrConn2 ? String(L"Connexion...") : String("Connecting...");

		// Start the connection timer to begin looking for shared memory/process
		LiveConnectTimer->Enabled = true;
		updateResetButtonState();
	} else {
		if (ButtonLoad != NULL) ButtonLoad->Enabled = true;
		if (Button1 != NULL) Button1->Enabled = true;

		stopLiveCapture(false);
		updateResetButtonState();
	}
}



void __fastcall TFormMotecSetup::LiveConnectTimerTimer(TObject *Sender)
{
    if (!liveActive || liveConnected)
        return;

    // Check if AMS2 process is actually running before attempting connection
    if (liveMode == LIVE_AMS2 && !Ams2IsProcessRunning()) {
        return; // Keep retrying, but don't try to open shared memory yet
    }

    std::string err;
    bool ok = false;

    if (liveMode == LIVE_EVO) {
        ok = AcsEvoOpenSharedMemory(err);
    } else if (liveMode == LIVE_PMR) {
        // PMR: bind the UDP socket. No process check — a successful bind means we are
        // ready to receive; telemetry arrives only while the game is running.
        String bindAddrW = ReadPmrBindAddressSetting().Trim();
        std::string bindAddr;  // empty string → INADDR_ANY (all interfaces)
        if (!bindAddrW.IsEmpty()) {
            int len = WideCharToMultiByte(CP_UTF8, 0, bindAddrW.c_str(), -1, nullptr, 0, nullptr, nullptr);
            if (len > 0) {
                bindAddr.resize(len - 1);
                WideCharToMultiByte(CP_UTF8, 0, bindAddrW.c_str(), -1, &bindAddr[0], len, nullptr, nullptr);
            }
        }
        int port = ReadPmrPortSetting();
        ok = PmrOpenSocket(bindAddr, port, err);
    } else {
        ok = Ams2OpenSharedMemory(err);
    }

    if (ok)
    {
        // Connected!
        const bool isFrConn3 = ReadReportLanguageSetting() == ReportLanguage::French;
        liveConnected = true;
        liveActive = true;
        LiveLed->Brush->Color = clLime;
        LabelLiveStatus->Caption = isFrConn3 ? String(L"Connect\u00e9") : String("Connected");
        LabelLiveStatus->Font->Color = clGreen;
        LiveConnectTimer->Enabled = false;

        // Start the high-rate data timer. AC EVO polls at a fixed fast rate from frame #1 so no physics
        // packet is ever overwritten between reads — see updateLiveAdaptiveInterval() for details.
        LiveTimer->Interval = 4;    // reset to default before adaptive logic takes over (plan Â§6)
        LiveTimer->Enabled = true;  // START the UI processing loop

        // Start the dedicated shared-memory capture thread for AC EVO and AMS2 so reads run
        // off the UI/VCL timer (bypasses Windows Timer resolution limits + keeps GDI+ repaints
        // from starving frame processing). PMR stays on the timer: its UDP drain is already
        // non-blocking. On reconnect the existing thread simply idles until liveConnected flips back.
        if ((liveMode == LIVE_EVO || liveMode == LIVE_AMS2) && captureThread == nullptr) {
            stopCaptureThread = false;
            captureThread = new std::thread(&TFormMotecSetup::captureThreadFunc, this);
        }

        liveLastNewFrameMs   = 0.0;
        liveMeasuredHz       = 0.0;
        liveEvoFpsAvg        = 0;
        liveAdaptiveCounter  = 0;

        // Session counters reset on connect too: a fresh connection starts a fresh collection-rate measurement (plan Â§10a).
        liveFrameCount       = 0;
        liveSessionStartMs   = 0.0;

        // packetId jump diagnostic resets with the session so stale tokens from a previous connection can't cause false positives.
        resetEvoPacketJumpTracking();
        if (liveMode == LIVE_EVO)
        {
            // Read static track name + variant from shared memory.
            // Combine "track" and "track_configuration" for a full display name
            // e.g. "Silverstone" + "GP" → "Silverstone GP".
            SPageFileStaticEvo stat;
            if (AcsEvoReadStatic(stat) && strlen(stat.track) > 0)
                liveTrackName = std::string(stat.track) +
                    (strlen(stat.track_configuration) ? " " : "") + stat.track_configuration;

            MemoSummary->Lines->Add("Live: connected to AC EVO. Waiting for car to leave pits...");

            // Surface channel-table problems immediately (e.g. mapping.json missing next to the exe).
            {
                std::string tblWarn = AcsEvoGetChannelTableWarning();
                if (!tblWarn.empty())
                    MemoSummary->Lines->Add(String(tblWarn.c_str()));
            }
			//StatusBar1->SimpleText = "Live: connected (AC EVO) — waiting for track";
        }
        else if (liveMode == LIVE_PMR)
        {
            liveTrackName.clear(); // refreshed from RaceInfo packets as they arrive
            String pmrAddr = ReadPmrBindAddressSetting().Trim();
            String pmrWhere = pmrAddr.IsEmpty() ? "all interfaces" : ("*" + pmrAddr);
            MemoSummary->Lines->Add("Live: PMR UDP listening on " + pmrWhere + ":" + IntToStr(ReadPmrPortSetting()) + ". Waiting for telemetry...");

            // Surface channel-table problems immediately (e.g. mapping.json missing next to the exe).
            {
                std::string tblWarn = PmrGetChannelTableWarning();
                if (!tblWarn.empty())
                    MemoSummary->Lines->Add(String(tblWarn.c_str()));
            }
			//StatusBar1->SimpleText = "Live: connected (PMR) — waiting for telemetry";
        }
        else
        {
            // We don't add the "Waiting..." message yet, because we might be 
            // immediately seeing a frame that says we are on track.
            // Instead, let processLiveFrame handle the status updates.
			//StatusBar1->SimpleText = "Live: connected — waiting for car to leave pits...";
            MemoSummary->Lines->Add("Live: connected to AMS2. Waiting for car to leave pits...");

            // Surface channel-table problems immediately (e.g. mapping.json missing next to the exe).
            {
                std::string tblWarn = Ams2GetChannelTableWarning();
                if (!tblWarn.empty())
                    MemoSummary->Lines->Add(String(tblWarn.c_str()));
            }
        }
    }
    else
    {
        // Still not connected — keep retrying
		//if (liveMode == LIVE_EVO)
			//StatusBar1->SimpleText = "Live: connecting... (AC EVO not detected)";
		//else
			//StatusBar1->SimpleText = "Live: connecting... (AMS2 not detected)";
    }
}

void __fastcall TFormMotecSetup::stopLiveCapture(bool isShuttingDown)
{
    // Stop the capture thread first to avoid accessing closed handles.
    // Reconnect mode: leave a live idling thread running (it sleeps while !liveConnected) so we
    // don't churn create/join on every reconnect cycle. Only join+delete when fully stopping,
    // shutting down, or switching simulator profile — the connect handler recreates it then.
    if (captureThread != nullptr && !(LiveCaptureCheckBox != NULL && LiveCaptureCheckBox->Checked && !isShuttingDown)) {
        stopCaptureThread = true;
        // Give the thread a moment to see the flag and exit its loop before joining
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

        if (captureThread->joinable())
            captureThread->join();
        delete captureThread;
        captureThread = nullptr;
    }

    if (LiveCaptureCheckBox != NULL && LiveCaptureCheckBox->Checked && !isShuttingDown) {
        // Reconnection mode: user still wants live capture, but connection is lost.
        liveConnected = false;
        LiveTimer->Enabled = false;

        // Close handles to avoid leaks/conflicts on next attempt
        if (liveMode == LIVE_EVO)
            AcsEvoCloseSharedMemory();
        else if (liveMode == LIVE_PMR)
            PmrCloseSocket();
        else
            Ams2CloseSharedMemory();

        // Visual feedback for reconnection state
        const bool isFrRec = ReadReportLanguageSetting() == ReportLanguage::French;
        LiveLed->Brush->Color = clRed;
        LabelLiveStatus->Caption = isFrRec ? String(L"Reconnexion...") : String("Reconnecting...");
        LabelLiveStatus->Font->Color = clGray;

        // Keep the connection timer running to look for the process again
        LiveConnectTimer->Enabled = true;
    } else {
        // Actual user-initiated stop or complete shutdown of live mode.
        liveActive = false;
        liveConnected = false;
        LiveConnectTimer->Enabled = false;
        LiveTimer->Enabled = false;

        if (!isShuttingDown) {
            const bool isFrDisc = ReadReportLanguageSetting() == ReportLanguage::French;
            LiveLed->Brush->Color = clRed;
            LabelLiveStatus->Caption = isFrDisc ? String(L"D\u00e9connect\u00e9") : String("Disconnected");
            LabelLiveStatus->Font->Color = clGray;
        }

        // If a lap is in progress, finalize it (only if not shutting down and UI exists)
        if (!isShuttingDown && liveLapInProgress && MemoSummary != NULL)
            finalizeLiveLap();

        Button1->Enabled = true;
        ButtonLoad->Enabled = true;

        if (liveMode == LIVE_EVO)
            AcsEvoCloseSharedMemory();
        else if (liveMode == LIVE_PMR)
            PmrCloseSocket();
        else
            Ams2CloseSharedMemory();

        if (!isShuttingDown && !liveLaps.empty())
            generateLiveReport();
        else if (!isShuttingDown)
        {
            MemoSummary->Lines->Add("Live capture stopped. No laps captured.");
		//StatusBar1->SimpleText = "Live capture stopped";
        }

        // packetId jump diagnostic summary (AC EVO): total missed sim writes over the whole session, if any.
        if ((liveEvoPhysJumps > 0 || liveEvoGfxJumps > 0) && MemoSummary != NULL)
            MemoSummary->Lines->Add("Session summary: " + IntToStr((int)liveEvoPhysJumps)
                + " physics + " + IntToStr((int)liveEvoGfxJumps)
                + " graphics packetId jumps (missed sim writes over the session).");

        // Plausibility cross-check rejects (AC EVO): frames whose values were excluded from lap data. A few is normal; hundreds suggests a real problem with that page's write pattern.
        if (liveMode == LIVE_EVO) {
            unsigned long plausRejects = AcsEvoGetPlausibilityRejectCount();
            if (plausRejects > 0 && MemoSummary != NULL)
                MemoSummary->Lines->Add("Session summary: " + IntToStr((int)plausRejects)
                    + " physics frame(s) rejected by plausibility check (values excluded from lap data).");
        }

        updateResetButtonState();
    }
}

// ---------------------------------------------------------------------------
// packetId jump diagnostic (AC EVO). Detects if sim writes were missed due to reader lag.
// ---------------------------------------------------------------------------
void TFormMotecSetup::resetEvoPacketJumpTracking()
{
    liveEvoLastPhysId  = -1;
    liveEvoLastGfxId   = -1;
    liveEvoPhysJumps   = 0;
    liveEvoGfxJumps    = 0;
    liveEvoJumpMsgMs   = 0.0;
}

void TFormMotecSetup::checkEvoPacketJump(const char* label, int id, bool isPhys)
{
    const int prevId = isPhys ? liveEvoLastPhysId : liveEvoLastGfxId;
    if (isPhys) liveEvoLastPhysId = id; else liveEvoLastGfxId = id;

    if (prevId < 0 || id <= prevId)
        return;   // first read this connection, or token wrap/step — nothing to report

    const int delta = id - prevId;
    if (delta > 1) {
        if (isPhys) ++liveEvoPhysJumps; else ++liveEvoGfxJumps;
    }
}

void __fastcall TFormMotecSetup::updateLiveAdaptiveInterval(double simFpsHint)
{
    // Session counters (plan Â§10a): count every accepted NewFrame and stamp the collection start on frame #1.
    ++liveFrameCount;
    if (liveSessionStartMs <= 0.0)
        liveSessionStartMs = (double)GetTickCount64();

    // Measure arrival rate from wall-clock time between accepted NewFrames, EMA-smoothed (alpha=0.1).
    double nowMs = (double)GetTickCount64();   // ms since boot; wraps only after ~49 days — fine for a live session
    if (liveLastNewFrameMs > 0.0) {
        double dtSec = (nowMs - liveLastNewFrameMs)/1000.0;
        if (dtSec > 0 && dtSec < 0.2) {            // sanity guard against huge gaps / clock jumps (>200ms => not a real frame interval)
            double instHz = 1.0/dtSec;
            liveMeasuredHz = (liveMeasuredHz <= 0.0) ? instHz : (0.9*liveMeasuredHz + 0.1*instHz);
        }
    }
    liveLastNewFrameMs = nowMs;

    // AC EVO: fixed fast poll (~3 ms) to ensure physics packetId increments are caught.
    int intervalMs = 3;                     // default: AC EVO fast-poll rate (overridden by adaptive logic for other modes)
    if (!(liveMode == LIVE_EVO && simFpsHint > 5.0)) {
        // Prefer the sim's own reported fps when available and plausible, else fall back to measured arrival rate.
        double targetFps = (simFpsHint > 5.0) ? simFpsHint : liveMeasuredHz;   // <5 => treat as unknown/low -> use ceiling below

    if (targetFps >= 10.0 && targetFps <= 240.0) {
        double raw = 1000.0/targetFps;      // ms per frame at sim rate
        intervalMs = (int)(raw + 0.5);
    } else if (targetFps > 0.0 && targetFps < 10.0) {
        intervalMs = 33;                     // low/unknown fps -> ceiling, avoid busy-looping on a slow sim
    }
    }

    // Clamp to [2ms floor .. 33ms ceiling] (timeBeginPeriod(1) in WinMain unlocks sub-4 ms timer resolution).
    if (intervalMs < 2) intervalMs = 2;
    else if (intervalMs > 33) intervalMs = 33;

    // Throttle the actual write to LiveTimer->Interval: only apply ~every 15 accepted frames (~4 Hz cadence at 60 fps),
    // and only when it differs from current, so we don't churn a VCL property on every tick. On this same cadence we also
    // refresh LabelLiveStatus with the live FPS readout (plan Â§6: "surface it in the live status line"). We never touch
    // Font->Color here — connection-state color is owned by connect/reconnect/disconnect handlers.
    if (++liveAdaptiveCounter >= 15) {
        liveAdaptiveCounter = 0;

        if (intervalMs != LiveTimer->Interval)
            LiveTimer->Interval = intervalMs;

        String cap;
        const bool isFrFps = ReadReportLanguageSetting() == ReportLanguage::French;
        if (liveMode == LIVE_EVO && liveEvoFpsAvg > 0)
            cap = isFrFps ? L"Connect\u00e9 Â· sim " + IntToStr(liveEvoFpsAvg) + L" Hz / lecture " + FormatFloat("0", liveMeasuredHz) + L" Hz"
                          : "Connected \xc2\xb7 sim " + IntToStr(liveEvoFpsAvg) + " Hz / read " + FormatFloat("0", liveMeasuredHz) + " Hz";
        else
            cap = isFrFps ? L"Connect\u00e9 Â· lecture " + (liveMeasuredHz > 0 ? FormatFloat("0", liveMeasuredHz) : L"--") + L" Hz"
                          : "Connected \xc2\xb7 read " + (liveMeasuredHz > 0 ? FormatFloat("0", liveMeasuredHz) : "--") + " Hz";
        LabelLiveStatus->Caption = cap;   // color stays as set by connect/disconnect handlers
    }
}

// Dedicated shared-memory capture loop for AC EVO and AMS2. Reads run here, off the UI thread;
// fresh frames are queued in frameBuffer under frameMutex and drained by LiveTimerTimer.
void TFormMotecSetup::captureThreadFunc()
{
    // Set thread priority to Normal as requested by user
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_NORMAL);

    while (!stopCaptureThread) {
        if (liveActive && liveConnected && liveMode == LIVE_EVO) {
            SPageFilePhysics phys;
            SPageFileGraphicEvo gfx;
            bool physUsable = true;

            // Use a local copy of the status to avoid any potential race during shutdown
            EvoReadStatus status = AcsEvoReadFrame(phys, gfx, &physUsable);

            if (status == EvoReadStatus::NewFrame) {
                std::lock_guard<std::mutex> lock(frameMutex);
                // Double check stop flag inside the lock to prevent pushing a frame during destruction
                if (!stopCaptureThread) {
                    CapturedFrame f;
                    f.evo.phys       = phys;
                    f.evo.gfx        = gfx;
                    f.evo.physUsable = physUsable;
                    f.timestampMs    = (double)GetTickCount64();
                    frameBuffer.push_back(f);
                    if (frameBuffer.size() > 1000) frameBuffer.erase(frameBuffer.begin());
                }
            } else if (status == EvoReadStatus::TornRetry) {
                liveEvoTornReads++;
            }

            // AC EVO writes the physics page at ~333 Hz (~every 3 ms). Polling every ~1-2 ms
            // (timeBeginPeriod(1) in WinMain unlocks sub-ms timer resolution) comfortably exceeds
            // that write rate, so no packetId increment goes unseen — while eliminating the previous
            // yield-only busy-loop that spun at full CPU speed between sim writes.
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        else if (liveActive && liveConnected && liveMode == LIVE_AMS2) {
            Ams2SharedMemory ams;

            // Same double-check + change-detection as before; only genuinely new frames are queued.
            FrameReadStatus status = Ams2ReadFrame(ams);

            if (status == FrameReadStatus::NewFrame) {
                std::lock_guard<std::mutex> lock(frameMutex);
                if (!stopCaptureThread) {
                    CapturedFrame f;
                    f.ams  = ams;   // POD copy of the whole shared-memory struct (~5 KB)
                    f.timestampMs = (double)GetTickCount64();
                    frameBuffer.push_back(f);
                    if (frameBuffer.size() > 1000) frameBuffer.erase(frameBuffer.begin());
                }
            }

            // AMS2 writes shared memory at ~60-90 Hz (~every 11 ms). An 8 ms poll comfortably exceeds
            // that rate, so no mSequenceNumber increment goes unseen — and the thread sleeps most of its time.
            std::this_thread::sleep_for(std::chrono::milliseconds(8));
        }
        else {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
}

void __fastcall TFormMotecSetup::LiveTimerTimer(TObject *Sender)
{
    if (!liveActive || !liveConnected)
        return;

    // If we are in AMS2 mode, check if the process is still running.
    // If it's not, the game has closed or crashed.
    if (liveMode == LIVE_AMS2 && !Ams2IsProcessRunning()) {
        MemoSummary->Lines->Add("Live: connection lost (AMS2 process not found).");
        stopLiveCapture();
        return;
    }

    // PMR stays on the timer: its UDP drain is already non-blocking and needs no capture thread.
    if (liveMode == LIVE_PMR)
        processLiveFramePmir();

    else if (liveMode == LIVE_AMS2)
    {
        // AMS2 now runs through the same dedicated-thread + buffer architecture as AC EVO:
        // captureThreadFunc() polls shared memory off the UI thread and queues fresh frames;
        // here we just drain whatever accumulated since last tick.
        std::vector<CapturedFrame> localFrames;
        {
            std::lock_guard<std::mutex> lock(frameMutex);
            localFrames.swap(frameBuffer);
        }

        if (localFrames.empty()) return;

        for (const auto& frame : localFrames) {
            updateLiveAdaptiveInterval(0.0);  // AMS2 has no sim-reported fps field: drive interval from measured arrival rate only
            processLiveFrame(frame.ams);
        }
    }
    else if (liveMode == LIVE_EVO)
    {
        // Retry reading the static page until we get a non-empty track name.
        // At connect time the game may not have written it yet → one-shot read can miss it.
        if (liveTrackName.empty())
        {
            SPageFileStaticEvo stat;
            if (AcsEvoReadStatic(stat) && strlen(stat.track) > 0)
                liveTrackName = std::string(stat.track) +
                    (strlen(stat.track_configuration) ? " " : "") + stat.track_configuration;
        }

        // Process all frames accumulated in the buffer since last tick
        std::vector<CapturedFrame> localFrames;
        {
            std::lock_guard<std::mutex> lock(frameMutex);
            localFrames.swap(frameBuffer);
        }

        if (localFrames.empty()) return;

        for (const auto& frame : localFrames) {
            liveEvoFpsAvg = frame.evo.gfx.player_fps_avg;
            updateLiveAdaptiveInterval(frame.evo.gfx.player_fps_avg);
            checkEvoPacketJump("physics", frame.evo.phys.packetId, true);
            checkEvoPacketJump("graphics", frame.evo.gfx.packetId, false);
            processLiveFrameEVO(frame.evo.phys, frame.evo.gfx, frame.evo.physUsable);
        }
    }
}

void __fastcall TFormMotecSetup::processLiveFrame(const Ams2SharedMemory& frame)
{
	// 1. Detect Pause (mGameState == 3)
	if (frame.mGameState == GAME_INGAME_PAUSED) {
        // If we were on track and now paused, just stop feeding data but don't finalize the lap yet
		liveWasOnTrack = false;
		lastPitMode = frame.mPitMode; // Keep track of pit mode even when paused
		//StatusBar1->SimpleText = "Live: PAUSED";
		return;
    }

	bool onTrack = Ams2IsOnTrack(frame);
	bool inPits = Ams2IsInPits(frame);

    // --- Event Detection based on transitions (AMS2_on_track_detection.md) ---

	// Auto-detect: if back on track after a completed session, reset for a fresh start
	if (onTrack && !liveWasOnTrack && AutoDetectTrackCheckBox != NULL && AutoDetectTrackCheckBox->Checked && !liveLaps.empty()) {
		resetLiveSession();
		MemoSummary->Lines->Add("Auto: new session started.");
	}

	// 0. Detect pit → track transition: start a new lap if none is currently in progress
	if (onTrack && !liveWasOnTrack && !liveLapInProgress && sessionLapCounter == 0) {
		// Wipe all previously displayed data so the fresh run starts from a clean slate:
		// clears tire/brake/water/oil + suspension caches and repaints every paintbox empty.
		resetAnalysisGraphics();
		if (MemoSummary != NULL)       MemoSummary->Lines->Clear();
		if (MemoGeneral != NULL)       MemoGeneral->Lines->Clear();
		if (MemoTyre != NULL)          MemoTyre->Lines->Clear();
		if (MemoBrake != NULL)         MemoBrake->Lines->Clear();
		if (MemoSuspension != NULL)    MemoSuspension->Lines->Clear();
		if (MemoEngineCooling != NULL) MemoEngineCooling->Lines->Clear();
		if (MemoHandling != NULL)      MemoHandling->Lines->Clear();
		MemoSummary->Lines->Add("Leaving Pits");
		if (frame.mTrackLocation[0])
			MemoSummary->Lines->Add(liveNameLabel(ReadReportLanguageSetting() == ReportLanguage::French, false) + liveNameUtf8(frame.mTrackLocation));
		if (frame.mCarName[0])
			MemoSummary->Lines->Add(liveNameLabel(ReadReportLanguageSetting() == ReportLanguage::French, true) + displayVehicleName(std::string(frame.mCarName)));
		pushLiveStatusToTablet(std::string(frame.mCarName), std::string(frame.mTrackLocation), 0);
		sessionLapCounter = 0;
		lastCompletedLaps = frame.mParticipantInfo[frame.mViewedParticipantIndex].mLapsCompleted;
		firstLapStarted = false;
		//return;
	}

	float delta = frame.mCurrentTime - previous_mCurrentTime;

	// 1. Lap Completion & Next Lap Start: mCompletedLaps > lastCompletedLaps
	if (delta < -1.0f || (!firstLapStarted && frame.mCurrentTime > 0.0f)) {
		sessionLapCounter++; // Increment our internal session counter
		firstLapStarted = true ;
        // If we are currently in a lap, finalize it because the new one has started
        if (liveLapInProgress) {
            liveCurrentLap.endTime = (double)GetTickCount64() / 1000.0;
            finalizeLiveLap();
		}

		// Start the next lap immediately if still on track (satisfies user request for Lap 1 start after crossing line)
		if (onTrack) {
			liveLapNumber = sessionLapCounter; // Use our internal counter to drive liveLapNumber
			liveLapInProgress = true;
			liveCurrentLap = LapStats();
            liveCurrentLap.lapNumber = liveLapNumber;
            liveCurrentLap.startTime = (double)GetTickCount64() / 1000.0;
            liveCurrentLap.vehicleName = frame.mCarName;
            liveCurrentLap.trackName = frame.mTrackLocation;

			MemoSummary->Lines->Add("Lap " + IntToStr(liveLapNumber) + " started.");
			pushLiveStatusToTablet(std::string(frame.mCarName), std::string(frame.mTrackLocation), liveLapNumber);
			//StatusBar1->SimpleText = "Live: Lap " + IntToStr(liveLapNumber) + " in progress";
		}
	}

	// 2. Teleport / Pit Entry: lastPitMode == 0 AND car in garage then generate the report
	if (lastPitMode == PIT_MODE_NONE && ( frame.mPitMode == PIT_MODE_IN_GARAGE || frame.mPitMode == PIT_MODE_IN_PIT)) {
		// If a lap was being recorded, finalize it first
		if (liveLapInProgress) {
			liveCurrentLap.endTime = (double)GetTickCount64() / 1000.0;
			finalizeLiveLap();
		}

		// Generate report if we have any completed laps
		if (!liveLaps.empty()) {
			generateLiveReport();
		}

		firstLapStarted = false;

		if (frame.mPitMode == PIT_MODE_IN_GARAGE) {
			MemoSummary->Lines->Add("Back to Garage");
		} else if (frame.mPitMode == PIT_MODE_IN_PIT) {
			MemoSummary->Lines->Add("Pistop");
		}

		// If no laps were ever recorded, reset the session counters for a fresh start next time
		if (!liveLapInProgress && liveLaps.empty()) {
			sessionLapCounter = 0;
			firstLapStarted = false;
			MemoSummary->Lines->Add("Back to garage with no valid lap.");
		}
	}
    // Feed data only when on track and lap is in progress
    if (onTrack && liveLapInProgress)
    {
        double timeSeconds = HighResClockSeconds();
		Ams2FeedLap(liveCurrentLap, frame, timeSeconds);
    }

	// Update history for next frame
    lastPitMode = frame.mPitMode;
	liveWasOnTrack = onTrack;
	previous_mCurrentTime = frame.mCurrentTime;

}
void __fastcall TFormMotecSetup::processLiveFrameEVO(const SPageFilePhysics& phys, const SPageFileGraphicEvo& gfx, bool physUsable)
{
    bool onTrack = AcsEvoIsOnTrack(gfx);
	bool inPits = AcsEvoIsInPits(gfx);
    // --- In-game restart detection: current_lap drops back to ~1 while we're at a high number.
    if (prevCurrentLapEvo > 2 && gfx.session_state.current_lap <= 1) {
        resetLiveSession();
        MemoSummary->Lines->Clear();
        MemoSummary->Lines->Add("Auto: in-game restart detected, session reset.");
    }
    prevCurrentLapEvo = gfx.session_state.current_lap;


	// Auto-detect: if back on track after a completed session, reset for a fresh start
	if (onTrack && !liveWasOnTrack && AutoDetectTrackCheckBox != NULL && AutoDetectTrackCheckBox->Checked && !liveLaps.empty()) {
		resetLiveSession();
		MemoSummary->Lines->Add("Auto: new session started.");
	}

    // Detect pit → track transition: start a new lap
    if (onTrack && !liveWasOnTrack)
    {
        liveEvoPitReportDone = false;  // allow report again on next pit visit
        liveLapInProgress = true;
        liveLapNumber++;
        liveCurrentLap = LapStats();
        liveCurrentLap.lapNumber = liveLapNumber;
        liveCurrentLap.startTime = (double)GetTickCount64() / 1000.0;
        liveCurrentLap.vehicleName = gfx.car_model;
        liveCurrentLap.trackName = liveTrackName;

        // Seed npos so the wrap detector doesn't fire on this same frame:
        // pit exit is at npos≈0.95, next frame drops to <0.2 — looks like a line
        // crossing but you've only driven ~3% of track length. By seeding here,
        // prevNPos == curNPos and the > 0.8 && < 0.2 condition can't trigger.
        liveEvoPrevNPos = gfx.npos;

        // Show car/track names as soon as the lap starts.
        if (gfx.car_model[0])
            MemoSummary->Lines->Add(liveNameLabel(ReadReportLanguageSetting() == ReportLanguage::French, true) + displayVehicleName(std::string(gfx.car_model)));
        if (!liveTrackName.empty())
            MemoSummary->Lines->Add(liveNameLabel(ReadReportLanguageSetting() == ReportLanguage::French, false) + liveNameUtf8(liveTrackName));
        pushLiveStatusToTablet(std::string(gfx.car_model), liveTrackName, liveLapNumber);
        MemoSummary->Lines->Add("Lap " + IntToStr(liveLapNumber) + " started.");
		//StatusBar1->SimpleText = "Live: Lap " + IntToStr(liveLapNumber) + " in progress";
    }

    // Detect track → pit arrival: finalize the lap.
    // No liveWasOnTrack guard — teleports (ESC menu) can skip a frame where neither
    // onTrack nor inPits is true, clearing liveWasOnTrack before we settle into pits.
    if (inPits && liveLapInProgress)
    {
        liveCurrentLap.endTime = (double)GetTickCount64() / 1000.0;
        finalizeLiveLap();
    }

    // If in pits and have captured laps, generate a report once per pit visit.
    // liveEvoPitReportDone prevents re-firing every tick while parked; reset on lap start / session reset.
    if (inPits && !liveLapInProgress && !liveLaps.empty() && !liveEvoPitReportDone)
    {
        generateLiveReport();
        liveEvoPitReportDone = true;
    }

    // --- npos wrap detection: backward jump >40% of track → start/finish line crossing on track ---
    // Catches laps that complete without a pit stop (normalized position wraps ~1 -> 0).
    // Using "backward delta" instead of fixed thresholds (>0.8/<0.2) makes this robust to
    // dropped frames: with frame-sync change-detection we may skip intermediate writes, so
    // prev can be e.g. 0.75 and cur 0.3 — the old threshold pair would miss that crossing.
    if (onTrack && liveLapInProgress && (liveEvoPrevNPos - gfx.npos) > 0.4f) {
        liveCurrentLap.endTime = (double)GetTickCount64() / 1000.0;
        finalizeLiveLap();

        // Immediately start the next lap so we don't lose data across the line crossing.
        ++liveLapNumber;
        liveLapInProgress    = true;   // re-arm: finalizeLiveLap() set it false above
        liveCurrentLap     = LapStats();
        liveCurrentLap.lapNumber   = liveLapNumber;
        liveCurrentLap.startTime   = (double)GetTickCount64() / 1000.0;
        liveCurrentLap.vehicleName = gfx.car_model;
        liveCurrentLap.trackName   = liveTrackName;

        MemoSummary->Lines->Add("Lap " + IntToStr(liveLapNumber) + " started (line crossing).");
    }

    // Feed data only when on track and lap is in progress. A plausibility-rejected physics page (physUsable=false) skips the feed for this tick ONLY — its values are excluded from lap stats, but npos tracking continues so line-crossing detection stays accurate.
    if (onTrack && liveLapInProgress)
    {
        double timeSeconds = HighResClockSeconds();   // QPC: GetTickCount64's ~15 ms resolution compressed damper velocities toward zero
        if (physUsable)
            AcsEvoFeedLap(liveCurrentLap, phys, gfx, timeSeconds);
        liveEvoPrevNPos = gfx.npos;   // track npos only while feeding a lap on-track
    }

    if (!onTrack)
        liveEvoPrevNPos = -1.0f;      // stale position must not trigger a wrap after rejoining the track

    liveWasOnTrack = onTrack;
}

void __fastcall TFormMotecSetup::processLiveFramePmir()
{
    // Drain all pending datagrams (non-blocking). Returns the count processed this tick.
    PmrDrainAllPackets();

    // Refresh track name from RaceInfo if it changed since last time.
    std::string t = PmrGetTrackName();
    if (!t.empty() && t != liveTrackName) {
        liveTrackName = t;
        MemoSummary->Lines->Add("PMR: track is " + String(t.c_str()));
    }

    // No player state yet — nothing to do this tick.
    int playerId = PmrGetPlayerId();
    if (playerId < 0) return;

    bool inPits   = PmrIsInPits(playerId);
    int  sector   = PmrGetCurrentSector(playerId);   // -1 when unknown / between laps
    bool onTrack  = !inPits && sector >= 0;          // "on track" means we have a valid sector reading

    // --- Auto-detect: back on track after a completed session → fresh start ---
    if (onTrack && !liveWasOnTrack &&
        AutoDetectTrackCheckBox != NULL && AutoDetectTrackCheckBox->Checked &&
        !liveLaps.empty()) {
        resetLiveSession();
        MemoSummary->Lines->Add("Auto: new PMR session started.");
        // After reset, pmrPrevSector is -1 again; re-read sector for this tick.
        sector = PmrGetCurrentSector(playerId);
    }

    // --- Pit → track transition: start a new lap ---
    if (onTrack && !liveWasOnTrack) {
        liveLapInProgress  = true;
        ++liveLapNumber;
        liveCurrentLap     = LapStats();
        liveCurrentLap.lapNumber   = liveLapNumber;
        liveCurrentLap.startTime   = (double)GetTickCount64() / 1000.0;

        std::string driverName = PmrGetDriverName(playerId);
        if (!driverName.empty())
            liveCurrentLap.vehicleName = driverName;
        liveCurrentLap.trackName  = liveTrackName;

        // PMR UDP feed has no car model: driver name is the vehicle identity.
        if (!driverName.empty())
            MemoSummary->Lines->Add(liveNameLabel(ReadReportLanguageSetting() == ReportLanguage::French, true) + liveNameUtf8(driverName));
        if (!liveTrackName.empty())
            MemoSummary->Lines->Add(liveNameLabel(ReadReportLanguageSetting() == ReportLanguage::French, false) + liveNameUtf8(liveTrackName));
        pushLiveStatusToTablet(driverName, liveTrackName, liveLapNumber);
        MemoSummary->Lines->Add("PMR Lap " + IntToStr(liveLapNumber) + " started.");
    }

    // --- Track → pit transition: finalize the lap and generate report if we have data ---
    if (inPits && liveWasOnTrack) {
        if (liveLapInProgress) {
            liveCurrentLap.endTime = (double)GetTickCount64() / 1000.0;
            finalizeLiveLap();
        }
        // Generate report when we enter pits and have at least one completed lap,
        // even if the current lap was just finalized above.
        if (!liveLaps.empty()) {
            generateLiveReport();
        }
    }

    // --- Sector-wrap detection: prevSector >= 3 && cur <= 1 → lap boundary on track ---
    // This catches laps that complete without a pit stop (sector wraps from S3 back to S1).
    if (onTrack && liveLapInProgress && pmrPrevSector >= 3 && sector <= 1) {
        liveCurrentLap.endTime = (double)GetTickCount64() / 1000.0;
        finalizeLiveLap();

        // Immediately start the next lap so we don't lose data between sectors.
        ++liveLapNumber;
        liveLapInProgress    = true;   // re-arm: finalizeLiveLap() set it false above
        liveCurrentLap     = LapStats();
        liveCurrentLap.lapNumber   = liveLapNumber;
        liveCurrentLap.startTime   = (double)GetTickCount64() / 1000.0;
        liveCurrentLap.vehicleName = PmrGetDriverName(playerId);
        liveCurrentLap.trackName   = liveTrackName;

        MemoSummary->Lines->Add("PMR Lap " + IntToStr(liveLapNumber) + " started (sector wrap).");
    }

    // --- Feed telemetry only when on track and a lap is in progress ---
    if (onTrack && liveLapInProgress) {
        const PmrVehicleTelemetryPacket* pkt = PmrLastTelemetry();
        if (pkt != nullptr) {
            double timeSeconds = HighResClockSeconds();   // QPC: consistent with EVO/AMS2 feed timestamps
            PmrFeedLap(liveCurrentLap, *pkt, timeSeconds);
        }
    }

    // Update tracking state for next tick.
    if (sector >= 0) pmrPrevSector = sector;   // only update when we have a valid reading
    liveWasOnTrack   = onTrack;
}

void __fastcall TFormMotecSetup::finalizeLiveLap()
{
    liveLapInProgress = false;

    // Only keep laps with meaningful data
    if (liveCurrentLap.rowCount > 10)
    {
        liveLaps.push_back(liveCurrentLap);
        MemoSummary->Lines->Add("Lap " + IntToStr(liveLapNumber) + " completed (" +
                               IntToStr((int)liveCurrentLap.rowCount) + " samples).");
    }
    else
    {
        MemoSummary->Lines->Add("Lap " + IntToStr(liveLapNumber) + " discarded (too few samples).");
    }

    liveCurrentLap = LapStats();
}

void __fastcall TFormMotecSetup::generateLiveReport()
{
    if (liveLaps.empty()) return;

    // Use the existing pipeline to generate the report
    ChannelProfile profile = ChannelProfile::AMS2;
    if (ChannelProfileCombo != NULL)
        profile = (ChannelProfile)ChannelProfileCombo->ItemIndex;

    ReportLanguage language = (LanguageCombo != NULL) ? (ReportLanguage)LanguageCombo->ItemIndex : ReadReportLanguageSetting();

	ReportResult result = buildAverageLapReport(liveLaps, language, profile);

    UpdateReportTabs(result);
    publishToTabletServer(result, liveLaps);

    // Update graphics (valid laps only, consistent with the report)
    std::vector<LapStats> validLiveLaps = LapReportGenerator::getValidLaps(liveLaps);
    UpdateTireTemps(validLiveLaps);
    UpdateBrakeTemps(validLiveLaps);
    UpdateSuspension(validLiveLaps);
    if (TabSummary != NULL && TabSummary->Visible)
        PageControlReport->ActivePage = TabSummary;

    //StatusBar1->SimpleText = "Live capture complete — " + IntToStr((int)liveLaps.size()) + " laps";
}

// Phase B2: live pit-exit status for the tablet - car/track/lap appear as soon as
// "Leaving Pits" is shown in the Summary memo. Reuses the report version counter so
// polling tablets never re-render a stale payload.
void __fastcall TFormMotecSetup::pushLiveStatusToTablet(const std::string& vehicleName,
                                   const std::string& trackLocation, int lapCount)
{
    if (!_tabletServer.isRunning()) return;

    ++tabletVersion_;
    std::string json = "{\"version\":" + std::to_string(tabletVersion_) + ",\"meta\":{\"hasData\":true,\"carName\":\"" + reportjson::jsonEscape(vehicleName) + "\",\"trackName\":\"" + reportjson::jsonEscape(trackLocation) + "\",\"lapCount\":"
        + (lapCount > 0 ? std::to_string(lapCount).c_str() : "null") + ",\"onTrack\":true},\"sections\":{},\"lapStats\":null}";
    _tabletServer.setPayload(json);
}

// B5: push a fresh JSON payload to the tablet server after every analysis.
// No-op when the server is disabled/not running. The version counter lets
// polling clients detect changes without re-rendering identical data.
void __fastcall TFormMotecSetup::publishToTabletServer(const ReportResult& result, const std::vector<LapStats>& laps)
{
    if (!_tabletServer.isRunning()) return;

    LapStats avg = buildAverageLapData(laps);
    int included = (int)LapReportGenerator::getValidLaps(laps).size();
    ++tabletVersion_;
    _tabletServer.setPayload(reportjson::buildReportJson(result, avg, included, tabletVersion_));
}

// Phase D3: live start/stop/restart the tablet web server when settings change in Settings dialog.
void TFormMotecSetup::ApplyWebServerSettings()
{
    if (_tabletServer.isRunning()) _tabletServer.stop();

    int port = ReadWebServerPort();
    bool enabled = ReadWebServerEnabled();

    if (enabled) {
        _tabletServer.setPort(port);
        if (!_tabletServer.start()) return;  // start() already shows an error message on failure
        StatusBar1->SimpleText = "Tablet viewer active — http://" + getLocalLanIp()
                + ":" + IntToStr(_tabletServer.port());
    } else {
        StatusBar1->SimpleText = "";
    }
}

void __fastcall TFormMotecSetup::UpdateReportTabs(const ReportResult& result)
{
    if (PageControlReport == NULL) return;

    // 1. Clear all memos first
    MemoSummary->Lines->Clear();
    MemoGeneral->Lines->Clear();
    MemoTyre->Lines->Clear();
    MemoBrake->Lines->Clear();
    MemoSuspension->Lines->Clear();
    MemoEngineCooling->Lines->Clear();
    MemoHandling->Lines->Clear();
    if (MemoDiagnostics) MemoDiagnostics->Lines->Clear();

    // 3. Populate content from result map
    auto formatForMemo = [](const std::string& s) -> String {
        std::string winS = s;
        size_t pos = 0;
        while ((pos = winS.find('\n', pos)) != std::string::npos) {
            if (pos == 0 || winS[pos - 1] != '\r') {
                winS.insert(pos, 1, '\r');
                pos += 2;
            } else {
                pos++;
            }
        }
        return String(winS.c_str());
    };

    if (result.count(ReportSection::Summary)) {
        MemoSummary->Text = formatForMemo(result.at(ReportSection::Summary).content);
    }

    // Plan Â§10a — live collection-rate line appended to the Summary memo so it stays
    // visible alongside report content while a live session is running.
    if (liveActive && liveFrameCount > 0) {
        const double elapsedSec = ((double)(GetTickCount64() - (__int64)liveSessionStartMs)) / 1000.0;
        String rateLine;
        bool lagWarning = false;
        if (elapsedSec >= 0.5) { // minimum window for a meaningful average
            const double avgHz = (double)liveFrameCount / elapsedSec;
            rateLine = "Collection rate: " + FormatFloat("0.0", avgHz) + " Hz avg over "
                + IntToStr((int)liveFrameCount) + " frames / " + IntToStr((int)(elapsedSec + 0.5))
                + (liveMode == LIVE_EVO ? " s [physics]" : " s");
            // AC EVO: compare against the sim-reported frame rate; warn on reader lag.
            if (liveMode == LIVE_EVO && liveEvoFpsAvg > 5) {
                const double ratio = avgHz / (double)liveEvoFpsAvg;
                rateLine += " | sim gfx " + IntToStr(liveEvoFpsAvg) + " Hz";
                if (ratio < 0.85)
                    lagWarning = true;
            }
        } else {
            rateLine = "Collection started (" + IntToStr((int)liveFrameCount) + " frames so far).";
        }
        MemoSummary->Lines->Add(rateLine);
        if (lagWarning)
            MemoSummary->Lines->Add("Warning: reader lag — consider lowering LiveTimer interval.");
    }

    if (result.count(ReportSection::General)) {
        MemoGeneral->Text = formatForMemo(result.at(ReportSection::General).content);
    }
    if (result.count(ReportSection::Tyre)) {
        MemoTyre->Text = formatForMemo(result.at(ReportSection::Tyre).content);
    }
    if (result.count(ReportSection::Brake)) {
        MemoBrake->Text = formatForMemo(result.at(ReportSection::Brake).content);
    }
    if (result.count(ReportSection::Suspension)) {
        MemoSuspension->Text = formatForMemo(result.at(ReportSection::Suspension).content);
    }
    if (result.count(ReportSection::EngineCooling)) {
        MemoEngineCooling->Text = formatForMemo(result.at(ReportSection::EngineCooling).content);
    }
    if (result.count(ReportSection::Handling)) {
        MemoHandling->Text = formatForMemo(result.at(ReportSection::Handling).content);
    }
    if (result.count(ReportSection::Diagnostics) && MemoDiagnostics) {
        MemoDiagnostics->Text = formatForMemo(result.at(ReportSection::Diagnostics).content);
    }

    // 3. Manage Tab Visibility: show a tab only when it has data and a recommendation
    auto setTabVisible = [&](TTabSheet* tab, ReportSection section) {
        if (tab == NULL) return;
        const bool hasData = result.count(section) && !result.at(section).content.empty();
        const bool visible = hasData && result.at(section).hasRecommendation;
        tab->Visible = visible;

        // Visual indicator: â— prefix when section has content, plain name otherwise.
        // Strip any previous dot before re-applying.
        String cap = tab->Caption;
        if (cap.Length() >= 2 && (System::WideChar)cap[1] == 0x25CF && cap[2] == ' ') {
            cap = cap.SubString(3, cap.Length() - 2);
        }
        tab->Caption = hasData ? (String((System::WideChar)0x25CF) + " " + cap) : cap;
    };

    setTabVisible(TabGeneral, ReportSection::General);
    setTabVisible(TabTyre, ReportSection::Tyre);
    setTabVisible(TabBrake, ReportSection::Brake);
    setTabVisible(TabSuspension, ReportSection::Suspension);
    setTabVisible(TabEngineCooling, ReportSection::EngineCooling);
    setTabVisible(TabHandling, ReportSection::Handling);
    if (TabDiagnostics) setTabVisible(TabDiagnostics, ReportSection::Diagnostics);

    // Summary is always visible if it has content.
    if (TabSummary != NULL) {
        TabSummary->Visible = result.count(ReportSection::Summary);
    }

    // Force a REAL page transition so Windows properly hides/shows tab windows
    // and triggers painting. If ActivePage is already Summary, VCL does nothing
    // on re-assignment — no show/hide, no repaint, stale pixels remain.
    // Bouncing through another visible tab replicates what clicking does.
    if (TabSummary != NULL && TabSummary->Visible) {
        if (PageControlReport->ActivePage == TabSummary) {
            TTabSheet* other = NULL;
            for (int i = 0; i < PageControlReport->PageCount; ++i) {
                TTabSheet* t = PageControlReport->Pages[i];
                if (t != TabSummary && t->Visible) { other = t; break; }
            }
            if (other) {
                PageControlReport->ActivePage = other;
                PageControlReport->ActivePage = TabSummary;
            }
        } else {
            PageControlReport->ActivePage = TabSummary;
        }
    }
}

void __fastcall TFormMotecSetup::UpdateSuspension(std::vector<LapStats>& laps)
{
    // No data: clear cached bins and repaint so the paint boxes show their empty state.
    if (laps.empty()) {
        for (int i = 0; i < 4; ++i) {
            suspensionData[i].valid = false;
            for (int b = 0; b < VelocityHistogram::kBinCount; ++b)
                suspensionData[i].bins[b] = 0.0;
            suspensionData[i].totalCount = 0;
        }
        suspensionMaxBin = 0.0;
        const bool showCombined =
            (DamperLayoutTabs != NULL && DamperLayoutTabs->ActivePageIndex == 1);
        if (showCombined) {
            if (DamperCombinedPaintBox != NULL) DamperCombinedPaintBox->Invalidate();
        } else {
            if (DamperFLPaintBox != NULL) DamperFLPaintBox->Invalidate();
            if (DamperFRPaintBox != NULL) DamperFRPaintBox->Invalidate();
            if (DamperRLPaintBox != NULL) DamperRLPaintBox->Invalidate();
            if (DamperRRPaintBox != NULL) DamperRRPaintBox->Invalidate();
        }
        return;
    }

    LapStats avg = buildAverageLapData(laps);

    // Corner order: 0=LF, 1=RF, 2=LR, 3=RR.
    // Priority (profile-agnostic): direct suspension-velocity channel first;
    // fall back to the virtual channel derived from damper travel over time.
    const VelocityHistogram* suspVels[4] = { &avg.suspVelLF, &avg.suspVelRF, &avg.suspVelLR, &avg.suspVelRR };
    const DamperCornerStats* dampers[4] = { &avg.damperLF, &avg.damperRF, &avg.damperLR, &avg.damperRR };
    const StatSummary* travels[4] = { &avg.susTravelLF, &avg.susTravelRF, &avg.susTravelLR, &avg.susTravelRR };
    const StatSummary* bumpstops[4] = { &avg.bumpStopLF, &avg.bumpStopRF, &avg.bumpStopLR, &avg.bumpStopRR };

    for (int i = 0; i < 4; i++) {
        const VelocityHistogram& hist =
            suspVels[i]->totalCount > 0 ? *suspVels[i] : dampers[i]->histogram;
        suspensionData[i].valid = hist.totalCount > 0;
        if (suspensionData[i].valid) {
            for (int b = 0; b < VelocityHistogram::kBinCount; b++) {
                suspensionData[i].bins[b] = static_cast<double>(hist.bins[b]) / static_cast<double>(hist.totalCount);
            }
            suspensionData[i].totalCount = hist.totalCount;
            suspensionData[i].travel = travels[i]->average();
            suspensionData[i].bumpstop = bumpstops[i]->max;
            suspensionData[i].issue = hasDamperIssue(hist);
        }
    }

    // Shared Y-axis scale: highest bin across all four corners.
    suspensionMaxBin = 0.0;
    for (int i = 0; i < 4; ++i)
        if (suspensionData[i].valid) {
            for (int b = 0; b < VelocityHistogram::kBinCount; ++b)
                if (suspensionData[i].bins[b] > suspensionMaxBin)
                    suspensionMaxBin = suspensionData[i].bins[b];
        }

    // Repaint only the visible damper view: invalidating a hidden tab's
    // paint box wastes work during live capture.
    const bool showCombined =
        (DamperLayoutTabs != NULL && DamperLayoutTabs->ActivePageIndex == 1);
    if (showCombined) {
        if (DamperCombinedPaintBox != NULL)
            DamperCombinedPaintBox->Invalidate();
    } else {
        if (DamperFLPaintBox != NULL) DamperFLPaintBox->Invalidate();
        if (DamperFRPaintBox != NULL) DamperFRPaintBox->Invalidate();
        if (DamperRLPaintBox != NULL) DamperRLPaintBox->Invalidate();
        if (DamperRRPaintBox != NULL) DamperRRPaintBox->Invalidate();
    }
}

// ---------------------------------------------------------------------------
// Release info : version dynamique + notes de release GitHub
// Toutes ces méthodes s'exécutent sur le thread UI uniquement. Le worker réseau
// écrit dans l'état global de ReleaseInfo.cpp ; takePendingRelease() fait la
// passerelle (voir ReleasePollTimerTimer).
// ---------------------------------------------------------------------------

void __fastcall TFormMotecSetup::loadReleaseInfoCache()
{
	TIniFile* settings = new TIniFile(SettingsFilePath());
	try {
		m_latestRelease.tagName     = WideStringToUtf8(settings->ReadString("ReleaseInfo", "Tag", ""));
		m_latestRelease.htmlUrl     = WideStringToUtf8(settings->ReadString("ReleaseInfo", "HtmlUrl", ""));
		m_latestRelease.publishedAt = WideStringToUtf8(settings->ReadString("ReleaseInfo", "PublishedAt", ""));
		m_latestRelease.body        = WideStringToUtf8(settings->ReadString("ReleaseInfo", "Body", ""));
		m_latestRelease.assetUrl    = WideStringToUtf8(settings->ReadString("ReleaseInfo", "Asset", ""));
		m_hasCachedRelease = !m_latestRelease.empty();   // tag présent => cache utile
		for (int i = 0; i < 8; ++i) {   // tags refuses : pas de re-proposition au demarrage suivant non plus
			m_declinedUpdateTags[i] = WideStringToUtf8(settings->ReadString("ReleaseInfo", ("Declined" + IntToStr(i)).c_str(), ""));
		}

	} __finally {
		delete settings;
	}
}

void __fastcall TFormMotecSetup::saveReleaseInfoCache(const AppRelease& rel)
{
	if (rel.empty()) return;   // rien à cacher sans tag
	TIniFile* settings = new TIniFile(SettingsFilePath());
	try {
		settings->WriteString("ReleaseInfo", "Tag",         Utf8ToWideString(rel.tagName));
		settings->WriteString("ReleaseInfo", "HtmlUrl",     Utf8ToWideString(rel.htmlUrl));
		settings->WriteString("ReleaseInfo", "PublishedAt", Utf8ToWideString(rel.publishedAt));
		settings->WriteString("ReleaseInfo", "Body",        Utf8ToWideString(rel.body));
		settings->WriteString("ReleaseInfo", "Asset",       Utf8ToWideString(rel.assetUrl));
	} __finally {
		delete settings;
	}
}


// [ReleaseInfo] Popup au demarrage : propose la mise a jour si le cache INI contient deja une version plus recente que l exe courante.
void __fastcall TFormMotecSetup::promptForAvailableUpdate()
{
	if (!m_hasCachedRelease || !isUpdateAvailable(m_latestRelease)) return;   // rien de neuf (ou pas encore de reseau)

	// Anti-boucle self-update : cette instance vient d'etre installee via /updated. Son kAppVersion
	// compile est encore l ancienne -> sans ce garde-fou elle se "mettrait a jour" en boucle sur son propre .new.exe.
	if (wasJustUpdated()) { ReleaseInfoLog("promptForAvailableUpdate: suppressed — this instance was just installed via /updated"); return; }

	const std::string tag = m_latestRelease.tagName;
	if (tag.empty()) return;
	for (int i = 0; i < 8; ++i) {       // deja refuse par l utilisateur ? on ne relance plus le popup.
		if (!m_declinedUpdateTags[i].empty() && m_declinedUpdateTags[i] == tag) return;
	}
	if (m_updatePromptTag == tag) return;   // deja propose ce lancement : pas de re-popup pendant un download en cours.

	const bool isFr = ReadReportLanguageSetting() == ReportLanguage::French;
	String msg = String(isFr ? "Version courante : " : "Current version: ") + kAppVersion + sLineBreak
	           + (isFr ? "Nouvelle version disponible : " : "New update available: ") + Utf8ToWideString(tag);

	m_updatePromptTag = tag;            // un seul popup par lancement, meme si le timer re-polle.
	int choice = MessageBoxW(NULL, msg.c_str(), isFr ? L"Mise a jour disponible" : L"Update available", MB_YESNO | MB_ICONINFORMATION);
	if (choice == IDYES) {
		startSelfUpdate();              // pas de bouton ici ; le timer ReleasePollTimer affichera la progression dans le dialog A propos.
	} else if (choice == IDNO) {        // memoire anti-spam : on ne re-proposera plus ce tag tant qu'une version suivante n'arrive pas.
		for (int i = 7; i > 0; --i) m_declinedUpdateTags[i] = m_declinedUpdateTags[i - 1];
		m_declinedUpdateTags[0] = tag;
		TIniFile* settings = new TIniFile(SettingsFilePath());   // meme fichier que le cache de release (MotecDataAnalyser.ini).
		try {
			for (int i = 0; i < 8; ++i)
				settings->WriteString("ReleaseInfo", ("Declined" + IntToStr(i)).c_str(), Utf8ToWideString(m_declinedUpdateTags[i]));
		} __finally { delete settings; }
	}
}

// [ReleaseInfo] Telechargement de l asset du release GitHub + echange des exe + relance avec /updated.
void __fastcall TFormMotecSetup::startSelfUpdate(TButton* progressBtn)
{
	{
		int st0 = m_updateState.load();
		if (st0 != 0) { ReleaseInfoLog(("startSelfUpdate: early return, state already=" + std::to_string(st0)).c_str()); return; }   // deja en cours
	}

	std::wstring curExeW = std::wstring(Application->ExeName.c_str());
	const String appDir = AppDirPath();      // se termine par un backslash.
	std::wstring exeDirW;   // dossier de l exe en wide (CreateProcessW lpCurrentDirectory).
	for (size_t k = curExeW.size(); k > 0; --k) { if (curExeW[k - 1] == L'\\') { exeDirW.assign(curExeW, 0, k); break; } }

	std::wstring newExeW = (appDir + "MotecDataAnalyser.new.exe").c_str();   // telecharge ici, puis echange avec l exe courante.
	std::wstring oldExeW = (appDir + "MotecDataAnalyser.old.exe").c_str();

	const std::string urlUtf8 = m_latestRelease.assetUrl;   // copie locale : le thread ne lit pas de membre du form apres detach().
	const std::string mappingUrlUtf8 = m_latestRelease.mappingUrl;   // URL raw.githubusercontent.com pour MotecDataAnalyser.mapping.json ("" si indisponible).
	if (urlUtf8.empty()) { MessageBoxW(NULL, L"No update asset URL available.", L"Update", MB_ICONWARNING); return; }

	m_downloadReceived.store(0);
	m_downloadTotal.store(-1);
	m_updateState.store(1);                  // 1 = telechargement en cours ; le timer ReleasePollTimer lit ces atomiques.
	if (progressBtn) progressBtn->Enabled = false;   // bouton du dialog A propos : desactive pendant l update, re-active par le timer a la fin.

	std::thread([urlUtf8, mappingUrlUtf8, exeDirW, newExeW, oldExeW, curExeW, this]() {
		ReleaseInfoLog("startSelfUpdate worker: download begin");   // trace : le thread a bien demarre
		bool ok = downloadReleaseAsset(urlUtf8, newExeW, [this](long long rec, long long tot) {   // thread HTTP : ne touche pas au VCL.
			m_downloadReceived.store(rec);
			if (tot > 0) m_downloadTotal.store(tot);
		});

		if (!ok) {
			ReleaseInfoLog("startSelfUpdate worker: download FAILED -> state=3 (timer affichera l'erreur)");   // echec : on nettoie et le timer affichera l erreur.
			DeleteFileW(newExeW.c_str()); m_updateState.store(3); return; }

		ReleaseInfoLog("startSelfUpdate worker: download OK -> swap phase (MoveFileEx + CreateProcess /updated)");

		// Telecharge aussi MotecDataAnalyser.mapping.json (version-matched). Echec non bloquant:
		// si le json ne peut pas etre telecharge, l app utilisera son fichier local existant.
		if (!mappingUrlUtf8.empty()) {
			downloadReleaseAsset(mappingUrlUtf8, exeDirW + L"MotecDataAnalyser.mapping.json");   // pas de callback : fichier petit.
		}

		// Windows interdit a un processus de re-creer son propre chemin d image (MoveFileExW
		// new->cur renvoie ACCESS_DENIED tant que ce process est vivant, meme apres avoir
		// rename cur). On ne fait donc QUE : 1) ecarte l ancienne exe vers .old.exe, puis
		// 2) lance directement la nouvelle depuis son chemin .new.exe avec /updated. Le WinMain
		// de la nouvelle instance renomme alors .new.exe -> nom canonique et supprime .old.exe.
		DeleteFileW(oldExeW.c_str());   // nettoie un .old.exe eventuel d une tentative precedente.

		bool renamed = false;
		for (int attempt = 0; attempt < 5 && !renamed; ++attempt) {   // retry : AV/Defender peut verrouiller l exe quelques secondes.
			if (MoveFileExW(curExeW.c_str(), oldExeW.c_str(), MOVEFILE_REPLACE_EXISTING)) renamed = true;
			else if (GetLastError() == ERROR_ACCESS_DENIED && attempt < 4) Sleep(1000);   // verrou temporaire : on retente.
		}

		bool created = false;
		if (!renamed) { DeleteFileW(newExeW.c_str()); m_updateState.store(3); return; }   // echec total du swap.

		{   // relance de la nouvelle exe avec /updated ; le WinMain nettoiera alors MotecDataAnalyser.old.exe au demarrage.
			std::wstring origNameW;                       // nom d origine de l exe courante : conserve les raccourcis utilisateur apres update.
			for (size_t k = curExeW.size(); k > 0; --k) { if (curExeW[k - 1] == L'\\') { origNameW.assign(curExeW, k); break; } }
			std::wstring cmdLine = L"\"" + newExeW + L"\" /updated \"" + origNameW + L"\"";
			wchar_t* cmdBuf = new wchar_t[cmdLine.size() + 1];   // CreateProcessW modifie la chaine : tampon mutable obligatoire.
			for (size_t i = 0; i <= cmdLine.size(); ++i) cmdBuf[i] = cmdLine[i];

			STARTUPINFOW siw{}; PROCESS_INFORMATION pi{};
			siw.cb = sizeof(siw);
			bool okProc = CreateProcessW(NULL, cmdBuf, NULL, NULL, FALSE, 0, NULL, exeDirW.c_str(), &siw, &pi) != 0;   // working dir = dossier de l exe.
			if (okProc)
				ReleaseInfoLog(("CreateProcess OK pid=" + std::to_string(pi.dwProcessId)).c_str());
			else
				ReleaseInfoLog(("CreateProcess FAILED gle=" + std::to_string(GetLastError())
				    + " (cmd = <dirExe>\\MotecDataAnalyser.new.exe /updated \"<curName>\")").c_str());   // verrou AV sur .new.exe -> state=3
			delete[] cmdBuf;

			if (okProc) { CloseHandle(pi.hThread); CloseHandle(pi.hProcess); created = true; }
		}   // fin bloc relance.

		m_updateState.store(created ? 2 : 3);   // 2 = relance en cours ; le timer terminera l ancienne instance.
	}).detach();

	if (progressBtn) progressBtn->Caption = "Mise a jour...";
}
String __fastcall TFormMotecSetup::buildAboutText() const
{
	const bool isFr = ReadReportLanguageSetting() == ReportLanguage::French;

	String text;
	if (isFr) {
		text  = AnsiString("Analyseur de Setup MoTeC") + sLineBreak;
		text += AnsiString("Version : ") + kAppVersion;
	} else {
		text  = AnsiString("MoTeC Setup Analyzer") + sLineBreak;
		text += AnsiString("Version: ") + kAppVersion;
	}

	if (!m_latestRelease.empty()) {
		const String tag     = Utf8ToWideString(m_latestRelease.tagName);
		const String dateStr = Utf8ToWideString(formatReleaseDate(m_latestRelease.publishedAt, isFr));
		const String relLabel = isFr ? "Dernière release : " : "Latest release: ";
		text += String(sLineBreak) + relLabel + tag;
		if (!dateStr.IsEmpty()) text += String(" (") + dateStr + String(")");
		const int cmp = compareVersions(kAppVersion, m_latestRelease.tagName);
		if (cmp >= 0) {
			text += String(sLineBreak) + (isFr ? "Vous etes a jour." : "You are up to date.");
	} else if (!isUpdateAvailable(m_latestRelease)) {
		const String noAsset = isFr
			? "(Nouvelle version publiee sans binaire .exe - rien a installer.)"
			: "Newer release published without an .exe asset - nothing to install.";
		text += String(sLineBreak) + noAsset;
	}

		const String body = Utf8ToWideString(m_latestRelease.body);
		if (!body.IsEmpty()) {
			const String notesLabel = isFr ? "Notes de version :" : "Release notes:";
			text += String(sLineBreak) + notesLabel + String(sLineBreak);
			text += body;
		}
	} else {
		const String noInfo = isFr
		    ? "(Aucune information de release disponible — hors-ligne ou premier lancement.)"
		    : "(No release info available - offline or first launch.)";
		text += String(sLineBreak) + noInfo;
	}

	return text;
}

void __fastcall TFormMotecSetup::refreshAboutFormIfOpen()
{
	if (m_aboutMemo != NULL) m_aboutMemo->Text = buildAboutText();
}

void __fastcall TFormMotecSetup::ButtonAboutClick(TObject *Sender)
{
	const bool isFr = ReadReportLanguageSetting() == ReportLanguage::French;
	// Sans owner : destruction deterministe ici (pas de fuite, pas d auto-delete).
	TForm* dlg  = new TForm(static_cast<TComponent*>(NULL));
	dlg->Caption    = isFr ? "A propos" : "About";
	dlg->Width      = 560;
	dlg->Height     = 420;
	dlg->Position   = poScreenCenter;
	TMemo* memo = new TMemo(dlg);
	memo->Parent     = dlg;
	memo->Align      = alClient;
	memo->ScrollBars = ssBoth;
	memo->WordWrap   = false;
	memo->Lines->Text = buildAboutText();
	// Bouton "Mettre a jour" si une release avec asset est disponible.
	if (!m_latestRelease.empty() && isUpdateAvailable(m_latestRelease)) {
		TPanel* panel = new TPanel(dlg);
		panel->Parent  = dlg;
		panel->Align   = alBottom;
		panel->Height  = 42;
		m_aboutUpdateBtn = new TButton(panel);
		m_aboutUpdateBtn->Left        = 8;
		m_aboutUpdateBtn->Top         = 6;
		m_aboutUpdateBtn->Width       = 150;
		m_aboutUpdateBtn->Height      = 30;
		m_aboutUpdateBtn->Caption     = isFr ? "Mettre a jour" : "Update now";
		m_aboutUpdateBtn->OnClick = UpdateButtonClick;
	}
	dlg->OnClose = AboutDialogClosing;
	// Pendant ShowModal, la boucle de messages traite les timers : ReleasePollTimer
	// peut donc arriver et rafraichir le memo en place via ces pointeurs.
	m_aboutForm  = dlg;
	m_aboutMemo  = memo;
	try {
		dlg->ShowModal();
	} __finally {
		if (m_aboutUpdateBtn != NULL) m_aboutUpdateBtn->OnClick = NULL;
		m_aboutForm      = NULL;   // plus de reference pendante apres fermeture
		m_aboutMemo      = NULL;
		m_aboutUpdateBtn = NULL;
	}
	delete dlg;           // supprime aussi le TMemo/TPanel/TButton (owned)
}


void __fastcall TFormMotecSetup::ReleasePollTimerTimer(TObject *Sender)
{
	AppRelease rel;
	if (takePendingRelease(rel)) {             // nouveau resultat depuis le worker fetch
		m_latestRelease = std::move(rel);      // main thread : pas besoin de mutex ici
		m_hasCachedRelease = true;
		saveReleaseInfoCache(m_latestRelease); // cache INI pour mode hors-ligne

	promptForAvailableUpdate();   // popup si la version reseau est plus recente que l exe courante
	}
	int state = m_updateState.load();
	if (state == 2) {                          // self-update termine, relance demarree
		ReleaseInfoLog("self-update complete: calling Application->Terminate() on old instance");   // trace : l ancienne instance doit mourir ici.
		Application->Terminate();              // main thread : arret propre de la VCL
		return;
	}
	if (state == 3) {                           // echec update : signaler puis re-activer
		const std::string tag = m_latestRelease.tagName;   // copie locale avant le reset.
		m_updateState.store(0); state = 0;                  // RESET AVANT la boite modale : TTimer re-entre dans sa pompe de messages, sans ce reset les popups se multipliaient (~6).
		if (!tag.empty() && m_lastFailedTag != tag) {       // anti-spam : une seule alerte d echec par release.
			m_lastFailedTag = tag;
			const bool isFr = ReadReportLanguageSetting() == ReportLanguage::French;
			MessageBox(NULL,
				isFr ? L"Echec de la mise a jour. Verifiez votre connexion et retentez."
				     : L"Update failed. Check your connection and try again.",
				isFr ? L"Mise a jour" : L"Update", MB_OK | MB_ICONWARNING);
		}
	}
	// Progression du telechargement sur le bouton de la boite a propos (si ouverte).
	if (m_aboutUpdateBtn != NULL && state == 1) {
		const bool isFr = ReadReportLanguageSetting() == ReportLanguage::French;
		const long long rec = m_downloadReceived.load();
		const long long tot = m_downloadTotal.load();
		String cap;
		if (tot > 0) {
			double pct = static_cast<double>(rec) / static_cast<double>(tot) * 100.0;
			cap = String(isFr ? "Mise a jour : " : "Updating: ") + FormatFloat("%.0f%%", pct);
		} else {
			String mb = FormatFloat("%.2f", rec / 1048576.0) + L" MB";
			cap = String(isFr ? "Mise a jour... (" : "Downloading (") + mb + ")";
		}
		m_aboutUpdateBtn->Caption = cap;
	} else if (m_aboutUpdateBtn != NULL && state == 0) {
		const bool isFr = ReadReportLanguageSetting() == ReportLanguage::French;
		m_aboutUpdateBtn->Enabled = true;
		m_aboutUpdateBtn->Caption = isFr ? "Mettre a jour" : "Update now";
	}
	if (m_hasCachedRelease) refreshAboutFormIfOpen();  // re-rendu si la boite est ouverte
}


void __fastcall TFormMotecSetup::UpdateButtonClick(TObject *Sender)
{
	// Garde : un update est deja en cours (1), relance attendue (2) ou echec a traiter (3).
	if (m_updateState.load() != 0) return;
	TButton* btn = dynamic_cast<TButton*>(Sender);   // bouton du dialog A propos ; le timer gera la progression
	startSelfUpdate(btn);                            // telechargement + echange exe + relance /updated
}

void __fastcall TFormMotecSetup::AboutDialogClosing(TObject *Sender, System::Uitypes::TCloseAction &Action)
{
	const int state = m_updateState.load();
	// Bloque la fermeture pendant le telechargement (1) ou l attente de relance (2).
	if (state == 1 || state == 2) Action = System::Uitypes::TCloseAction::caNone;   // block close while updating/restarting
}