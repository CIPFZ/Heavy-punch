# ESP32-S3 LLM Quick Test (Arduino)

Sketch:

- `AIBoardLLMQuickTest/AIBoardLLMQuickTest.ino`

## Steps

1. Open the sketch in Arduino IDE.
2. Set board to `ESP32S3 Dev Module`.
3. Fill these constants in the sketch:
   - `WIFI_SSID`
   - `WIFI_PASS`
   - `LLM_URL`
   - `LLM_API_KEY`
   - `LLM_MODEL`
4. Upload.
5. Open Serial Monitor at `115200`.

## Expected output

- Wi-Fi connected IP
- HTTP status code (`200` expected)
- Raw JSON response
- Parsed assistant text (`Parsed assistant content`)

