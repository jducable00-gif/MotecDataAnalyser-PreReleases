//---------------------------------------------------------------------------
// MotecSettingsForm.h � Dialog for configuring analysis thresholds.
//---------------------------------------------------------------------------

#pragma once

#include <System.Classes.hpp>
#include <Vcl.Controls.hpp>
#include <Vcl.StdCtrls.hpp>
#include <Vcl.Forms.hpp>
#include <Vcl.ExtCtrls.hpp>
#include <Vcl.Graphics.hpp>
#include <Vcl.ComCtrls.hpp>
#include "MotecSetupAnalyzer.h"

class TFormMotecSettings : public TForm
{
__published:
    TPageControl *PageControl;
    TTabSheet *TabGeneral;
    TTabSheet *TabTyre;
    TTabSheet *TabHandling;
    TTabSheet *TabThermal;
    TTabSheet *TabSuspension;

    // General Tab
    TGroupBox *GroupGeneral;
    TLabel *LblMinDamperSamples;
    TEdit *EditMinDamperSamples;

    // Valid Lap Time Tab (General)
    TGroupBox *GroupLapValidity;
    TLabel *LblMinValidLapTime;
    TEdit *EditMinValidLapTime;
    TLabel *LblMaxValidLapTime;
    TEdit *EditMaxValidLapTime;
    TCheckBox *ChkPreReleaseUpdates;   // [General] feed pre-release de self-update
    TLabel *LblPrereleaseHint;         // [General] aide sous la case pre-release

    // Tyre Tab
    TGroupBox *GroupTyrePressure;
    TLabel *LblPressLow;
    TEdit *EditPressLow;
    TLabel *LblPressHigh;
    TEdit *EditPressHigh;

    TGroupBox *GroupTyreTemp;
    // Front axle
    TLabel *LblFrontMinSpread;
    TEdit  *EditFrontMinSpread;
    TLabel *LblFrontMaxSpread;
    TEdit  *EditFrontMaxSpread;
    // Rear axle
    TLabel *LblRearMinSpread;
    TEdit  *EditRearMinSpread;
    TLabel *LblRearMaxSpread;
    TEdit  *EditRearMaxSpread;
    // Rear outer vs front outer temp diff (deg C): suspension balance check.
    TLabel *LblRearOuterVsFrontStiffC;
    TEdit  *EditRearOuterVsFrontStiffC;

    // Damper 4-Zone Targets Tab
    TGroupBox *GroupDamperZones;
    TLabel *LblHscTarget;
    TEdit *EditHscTarget;
    TLabel *LblLscTarget;
    TEdit *EditLscTarget;
    TLabel *LblLsrTarget;
    TEdit *EditLsrTarget;
    TLabel *LblHsrTarget;
    TEdit *EditHsrTarget;
    TLabel *LblZoneToleranceHsc;
    TEdit *EditZoneToleranceHsc;
    TLabel *LblZoneToleranceLsc;
    TEdit *EditZoneToleranceLsc;
    TLabel *LblZoneToleranceLsr;
    TEdit *EditZoneToleranceLsr;
    TLabel *LblZoneToleranceHsr;
    TEdit *EditZoneToleranceHsr;
    TLabel *LblDamperSlowSpeedRange;
    TEdit *EditDamperSlowSpeedRange;
    TLabel *LblHistogramYOffset;
    TEdit *EditHistogramYOffset;
    TLabel *LblHistogramMinYPct;
    TEdit *EditHistogramMinYPct;

    // Handling Tab
    TGroupBox *GroupHandling;
    TLabel *LblBrakePedalMin;
    TEdit *EditBrakePedalMin;
    TLabel *LblThrottleMaxBraking;
    TEdit *EditThrottleMaxBraking;
    TLabel *LblSteerMinCornering;
    TEdit *EditSteerMinCornering;

    // Thermal Tab
    TGroupBox *GroupThermal;
    TLabel *LblWaterTempMax;
    TEdit *EditWaterTempMax;
    TLabel *LblOilTempMax;
    TEdit *EditOilTempMax;

    // PMR UDP Tab
    TTabSheet *TabPmr;
    TGroupBox *GroupPmrNetwork;
    TLabel *LblPmrBindAddress;
    TEdit *EditPmrBindAddress;
    TLabel *LblPmrPort;
    TEdit *EditPmrPort;
    TLabel *LblPmrHint;

    // Tablet Viewer Tab (Phase D)
    TTabSheet *TabTablet;
    TGroupBox *GroupWebServer;
    TCheckBox *ChkWebServerEnabled;
    TLabel *LblWebUrl;
    TLabel *LblWebPort;
    TEdit *EditWebPort;
    TLabel *LblWebHint;

    // Buttons
    TButton *ButtonSave;
    TButton *ButtonCancel;

    void __fastcall ButtonSaveClick(TObject *Sender);
    void __fastcall ButtonCancelClick(TObject *Sender);
    void __fastcall FormShow(TObject *Sender);
    // Phase D: live start/stop on checkbox toggle (no Save needed)
    void __fastcall ChkWebServerEnabledClick(TObject *Sender);
    void __fastcall EditWebPortChange(TObject *Sender);   // keep LblWebUrl in sync while typing the port
    // DFM-referenced handlers must stay in __published so VCL streaming can resolve them by name.
    void __fastcall EditPmrBindAddressChange(TObject *Sender);
    void __fastcall EditPmrBindAddressEnter(TObject *Sender);
    void __fastcall EditPmrBindAddressLeave(TObject *Sender);

private:
    bool pmrAddrShowingPlaceholder = false;  // true while the gray "127.0.0.1" hint stands for an empty value
    bool pmrAddrEditedOnFocus = false;       // set by OnChange: user actually typed something since focus

public:
    __fastcall TFormMotecSettings(TComponent* Owner);
    void LoadFromThresholds();
    void SaveToThresholds();
    void TranslateUI();
    // Internal helper (NOT a VCL event handler — must stay out of __published).
    void UpdateWebUrlLabel();
};

extern TFormMotecSettings *FormMotecSettings;
