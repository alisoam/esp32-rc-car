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
┌──────────────────────────────┐     WiFi (AP)     ┌──────────────────────────────┐
│  Android App (Kotlin)        │ ◄───────────────► │  ESP32 (ESP-IDF v5.x)        │
│                              │                    │                              │
│  ┌────────────────────────┐  │  HTTP /stream      │  ┌────────────────────────┐  │
│  │ MJPEG Stream Viewer    │◄─┼────────────────────┼──│ HTTP Server            │  │
│  │ (fullscreen ImageView) │  │  multipart/x-mixed │  │ /stream /control /status│  │
│  ├────────────────────────┤  │                    │  └───────────┬────────────┘  │
│  │ Dual-Axis Joystick     │──┼──HTTP /control────►│              │               │
│  │ (custom Canvas view)   │  │  ?l=±255&r=±255   │  ┌───────────▼────────────┐  │
│  ├────────────────────────┤  │                    │  │ Frame Buffer (mutex)   │  │
│  │ Connection Screen      │  │                    │  │ JPEG Encoder           │  │
│  │ (IP entry + connect)   │  │                    │  └───────────▲────────────┘  │
│  └────────────────────────┘  │                    │              │               │
│                              │                    │  ┌───────────┴────────────┐  │
│  Dependencies:               │                    │  │ I2S Parallel DMA       │  │
│  - OkHttp                    │                    │  │ (PCLK-gated capture)   │  │
│  - Custom JoystickView       │                    │  └───────────▲────────────┘  │
└──────────────────────────────┘                    │              │               │
                                                    │  ┌───────────┴────────────┐  │
                                                    │  │ OV7670 Camera (raw)    │  │
                                                    │  │ XCLK←LEDC, SCCB←I2C   │  │
                                                    │  │ D0-D7→I2S parallel in  │  │
                                                    │  │ HREF/VSYNC→GPIO ISRs   │  │
                                                    │  └────────────────────────┘  │
                                                    │  ┌────────────────────────┐  │
                                                    │  │ MX1508 Motor Driver    │  │
                                                    │  │ 4×LEDC PWM channels    │  │
                                                    │  │ Watchdog stop @ 500ms  │  │
                                                    │  └────────────────────────┘  │
                                                    └──────────────────────────────┘
```

## Project Structure

```
esp32-rc-car/
├── plan.md
├── esp32-firmware/                  # ESP-IDF v5.x project
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
└── android-app/                     # Native Android (Kotlin)
    ├── build.gradle.kts
    ├── settings.gradle.kts
    ├── gradle.properties
    └── app/
        ├── build.gradle.kts
        └── src/main/
            ├── AndroidManifest.xml
            ├── java/com/esp32rc/
            │   ├── MainActivity.kt
            │   ├── ConnectActivity.kt
            │   ├── ControlActivity.kt
            │   ├── ui/
            │   │   └── JoystickView.kt
            │   ├── network/
            │   │   ├── MjpegStreamer.kt
            │   │   └── MotorClient.kt
            │   └── model/
            │       └── MotorCommand.kt
            └── res/
                ├── layout/
                │   ├── activity_connect.xml
                │   └── activity_control.xml
                └── values/
                    ├── strings.xml
                    └── colors.xml
```

---

## 1. ESP32 Firmware (ESP-IDF v5.x)

### 1.1 GPIO Pin Map

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

### 1.2 Firmware Modules

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
- **`/control?l=<N>&r=<N>`** — GET handler:
  - Parse `l` and `r` params (int, -255 to 255)
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

### 1.3 Build & Flash
```bash
cd esp32-firmware
idf.py set-target esp32
idf.py menuconfig   # verify flash size, CPU freq 240 MHz
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

### 1.4 Performance Targets (QCIF 176×144)

| Metric              | Target     |
|--------------------|------------|
| Frame capture rate | 15-20 FPS  |
| JPEG encode time   | 30-50 ms   |
| Stream frame rate  | 8-15 FPS   |
| Motor response     | <100ms     |
| Control latency    | <50ms      |

---

## 2. Android App (Kotlin, min SDK 26)

### 2.1 Dependencies

```kotlin
// app/build.gradle.kts
dependencies {
    implementation("com.squareup.okhttp3:okhttp:4.12.0")
    implementation("androidx.core:core-ktx:1.12.0")
    implementation("androidx.appcompat:appcompat:1.6.1")
}
```

AndroidManifest.xml permissions:
- `INTERNET`
- `ACCESS_NETWORK_STATE`
- `ACCESS_WIFI_STATE`
- `KEEP_SCREEN_ON` (prevent screen sleep while driving)

### 2.2 Screens

#### `ConnectActivity` — Connection Screen
- `EditText` for IP address (default hint: `192.168.4.1`)
- `EditText` for port (default hint: `80`)
- "Connect" button → starts `ControlActivity` with IP + port as Intent extras
- If connection fails, show Snackbar with error

