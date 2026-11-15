@echo off
rem =============================================================================
rem  build_test_report_json.bat - Phase A (A3) standalone harness for ReportJsonWriter.h.
rem  Pure ASCII on purpose (PowerShell/cmd ANSI quirks). Run from anywhere:
rem      scripts\build_test_report_json.bat [Clean]
rem =============================================================================

setlocal
cd /d "%~dp0"

call "C:\Program Files (x86)\Embarcadero\Studio\23.0\bin\rsvars.bat" >nul 2>&1

del /q test_report_json.exe 2>nul   rem always relink fresh (avoids stale incremental-link state)
if "%~1"=="Clean" del /q report_payload.json *.o 2>nul

rem analyze_motec_setup.cpp defines the real TIniFile-based save/loadSymptomWeights,
rem which pulls in RTLE.LIB -> link rtl.lib + vcl.lib. cp32mt.lib provides __Mtx_destroy
rem (multithreaded C++ runtime, matches <Multithreaded>true</Multithreaded> in Project1.cbproj).
rem VehicleDb*.cpp + ReleaseInfo.cpp: LapReportGenerator now references Ams2VehicleDb.
bcc32c -I.. -o test_report_json.exe ^
    ..\analyze_motec_setup.cpp ..\MotecToCSV.cpp ..\RuleConfig.cpp ..\LapReportGenerator.cpp ^
    ..\VehicleDb.cpp ..\ReleaseInfo.cpp ^
    ..\VehicleDbData_A_F.cpp ..\VehicleDbData_F_M.cpp ..\VehicleDbData_M_S.cpp ..\VehicleDbData_S_V.cpp ^
    test_report_json.cpp rtl.lib vcl.lib cp32mt.lib

if errorlevel 1 (
    echo BUILD FAILED
    exit /b 1
)
echo BUILD OK: %cd%\test_report_json.exe
endlocal
