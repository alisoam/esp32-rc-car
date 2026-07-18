# ESP32 RC Car — Project Plan

## Hardware

| Component        | Part                                          |
|------------------|-----------------------------------------------|
| MCU              | ESP32 Dev Board                               |
| Camera           | OV7670 (raw parallel, no FIFO)                |
| Motor Driver     | MX1508 dual H-bridge                          |
| Motors           | 2× DC motors, differential drive (tank steer) |

## Architecture Overview

```
┌──────────────────────────────┐     WiFi (AP)     ┌────────────────────────────────┐
│  Android App (Kotlin)        │ ◄───────────────► │  ESP32 (ESP-IDF v5.x)          │
│                              │                   │                                │
│  ┌────────────────────────┐  │ HTTP /stream      │  ┌──────────────────────────┐  │
│  │ MJPEG Stream Viewer    │◄─┼───────────────────┼──│ HTTP Server              │  │
│  │ (fullscreen ImageView) │  │ multipart/x-mixed │  │ /stream /control /status │  │
│  ├────────────────────────┤  │                   │  └───────────┬──────────────┘  │
│  │ Dual-Axis Joystick     │──┼─HTTP /control────►│              │                 │
│  │ (custom Canvas view)   │  │ ?l=±255&r=±255    │  ┌───────────▼──────────────┐  │
│  ├────────────────────────┤  │ &s=<seq>          │  │ Frame Buffer (mutex)     │  │
│  │ Connection Screen      │  │                   │  │ JPEG Encoder             │  │
│  │ (IP entry + connect)   │  │                   │  └───────────▲──────────────┘  │
│  └────────────────────────┘  │                   │              │                 │
│                              │                   │  ┌───────────┴──────────────┐  │
│  Dependencies:               │                   │  │ I2S Parallel DMA         │  │
│  - OkHttp                    │                   │  │ (PCLK-gated capture)     │  │
│  - Material Components       │                   │  └───────────▲──────────────┘  │
│  - Custom JoystickView       │                   │              │                 │
└──────────────────────────────┘                   │  ┌───────────┴──────────────┐  │
                                                   │  │ OV7670 Camera (raw)      │  │
                                                   │  │ XCLK←LEDC, SCCB←I2C      │  │
                                                   │  │ D0-D7→I2S parallel in    │  │
                                                   │  │ HREF/VSYNC→GPIO ISRs     │  │
                                                   │  └──────────────────────────┘  │
                                                   │  ┌──────────────────────────┐  │
                                                   │  │ MX1508 Motor Driver      │  │
                                                   │  │ 4×LEDC PWM channels      │  │
                                                   │  │ Watchdog stop @ 500ms    │  │
                                                   │  └──────────────────────────┘  │
                                                   └────────────────────────────────┘
```

## Project Structure

```
esp32-rc-car/
├── plan.md
├── .envrc                           # direnv: activates .venv
├── requirements.txt                 # Pillow (for test_server.py)
├── test_server.py                   # Python HTTP server simulating ESP32 firmware
│
├── esp32/                          # ESP-IDF v5.x project (IN PROGRESS)
│   ├── CMakeLists.txt
│   ├── sdkconfig
│   └── main/
│       ├── CMakeLists.txt
│       ├── main.c                   # Entry point
│       ├── wifi_ap.c / .h           # SoftAP (SSID: esp32-rc-car, IP: 192.168.4.1)
│       ├── ov7670.c / .h            # SCCB init + register config
│       ├── ov7670_frame.c / .h      # I2S parallel DMA, HREF/VSYNC ISRs, frame assembly
│       ├── jpeg_encoder.c / .h      # RGB565 → JPEG conversion
│       ├── http_server.c / .h       # MJPEG stream + motor control endpoints
│       └── motor_control.c / .h     # LEDC PWM, tank mix, watchdog, ramping
│
└── android/                         # Native Android (Kotlin) — IMPLEMENTED
    ├── build.gradle.kts
    ├── settings.gradle.kts
    ├── gradle.properties
    ├── gradlew / gradlew.bat
    ├── gradle/
    │   ├── libs.versions.toml       # Gradle version catalog
    │   └── wrapper/
    └── app/
        ├── build.gradle.kts
        ├── proguard-rules.pro
        └── src/main/
            ├── AndroidManifest.xml
            ├── java/com/esp32rc/
            │   ├── ConnectActivity.kt      # LAUNCHER — IP/port entry, probes /status
            │   ├── ControlActivity.kt      # Fullscreen landscape, MJPEG + joystick
            │   ├── ui/
            │   │   └── JoystickView.kt     # Custom Canvas dual-axis joystick
            │   ├── network/
            │   │   ├── MjpegStreamer.kt    # HttpURLConnection-based MJPEG decoder
            │   │   └── MotorClient.kt      # OkHttp fire-and-forget with seq numbers
            │   └── model/
            │       └── MotorCommand.kt     # Data class + tank-mix math
            └── res/
                ├── layout/
                │   ├── activity_connect.xml
                │   └── activity_control.xml
                ├── drawable/
                │   └── ic_car.xml
                └── values/
                    ├── strings.xml
                    ├── colors.xml
                    └── themes.xml
```

