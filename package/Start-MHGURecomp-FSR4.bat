@echo off
setlocal
cd /d "%~dp0"
set "FSR4_PLUGIN=%~dp0fsr4plugin"
if not exist "%FSR4_PLUGIN%\VK_LAYER_MHGU_fsr4.dll" (
    echo fsr4plugin\VK_LAYER_MHGU_fsr4.dll is missing: put the fsr4plugin folder beside this file.
    pause
    exit /b 1
)
if defined VK_LAYER_PATH (set "VK_LAYER_PATH=%FSR4_PLUGIN%;%VK_LAYER_PATH%") else set "VK_LAYER_PATH=%FSR4_PLUGIN%"
if defined VK_ADD_LAYER_PATH (set "VK_ADD_LAYER_PATH=%FSR4_PLUGIN%;%VK_ADD_LAYER_PATH%") else set "VK_ADD_LAYER_PATH=%FSR4_PLUGIN%"
if defined VK_INSTANCE_LAYERS (set "VK_INSTANCE_LAYERS=VK_LAYER_MHGU_fsr4;%VK_INSTANCE_LAYERS%") else set "VK_INSTANCE_LAYERS=VK_LAYER_MHGU_fsr4"
if defined VK_LOADER_LAYERS_ENABLE (set "VK_LOADER_LAYERS_ENABLE=VK_LAYER_MHGU_fsr4,%VK_LOADER_LAYERS_ENABLE%") else set "VK_LOADER_LAYERS_ENABLE=VK_LAYER_MHGU_fsr4"
set "TARGET=mhgurecomp-updater.exe"
for %%A in (%*) do (
    if /i "%%~A"=="direct" set "TARGET=mhgurecomp.exe"
    if /i "%%~A"=="log" set "MHGU_FSR4_LOG=1"
)
if not exist "%TARGET%" set "TARGET=mhgurecomp.exe"
start "" "%TARGET%"
