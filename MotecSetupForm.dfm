object FormMotecSetup: TFormMotecSetup
  Left = 0
  Top = 0
  Caption = 'AC EVO  - MoTeC Setup Analyzer'
  ClientHeight = 1061
  ClientWidth = 1471
  Color = clWhite
  Font.Charset = DEFAULT_CHARSET
  Font.Color = clWindowText
  Font.Height = -12
  Font.Name = 'Segoe UI'
  Font.Style = []
  Position = poScreenCenter
  OnResize = FormResize
  TextHeight = 15
  object LabelProfile: TLabel
    Left = 4
    Top = 28
    Width = 37
    Height = 15
    Caption = 'Profile:'
  end
  object LabelLanguage: TLabel
    Left = 178
    Top = 28
    Width = 55
    Height = 15
    Caption = 'Language:'
  end
  object TireTempPaintBox: TPaintBox
    Left = 8
    Top = 100
    Width = 280
    Height = 362
    OnPaint = TireTempPaintBoxPaint
  end
  object BrakeTempPaintBox: TPaintBox
    Left = 294
    Top = 100
    Width = 300
    Height = 358
    OnPaint = BrakeTempPaintBoxPaint
  end
  object LiveLed: TShape
    Left = 508
    Top = 31
    Width = 14
    Height = 15
    Brush.Color = clRed
    Shape = stCircle
  end
  object LabelLiveStatus: TLabel
    Left = 528
    Top = 28
    Width = 72
    Height = 15
    Caption = 'Disconnected'
    Font.Charset = DEFAULT_CHARSET
    Font.Color = clGray
    Font.Height = -12
    Font.Name = 'Segoe UI'
    Font.Style = []
    ParentFont = False
  end
  object LiveRecordingLabel: TLabel
    Left = 680
    Top = 31
    Width = 21
    Height = 15
    Caption = 'REC'
    Font.Charset = DEFAULT_CHARSET
    Font.Color = clRed
    Font.Height = -12
    Font.Name = 'Segoe UI'
    Font.Style = []
    ParentFont = False
    Visible = False
  end
  object DamperLayoutTabs: TPageControl
    Left = 600
    Top = 88
    Width = 831
    Height = 374
    ActivePage = TabDamperCombined
    TabOrder = 7
    OnChange = DamperLayoutTabsChange
    object TabDamperGrid: TTabSheet
      Caption = 'Grid'
      object DamperFLPaintBox: TPaintBox
        Left = 8
        Top = 4
        Width = 402
        Height = 152
        OnPaint = DamperFLPaintBoxPaint
      end
      object DamperFRPaintBox: TPaintBox
        Left = 419
        Top = 4
        Width = 402
        Height = 152
        OnPaint = DamperFRPaintBoxPaint
      end
      object DamperRLPaintBox: TPaintBox
        Left = 8
        Top = 160
        Width = 402
        Height = 152
        OnPaint = DamperRLPaintBoxPaint
      end
      object DamperRRPaintBox: TPaintBox
        Left = 419
        Top = 160
        Width = 402
        Height = 152
        OnPaint = DamperRRPaintBoxPaint
      end
    end
    object TabDamperCombined: TTabSheet
      Caption = 'Combined'
      object DamperCombinedPaintBox: TPaintBox
        Left = 0
        Top = 0
        Width = 823
        Height = 344
        Align = alClient
        OnPaint = DamperCombinedPaintBoxPaint
        ExplicitLeft = -4
        ExplicitTop = 4
      end
    end
  end
  object ButtonLoad: TButton
    Left = 190
    Top = 804
    Width = 120
    Height = 32
    Caption = 'Import Motec Data'
    TabOrder = 0
    Visible = False
    OnClick = ButtonLoadClick
  end
  object PageControlReport: TPageControl
    Left = 8
    Top = 468
    Width = 1069
    Height = 317
    ActivePage = TabHandling
    TabOrder = 1
    object TabSummary: TTabSheet
      Caption = 'Summary'
      object MemoSummary: TMemo
        Left = 0
        Top = 0
        Width = 1061
        Height = 287
        Align = alClient
        Color = clWhite
        Font.Charset = DEFAULT_CHARSET
        Font.Color = clWindowText
        Font.Height = -13
        Font.Name = 'Consolas'
        Font.Style = []
        ParentFont = False
        ReadOnly = True
        ScrollBars = ssVertical
        TabOrder = 0
      end
    end
    object TabGeneral: TTabSheet
      Caption = 'General'
      object MemoGeneral: TMemo
        Left = 0
        Top = 0
        Width = 1061
        Height = 287
        Align = alClient
        Color = clWhite
        Font.Charset = DEFAULT_CHARSET
        Font.Color = clWindowText
        Font.Height = -13
        Font.Name = 'Consolas'
        Font.Style = []
        ParentFont = False
        ReadOnly = True
        ScrollBars = ssVertical
        TabOrder = 0
      end
    end
    object TabTyre: TTabSheet
      Caption = 'Tyre'
      object MemoTyre: TMemo
        Left = 0
        Top = 0
        Width = 1061
        Height = 287
        Align = alClient
        Color = clWhite
        Font.Charset = DEFAULT_CHARSET
        Font.Color = clWindowText
        Font.Height = -13
        Font.Name = 'Consolas'
        Font.Style = []
        ParentFont = False
        ReadOnly = True
        ScrollBars = ssVertical
        TabOrder = 0
      end
    end
    object TabBrake: TTabSheet
      Caption = 'Brake'
      object MemoBrake: TMemo
        Left = 0
        Top = 0
        Width = 1061
        Height = 287
        Align = alClient
        Color = clWhite
        Font.Charset = DEFAULT_CHARSET
        Font.Color = clWindowText
        Font.Height = -13
        Font.Name = 'Consolas'
        Font.Style = []
        ParentFont = False
        ReadOnly = True
        ScrollBars = ssVertical
        TabOrder = 0
      end
    end
    object TabSuspension: TTabSheet
      Caption = 'Suspension'
      object MemoSuspension: TMemo
        Left = 0
        Top = 0
        Width = 1061
        Height = 287
        Align = alClient
        Color = clWhite
        Font.Charset = DEFAULT_CHARSET
        Font.Color = clWindowText
        Font.Height = -13
        Font.Name = 'Consolas'
        Font.Style = []
        ParentFont = False
        ReadOnly = True
        ScrollBars = ssVertical
        TabOrder = 0
      end
    end
    object TabEngineCooling: TTabSheet
      Caption = 'Engine & Cooling'
      object MemoEngineCooling: TMemo
        Left = 0
        Top = 0
        Width = 1061
        Height = 287
        Align = alClient
        Color = clWhite
        Font.Charset = DEFAULT_CHARSET
        Font.Color = clWindowText
        Font.Height = -13
        Font.Name = 'Consolas'
        Font.Style = []
        ParentFont = False
        ReadOnly = True
        ScrollBars = ssVertical
        TabOrder = 0
      end
    end
    object TabHandling: TTabSheet
      Caption = 'Handling'
      object MemoHandling: TMemo
        Left = 0
        Top = 0
        Width = 1061
        Height = 287
        Align = alClient
        Color = clWhite
        Font.Charset = DEFAULT_CHARSET
        Font.Color = clWindowText
        Font.Height = -13
        Font.Name = 'Consolas'
        Font.Style = []
        ParentFont = False
        ReadOnly = True
        ScrollBars = ssVertical
        TabOrder = 0
      end
    end
    object TabDiagnostics: TTabSheet
      Caption = 'Diagnostics'
      object MemoDiagnostics: TMemo
        Left = 0
        Top = 0
        Width = 1061
        Height = 287
        Align = alClient
        Color = clWhite
        Font.Charset = DEFAULT_CHARSET
        Font.Color = clWindowText
        Font.Height = -13
        Font.Name = 'Consolas'
        Font.Style = []
        ParentFont = False
        ReadOnly = True
        ScrollBars = ssVertical
        TabOrder = 0
      end
    end
  end
  object StatusBar1: TStatusBar
    Left = 0
    Top = 1042
    Width = 1471
    Height = 19
    Panels = <
      item
        Text = 'FADOQ '
        Width = 50
      end
      item
        Text = 'V5.6'
        Width = 50
      end>
  end
  object Button1: TButton
    Left = 939
    Top = 24
    Width = 170
    Height = 34
    Caption = 'Import Motec Data'
    TabOrder = 2
    OnClick = Button1Click
  end
  object ButtonSettings: TButton
    Left = 1165
    Top = 20
    Width = 170
    Height = 34
    Caption = 'Settings'
    TabOrder = 8
    OnClick = ButtonSettingsClick
  end
  object ButtonAbout: TButton
    Left = 1341
    Top = 20
    Width = 130
    Height = 34
    Caption = 'About'
    TabOrder = 11
    OnClick = ButtonAboutClick
  end
  object ProgressBarExport: TProgressBar
    Left = 36
    Top = 64
    Width = 1073
    Height = 18
    TabOrder = 3
  end
  object LanguageCombo: TComboBox
    Left = 239
    Top = 25
    Width = 130
    Height = 23
    Style = csDropDownList
    TabOrder = 4
    OnChange = LanguageComboChange
  end
  object ChannelProfileCombo: TComboBox
    Left = 47
    Top = 25
    Width = 110
    Height = 23
    Style = csDropDownList
    TabOrder = 5
    OnChange = ChannelProfileComboChange
  end
  object LiveCaptureCheckBox: TCheckBox
    Left = 402
    Top = 27
    Width = 120
    Height = 19
    Caption = 'Live Capture'
    TabOrder = 6
    OnClick = LiveCaptureCheckBoxClick
  end
  object AutoDetectTrackCheckBox: TCheckBox
    Left = 779
    Top = 8
    Width = 89
    Height = 19
    Caption = 'Auto session'
    TabOrder = 9
    OnClick = AutoDetectTrackCheckBoxClick
  end
  object ButtonResetSession: TButton
    Left = 779
    Top = 24
    Width = 110
    Height = 34
    Caption = 'Reset session'
    Enabled = False
    TabOrder = 10
    OnClick = ButtonResetSessionClick
  end
  object LiveTimer: TTimer
    Enabled = False
    Interval = 16
    OnTimer = LiveTimerTimer
    Left = 112
    Top = 800
  end
  object LiveConnectTimer: TTimer
    Enabled = False
    OnTimer = LiveConnectTimerTimer
    Left = 24
    Top = 792
  end
  object ReleasePollTimer: TTimer
    Enabled = False
    OnTimer = ReleasePollTimerTimer
    Left = 56
    Top = 800
  end
end