#### `ControlActivity` — Main Driving Screen
- `FrameLayout` root layout:
  - **Layer 1**: `ImageView` (`match_parent`, scaleType `fitCenter`) — MJPEG video feed
  - **Layer 2**: `JoystickView` (bottom center, 200dp, semi-transparent black)
- On create: start `MjpegStreamer` thread with IP:port, start sending zero-commands

### 2.3 UI Components

#### `JoystickView` — Custom View
- Extends `View`, renders with `Canvas`
- Draw outer circle (base, 200dp diameter) + inner thumb circle (60dp)
- Touch handling:
  - `ACTION_DOWN` / `ACTION_MOVE`: compute normalized `(x, y)` from center, clamp to outer circle radius, map to `(-255..255, -255..255)`
  - `ACTION_UP`: snap thumb to center, send `(0, 0)`
- Calls `onJoystickChanged(left: Int, right: Int)` callback with tank-steer mixing:
  ```
  left  = y + x    // forward + turn
  right = y - x    // forward - turn
  ```
  Clamp each to ±255
- Throttle sends commands at ~100ms intervals (Handler + Runnable)

### 2.4 Network Layer

#### `MjpegStreamer` — MJPEG Decoder (runs on background thread)

```kotlin
class MjpegStreamer(
    private val url: String,
    private val onFrame: (Bitmap) -> Unit
) : Thread() {
    // 1. Open HttpURLConnection to url
    // 2. Get InputStream
    // 3. Parse multipart boundary from Content-Type header
    // 4. Loop:
    //    a. Read until boundary string
    //    b. Read headers, extract Content-Length
    //    c. Read Content-Length bytes into ByteArray
    //    d. BitmapFactory.decodeByteArray(bytes, 0, length)
    //    e. Post bitmap to main thread via Handler(Looper.getMainLooper())
    //       call onFrame(bitmap) → set ImageView bitmap
}
```

#### `MotorClient` — Motor Command Sender

```kotlin
class MotorClient(private val baseUrl: String) {
    private val client = OkHttpClient()
    
    fun sendCommand(left: Int, right: Int) {
        val url = "$baseUrl/control?l=$left&r=$right"
        val request = Request.Builder().url(url).build()
        client.newCall(request).enqueue(object : Callback {
            override fun onFailure(call: Call, e: IOException) { /* ignore */ }
            override fun onResponse(call: Call, response: Response) { response.close() }
        })
    }
}
```

- No retry necessary — next tick replaces stale command
- Fire-and-forget pattern, no UI blocking

### 2.5 Activity Lifecycle

| State          | Action                                     |
|---------------|--------------------------------------------|
| onResume       | Connect MJPEG stream, start joystick timer |
| onPause        | Send zero command, disconnect stream       |
| onStop         | Stop all threads                           |
| onDestroy      | Cleanup                                    |

### 2.6 Screen Layouts

#### `activity_connect.xml`
```
LinearLayout (vertical, center)
├── ImageView (app icon, 80dp)
├── TextView ("ESP32 RC Car")
├── CardView
│   └── LinearLayout (vertical)
│       ├── TextInputLayout → EditText (IP)
│       ├── TextInputLayout → EditText (Port)
│       └── Button ("Connect")
└── TextView (status, connection errors)
```

#### `activity_control.xml`
```
FrameLayout (match_parent)
├── ImageView (match_parent, scaleType=fitCenter)
└── com.esp32rc.ui.JoystickView
    (layout_gravity=bottom|center_horizontal,
     width=200dp, height=200dp,
     marginBottom=32dp)
```

---

## 3. Integration Testing Checklist

- [ ] ESP32 boots, creates `esp32-rc-car` WiFi network
- [ ] OV7670 initializes, I2C probe returns ACK
- [ ] I2S DMA captures raw frames without buffer overrun
- [ ] JPEG encoding produces valid images (verify via `/stream` in browser)
- [ ] `/control?l=100&r=100` drives both motors forward
- [ ] `/control?l=0&r=0` stops motors
- [ ] Watchdog stops motors when no commands sent for 500ms
- [ ] Android app connects to `192.168.4.1:80`
- [ ] MJPEG stream displays in ImageView
- [ ] Joystick drag sends correct `l`/`r` values
- [ ] Joystick release sends `l=0&r=0`
- [ ] Screen stays on while driving
- [ ] App survives rotation without crashing

---

## 4. Implementation Order

1. **ESP32: WiFi AP** — get network up, verify connection from phone
2. **ESP32: Motor control** — `motor_control.c`, test with `/control` from browser
3. **ESP32: HTTP server skeleton** — `/status` endpoint first, then `/control`, then `/stream`
4. **ESP32: OV7670 driver** — I2C init, I2S DMA, frame capture, verify with logic analyzer or serial debug
5. **ESP32: JPEG encoder** — integrate, test `/stream` in desktop browser
6. **Android: ConnectActivity** — UI, navigation to ControlActivity
7. **Android: MjpegStreamer** — connect to ESP32 `/stream`, show video
8. **Android: JoystickView** — rendering + touch + tank mix
9. **Android: MotorClient** — wire joystick output to HTTP commands
10. **End-to-end test** — drive the car via WiFi with video feed
