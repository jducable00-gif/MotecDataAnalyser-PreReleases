//---------------------------------------------------------------------------
// MotecSettingsForm.cpp � Implementation of the settings dialog.
//---------------------------------------------------------------------------

#pragma hdrstop

// WinSock for the LAN-IP lookup below; needed even when the web server (which normally calls WSAStartup) is off.
#include <winsock2.h>

#include <vcl.h>
#include "MotecSettingsForm.h"
#include "MotecSetupAnalyzer.h"
#include "MotecSetupForm.h"
#include <IniFiles.hpp>

// PMR INI helpers (defined in MotecSetupForm.cpp)
extern String ReadPmrBindAddressSetting();
extern void   SavePmrBindAddressSetting(const String& addr);
extern int     ReadPmrPortSetting();

// Tablet Viewer web server INI helpers (defined in MotecSetupForm.cpp)
extern bool   ReadWebServerEnabled();
extern void   SaveWebServerEnabled(bool enabled);
extern int    ReadWebServerPort();
extern void   SaveWebServerPort(int port);
extern int    ReadPreReleaseUpdatesSetting();
extern void   SavePreReleaseUpdatesSetting(bool pre);


// Access to global thresholds and helper functions from other units
extern Thresholds g_thresholds;
extern String ThresholdsFilePath();

