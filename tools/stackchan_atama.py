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
import json
import re
import sys
import time

import requests
import serial

# ---- Defaults ----
DEFAULT_PORT = "/dev/ttyACM0"
DEFAULT_BAUD = 921600
DEFAULT_VOICEVOX_URL = "http://127.0.0.1:50021"
DEFAULT_VOICEVOX_SPEAKER = 1  # ずんだもん（あまあま）


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


# ---- VOICEVOX ----
def voicevox_synthesize(text, voicevox_url=DEFAULT_VOICEVOX_URL, speaker=DEFAULT_VOICEVOX_SPEAKER):
    """Generate WAV from text using VOICEVOX"""
    resp = requests.post(f"{voicevox_url}/audio_query", params={"text": text, "speaker": speaker})
    resp.raise_for_status()
    query = resp.json()

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
def cmd_say(args):
    sc = StackchanSerial(args.port, args.baud)
    sc.open()

    if args.pipeline:
        chunks = split_text(args.text)
        print(f"Pipeline: {len(chunks)} chunks", file=sys.stderr)
        for i, chunk in enumerate(chunks):
            t0 = time.time()
            wav = voicevox_synthesize(chunk, args.voicevox_url, args.voice)
            tts_time = time.time() - t0
            t0 = time.time()
            result = sc.send_wav(wav)
            send_time = time.time() - t0
            print(f"  [{i+1}/{len(chunks)}] TTS:{tts_time:.2f}s Send:{send_time:.2f}s ({len(wav)}B) {chunk}", file=sys.stderr)
    else:
        wav = voicevox_synthesize(args.text, args.voicevox_url, args.voice)
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
    p_say.set_defaults(func=cmd_say)

    p_face = sub.add_parser("face", help="Change face expression")
    p_face.add_argument("expression", help="Expression: neutral/happy/sleepy/doubt/sad/angry")
    p_face.set_defaults(func=cmd_face)

    p_status = sub.add_parser("status", help="Check device status")
    p_status.set_defaults(func=cmd_status)

    p_volume = sub.add_parser("volume", help="Set speaker volume")
    p_volume.add_argument("level", type=int, help="Volume level (0-255)")
    p_volume.set_defaults(func=cmd_volume)

    args = parser.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
