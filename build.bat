@echo off
rem Configure + build Release with the Visual Studio generator.
setlocal
where cmake >nul 2>nul
if errorlevel 1 (
  set "CMAKE=C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
) else (
  set "CMAKE=cmake"
)
"%CMAKE%" -S "%~dp0." -B "%~dp0build" -G "Visual Studio 16 2019" -A x64 || exit /b 1
"%CMAKE%" --build "%~dp0build" --config Release --parallel || exit /b 1
echo.
echo Built: %~dp0build\Release\LowLatencyKaraoke.exe
