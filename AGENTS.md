# AGENTS.md — Project Conventions

## Repo overview

ESP32 RC car: Android app (Kotlin) + planned ESP32 firmware (ESP-IDF v5.x). The Android app  
is implemented; the firmware is the next step. A Python test server simulates the ESP32 for  
app development.

## Directory layout

```
esp32-rc-car/
├── plan.md                       # Project roadmap — keep in sync with reality
├── test_server.py                # Python HTTP server → simulates ESP32 endpoints
├── requirements.txt              # Pillow >=11.0.0 (for test_server.py frame generation)
├── .envrc                        # direnv → source to activate .venv/
├── .gitignore                    # .venv/, __pycache__/, *.pyc
├── android/                      # Android app (NOT android-app/)
│   ├── settings.gradle.kts       # rootProject.name = "ESP32 RC Car"
│   ├── build.gradle.kts          # just declares AGP plugin (apply false)
│   ├── gradle.properties
│   ├── gradlew / gradlew.bat
│   └── app/
│       ├── build.gradle.kts
│       └── src/main/
│           ├── AndroidManifest.xml
│           └── java/com/esp32rc/  # package: com.esp32rc
└── esp32-firmware/               # TODO — not created yet
```

## How to run

### Test server (Python)
```bash
# first time: python -m venv .venv && .venv/bin/pip install -r requirements.txt
# then:
source .envrc                      # direnv auto-activates .venv
python test_server.py              # binds 0.0.0.0:8080
# custom port:
python test_server.py --port 9000
```

The server generates synthetic 176×144 JPEG frames at 15 FPS with motor bar visualizations.
Connect the Android app to `<your-lan-ip>:8080`.

### Android app
```bash
cd android
./gradlew assembleDebug            # build
./gradlew installDebug             # install to device/emulator
```

No lint or typecheck scripts configured yet. AGP 9.2.1 + Gradle wrapper 9.4.1.

## Key conventions

### Package: `com.esp32rc`
All Kotlin files live under `android/app/src/main/java/com/esp32rc/`. No sub-packages for  
platform-level classes; `ui/`, `network/`, `model/` for layers.

### Gradle version catalog
Dependencies *must* be declared in `gradle/libs.versions.toml`, not inline strings.  
The catalog defines versions, libraries, and plugins. When adding a dependency:
1. Add version to `[versions]`
2. Add library entry to `[libraries]`
3. Reference as `libs.<alias>` in `build.gradle.kts`

### Android activities
- `ConnectActivity` is the launcher (declared in manifest with MAIN/LAUNCHER). There is no  
  `MainActivity`.
- `ControlActivity` is landscape-locked, fullscreen immersive. It receives `EXTRA_IP` and  
  `EXTRA_PORT` via Intent. Use `companion object` for extras keys and constants.

### Network layer separation
- `MjpegStreamer` → raw `HttpURLConnection` (not OkHttp). Parses multipart boundaries manually.
  Runs on a named `Thread`. Call `stopStream()` to shut down.
- `MotorClient` → OkHttp, fire-and-forget `GET` requests. Sends `s=<seq>` (AtomicLong)  
  for dedup on the server side. Call `shutdown()` on lifecycle end.

### Joystick ↔ motor mapping
`JoystickView` outputs **raw normalized coords** `(x: Float, y: Float)` where each is -1..1.  
Tank mixing to L/R motor values happens in `MotorCommand.fromJoystick(x, y)`:
```
forward = -y * 255
turn    =  x * 255
left    = clamp(forward + turn, -255, 255)
right   = clamp(forward - turn, -255, 255)
```

### ESP32 firmware conventions (planned)
When implementing firmware:
- ESP-IDF v5.x project at `esp32-firmware/`
- Module-per-concern: `wifi_ap.c/.h`, `ov7670.c/.h`, `motor_control.c/.h`, etc.
- HTTP server uses built-in `esp_http_server` (no external HTTP lib)
- All endpoints support `s=<seq>` query param for command dedup
- FreeRTOS tasks: `camera_task` (core 1, priority 5), `motor_watchdog_task` (core 0, priority 1)

## When making changes
1. Check `plan.md` — it describes both what's built and what's next. Update it if you change  
   the architecture or add/remove components.
2. The Android app builds with `./gradlew assembleDebug` from the `android/` directory.
3. Test against `test_server.py` if real hardware isn't available.
4. No CI/CD pipeline yet — manual testing only.
