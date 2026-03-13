# zclaw on ESP32

This branch is now dedicated to the `zclaw` ESP-IDF firmware and its ESP32 hardware integration.

## What zclaw is

[`tnm/zclaw`](https://github.com/tnm/zclaw) is a small ESP-IDF firmware project for ESP32 boards. Upstream README states it supports `ESP32`, `ESP32-C3`, `ESP32-S3`, and `ESP32-C6`, with the default `sdkconfig.defaults` targeting `esp32c3`.

For a classic ESP32 dev board, the important step is switching the target before build/flash:

```powershell
idf.py set-target esp32
```

## What this branch contains

This branch keeps the current standalone device flow:

- AP setup portal on first boot or recovery
- local mobile web chat page
- `POST + SSE` response streaming
- DHT11 sensor tool support on `GPIO5`
- 0.91 inch I2C OLED status display on `GPIO21` and `GPIO22`

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

## Local environment

The helper scripts were updated for a Windows ESP-IDF install under `C:\Espressif`.

Expected environment on this machine:

- `IDF_PATH=C:\Espressif\frameworks\esp-idf-v5.5.3`
- `IDF_TOOLS_PATH=C:\Espressif`
- `IDF_PYTHON_ENV_PATH=C:\Espressif\python_env\idf5.5_py3.11_env`

## Typical bring-up flow

From a fresh PowerShell window:

```powershell
.\scripts\zclaw-sync.ps1
.\scripts\zclaw-build.ps1 -Target esp32
.\scripts\zclaw-flash.ps1 -Target esp32 -Port COM5
```

After flashing:

1. Power the board.
2. Join the setup AP if the device is unconfigured or in recovery.
3. Open `http://192.168.4.1`.
4. Save Wi-Fi and AI settings from a phone.
5. Open the chat page from the device DHCP IP.
