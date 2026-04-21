@echo off
setlocal

set "NSISDIR=C:\dev\Progs\NSIS"
if not exist "%NSISDIR%\Contrib\UIs\modern.exe" (
    if exist "%ProgramFiles(x86)%\NSIS\Contrib\UIs\modern.exe" (
        set "NSISDIR=%ProgramFiles(x86)%\NSIS"
    )
)

if not exist "%NSISDIR%\makensis.exe" (
    echo Could not find makensis.exe under "%NSISDIR%".
    goto error
)

set "PATH=%PATH%;%WSDK81%\bin\x86;%NSISDIR%;C:\Program Files (x86)\Windows Kits\10\bin\10.0.19041.0\x86"

rem sign using SHA-256
signtool sign /v /sha1 86E1D426731E79117452F090188A828426B29B5F /ac GlobalSign_SHA256_EV_CodeSigning_CA.cer /fd sha256 /tr http://timestamp.digicert.com /td SHA256 "Bin\Win32\Release\HashCheck.dll" "Bin\x64\Release\HashCheck.dll"
if errorlevel 1 goto error

"%NSISDIR%\makensis.exe" installer\HashCheck.nsi
if errorlevel 1 goto error

signtool sign /v /sha1 86E1D426731E79117452F090188A828426B29B5F /ac GlobalSign_SHA256_EV_CodeSigning_CA.cer /fd sha256 /tr http://timestamp.digicert.com /td SHA256  "installer\HashCheckSetup-v2.6.0.0.exe"
if errorlevel 1 goto error

pause
exit /b 0

:error
set "ERR=%ERRORLEVEL%"
if "%ERR%"=="0" set "ERR=1"
echo.
echo Signing build failed with error %ERR%.
pause
exit /b %ERR%