---

## 0. Test Server (Python)

`test_server.py` is a Python HTTP server that simulates the ESP32 firmware — enables testing the Android app without hardware.

- **Run**: `./.venv/bin/python test_server.py` (binds `0.0.0.0:8080` by default)
- **Dependency**: `requirements.txt` → Pillow >=11.0.0 (generates synthetic 176×144 JPEG frames)
- **Virtualenv**: `.envrc` activates `.venv/` via direnv

### Endpoints

| Path | Description |
|------|-------------|
| `/status` | JSON: `{"fps", "clients", "uptime", "left", "right", "resolution"}` |
| `/control?l=<N>&r=<N>&s=<seq>` | Accepts motor speeds (-255..255). Sequence number `s` deduplicates stale commands. |
| `/stream` | MJPEG stream (`multipart/x-mixed-replace; boundary=FRAME`), 15 FPS synthetic frames with motor visualization |

### CLI
```bash
# start the test server
./.venv/bin/python test_server.py

# custom port
./.venv/bin/python test_server.py --port 9000
```

---

## 1. Android App (Kotlin, min SDK 26) — IMPLEMENTED

### 1.1 Build System

- **Gradle**: wrapper 9.4.1, AGP 9.2.1, Java 11
- **compileSdk**: 36, **minSdk**: 26, **targetSdk**: 36
- **Dependency management**: Gradle version catalog at `gradle/libs.versions.toml`

### Dependencies

| Library | Version | Used for |
|---------|---------|----------|
| `androidx.core:core-ktx` | 1.12.0 | Core Android extensions |
| `androidx.appcompat:appcompat` | 1.6.1 | AppCompat theme, dark status bar |
| `com.google.android.material:material` | 1.11.0 | MaterialButton, TextInputLayout, Snackbar, CardView |
| `com.squareup.okhttp3:okhttp` | 4.12.0 | MotorClient HTTP calls |

### Manifest

- Permissions: `INTERNET`, `ACCESS_NETWORK_STATE`, `ACCESS_WIFI_STATE`
- `android:usesCleartextTraffic="true"` (HTTP to ESP32/test server)
- `ConnectActivity` is MAIN/LAUNCHER
- `ControlActivity` is landscape-locked, fullscreen theme, handles config changes

### 1.2 Activities

#### `ConnectActivity` — Connection Screen
- Validates IP with `Patterns.IP_ADDRESS`, port must be 1–65535
- Persists last-used IP/port in `SharedPreferences`
- Probes `/status` via OkHttp before navigating — shows Snackbar on failure
- Passes IP + port as Intent extras to `ControlActivity`

#### `ControlActivity` — Main Driving Screen
- **Layout**: `FrameLayout` with `ImageView` (match_parent, fitCenter) + `JoystickView` (bottom-start, 180dp)
- **Orientation**: landscape, fullscreen immersive (transient bars by swipe)
- **Screen-on**: `android:keepScreenOn="true"` on root layout
- **Command loop**: Handler-based 100ms ticker sends keep-alive commands
  - 20ms minimum interval between unique commands
  - On pause: sends zero command, stops stream
  - On stop: shuts down MotorClient

### 1.3 UI Components

#### `JoystickView` — Custom Canvas View
- Renders outer circle (base) + inner thumb circle
- Touch handling: `ACTION_DOWN`/`ACTION_MOVE` clamp to base radius, `ACTION_UP` snaps to center
- Callback: `onJoystickChanged(x: Float, y: Float)` — normalized coords (-1..1)
- In-view debug text shows computed L/R values
- **Tank mixing lives in `MotorCommand.fromJoystick()`**, not in JoystickView:
  ```
  forward = -y * 255
  turn    =  x * 255
  left    = forward + turn   (clamped ±255)
  right   = forward - turn   (clamped ±255)
  ```

### 1.4 Network Layer

#### `MjpegStreamer` — Thread-based, uses `HttpURLConnection`
- Runs on a named thread (`"MjpegStreamer"`)
- Parses `boundary=` from Content-Type header
- Reads frame-by-frame via manual boundary-scanning (no OkHttp MultipartReader)
- Posts decoded `Bitmap` to main thread via `Handler`
- Auto-reconnects with 1s delay on error/disconnect
- `onError` callback for UI status
- Call `stopStream()` → sets `@Volatile running = false`

