@echo off
REM ==========================================
REM Reaction Timer - ESP32-S3 Build Helper
REM ==========================================

set IDF_PATH=C:\esp-idf\v5.5.4\esp-idf
set PYTHON_ENV=C:\Users\hanie\.espressif\python_env\idf5.5_py3.11_env\Scripts\python.exe
set TOOLS_PATH=C:\Users\hanie\.espressif\tools
set PATH=%TOOLS_PATH%\cmake\3.30.2\bin;%TOOLS_PATH%\ninja\1.12.1;%TOOLS_PATH%\xtensa-esp-elf\esp-14.2.0_20260121\xtensa-esp-elf\bin;%PATH%

cd /d "C:\Users\hanie\Desktop\Haniel_Garcia_Micro_2026_C2\reaction_timer"

if "%1"=="" (
    echo.
    echo Uso: build_flash.bat [build|flash|monitor|flash_monitor|clean|menuconfig]
    echo.
    echo Comandos:
    echo   build           - Compilar el proyecto
    echo   flash           - Flashear al ESP32-S3
    echo   monitor         - Abrir monitor serie
    echo   flash_monitor   - Flashear y abrir monitor
    echo   clean           - Limpiar build
    echo   menuconfig      - Configurar proyecto (sdkconfig)
    echo.
    pause
    goto :eof
)

if "%1"=="build" (
    echo [BUILD] Compilando proyecto...
    %PYTHON_ENV% %IDF_PATH%\tools\idf.py build
    goto :eof
)

if "%1"=="flash" (
    echo [FLASH] Flasheando ESP32-S3...
    %PYTHON_ENV% %IDF_PATH%\tools\idf.py flash
    goto :eof
)

if "%1"=="monitor" (
    echo [MONITOR] Abriendo monitor serie...
    %PYTHON_ENV% %IDF_PATH%\tools\idf.py monitor
    goto :eof
)

if "%1"=="flash_monitor" (
    echo [FLASH+MONITOR] Flasheando y abriendo monitor...
    %PYTHON_ENV% %IDF_PATH%\tools\idf.py flash monitor
    goto :eof
)

if "%1"=="clean" (
    echo [CLEAN] Limpiando build...
    %PYTHON_ENV% %IDF_PATH%\tools\idf.py fullclean
    goto :eof
)

if "%1"=="menuconfig" (
    echo [MENUCONFIG] Abriendo configuración...
    %PYTHON_ENV% %IDF_PATH%\tools\idf.py menuconfig
    goto :eof
)

echo Comando desconocido: %1
echo Usa: build_flash.bat [build|flash|monitor|flash_monitor|clean|menuconfig]
pause