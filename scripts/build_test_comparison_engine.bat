@echo off
rem =============================================================================
rem  build_test_comparison_engine.bat - Phase 4 standalone harness for the
rem  diagnostic engine.
rem  Pure ASCII on purpose (PowerShell/cmd ANSI quirks). Run from anywhere:
rem      scripts\build_test_comparison_engine.bat [Clean]
rem =============================================================================

setlocal
cd /d "%~dp0"

call "C:\Program Files (x86)\Embarcadero\Studio\23.0\bin\rsvars.bat" >nul 2>&1

del /q test_comparison_engine.exe 2>nul   rem always relink fresh (avoids stale incremental-link state)
if "%~1"=="Clean" del /q *.o 2>nul

rem analyze_motec_setup.cpp defines the real TIniFile-based save/loadSymptomWeights,
rem which pulls in RTLE.LIB -> link rtl.lib + vcl.lib. cp32mt.lib provides __Mtx_destroy
rem (multithreaded C++ runtime, matches <Multithreaded>true</Multithreaded> in Project1.cbproj).
rem VehicleDbData_*.cpp = generated C++ data tables (see scripts/json_to_vehicle_db.py).
bcc32c -I.. -o test_comparison_engine.exe ^
    ..\analyze_motec_setup.cpp ..\MotecToCSV.cpp ..\RuleConfig.cpp ..\LapReportGenerator.cpp ..\VehicleDb.cpp ..\ReleaseInfo.cpp ^
    ..\VehicleDbData_A_F.cpp ..\VehicleDbData_F_M.cpp ..\VehicleDbData_M_S.cpp ..\VehicleDbData_S_V.cpp ^
    test_comparison_engine.cpp rtl.lib vcl.lib cp32mt.lib winhttp.lib

if errorlevel 1 (
    echo BUILD FAILED
    exit /b 1
)
echo BUILD OK: %cd%\test_comparison_engine.exe
endlocal
