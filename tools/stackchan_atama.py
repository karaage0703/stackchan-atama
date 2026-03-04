#!/usr/bin/env python3
"""
stackchan-atama controller

USB シリアル経由でスタックチャン（アタマのみ版）を制御。
ローカルVOICEVOXで音声合成してWAVをスタックチャンに送信。
パイプライン再生対応（文を分割して順次送信、最初のチャンクを即座に再生開始）。

Usage:
    uv run stackchan_atama.py say "こんにちは"
    uv run stackchan_atama.py say "長い文章。複数に分割されます。" --pipeline
    uv run stackchan_atama.py face happy
    uv run stackchan_atama.py status
    uv run stackchan_atama.py volume 200
"""
# /// script
# requires-python = ">=3.10"
# dependencies = ["pyserial", "requests"]
# ///

import argparse
import base64
import json
import re
import sys
import time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
from queue import Queue

import requests
import serial

# ---- Defaults ----
DEFAULT_BAUD = 921600
DEFAULT_VOICEVOX_URL = "http://127.0.0.1:50021"
DEFAULT_VOICEVOX_SPEAKER = 1  # ずんだもん（あまあま）
DEFAULT_SAMPLE_RATE = 16000  # 16kHz (M5Stackスピーカーには十分)


def detect_serial_port():
    """Auto-detect M5Stack serial port (macOS / Linux)"""
    import serial.tools.list_ports

    # ESP32-S3 (CoreS3): VID=303A PID=1001
    # CP2104 (Core/Core2): VID=10C4 PID=EA60
    # CH9102/CH340 (Core/Core2): VID=1A86 PID=55D4 or 7523
    known_vids = {0x303A, 0x10C4, 0x1A86}

    for port_info in serial.tools.list_ports.comports():
        if port_info.vid in known_vids:
            return port_info.device

    # Fallback: platform-specific common names
    import glob
    import platform

    if platform.system() == "Darwin":
        candidates = glob.glob("/dev/cu.usbmodem*") + glob.glob("/dev/cu.usbserial*")
    else:
        candidates = glob.glob("/dev/ttyACM*") + glob.glob("/dev/ttyUSB*")

    if candidates:
        return candidates[0]

    return "/dev/ttyACM0"  # ultimate fallback


DEFAULT_PORT = detect_serial_port()


# ---- Serial communication ----
class StackchanSerial:
    """USB Serial interface to stackchan-atama"""

    def __init__(self, port=DEFAULT_PORT, baud=DEFAULT_BAUD):
        self.port = port
        self.baud = baud
        self.ser = None

    def open(self):
        self.ser = serial.Serial(self.port, self.baud, timeout=5)
        time.sleep(0.5)
        while self.ser.in_waiting:
            self.ser.read(self.ser.in_waiting)

    def close(self):
        if self.ser and self.ser.is_open:
            self.ser.close()

    def send_command(self, cmd):
        """Send a text command and return the JSON response"""
        self.ser.write(f"{cmd}\n".encode())
        self.ser.flush()
        time.sleep(0.5)
        response = ""
        while self.ser.in_waiting:
            line = self.ser.readline().decode("utf-8", errors="replace").strip()
            if line:
                response = line
        try:
            return json.loads(response)
        except json.JSONDecodeError:
            return {"raw": response}

    def send_wav(self, wav_data):
        """Send WAV binary data with flow control"""
        self.ser.write(f"WAV:{len(wav_data)}\n".encode())
        self.ser.flush()

        # Wait for READY
        deadline = time.time() + 3
        ready = False
        while time.time() < deadline:
            if self.ser.in_waiting:
                line = self.ser.readline().decode("utf-8", errors="replace").strip()
                if line == "READY":
                    ready = True
                    break
            time.sleep(0.05)
        if not ready:
            return {"status": "error", "error": "no READY response"}

        # Send in 1KB chunks with 5ms delay
        CHUNK = 1024
        sent = 0
        while sent < len(wav_data):
            end = min(sent + CHUNK, len(wav_data))
            self.ser.write(wav_data[sent:end])
            sent = end
            time.sleep(0.005)
        self.ser.flush()

        # Wait for OK response
        deadline = time.time() + 10
        while time.time() < deadline:
            if self.ser.in_waiting:
                line = self.ser.readline().decode("utf-8", errors="replace").strip()
                if line and line.startswith("{"):
                    try:
                        return json.loads(line)
                    except json.JSONDecodeError:
                        pass
            time.sleep(0.05)
        return {"status": "ok", "size": len(wav_data), "note": "no confirmation received"}

    def capture(self):
        """Capture JPEG image from camera and return bytes"""
        self.ser.write(b"CAPTURE\n")
        self.ser.flush()

        # Read header JSON
        deadline = time.time() + 5
        header = None
        while time.time() < deadline:
            if self.ser.in_waiting:
                line = self.ser.readline().decode("utf-8", errors="replace").strip()
                if line.startswith("{"):
                    try:
                        header = json.loads(line)
                        break
                    except json.JSONDecodeError:
                        pass
            time.sleep(0.05)

        if not header:
            return None, {"status": "error", "error": "no response"}
        if header.get("status") == "error":
            return None, header

        # Read base64 data until END_CAPTURE
        b64_data = b""
        deadline = time.time() + 10
        while time.time() < deadline:
            if self.ser.in_waiting:
                line = self.ser.readline()
                text = line.decode("utf-8", errors="replace").strip()
                if text == "END_CAPTURE":
                    break
                b64_data += line.strip()
            time.sleep(0.01)

        try:
            jpg_data = base64.b64decode(b64_data)
        except Exception as e:
            return None, {"status": "error", "error": f"base64 decode failed: {e}"}

        return jpg_data, header


