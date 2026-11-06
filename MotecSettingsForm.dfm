object FormMotecSettings: TFormMotecSettings
  Left = 0
  Top = 0
  Caption = 'Threshold Settings'
  ClientHeight = 500
  ClientWidth = 600
  Color = clWhite
  Font.Charset = DEFAULT_CHARSET
  Font.Color = clWindowText
  Font.Height = -12
  Font.Name = 'Segoe UI'
  Font.Style = []
  Position = poScreenCenter
  OnShow = FormShow
  TextHeight = 15
  object PageControl: TPageControl
    Left = 8
    Top = 8
    Width = 584
    Height = 430
    ActivePage = TabSuspension
    TabHeight = 25
    TabOrder = 0
    TabWidth = 80
    object TabGeneral: TTabSheet
      Caption = 'General'
      object GroupGeneral: TGroupBox
        Left = 21
        Top = 21
        Width = 552
        Height = 160
        Caption = 'Sampling & Confidence'
        TabOrder = 0
        object LblMinDamperSamples: TLabel
          Left = 16
          Top = 32
          Width = 116
          Height = 15
          Caption = 'Min Damper Samples:'
        end
        object EditMinDamperSamples: TEdit
          Left = 212
          Top = 30
          Width = 60
          Height = 23
          TabOrder = 0
          Text = '20'
        end
      end
      object GroupLapValidity: TGroupBox
        Left = 21
        Top = 200
        Width = 552
        Height = 90
        Caption = 'Valid Lap Time'
        TabOrder = 1
        object LblMinValidLapTime: TLabel
          Left = 16
          Top = 32
          Width = 145
          Height = 15
          Caption = 'Min Valid Lap Time (x best):'
        end
        object LblMaxValidLapTime: TLabel
          Left = 16
          Top = 64
          Width = 147
          Height = 15
          Caption = 'Max Valid Lap Time (x best):'
        end
        object EditMinValidLapTime: TEdit
          Left = 212
          Top = 30
          Width = 60
          Height = 23
          TabOrder = 0
          Text = '0.7'
        end
        object EditMaxValidLapTime: TEdit
          Left = 212
          Top = 62
          Width = 60
          Height = 23
          TabOrder = 1
          Text = '1.3'
        end
      end
       object GroupUpdates: TGroupBox
         Left = 21
         Top = 305
         Width = 552
         Height = 84
         Caption = 'Update Feed'
         TabOrder = 2
         object ChkPreReleaseUpdates: TCheckBox
           Left = 16
           Top = 30
           Width = 400
           Height = 17
           Caption = 'Receive pre-releases (testers)'
           TabOrder = 0
         end
         object LblPrereleaseHint: TLabel
           Left = 16
           Top = 54
           Width = 520
           Height = 15
           Caption = 'The update feed switches to the Pre-Releases repository.'
         end
       end

    end
    object TabTyre: TTabSheet
      Caption = 'Tyre'
      object GroupTyrePressure: TGroupBox
        Left = 16
        Top = 16
        Width = 552
        Height = 96
        Caption = 'Pressure (psi)'
        TabOrder = 0
        object LblPressLow: TLabel
          Left = 16
          Top = 32
          Width = 25
          Height = 15
          Caption = 'Low:'
        end
        object LblPressHigh: TLabel
          Left = 16
          Top = 64
          Width = 29
          Height = 15
          Caption = 'High:'
        end
        object EditPressLow: TEdit
          Left = 64
          Top = 28
          Width = 60
          Height = 23
          TabOrder = 0
          Text = '23.0'
        end
        object EditPressHigh: TEdit
          Left = 64
          Top = 60
          Width = 60
          Height = 23
          TabOrder = 1
          Text = '33.0'
        end
      end
      object GroupTyreTemp: TGroupBox
        Left = 16
        Top = 120
        Width = 552
        Height = 210
        Caption = 'Temperature (C)'
        TabOrder = 1
        object LblFrontMinSpread: TLabel
          Left = 12
          Top = 32
          Width = 55
          Height = 15
          Caption = 'Front Min:'
        end
        object LblFrontMaxSpread: TLabel
          Left = 12
          Top = 61
          Width = 57
          Height = 15
          Caption = 'Front Max:'
        end
        object LblRearMinSpread: TLabel
          Left = 340
          Top = 32
          Width = 50
          Height = 15
          Caption = 'Rear Min:'
        end
        object LblRearMaxSpread: TLabel
          Left = 340
          Top = 61
          Width = 52
          Height = 15
          Caption = 'Rear Max:'
        end
        object LblRearOuterVsFrontStiffC: TLabel
          Left = 12
          Top = 92
          Width = 158
          Height = 15
          Caption = 'Rear Outer vs Front Temp Diff:'
        end
        object EditFrontMinSpread: TEdit
          Left = 165
          Top = 28
          Width = 60
          Height = 23
          TabOrder = 0
          Text = '4.0'
        end
        object EditFrontMaxSpread: TEdit
          Left = 165
          Top = 57
          Width = 60
          Height = 23
          TabOrder = 1
          Text = '10.0'
        end
        object EditRearMinSpread: TEdit
          Left = 460
          Top = 28
          Width = 60
          Height = 23
          TabOrder = 2
          Text = '2.0'
        end
        object EditRearMaxSpread: TEdit
          Left = 460
          Top = 57
          Width = 60
          Height = 23
          TabOrder = 3
          Text = '7.0'
        end
        object EditRearOuterVsFrontStiffC: TEdit
          Left = 300
          Top = 88
          Width = 60
          Height = 23
          TabOrder = 4
          Text = '5.0'
        end
      end
    end
    object TabHandling: TTabSheet
      Caption = 'Handling'
      object GroupHandling: TGroupBox
        Left = 16
        Top = 16
        Width = 552
        Height = 384
        Caption = 'Detection Thresholds'
        TabOrder = 0
        object LblBrakePedalMin: TLabel
          Left = 16
          Top = 32
          Width = 88
          Height = 15
          Caption = 'Brake Pedal Min:'
        end
        object LblThrottleMaxBraking: TLabel
          Left = 16
          Top = 64
          Width = 113
          Height = 15
          Caption = 'Throttle Max Braking:'
        end
        object LblSteerMinCornering: TLabel
          Left = 16
          Top = 96
          Width = 109
          Height = 15
          Caption = 'Steer Min Cornering:'
        end
        object EditBrakePedalMin: TEdit
          Left = 170
          Top = 29
          Width = 60
          Height = 23
          TabOrder = 0
          Text = '10.0'
        end
        object EditThrottleMaxBraking: TEdit
          Left = 170
          Top = 60
          Width = 60
          Height = 23
          TabOrder = 1
          Text = '35.0'
        end
        object EditSteerMinCornering: TEdit
          Left = 170
          Top = 89
          Width = 60
          Height = 23
          TabOrder = 2
          Text = '5.0'
        end
      end
    end
    object TabThermal: TTabSheet
      Caption = 'Thermal'
      object GroupThermal: TGroupBox
        Left = 16
        Top = 16
        Width = 552
        Height = 384
        Caption = 'Engine Thermal Thresholds'
        TabOrder = 0
        object LblWaterTempMax: TLabel
          Left = 16
          Top = 32
          Width = 92
          Height = 15
          Caption = 'Max Water Temp:'
        end
        object LblOilTempMax: TLabel
          Left = 16
          Top = 64
          Width = 76
          Height = 15
          Caption = 'Max Oil Temp:'
        end
        object EditWaterTempMax: TEdit
          Left = 170
          Top = 28
          Width = 60
          Height = 23
          TabOrder = 0
          Text = '95.0'
        end
        object EditOilTempMax: TEdit
          Left = 170
          Top = 60
          Width = 60
          Height = 23
          TabOrder = 1
          Text = '115.0'
        end
      end
    end
    object TabSuspension: TTabSheet
      Caption = 'Suspension'
      object GroupDamperZones: TGroupBox
        Left = 16
        Top = 16
        Width = 552
        Height = 306
        Caption = 'Damper 4-Zone Targets (%)'
        TabOrder = 0
        object LblHscTarget: TLabel
          Left = 16
          Top = 32
          Width = 51
          Height = 15
          Caption = 'Hi Comp:'
        end
        object LblLscTarget: TLabel
          Left = 16
          Top = 64
          Width = 52
          Height = 15
          Caption = 'Lo Comp:'
        end
        object LblLsrTarget: TLabel
          Left = 16
          Top = 95
          Width = 67
          Height = 15
          Caption = 'Lo Rebound:'
        end
        object LblHsrTarget: TLabel
          Left = 16
          Top = 128
          Width = 66
          Height = 15
          Caption = 'Hi Rebound:'
        end
        object LblZoneToleranceHsc: TLabel
          Left = 16
          Top = 160
          Width = 68
          Height = 15
          Caption = 'HSC Tol. (%):'
        end
        object LblZoneToleranceLsr: TLabel
          Left = 252
          Top = 74
          Width = 64
          Height = 15
          Caption = 'LSR Tol. (%):'
        end
        object LblZoneToleranceLsc: TLabel
          Left = 16
          Top = 192
          Width = 65
          Height = 15
          Caption = 'LSC Tol. (%):'
        end
        object LblZoneToleranceHsr: TLabel
          Left = 252
          Top = 95
          Width = 67
          Height = 15
          Caption = 'HSR Tol. (%):'
        end
        object LblDamperSlowSpeedRange: TLabel
          Left = 252
          Top = 31
          Width = 142
          Height = 15
          Caption = 'Slow Speed Range (mm/s):'
        end
        object LblHistogramYOffset: TLabel
          Left = 16
          Top = 256
          Width = 173
          Height = 15
          Caption = 'Histogram Y-axis headroom (%):'
        end
        object LblHistogramMinYPct: TLabel
          Left = 16
          Top = 277
          Width = 145
          Height = 15
          Caption = 'Minimum Y-axis range (%):'
        end
        object EditHscTarget: TEdit
          Left = 130
          Top = 28
          Width = 60
          Height = 23
          TabOrder = 0
          Text = '15.0'
        end
        object EditLscTarget: TEdit
          Left = 130
          Top = 63
          Width = 60
          Height = 23
          TabOrder = 1
          Text = '30.0'
        end
        object EditLsrTarget: TEdit
          Left = 130
          Top = 92
          Width = 60
          Height = 23
          TabOrder = 2
          Text = '30.0'
        end
        object EditHsrTarget: TEdit
          Left = 130
          Top = 124
          Width = 60
          Height = 23
          TabOrder = 3
          Text = '15.0'
        end
        object EditZoneToleranceHsc: TEdit
          Left = 130
          Top = 156
          Width = 60
          Height = 23
          TabOrder = 4
          Text = '5.0'
        end
        object EditZoneToleranceLsr: TEdit
          Left = 476
          Top = 71
          Width = 60
          Height = 23
          TabOrder = 5
          Text = '5.0'
        end
        object EditZoneToleranceLsc: TEdit
          Left = 130
          Top = 188
          Width = 60
          Height = 23
          TabOrder = 6
          Text = '5.0'
        end
        object EditZoneToleranceHsr: TEdit
          Left = 476
          Top = 100
          Width = 60
          Height = 23
          TabOrder = 7
          Text = '5.0'
        end
        object EditDamperSlowSpeedRange: TEdit
          Left = 476
          Top = 28
          Width = 60
          Height = 23
          TabOrder = 8
          Text = '50.0'
        end
        object EditHistogramYOffset: TEdit
          Left = 230
          Top = 252
          Width = 60
          Height = 23
          TabOrder = 9
          Text = '20.0'
        end
        object EditHistogramMinYPct: TEdit
          Left = 230
          Top = 281
          Width = 60
          Height = 23
          TabOrder = 10
          Text = '10.0'
        end
      end
    end
    object TabPmr: TTabSheet
      Caption = 'PMR UDP'
      object GroupPmrNetwork: TGroupBox
        Left = 16
        Top = 16
        Width = 552
        Height = 140
        Caption = 'UDP Network Settings'
        TabOrder = 0
        object LblPmrBindAddress: TLabel
          Left = 16
          Top = 32
          Width = 72
          Height = 15
          Caption = 'Bind Address:'
        end
        object LblPmrPort: TLabel
          Left = 16
          Top = 64
          Width = 51
          Height = 15
          Caption = 'UDP Port:'
        end
        object LblPmrHint: TLabel
          Left = 16
          Top = 96
          Width = 278
          Height = 15
          Caption = '(Leave Bind Address empty to listen on all interfaces)'
        end
        object EditPmrBindAddress: TEdit
          Left = 170
          Top = 28
          Width = 140
          Height = 23
          TabOrder = 0
          OnChange = EditPmrBindAddressChange
          OnEnter = EditPmrBindAddressEnter
          OnExit = EditPmrBindAddressLeave
        end
        object EditPmrPort: TEdit
          Left = 170
          Top = 60
          Width = 80
          Height = 23
          TabOrder = 1
          Text = '7576'
        end
      end
    end
    object TabTablet: TTabSheet
      Caption = 'Tablet Viewer'
      object GroupWebServer: TGroupBox
        Left = 16
        Top = 16
        Width = 552
        Height = 140
        Caption = 'Local Web Server'
        TabOrder = 0
        object LblWebUrl: TLabel
          Left = 384
          Top = 35
          Width = 44
          Height = 15
          Caption = 'http://...'
        end
        object LblWebPort: TLabel
          Left = 16
          Top = 64
          Width = 56
          Height = 15
          Caption = 'HTTP Port:'
        end
        object LblWebHint: TLabel
          Left = 16
          Top = 92
          Width = 480
          Height = 32
          AutoSize = False
          Caption = 
            '(When enabled, a local web server runs so connected tablets can ' +
            'view analysis results automatically.\r\nNote: Windows Firewall w' +
            'ill prompt once on first start to allow inbound connections on t' +
            'he configured port.)'
        end
        object ChkWebServerEnabled: TCheckBox
          Left = 16
          Top = 32
          Width = 350
          Height = 17
          Caption = 'Enable tablet report viewer (local web server)'
          TabOrder = 0
          OnClick = ChkWebServerEnabledClick
        end
        object EditWebPort: TEdit
          Left = 170
          Top = 60
          Width = 80
          Height = 23
          TabOrder = 1
          Text = '8765'
          OnChange = EditWebPortChange
        end
      end
    end
  end
  object ButtonSave: TButton
    Left = 384
    Top = 450
    Width = 90
    Height = 37
    Caption = 'Save'
    TabOrder = 1
    OnClick = ButtonSaveClick
  end
  object ButtonCancel: TButton
    Left = 486
    Top = 450
    Width = 90
    Height = 37
    Caption = 'Cancel'
    TabOrder = 2
    OnClick = ButtonCancelClick
  end
end
