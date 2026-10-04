#!/usr/bin/env python3
"""
INMP441 Audio WAV Recorder - PC Companion Script
Captures 1-second audio recordings transmitted over binary USB serial
from the ESP32 WROOM and saves them as standard 16kHz 16-bit Mono WAV files.

Usage:
    python record_wav.py
    python record_wav.py --dir ../esp32_wakeword_trainer/data/wake_word --prefix my_wakeword
    python record_wav.py --port /dev/ttyUSB0 --baud 115200
"""

import os
import sys
import time
import struct
import wave
import argparse
import glob
import math
import subprocess

try:
    import serial
    import serial.tools.list_ports
except ImportError:
    print("\n❌ Error: 'pyserial' package is required.")
    print("👉 Install it with: pip install pyserial\n")
    sys.exit(1)

PACKET_MAGIC_START = 0x55AA55AA
PACKET_MAGIC_END   = 0xAA55AA55
HEADER_FORMAT = "<IIIHHI"   # magic_start (4B), sample_rate (4B), sample_count (4B), channels (2B), bits_per_sample (2B), checksum (4B)
HEADER_SIZE = struct.calcsize(HEADER_FORMAT)

def find_serial_port():
    """Detects likely ESP32 serial port."""
    ports = list(serial.tools.list_ports.comports())
    for p in ports:
        desc = p.description.lower()
        if "cp210" in desc or "ch340" in desc or "ftdi" in desc or "usb" in desc or "uart" in desc:
            return p.device
    # Fallback Linux/Mac search
    candidates = glob.glob("/dev/ttyUSB*") + glob.glob("/dev/ttyACM*")
    if candidates:
        return candidates[0]
    return None

def get_next_filename(output_dir, prefix):
    """Finds the next available sample_NNN.wav number."""
    os.makedirs(output_dir, exist_ok=True)
    existing = [f for f in os.listdir(output_dir) if f.startswith(prefix) and f.endswith(".wav")]
    max_num = 0
    for f in existing:
        parts = f.replace(".wav", "").split("_")
        try:
            num = int(parts[-1])
            if num > max_num:
                max_num = num
        except ValueError:
            pass
    return os.path.join(output_dir, f"{prefix}_{max_num + 1:04d}.wav")

def calculate_audio_stats(raw_bytes):
    """Calculates RMS energy and peak amplitude from 16-bit signed PCM."""
    sample_count = len(raw_bytes) // 2
    if sample_count == 0:
        return 0, 0
    samples = struct.unpack(f"<{sample_count}h", raw_bytes)
    peak = max(abs(s) for s in samples)
    sum_sq = sum(s * s for s in samples)
    rms = math.sqrt(sum_sq / sample_count)
    return rms, peak

def main():
    parser = argparse.ArgumentParser(description="ESP32 INMP441 1-Second WAV Audio Recorder")
    parser.add_argument("--port", "-p", default=None, help="Serial port (e.g. /dev/ttyUSB0)")
    parser.add_argument("--baud", "-b", type=int, default=115200, help="Baud rate (default: 115200)")
    parser.add_argument("--dir", "-d", default="recordings", help="Output directory for WAV files (default: ./recordings)")
    parser.add_argument("--prefix", default="sample", help="WAV filename prefix (default: sample)")
    parser.add_argument("--play", action="store_true", help="Automatically play back recorded audio through speakers")
    args = parser.parse_args()

    port = args.port or find_serial_port()
    if not port:
        print("❌ No serial port detected! Please connect your ESP32 or specify --port /dev/ttyUSBx")
        sys.exit(1)

    print("\n" + "=" * 65)
    print("      🎙️  ESP32 INMP441 TOUCH AUDIO WAV RECORDER")
    print("=" * 65)
    print(f" Port      : {port}")
    print(f" Baud Rate : {args.baud}")
    print(f" Directory : {os.path.abspath(args.dir)}")
    print(f" Prefix    : {args.prefix}")
    print("=" * 65)
    print("👉 Touch GPIO 4 on your ESP32 (or press BOOT button) to record!")
    print("   The ESP32 LED turns SOLID ON during the 1.0-second recording.")
    print("   Speak your wakeword clearly into the mic while LED is ON.\n")

    try:
        ser = serial.Serial(port, args.baud, timeout=1.0)
        ser.reset_input_buffer()
    except Exception as e:
        print(f"❌ Failed to open serial port {port}: {e}")
        sys.exit(1)

    saved_count = 0
    sync_pattern = struct.pack("<I", PACKET_MAGIC_START)

    try:
        while True:
            # Search for sync header
            byte = ser.read(1)
            if not byte:
                continue

            # Check if start of magic
            if byte == sync_pattern[0:1]:
                rest = ser.read(3)
                if rest == sync_pattern[1:4]:
                    # Magic start matched! Read the rest of header
                    header_rest = ser.read(HEADER_SIZE - 4)
                    if len(header_rest) != (HEADER_SIZE - 4):
                        continue

                    magic, sample_rate, sample_count, channels, bits_per_sample, checksum = struct.unpack(
                        HEADER_FORMAT, sync_pattern + header_rest
                    )

                    total_payload_bytes = sample_count * (bits_per_sample // 8) * channels
                    print(f"📥 Receiving audio packet: {sample_rate} Hz, {channels} ch, {sample_count} samples ({total_payload_bytes} bytes)...")

                    # Read payload
                    payload = bytearray()
                    start_recv = time.time()
                    while len(payload) < total_payload_bytes:
                        chunk = ser.read(total_payload_bytes - len(payload))
                        if chunk:
                            payload.extend(chunk)
                        if time.time() - start_recv > 10.0:  # Timeout safety
                            print("⚠️ Transfer timed out.")
                            break

                    # Read end magic
                    end_bytes = ser.read(4)
                    if len(end_bytes) == 4:
                        end_magic = struct.unpack("<I", end_bytes)[0]
                        if end_magic != PACKET_MAGIC_END:
                            print("⚠️ Warning: Packet end marker mismatch, saving anyway...")

                    if len(payload) == total_payload_bytes:
                        # Verify checksum
                        actual_checksum = sum(payload) & 0xFFFFFFFF
                        if actual_checksum != checksum:
                            print(f"⚠️ Checksum mismatch! (Expected {checksum}, got {actual_checksum})")

                        # Save WAV file
                        filename = get_next_filename(args.dir, args.prefix)
                        with wave.open(filename, "wb") as wav_file:
                            wav_file.setnchannels(channels)
                            wav_file.setsampwidth(bits_per_sample // 8)
                            wav_file.setframerate(sample_rate)
                            wav_file.writeframes(payload)

                        rms, peak = calculate_audio_stats(payload)
                        saved_count += 1

                        print(f"✅ [{saved_count}] Saved: {os.path.basename(filename)} ({len(payload)} bytes, 1.0s)")
                        print(f"   📊 Volume RMS: {rms:.0f} | Peak: {peak} | Path: {filename}")

                        if args.play:
                            try:
                                subprocess.run(["paplay", filename], check=False)
                            except Exception:
                                pass

                        print("👉 Ready for next touch!\n")

    except KeyboardInterrupt:
        print(f"\n🛑 Recording session stopped. Total samples recorded: {saved_count}")
    finally:
        ser.close()

if __name__ == "__main__":
    main()
