@echo off
REM ==========================================
REM TAREA 7 - Reaction Timer ESP32-S3
REM Build helper - ESP-IDF v5.5.4
REM ==========================================
set IDF_PATH=C:\esp-idf\v5.5.4\esp-idf
set PYTHON_ENV=C:\Users\hanie\.espressif\python_env\idf5.5_py3.11_env\Scripts\python.exe
set TOOLS_PATH=C:\Users\hanie\.espressif\tools
set PATH=%TOOLS_PATH%\cmake\3.30.2\bin;%TOOLS_PATH%\ninja\1.12.1;%TOOLS_PATH%\xtensa-esp-elf\esp-14.2.0_20260121\xtensa-esp-elf\bin;%PATH%

cd /d "C:\Users\hanie\Desktop\Haniel_Garcia_Micro_2026_C2\TAREA7"

if "%1"=="" (
    echo.
    echo Uso: build_flash.bat [build^|flash^|monitor^|flash_monitor^|clean^|menuconfig]
    echo.
    echo  build         - Compilar
    echo  flash         - Flashear
    echo  monitor       - Monitor serie
    echo  flash_monitor - Flashear + monitor
    echo  clean         - Limpiar
    echo  menuconfig    - sdkconfig
    echo.
    pause
    goto :eof
)
if "%1"=="build" (
    echo [BUILD] Compilando TAREA7...
    %PYTHON_ENV% %IDF_PATH%\tools\idf.py build
    goto :eof
)
if "%1"=="flash" (
    echo [FLASH] Flasheando...
    %PYTHON_ENV% %IDF_PATH%\tools\idf.py flash
    goto :eof
)
if "%1"=="monitor" (
    %PYTHON_ENV% %IDF_PATH%\tools\idf.py monitor
    goto :eof
)
if "%1"=="flash_monitor" (
    %PYTHON_ENV% %IDF_PATH%\tools\idf.py flash monitor
    goto :eof
)
if "%1"=="clean" (
    %PYTHON_ENV% %IDF_PATH%\tools\idf.py fullclean
    goto :eof
)
if "%1"=="menuconfig" (
    %PYTHON_ENV% %IDF_PATH%\tools\idf.py menuconfig
    goto :eof
)
echo Comando desconocido: %1
pause
