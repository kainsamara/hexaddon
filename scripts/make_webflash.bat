@echo off
chcp 65001 >nul
setlocal EnableDelayedExpansion
rem ================================================================
rem  HEXADDON - сборка файлов прошивки
rem  Результат: dist\hexaddon-<версия>\
rem ================================================================
cd /d "%~dp0.."

rem --- выбираем pio: сначала из PATH, потом penv PlatformIO ---
set "PIO=pio"
where pio >nul 2>&1
if errorlevel 1 (
  set "PIO=%USERPROFILE%\.platformio\penv\Scripts\pio.exe"
  if not exist "!PIO!" (
    echo [FAIL] Не найден pio. Запусти скрипт из терминала VSCode
    echo        либо установи PlatformIO Core.
    goto :fail
  )
)

echo [1/2] Сборка прошивки...
"!PIO!" run
if errorlevel 1 goto :fail

echo [2/2] Формирование пакета...
rem --- версия из FW_VERSION ---
set "V=unknown"
for /f "tokens=3" %%A in ('findstr /c:"#define FW_VERSION" src\settings.h') do set "V=%%A"
set "V=!V:"=!"

set "B=.pio\build\esp32c3"
set "OUT=dist\hexaddon-!V!"

if exist "!OUT!" rmdir /s /q "!OUT!"
mkdir "!OUT!"
if errorlevel 1 goto :fail

copy /y "!B!\bootloader.bin" "!OUT!" >nul || goto :fail
copy /y "!B!\partitions.bin" "!OUT!" >nul || goto :fail
copy /y "!B!\firmware.bin"   "!OUT!" >nul || goto :fail

rem --- manifest.json для ESP Web Tools (адреса десятичные) ---
(
echo {
echo   "schemaVersion": 1,
echo   "name": "HEXADDON",
echo   "version": "!V!",
echo   "new_install_prompt_erase": false,
echo   "builds": [
echo     {
echo       "chipFamily": "ESP32-C3",
echo       "parts": [
echo         { "path": "bootloader.bin", "offset": 0 },
echo         { "path": "partitions.bin", "offset": 32768 },
echo         { "path": "firmware.bin", "offset": 65536 }
echo       ]
echo     }
echo   ]
echo }
) > "!OUT!\manifest.json"

echo.
echo Готово: !OUT!
dir /b "!OUT!"
pause
exit /b 0

:fail
echo.
echo [FAIL] Ошибка сборки или копирования.
echo.
pause
exit /b 1