// Best-effort primary LAN IPv4 for the settings label. Self-contained WSA pair: Windows refcounts
// WSAStartup/WSACleanup per process, so this is independent of the TabletReportServer reference.
// Iterates ALL local addresses (gethostbyname returns them in adapter order) and picks the first
// that looks like a real private-LAN address — skipping loopback (127.x), APIPA/link-local
// (169.254.x, assigned to disconnected Ethernet ports), and other non-routable ranges.
static String getLocalLanIpForLabel()
{
    WSADATA wsa; bool started = (::WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
    if (!started) return L"localhost";

    char namebuf[256];
    struct hostent* he = NULL;
    String result = L"";   // empty until we find a good candidate

    if (::gethostname(namebuf, sizeof(namebuf)) == 0)
        he = ::gethostbyname(namebuf);

    for (int i = 0; he != NULL && he->h_addr_list[i] != NULL; ++i) {
        const struct in_addr& addr = *reinterpret_cast<const struct in_addr*>(he->h_addr_list[i]);
        const unsigned char* b = reinterpret_cast<const unsigned char*>(&addr);

        // Skip loopback (127.x), APIPA/link-local (169.254.x — disconnected adapters)
        if (b[0] == 127 || b[0] == 169) continue;

        // Prefer private LAN ranges: 10.x, 172.16-31.x, 192.168.x
        bool isPrivate = false;
        if (b[0] == 10)                                        isPrivate = true;
        else if (b[0] == 172 && b[1] >= 16 && b[1] <= 31)      isPrivate = true;
        else if (b[0] == 192 && b[1] == 168)                   isPrivate = true;

        char buf[INET_ADDRSTRLEN];
        if (::inet_ntop(AF_INET, &addr, buf, sizeof(buf)) != NULL) {
            result = String(buf);
            if (isPrivate) break;   // best candidate found — stop scanning
        }
    }

    ::WSACleanup();
    if (result.Length() == 0) return L"localhost";
    return result;
} 

__fastcall TFormMotecSettings::TFormMotecSettings(TComponent* Owner)
    : TForm(Owner)
{
}

void __fastcall TFormMotecSettings::FormShow(TObject *Sender)
{
    TranslateUI();
    LoadFromThresholds();
    UpdateWebUrlLabel();   // show the tablet URL (IP+port) as soon as the dialog opens
}

void TFormMotecSettings::LoadFromThresholds()
{
    // General
    EditMinDamperSamples->Text = IntToStr((int)g_thresholds.minDamperSamples);
    EditMinValidLapTime->Text = FloatToStr(g_thresholds.minValidLapTimeMultiplier);
    EditMaxValidLapTime->Text = FloatToStr(g_thresholds.maxValidLapTimeMultiplier);

    ChkPreReleaseUpdates->Checked = (ReadPreReleaseUpdatesSetting() != 0);   // [General] feed pre-release

    // Pressure (psi)
    EditPressLow->Text = FloatToStr(g_thresholds.pressureLowPsi);
    EditPressHigh->Text = FloatToStr(g_thresholds.pressureHighPsi);

    // Tyre temp spread (C) - per axle
    EditFrontMinSpread->Text  = FloatToStr(g_thresholds.camberMinSpreadFront);
    EditFrontMaxSpread->Text  = FloatToStr(g_thresholds.camberMaxSpreadFront);
    EditRearMinSpread->Text   = FloatToStr(g_thresholds.camberMinSpreadRear);
    EditRearMaxSpread->Text   = FloatToStr(g_thresholds.camberMaxSpreadRear);
    EditRearOuterVsFrontStiffC->Text = FloatToStr(g_thresholds.rearOuterVsFrontStiffC);

    // Damper 4-zone targets (%)
    // Damper 4-zone targets (%)
    EditHscTarget->Text = FloatToStr(g_thresholds.damperHscTarget);
    EditLscTarget->Text = FloatToStr(g_thresholds.damperLscTarget);
    EditLsrTarget->Text = FloatToStr(g_thresholds.damperLsrTarget);
    EditHsrTarget->Text = FloatToStr(g_thresholds.damperHsrTarget);
    EditZoneToleranceHsc->Text = FloatToStr(g_thresholds.damperZoneToleranceHsc);
    EditZoneToleranceLsc->Text = FloatToStr(g_thresholds.damperZoneToleranceLsc);
    EditZoneToleranceLsr->Text = FloatToStr(g_thresholds.damperZoneToleranceLsr);
    EditZoneToleranceHsr->Text = FloatToStr(g_thresholds.damperZoneToleranceHsr);
    EditDamperSlowSpeedRange->Text = FloatToStr(g_thresholds.damperSlowSpeedRange);
    EditHistogramYOffset->Text = FloatToStr(g_thresholds.histogramYOffset);
    EditHistogramMinYPct->Text = FloatToStr(g_thresholds.histogramMinYPct);

    // Handling detection
    EditBrakePedalMin->Text = FloatToStr(g_thresholds.brakePedalMin);
    EditThrottleMaxBraking->Text = FloatToStr(g_thresholds.throttleMaxBraking);
    EditSteerMinCornering->Text = FloatToStr(g_thresholds.steerMinCornering);

    // Thermal
    EditWaterTempMax->Text = FloatToStr(g_thresholds.waterTempMax);
    EditOilTempMax->Text = FloatToStr(g_thresholds.oilTempMax);

    // PMR UDP network settings (from INI, not g_thresholds).
    // Empty stored value → show a grayed-out hint; empty still means "all interfaces".
    String pmrAddrStored = ReadPmrBindAddressSetting().Trim();
    if (pmrAddrStored.IsEmpty()) {
        EditPmrBindAddress->Text = "127.0.0.1";
        EditPmrBindAddress->Font->Color = clGray;
        pmrAddrShowingPlaceholder = true;
    } else {
        EditPmrBindAddress->Text = pmrAddrStored;
        EditPmrBindAddress->Font->Color = clWindowText;
        pmrAddrShowingPlaceholder = false;
    }
    EditPmrPort->Text = IntToStr(ReadPmrPortSetting());

    // Tablet Viewer web server settings (from INI, not g_thresholds)
    ChkWebServerEnabled->Checked = ReadWebServerEnabled();
    EditWebPort->Text = IntToStr(ReadWebServerPort());
}

// Grayed-out "127.0.0.1" hint for the Bind Address field (this VCL has no PlaceholderText).
// Focus does NOT erase anything: it just selects all text in black so typing replaces it,
// exactly like editing a normal pre-filled value.
void __fastcall TFormMotecSettings::EditPmrBindAddressEnter(TObject *Sender)
{
    pmrAddrEditedOnFocus = false;
    if (pmrAddrShowingPlaceholder) {
        EditPmrBindAddress->Font->Color = clWindowText;  // hint becomes editable black text
        EditPmrBindAddress->SelectAll();
    }
}

void __fastcall TFormMotecSettings::EditPmrBindAddressChange(TObject *Sender)
{
    pmrAddrEditedOnFocus = true;
}

void __fastcall TFormMotecSettings::EditPmrBindAddressLeave(TObject *Sender)
{
    if (pmrAddrShowingPlaceholder && !pmrAddrEditedOnFocus) {
        // Focused but never typed → back to the gray hint (= empty value).
        EditPmrBindAddress->Font->Color = clGray;
    } else {
        pmrAddrShowingPlaceholder = false;  // real content, even if it was cleared on purpose
        EditPmrBindAddress->Font->Color = clWindowText;
    }
}

void TFormMotecSettings::SaveToThresholds()
{
    try {
        // Update global g_thresholds from UI
        g_thresholds.minDamperSamples = (int)StrToInt(EditMinDamperSamples->Text);
        g_thresholds.minValidLapTimeMultiplier = StrToFloatDef(EditMinValidLapTime->Text, g_thresholds.minValidLapTimeMultiplier);
        g_thresholds.maxValidLapTimeMultiplier = StrToFloatDef(EditMaxValidLapTime->Text, g_thresholds.maxValidLapTimeMultiplier);

        g_thresholds.pressureLowPsi = StrToFloat(EditPressLow->Text);
        g_thresholds.pressureHighPsi = StrToFloat(EditPressHigh->Text);

        g_thresholds.camberMinSpreadFront  = StrToFloatDef(EditFrontMinSpread->Text,  g_thresholds.camberMinSpreadFront);
        g_thresholds.camberMaxSpreadFront  = StrToFloatDef(EditFrontMaxSpread->Text,  g_thresholds.camberMaxSpreadFront);
        g_thresholds.camberMinSpreadRear   = StrToFloatDef(EditRearMinSpread->Text,   g_thresholds.camberMinSpreadRear);
        g_thresholds.camberMaxSpreadRear   = StrToFloatDef(EditRearMaxSpread->Text,   g_thresholds.camberMaxSpreadRear);
        g_thresholds.rearOuterVsFrontStiffC = StrToFloat(EditRearOuterVsFrontStiffC->Text);

        // Damper 4-zone targets (%)
        // Damper 4-zone targets (%)
        g_thresholds.damperHscTarget = StrToFloatDef(EditHscTarget->Text, g_thresholds.damperHscTarget);
        g_thresholds.damperLscTarget = StrToFloatDef(EditLscTarget->Text, g_thresholds.damperLscTarget);
        g_thresholds.damperLsrTarget = StrToFloatDef(EditLsrTarget->Text, g_thresholds.damperLsrTarget);
        g_thresholds.damperHsrTarget = StrToFloatDef(EditHsrTarget->Text, g_thresholds.damperHsrTarget);
        g_thresholds.damperZoneToleranceHsc = StrToFloatDef(EditZoneToleranceHsc->Text, g_thresholds.damperZoneToleranceHsc);
        g_thresholds.damperZoneToleranceLsc = StrToFloatDef(EditZoneToleranceLsc->Text, g_thresholds.damperZoneToleranceLsc);
        g_thresholds.damperZoneToleranceLsr = StrToFloatDef(EditZoneToleranceLsr->Text, g_thresholds.damperZoneToleranceLsr);
        g_thresholds.damperZoneToleranceHsr = StrToFloatDef(EditZoneToleranceHsr->Text, g_thresholds.damperZoneToleranceHsr);
        g_thresholds.damperSlowSpeedRange = StrToFloatDef(EditDamperSlowSpeedRange->Text, g_thresholds.damperSlowSpeedRange);
        g_thresholds.histogramYOffset = StrToFloatDef(EditHistogramYOffset->Text, g_thresholds.histogramYOffset);
        g_thresholds.histogramMinYPct = StrToFloatDef(EditHistogramMinYPct->Text, g_thresholds.histogramMinYPct);

        g_thresholds.brakePedalMin = StrToFloat(EditBrakePedalMin->Text);
        g_thresholds.throttleMaxBraking = StrToFloat(EditThrottleMaxBraking->Text);
        g_thresholds.steerMinCornering = StrToFloat(EditSteerMinCornering->Text);

        g_thresholds.waterTempMax = StrToFloat(EditWaterTempMax->Text);
        g_thresholds.oilTempMax = StrToFloat(EditOilTempMax->Text);

    } catch (...) {
        ShowMessage("Error: Invalid input values.");
        throw; 
    }
}

void __fastcall TFormMotecSettings::ButtonSaveClick(TObject *Sender)
{
    try {
        SaveToThresholds();
        
        // Write the updated g_thresholds to the INI file.
        TIniFile* ini = new TIniFile(ThresholdsFilePath());
        try {
            ini->WriteInteger("General", "MinDamperSamples", (int)g_thresholds.minDamperSamples);

            ini->WriteFloat("Pressure", "Low", g_thresholds.pressureLowPsi);
            ini->WriteFloat("Pressure", "High", g_thresholds.pressureHighPsi);

            ini->WriteFloat("TyreTemp", "CamberMinSpreadFront",  g_thresholds.camberMinSpreadFront);
            ini->WriteFloat("TyreTemp", "CamberMaxSpreadFront",  g_thresholds.camberMaxSpreadFront);
            ini->WriteFloat("TyreTemp", "CamberMinSpreadRear",   g_thresholds.camberMinSpreadRear);
            ini->WriteFloat("TyreTemp", "CamberMaxSpreadRear",   g_thresholds.camberMaxSpreadRear);
            ini->WriteFloat("TyreTemp", "RearOuterVsFrontStiffC", g_thresholds.rearOuterVsFrontStiffC);



            ini->WriteFloat("DamperZones", "HscTarget", g_thresholds.damperHscTarget);
            ini->WriteFloat("DamperZones", "LscTarget", g_thresholds.damperLscTarget);
            ini->WriteFloat("DamperZones", "LsrTarget", g_thresholds.damperLsrTarget);
            ini->WriteFloat("DamperZones", "HsrTarget", g_thresholds.damperHsrTarget);
            ini->WriteFloat("DamperZones", "ToleranceHsc", g_thresholds.damperZoneToleranceHsc);
            ini->WriteFloat("DamperZones", "ToleranceLsc", g_thresholds.damperZoneToleranceLsc);
            ini->WriteFloat("DamperZones", "ToleranceLsr", g_thresholds.damperZoneToleranceLsr);
            ini->WriteFloat("DamperZones", "ToleranceHsr", g_thresholds.damperZoneToleranceHsr);
            ini->WriteFloat("Damper", "SlowSpeedRange", g_thresholds.damperSlowSpeedRange);
            ini->WriteFloat("Damper", "HistogramYOffset", g_thresholds.histogramYOffset);
            ini->WriteFloat("Damper", "HistogramMinYPct", g_thresholds.histogramMinYPct);

            ini->WriteFloat("Handling", "BrakePedalMin", g_thresholds.brakePedalMin);
            ini->WriteFloat("Handling", "ThrottleMaxBraking", g_thresholds.throttleMaxBraking);
            ini->WriteFloat("Handling", "SteerMinCornering", g_thresholds.steerMinCornering);
            ini->WriteFloat("Lap", "MinValidTimeMult", g_thresholds.minValidLapTimeMultiplier);
            ini->WriteFloat("Lap", "MaxValidTimeMult", g_thresholds.maxValidLapTimeMultiplier);

        } __finally {
            delete ini;
        }

        // PMR network settings → MotecDataAnalyser.ini [PMR] section (separate file).
        // If the gray hint is showing, treat it as empty (= all interfaces), never persist "127.0.0.1".
        String pmrAddrOut = pmrAddrShowingPlaceholder ? "" : EditPmrBindAddress->Text.Trim();
        SavePmrBindAddressSetting(pmrAddrOut);
        int pmrPort = StrToIntDef(EditPmrPort->Text, 7576);
        if (pmrPort < 1 || pmrPort > 65535) {
            ShowMessage("PMR port must be between 1 and 65535.");
            return;
        }

        TIniFile* iniMain = new TIniFile(ExtractFilePath(Application->ExeName) + "MotecDataAnalyser.ini");
        try {
            iniMain->WriteInteger("PMR", "Port", pmrPort);
        } __finally {
            delete iniMain;
        }

        // Tablet Viewer web server settings → [WebServer] section (same INI file).
        int webPort = StrToIntDef(EditWebPort->Text, 8765);
        if (webPort < 1024 || webPort > 65535) {
            ShowMessage("Tablet viewer port must be between 1024 and 65535.");
            return;
        }
        UpdateWebUrlLabel();   // refresh the tablet URL label on enable/disable toggle
    SaveWebServerEnabled(ChkWebServerEnabled->Checked);
        SaveWebServerPort(webPort);

        // Live-apply: start/stop the server immediately without restarting.
        FormMotecSetup->ApplyWebServerSettings();

        SavePreReleaseUpdatesSetting(ChkPreReleaseUpdates->Checked);   // [General] persiste le choix de feed (re-fetch cote form principale)

        ModalResult = mrOk;
    } catch (...) {
        // Error message already shown in SaveToThresholds
    }
}

void __fastcall TFormMotecSettings::ButtonCancelClick(TObject *Sender)
{
    ModalResult = mrCancel;
}

// Phase D3: live start/stop on checkbox toggle — persist immediately so the running app reacts.
void __fastcall TFormMotecSettings::ChkWebServerEnabledClick(TObject *Sender)
{
    UpdateWebUrlLabel();   // refresh the tablet URL label on enable/disable toggle
    SaveWebServerEnabled(ChkWebServerEnabled->Checked);
    FormMotecSetup->ApplyWebServerSettings();
}

void TFormMotecSettings::UpdateWebUrlLabel()
{
    const int port = StrToIntDef(EditWebPort->Text, 8765);
    LblWebUrl->Caption = "http://" + getLocalLanIpForLabel() + ":" + IntToStr(port);
    // Dim the URL while disabled so it reads as informational, not active.
    if (ChkWebServerEnabled->Checked)
        LblWebUrl->Font->Color = clNavy;
    else
        LblWebUrl->Font->Color = clGray;
}

void __fastcall TFormMotecSettings::EditWebPortChange(TObject *Sender)
{
    UpdateWebUrlLabel();   // live-update the URL while the user types a new port
}

#pragma package(smart_init)

#pragma resource "*.dfm"

TFormMotecSettings *FormMotecSettings;

void TFormMotecSettings::TranslateUI()
{
    ReportLanguage lang = TFormMotecSetup::ReadReportLanguageSetting();
    bool isFr = (lang == ReportLanguage::French);

    Caption = isFr ? "Réglages des Seuils" : "Threshold Settings";

    TabGeneral->Caption = isFr ? "Général" : "General";
    TabTyre->Caption = isFr ? "Pneus" : "Tyre";
    TabHandling->Caption = isFr ? "Comportement" : "Handling";
    TabThermal->Caption = isFr ? "Thermique" : "Thermal";
    TabSuspension->Caption = isFr ? "Suspension" : "Suspension";
    TabPmr->Caption = isFr ? "PMR UDP" : "PMR UDP";

    GroupGeneral->Caption = isFr ? "Échantillonnage et Confiance" : "Sampling & Confidence";
    LblMinDamperSamples->Caption = isFr ? "Nb min échantillons amortisseurs :" : "Min Damper Samples:";
    GroupLapValidity->Caption = isFr ? "Temps de tour valide" : "Valid Lap Time";
    LblMinValidLapTime->Caption = isFr ? "Temps min. tour valide (x meilleur):" : "Min Valid Lap Time (x best):";
    LblMaxValidLapTime->Caption = isFr ? "Temps max. tour valide (x meilleur):" : "Max Valid Lap Time (x best):";
    ChkPreReleaseUpdates->Caption = isFr ? "Recevoir aussi les versions pré-release (testeurs)" : "Also receive pre-releases (testers)";
    LblPrereleaseHint->Caption   = isFr ? "Le self-update regarde aussi le dépôt Pre-Releases ; la version installable la plus récente gagne." : "The update check also looks at the Pre-Releases repository; the newest installable version wins.";

    GroupTyrePressure->Caption = isFr ? "Pression (psi)" : "Pressure (psi)";
    LblPressLow->Caption = isFr ? "Minimum :" : "Low:";
    LblPressHigh->Caption = isFr ? "Maximum :" : "High:";

    GroupTyreTemp->Caption = isFr ? "Température (C)" : "Temperature (C)";
    LblFrontMinSpread->Caption  = isFr ? "Écart Int/Ext Avant Min :" : "Front Inner/Outer Spread Min:";
    LblFrontMaxSpread->Caption  = isFr ? "Écart Int/Ext Avant Max :" : "Front Inner/Outer Spread Max:";
    LblRearMinSpread->Caption   = isFr ? "Écart Int/Ext Arrière Min :" : "Rear Inner/Outer Spread Min:";
    LblRearMaxSpread->Caption   = isFr ? "Écart Int/Ext Arrière Max :" : "Rear Inner/Outer Spread Max:";
    LblRearOuterVsFrontStiffC->Caption = isFr ? "Écart externe Arrière vs Avant (°C) :" : "Rear Outer vs Front Temp Diff:";

    GroupDamperZones->Caption = isFr ? "Cibles 4 Zones Amortisseur (%)" : "Damper 4-Zone Targets (%)";
	LblHscTarget->Caption = isFr ? "Comp. Rapide :" : "Hi Comp:";
	LblLscTarget->Caption = isFr ? "Comp. Lente :" : "Lo Comp:";
	LblLsrTarget->Caption = isFr ? "Détente Lente :" : "Lo Rebound:";
	LblHsrTarget->Caption = isFr ? "Détente Rapide :" : "Hi Rebound:";
    LblZoneToleranceHsc->Caption = isFr ? "Tol. HSC (%) :" : "HSC Tol. (%):";
    LblZoneToleranceLsr->Caption = isFr ? "Tol. LSR (%) :" : "LSR Tol. (%):";
    LblZoneToleranceLsc->Caption = isFr ? "Tol. LSC (%) :" : "LSC Tol. (%):";
    LblZoneToleranceHsr->Caption = isFr ? "Tol. HSR (%) :" : "HSR Tol. (%):";
	LblDamperSlowSpeedRange->Caption = isFr ? "Largeur Bande Vitesse Lente (mm/s) :" : "Slow Speed Range (mm/s):";
    LblHistogramYOffset->Caption = isFr ? "Marge axe Y histogramme (%) :" : "Histogram Y-axis headroom (%):";
	LblHistogramMinYPct->Caption = isFr ? "Échelle axe Y minimum (%) :" : "Minimum Y-axis range (%):";

    GroupHandling->Caption = isFr ? "Seuils de Détection" : "Detection Thresholds";
    LblBrakePedalMin->Caption = isFr ? "Pédale Frein Min :" : "Brake Pedal Min:";
    LblThrottleMaxBraking->Caption = isFr ? "Accélérateur Max au freinage :" : "Throttle Max Braking:";
    LblSteerMinCornering->Caption = isFr ? "Direction Min en virage :" : "Steer Min Cornering:";

    GroupThermal->Caption = isFr ? "Seuils Thermiques Moteur" : "Engine Thermal Thresholds";
    LblWaterTempMax->Caption = isFr ? "Temp Eau Max :" : "Max Water Temp:";
    LblOilTempMax->Caption = isFr ? "Temp Huile Max :" : "Max Oil Temp:";

    GroupPmrNetwork->Caption = isFr ? "Réglages Réseau UDP" : "UDP Network Settings";
    LblPmrBindAddress->Caption = isFr ? "Adresse de liaison (IP) :" : "Bind Address:";
    LblPmrPort->Caption = isFr ? "Port UDP :" : "UDP Port:";
    LblPmrHint->Caption = isFr ? "(Laisser vide pour écouter sur toutes les interfaces)" : "(Leave Bind Address empty to listen on all interfaces)";

    TabTablet->Caption = isFr ? "Visionneuse Tablette" : "Tablet Viewer";
    GroupWebServer->Caption = isFr ? "Serveur Web Local" : "Local Web Server";
    ChkWebServerEnabled->Caption = isFr ? "Activer le visionneur de rapport tablette (serveur web local)" : "Enable tablet report viewer (local web server)";
    LblWebPort->Caption = isFr ? "Port HTTP :" : "HTTP Port:";
    LblWebHint->Caption = isFr
		? "(Quand activé, un serveur web local permet aux appareils Mobiles de voir les résultats d'analyse automatiquement.\r\nNote : le Pare-feu Windows demandera une autorisation à la première activation pour le port configuré.)"
        : "(When enabled, a local web server runs so mobil devices can view analysis results automatically.\r\nNote: Windows Firewall will prompt once on first start to allow inbound connections on the configured port.)";

    ButtonSave->Caption = isFr ? "Enregistrer" : "Save";
    ButtonCancel->Caption = isFr ? "Annuler" : "Cancel";
}

#pragma package(smart_init)