#### `MotorClient` — OkHttp, fire-and-forget
- Sends `GET /control?l=<L>&r=<R>&s=<seq>` with incrementing sequence numbers (`AtomicLong`)
- Sequence numbers allow the server (real ESP32 or test_server.py) to drop stale/delayed commands
- 1s connect/read timeouts
- `shutdown()` cleans up dispatcher executor + connection pool

### 1.5 Lifecycle

| State | Action |
|-------|--------|
| onResume | Create MotorClient, start MJPEG stream thread, start command ticker |
| onPause | Send zero command, stop stream, remove ticker callbacks |
| onStop | Call `motorClient.shutdown()`, null ref |

---

## 2. ESP32 Firmware (ESP-IDF v5.x) — TODO

### 2.1 GPIO Pin Map

| Signal        | GPIO | Peripheral      | Notes                           |
|--------------|------|-----------------|---------------------------------|
| OV7670 D0    | 2    | I2S1 DATA[0]    | 8-bit parallel data bus         |
| OV7670 D1    | 4    | I2S1 DATA[1]    |                                 |
| OV7670 D2    | 12   | I2S1 DATA[2]    |                                 |
| OV7670 D3    | 13   | I2S1 DATA[3]    |                                 |
| OV7670 D4    | 14   | I2S1 DATA[4]    |                                 |
| OV7670 D5    | 15   | I2S1 DATA[5]    |                                 |
| OV7670 D6    | 16   | I2S1 DATA[6]    |                                 |
| OV7670 D7    | 17   | I2S1 DATA[7]    | Avoid boot-strapping pins       |
| OV7670 XCLK  | 21   | LEDC ch 4       | 10-20 MHz master clock          |
| OV7670 PCLK  | 5    | I2S1 D_IN (WS)  | Pixel clock → I2S sample strobe |
| OV7670 HREF  | 18   | GPIO INT        | Rising edge → line start        |
| OV7670 VSYNC | 19   | GPIO INT        | Falling edge → frame start      |
| OV7670 SIOC  | 22   | I2C SCL         | SCCB clock                      |
| OV7670 SIOD  | 23   | I2C SDA         | SCCB data                       |
| MX1508 IN1   | 32   | LEDC ch 0       | Left motor forward              |
| MX1508 IN2   | 33   | LEDC ch 1       | Left motor reverse              |
| MX1508 IN3   | 25   | LEDC ch 2       | Right motor forward             |
| MX1508 IN4   | 26   | LEDC ch 3       | Right motor reverse             |

### 2.2 Firmware Modules

#### `main.c` — Entry Point
- Initialize NVS flash
- Initialize WiFi AP (`esp32-rc-car`, open, channel 1)
- Start the HTTP server
- Spawn FreeRTOS tasks:
  - `camera_task` (priority 5, core 1): camera init, I2S start, frame loop
  - `motor_watchdog_task` (priority 1, core 0): auto-stop after 500ms idle

#### `wifi_ap.c` — WiFi Access Point
- SSID: `esp32-rc-car`
- Authentication: open
- IP: `192.168.4.1` (static)
- Max connections: 4
- Uses `esp_wifi` and `esp_netif` APIs

#### `ov7670.c` — Camera Initialization
- I2C bus init at 100 kHz on GPIO 22/23
- Probe OV7670 at SCCB address 0x21 (write) / 0x42 (read)
- Write register set for QCIF (176×144) RGB565:
  - COM7: 0x04 (QCIF), 0x0C (RGB)
  - COM15: 0xD0 (RGB565 range)
  - CLKRC: internal PLL for pixel clock
  - Window/format registers as specified in OV7670 datasheet
- Start LEDC clock on GPIO 21 at 10 MHz (XCLK)

#### `ov7670_frame.c` — I2S Parallel DMA Frame Capture
- Configure I2S1 in parallel input mode:
  - `I2S_COMM_FORMAT_STAND_I2S`
  - 8-bit parallel, sample on rising PCLK
  - DMA buffer: 2× 4096-byte descriptors, linked list
- HREF GPIO ISR (rising edge → line active, falling edge → line done)
- VSYNC GPIO ISR (falling edge → frame complete, swap buffer)
- Double-buffered frame: capture fills buffer A while HTTP server reads buffer B
- Mutex `frame_mutex` protects buffer swap
- Output: raw RGB565 byte array, 176×144×2 = 50688 bytes per frame

