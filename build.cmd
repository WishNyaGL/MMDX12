@echo off
rem Usage: build.cmd [build-dir] [extra cmake --build args...]
rem   build.cmd                      -> builds everything into .\build
rem   build.cmd build-x --target mmdx_core
setlocal
rem %~dp0 must be read before "shift" (it shifts %0 too)
set ROOT=%~dp0
set BUILD_DIR=%~1
if "%BUILD_DIR%"=="" set BUILD_DIR=build
call "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul
set PATH=%USERPROFILE%\miniconda3\Scripts;%PATH%
rem Header dependencies: ninja recognises cl's localized "Note: including file:" prefix by the bytes CMake stored at
rem configure time. A directory configured from a UTF-8 console stored the Korean prefix in UTF-8 while cl writes it
rem in the OEM code page, so no header was ever recorded and header edits rebuilt nothing (stale objects, crashes).
rem Such a directory is reconfigured once.
set RULES=%~dp0%BUILD_DIR%\CMakeFiles\rules.ninja
if exist "%RULES%" (
  powershell -NoProfile -Command "$s = [Text.Encoding]::GetEncoding(28591).GetString([IO.File]::ReadAllBytes($env:RULES)); if ($s.Contains([string][char]0xEC + [char]0xB0 + [char]0xB8)) { exit 1 }" || (
    echo Reconfiguring %BUILD_DIR%: its header dependency prefix was stored in the wrong code page
    del /q "%~dp0%BUILD_DIR%\CMakeCache.txt" "%~dp0%BUILD_DIR%\build.ninja" 2>nul
    rmdir /s /q "%~dp0%BUILD_DIR%\CMakeFiles" 2>nul
  )
)
if not exist "%~dp0%BUILD_DIR%\build.ninja" (
  cmake -S "%~dp0." -B "%~dp0%BUILD_DIR%" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo || exit /b 1
)
shift
cmake --build "%ROOT%%BUILD_DIR%" %1 %2 %3 %4 %5 %6 %7 %8
