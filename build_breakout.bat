@echo off
setlocal
set ROOT=C:\Users\yu617\WorkBuddy\2026-11-04-15-37-09
set MSVC=C:\Program Files\Microsoft Visual Studio\18\Community\VC\Tools\MSVC\14.51.36231
set SDK=C:\Program Files (x86)\Windows Kits\10
set SDKV=10.0.26100.0
set EASYX=%ROOT%\_easyx
set INCLUDE=%MSVC%\include;%SDK%\Include\%SDKV%\ucrt;%SDK%\Include\%SDKV%\um;%SDK%\Include\%SDKV%\shared;%EASYX%\include
set LIB=%MSVC%\lib\x64;%SDK%\Lib\%SDKV%\ucrt\x64;%SDK%\Lib\%SDKV%\um\x64;%EASYX%\lib_vc2015\x64
set PATH=%MSVC%\bin\HostX64\x64;%PATH%
cd /d "%ROOT%"
if "%~1"=="" (
  cl /nologo /EHsc /W3 /utf-8 /DUNICODE /D_UNICODE physics_breakout.cpp /Fe:physics_breakout.exe user32.lib gdi32.lib shell32.lib
) else (
  cl /nologo /EHsc /W3 /utf-8 /DUNICODE /D_UNICODE %~1 /Fe:%~n1.exe user32.lib gdi32.lib shell32.lib
)
echo BUILD_EXIT=%ERRORLEVEL%
endlocal
