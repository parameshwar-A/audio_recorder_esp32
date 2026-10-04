# ESP32 INMP441 Touch Audio WAV Recorder (`inmp441_audio_recorder`)

An ESP32-based 1-second audio dataset recording tool designed for capturing wake word and speech samples. It pairs the **INMP441 I2S digital microphone** with an ESP32 capacitive touch trigger (or the on-board BOOT button) to capture high-fidelity 16 kHz 16-bit mono PCM audio and transmit it losslessly over USB Serial to a host PC companion script that saves standard `.wav` files.

---

## Features

- **High-Quality Audio Capture**: 16 kHz sample rate, 16-bit signed PCM mono from an INMP441 microphone over I2S DMA.
- **Multiple Trigger Modes**:
  - **Capacitive Touch**: Touch wire connected to **GPIO 4** (Touch Channel 0) or **GPIO 32** (Touch Channel 9).
  - **On-Board BOOT Button**: Press **GPIO 0** as a zero-wire fallback trigger.
- **Visual Recording Feedback**:
  - Indicator LED (GPIO 16 & onboard GPIO 2) turns **Solid ON** for exactly 1.0 second during recording.
  - Gives **3 quick blinks** when audio has been successfully transmitted and saved.
- **Reliable Binary Protocol**: Framed USB serial transmission with magic headers (`0x55AA55AA`), packet payload checksum verification, and end markers (`0xAA55AA55`) to prevent corrupted audio files.
- **PC Companion Script (`record_wav.py`)**:
  - Auto-detects ESP32 serial port.
  - Automatically numbers output files (`sample_0001.wav`, `sample_0002.wav`, etc.).
  - Calculates and logs audio statistics (RMS volume and peak amplitude) in real-time.
  - Supports custom output directories and prefixes for direct training dataset collection.

---

## Hardware Pinout

| Hardware | ESP32 Pin | Function / Description |
| :--- | :--- | :--- |
| **INMP441 SCK / BCLK** | `GPIO 26` | I2S Bit Clock |
| **INMP441 WS / LRCK**  | `GPIO 25` | I2S Word Select (Left/Right Clock) |
| **INMP441 SD / DOUT**  | `GPIO 33` | I2S Serial Data Out |
| **INMP441 L/R**        | `GND`     | Channel Select (GND = Left Channel) |
| **INMP441 VDD**        | `3V3`     | 3.3V Power (*Do not connect to 5V*) |
| **INMP441 GND**        | `GND`     | Ground |
| **Primary Touch Wire** | `GPIO 4`  | Capacitive Touch Sensor (Touch CH 0) |
| **Secondary Touch**    | `GPIO 32` | Capacitive Touch Sensor (Touch CH 9) |
| **BOOT Button**        | `GPIO 0`  | On-board push button trigger (Active LOW fallback) |
| **Indicator LED**      | `GPIO 16` | External LED (Solid ON while recording, 3 blinks when saved) |
| **Built-in LED**       | `GPIO 2`  | Mirrored status indicator on ESP32 board |

---

## Project Structure

```text
inmp441_audio_recorder/
├── CMakeLists.txt              # Root CMake configuration
├── sdkconfig.defaults          # Project default overrides (target esp32, 2MB flash, 160MHz CPU)
├── .gitignore                  # Git exclusions (build artifacts, *.wav files, sdkconfig, etc.)
├── README.md                   # Project documentation
├── record_wav.py               # PC companion Python script to capture and save WAV files
├── recordings/                 # Output folder for recorded WAV files (kept via .gitkeep)
└── main/
    ├── CMakeLists.txt          # Component build configuration
    └── inmp441_audio_recorder.c# ESP32 firmware source (I2S capture, touch/button triggers, packet transmission)
```

---

## Step-by-Step Workflow: Setup, Build, Flash & Record

Follow these steps in sequential order. Notice the environment tags specifying where each action takes place:
- `[HOST PC]` — Run in your regular PC terminal (Python environment).
- `[ESP-IDF]` — Run in a terminal with the ESP-IDF toolchain activated.
- `[ESP32 HARDWARE]` — Physical interaction with the ESP32 board and microphone.

---

### Prerequisites

| Environment | Requirement | Setup Command |
| :--- | :--- | :--- |
| `[ESP-IDF]` | **ESP-IDF v5.x** toolchain | `. $HOME/esp/export.sh` |
| `[HOST PC]` | **Python 3** with `pyserial` | `pip install pyserial` |

---

### Step 1: Build the Firmware `[ESP-IDF]`

Run this in an ESP-IDF enabled terminal on your computer to compile the C firmware:

```bash
# 1. Source ESP-IDF tools (sets up IDF environment variables and compilers)
. $HOME/esp/export.sh

# 2. Set chip target (only needed once or after a clean build)
idf.py set-target esp32

# 3. Build the binary
idf.py build
```

---

### Step 2: Flash Firmware to ESP32 `[ESP-IDF]`

Connect your ESP32 board to your PC using a USB data cable and flash the binary:

```bash
idf.py -p /dev/ttyUSB0 flash
```
> [!NOTE]
> Replace `/dev/ttyUSB0` with your serial device port (e.g. `/dev/ttyACM0` on Linux, or `COM3` on Windows).
> **Do NOT run `idf.py monitor` here** because the companion Python script in Step 3 needs exclusive access to this serial port!

---

### Step 3: Launch Companion Recording Script `[HOST PC]`

Run this in your **regular PC terminal** (does not need ESP-IDF activated, just Python):

```bash
python3 record_wav.py
```

The script connects to the ESP32 over serial and waits for audio packets.

#### Script Options & Arguments:

| Option | Flag | Description | Default |
| :--- | :--- | :--- | :--- |
| `--port` | `-p` | Serial port of ESP32 (e.g. `/dev/ttyUSB0`) | Auto-detected |
| `--baud` | `-b` | Serial baud rate | `115200` |
| `--dir` | `-d` | Folder where `.wav` files are saved | `./recordings` |
| `--prefix` | | Filename prefix | `sample` |
| `--play` | | Automatically play back recorded audio on PC speakers | `False` |

#### Examples:

- **Collect Wake Word Training Samples**:
  ```bash
  python3 record_wav.py --dir ../esp32_wakeword_trainer/data/wake_word --prefix solomon
  ```
- **Collect Ambient Noise Samples**:
  ```bash
  python3 record_wav.py --dir ../esp32_wakeword_trainer/data/background --prefix noise
  ```

---

### Step 4: Trigger and Record `[ESP32 HARDWARE]`

Once `record_wav.py` in Step 3 displays that it is ready:

1. **Trigger**: Touch the wire connected to **GPIO 4** (or press the on-board **BOOT** button on the ESP32).
2. **Observe LED**: The external LED (GPIO 16) turns **Solid ON** for exactly 1.0 second.
3. **Speak**: Speak your wake word or phrase clearly into the INMP441 microphone while the LED is ON.
4. **Completion**: When the 1.0s completes, the LED gives **3 quick blinks**, confirming the audio packet was transmitted over USB.
5. **PC Verification**: The Python script verifies the checksum and saves the `.wav` file:
   ```text
   Receiving audio packet: 16000 Hz, 1 ch, 16000 samples (32000 bytes)...
   [1] Saved: sample_0001.wav (32000 bytes, 1.0s)
       Volume RMS: 1250 | Peak: 8430 | Path: recordings/sample_0001.wav
   Ready for next touch!
   ```