# ---- VOICEVOX ----
def voicevox_synthesize(text, voicevox_url=DEFAULT_VOICEVOX_URL, speaker=DEFAULT_VOICEVOX_SPEAKER, sample_rate=DEFAULT_SAMPLE_RATE):
    """Generate WAV from text using VOICEVOX"""
    resp = requests.post(f"{voicevox_url}/audio_query", params={"text": text, "speaker": speaker})
    resp.raise_for_status()
    query = resp.json()
    if sample_rate:
        query["outputSamplingRate"] = sample_rate

    resp = requests.post(f"{voicevox_url}/synthesis", params={"speaker": speaker}, json=query)
    resp.raise_for_status()
    return resp.content


def split_text(text):
    """Split text at Japanese/English punctuation for pipeline playback"""
    parts = re.split(r"(?<=[。！？!?])", text)
    chunks = [p.strip() for p in parts if p.strip()]
    if not chunks:
        chunks = [text]
    return chunks


# ---- Commands ----
def check_voicevox(url):
    """Check if VOICEVOX Engine is running"""
    try:
        resp = requests.get(f"{url}/version", timeout=2)
        resp.raise_for_status()
        return True
    except (requests.ConnectionError, requests.Timeout):
        print(f"Error: VOICEVOX Engine is not running at {url}", file=sys.stderr)
        print("Please start VOICEVOX before using the 'say' command.", file=sys.stderr)
        sys.exit(1)


def cmd_say(args):
    check_voicevox(args.voicevox_url)
    sc = StackchanSerial(args.port, args.baud)
    sc.open()

    if args.pipeline:
        chunks = split_text(args.text)
        print(f"Pipeline: {len(chunks)} chunks", file=sys.stderr)
        wav_queue = Queue(maxsize=4)

        def tts_worker():
            for i, chunk in enumerate(chunks):
                t0 = time.time()
                wav = voicevox_synthesize(chunk, args.voicevox_url, args.voice, args.sample_rate)
                tts_time = time.time() - t0
                wav_queue.put((i, chunk, wav, tts_time))
            wav_queue.put(None)  # sentinel

        executor = ThreadPoolExecutor(max_workers=1)
        executor.submit(tts_worker)

        while True:
            item = wav_queue.get()
            if item is None:
                break
            i, chunk, wav, tts_time = item
            t0 = time.time()
            result = sc.send_wav(wav)
            send_time = time.time() - t0
            print(f"  [{i+1}/{len(chunks)}] TTS:{tts_time:.2f}s Send:{send_time:.2f}s ({len(wav)}B) {chunk}", file=sys.stderr)

        executor.shutdown(wait=False)
    else:
        wav = voicevox_synthesize(args.text, args.voicevox_url, args.voice, args.sample_rate)
        result = sc.send_wav(wav)
        result["text"] = args.text
        result["wav_size"] = len(wav)
        print(json.dumps(result, ensure_ascii=False))

    sc.close()


