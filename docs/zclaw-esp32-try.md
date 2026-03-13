# zclaw on ESP32 Tryout

This branch isolates a `zclaw` experiment from the existing Arduino tank-control code.

## What zclaw is

[`tnm/zclaw`](https://github.com/tnm/zclaw) is a small ESP-IDF firmware project for ESP32 boards. Upstream README states it supports `ESP32`, `ESP32-C3`, `ESP32-S3`, and `ESP32-C6`, with the default `sdkconfig.defaults` targeting `esp32c3`.

For a classic ESP32 dev board, the important step is switching the target before build/flash:

```powershell
idf.py set-target esp32
```

## Why this branch exists

The current repository mainline is Arduino-based and oriented around `ESP32-S3`. `zclaw` is an ESP-IDF project with a separate toolchain and workflow, so the safest approach is to keep the trial isolated.

## Windows workflow

PowerShell helpers were added under [`scripts/zclaw-sync.ps1`](/C:/Users/ytq/work/ai/Heavy-punch/scripts/zclaw-sync.ps1), [`scripts/zclaw-build.ps1`](/C:/Users/ytq/work/ai/Heavy-punch/scripts/zclaw-build.ps1), and [`scripts/zclaw-flash.ps1`](/C:/Users/ytq/work/ai/Heavy-punch/scripts/zclaw-flash.ps1).

1. Install ESP-IDF on Windows so a full ESP-IDF + `python_env` exists in one of:
   - `C:\Espressif\frameworks\esp-idf-v5.5.3`
   - `C:\Espressif\v5.5.2\esp-idf`
   - `C:\Espressif\v5.4\esp-idf`
   - `%IDF_PATH%`
2. Sync upstream:

```powershell
.\scripts\zclaw-sync.ps1
```

3. Build for classic ESP32:

```powershell
.\scripts\zclaw-build.ps1 -Target esp32
```

4. Flash and open serial monitor:

```powershell
.\scripts\zclaw-flash.ps1 -Target esp32 -Port COM5
```

If you are actually targeting an `ESP32-S3`, switch `-Target esp32s3`.

## Current blocker on this machine

This branch originally assumed a `$HOME\esp\...` layout. On this machine ESP-IDF is installed under `C:\Espressif`, so the helper scripts were updated to detect that layout and use `idf_tools.py export` directly instead of relying on a stale `idf-env` default.

## Recommended next step

Finish the ESP-IDF install first, then run:

```powershell
.\scripts\zclaw-sync.ps1
.\scripts\zclaw-build.ps1 -Target esp32
.\scripts\zclaw-flash.ps1 -Target esp32 -Port COM5
```