#### `jpeg_encoder.c` — RGB565 → JPEG
- Takes raw RGB565 buffer, outputs JPEG byte array + length
- Uses `esp_jpeg` component (TJpgDec-based) or a lightweight software JPEG encoder
- Quality: ~70 (trade off size vs speed)
- Target: ~5-10 KB per frame
- Encoding time: ~30-50ms on ESP32 at 240 MHz for QCIF

#### `http_server.c` — HTTP Server
- Uses `esp_http_server` (built-in, no external dependency)
- **`/stream`** — GET handler:
  ```
  HTTP/1.1 200 OK
  Content-Type: multipart/x-mixed-replace; boundary=FRAME
  
  --FRAME
  Content-Type: image/jpeg
  Content-Length: <len>
  
  <JPEG bytes>
  ```
  - Loop: take mutex, copy latest JPEG frame, release mutex, send chunk via `httpd_resp_send_chunk()`
  - Re-check for new frame every 10ms
  - Send empty chunk as heartbeat if no new frame (keeps connection alive)
  - Support `?s=<seq>` query param — drop stale frames (seq ≤ lastSeq)
- **`/control?l=<N>&r=<N>&s=<seq>`** — GET handler:
  - Parse `l`, `r` (int, -255 to 255) and `s` (unsigned long, sequence number)
  - Drop if `s <= lastSeq` (prevent out-of-order execution)
  - Update global `motor_left_speed` and `motor_right_speed`
  - Reset watchdog timer
  - Return 200
- **`/status`** — GET handler:
  - Return JSON: `{"fps": N, "clients": N, "uptime": N, "left": N, "right": N, "resolution": "176x144"}`

#### `motor_control.c` — MX1508 Motor Driver
- 4× LEDC PWM channels, 5 kHz, 10-bit resolution (0–1023)
- `motor_set(left: int, right: int)`:
  - Deadband: ±20 → zero
  - Ramp limit: ±50 per call (smoothed acceleration)
  - Left motor: IN1=forward PWM, IN2=0 when speed>0; IN1=0, IN2=reverse PWM when speed<0
  - Right motor: IN3/IN4 same logic
- `motor_stop()`: all pins low
- Watchdog: `motor_watchdog_task` checks `last_command_ms`, calls `motor_stop()` if > 500ms

### 2.3 Build & Flash
```bash
cd esp32
idf.py set-target esp32
idf.py menuconfig   # verify flash size, CPU freq 240 MHz
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

### 2.4 Performance Targets (QCIF 176×144)

| Metric              | Target     |
|--------------------|------------|
| Frame capture rate | 15-20 FPS  |
| JPEG encode time   | 30-50 ms   |
| Stream frame rate  | 8-15 FPS   |
| Motor response     | <100ms     |
| Control latency    | <50ms      |

---

## 3. Integration Testing Checklist

- [ ] ESP32 boots, creates `esp32-rc-car` WiFi network
- [ ] OV7670 initializes, I2C probe returns ACK
- [ ] I2S DMA captures raw frames without buffer overrun
- [ ] JPEG encoding produces valid images (verify via `/stream` in browser)
- [ ] `/control?l=100&r=100&s=1` drives both motors forward
- [ ] `/control?l=0&r=0&s=2` stops motors
- [ ] Sequence number dedup works (stale commands dropped)
- [ ] Watchdog stops motors when no commands sent for 500ms
- [x] Android app connects to `192.168.4.1:80` (verified with test_server.py)
- [x] MJPEG stream displays in ImageView
- [x] Joystick drag sends correct `l`/`r` values with sequence numbers
- [x] Joystick release sends `l=0&r=0`
- [x] Screen stays on while driving
- [x] App survives rotation without crashing
- [x] ConnectActivity validates IP and probes `/status` before navigating

---

## 4. Implementation Order

1. **[done]** **Android: ConnectActivity** — IP/port entry, `/status` probe, navigation
2. **[done]** **Android: MjpegStreamer** — connect to `/stream`, show video
3. **[done]** **Android: JoystickView** — rendering + touch + tank mix in MotorCommand
4. **[done]** **Android: MotorClient** — sequence-numbered fire-and-forget commands
5. **[done]** **Python test server** — simulate ESP32 HTTP endpoints for app testing
6. **[done]** **ESP32: WiFi AP** — get network up, verify connection from phone
7. **ESP32: Motor control** — `motor_control.c`, test with `/control` from browser (NEXT)
8. **[done]** **ESP32: HTTP server skeleton** — `/status` + `/control` + `/stream` stubs (seq dedup live)
9. **ESP32: OV7670 driver** — I2C init, I2S DMA, frame capture
10. **ESP32: JPEG encoder** — integrate, test `/stream` in desktop browser
11. **End-to-end test** — drive the car via WiFi with video feed