def cmd_face(args):
    sc = StackchanSerial(args.port, args.baud)
    sc.open()
    result = sc.send_command(f"FACE:{args.expression}")
    print(json.dumps(result, ensure_ascii=False))
    sc.close()


def cmd_status(args):
    sc = StackchanSerial(args.port, args.baud)
    sc.open()
    result = sc.send_command("STATUS")
    print(json.dumps(result, ensure_ascii=False))
    sc.close()


def cmd_volume(args):
    sc = StackchanSerial(args.port, args.baud)
    sc.open()
    result = sc.send_command(f"VOLUME:{args.level}")
    print(json.dumps(result, ensure_ascii=False))
    sc.close()


def cmd_wifi(args):
    sc = StackchanSerial(args.port, args.baud)
    sc.open()
    if args.clear:
        result = sc.send_command("WIFI:CLEAR")
    else:
        if not args.ssid:
            print("Error: --ssid is required (or use --clear)", file=sys.stderr)
            sys.exit(1)
        password = args.password or ""
        result = sc.send_command(f"WIFI:{args.ssid}:{password}")
    print(json.dumps(result, ensure_ascii=False))
    sc.close()


def cmd_capture(args):
    sc = StackchanSerial(args.port, args.baud)
    sc.open()
    jpg_data, info = sc.capture()
    sc.close()

    if jpg_data is None:
        print(json.dumps(info, ensure_ascii=False), file=sys.stderr)
        sys.exit(1)

    output = args.output or "capture.jpg"
    Path(output).write_bytes(jpg_data)
    print(f"Saved {len(jpg_data)} bytes to {output}", file=sys.stderr)
    print(json.dumps({"status": "ok", "file": output, "size": len(jpg_data)}, ensure_ascii=False))


def main():
    parser = argparse.ArgumentParser(description="stackchan-atama controller")
    parser.add_argument("--port", default=DEFAULT_PORT, help="Serial port (default: /dev/ttyACM0)")
    parser.add_argument("--baud", type=int, default=DEFAULT_BAUD, help="Baud rate (default: 921600)")
    parser.add_argument("--voicevox-url", default=DEFAULT_VOICEVOX_URL, help="VOICEVOX Engine URL")
    sub = parser.add_subparsers(dest="command", required=True)

    p_say = sub.add_parser("say", help="Speak text via VOICEVOX")
    p_say.add_argument("text", help="Text to speak")
    p_say.add_argument("--voice", type=int, default=DEFAULT_VOICEVOX_SPEAKER, help="VOICEVOX speaker ID")
    p_say.add_argument("--pipeline", action="store_true", help="Split text for faster first response")
    p_say.add_argument("--sample-rate", type=int, default=DEFAULT_SAMPLE_RATE, help="WAV sample rate (default: 16000)")
    p_say.set_defaults(func=cmd_say)

    p_face = sub.add_parser("face", help="Change face expression")
    p_face.add_argument("expression", help="Expression: neutral/happy/sleepy/doubt/sad/angry")
    p_face.set_defaults(func=cmd_face)

    p_status = sub.add_parser("status", help="Check device status")
    p_status.set_defaults(func=cmd_status)

    p_volume = sub.add_parser("volume", help="Set speaker volume")
    p_volume.add_argument("level", type=int, help="Volume level (0-255)")
    p_volume.set_defaults(func=cmd_volume)

    p_wifi = sub.add_parser("wifi", help="Set or clear WiFi credentials (saved to NVS)")
    p_wifi.add_argument("--ssid", help="WiFi SSID")
    p_wifi.add_argument("--password", default="", help="WiFi password")
    p_wifi.add_argument("--clear", action="store_true", help="Clear saved WiFi credentials")
    p_wifi.set_defaults(func=cmd_wifi)

    p_capture = sub.add_parser("capture", help="Capture image from camera (CoreS3 only)")
    p_capture.add_argument("-o", "--output", default=None, help="Output file (default: capture.jpg)")
    p_capture.set_defaults(func=cmd_capture)

    args = parser.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
