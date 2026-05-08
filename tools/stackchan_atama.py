#!/usr/bin/env python3
"""
stackchan-atama controller

USB シリアルまたは WiFi HTTP API 経由でスタックチャン（アタマのみ版）を制御。
ローカルVOICEVOXで音声合成してWAVをスタックチャンに送信。
パイプライン再生対応（文を分割して順次送信、最初のチャンクを即座に再生開始）。

Usage:
    # USB Serial (default)
    uv run tools/stackchan_atama.py say "こんにちは"
    uv run tools/stackchan_atama.py say "長い文章。複数に分割されます。" --pipeline
    uv run tools/stackchan_atama.py face happy
    uv run tools/stackchan_atama.py status
    uv run tools/stackchan_atama.py capture -o photo.jpg

    # WiFi HTTP API
    uv run tools/stackchan_atama.py --wifi say "こんにちは"
    uv run tools/stackchan_atama.py --wifi --host $STACKCHAN_IP face happy
    uv run tools/stackchan_atama.py --wifi capture -o photo.jpg
"""
# /// script
# requires-python = ">=3.10"
# dependencies = ["pyserial", "requests"]
# ///

import argparse
import base64
import json
import os
import re
import sys
import time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
from queue import Queue
from typing import Iterator

import subprocess
import tempfile
import threading

import requests
import serial

# ---- Defaults ----
DEFAULT_BAUD = 921600
DEFAULT_VOICEVOX_URL = "http://127.0.0.1:50021"
DEFAULT_VOICEVOX_SPEAKER = 1  # ずんだもん（あまあま）
DEFAULT_SAMPLE_RATE = 16000  # 16kHz (M5Stackスピーカーには十分)
DEFAULT_WIFI_HOST = os.environ.get("STACKCHAN_IP", "192.168.1.100")
DEFAULT_TTS = os.environ.get("STACKCHAN_TTS", "piper")  # "voicevox" or "piper"
DEFAULT_XANGI_URL = os.environ.get("XANGI_URL", "http://127.0.0.1:18888")

# piper-plus: auto-detect from skill directory.
# Exclude piper-plus generated optimization caches (*.cpu.opt*.onnx); they can
# recursively generate more caches and should not become the default model.
_SCRIPT_DIR = Path(__file__).resolve().parent  # tools/
_SKILL_DIR = _SCRIPT_DIR.parent               # stackchan-atama/
_LOCAL_PIPER_BIN = _SCRIPT_DIR / "piper"
_LOCAL_PIPER_MODELS = sorted(
    p for p in _SKILL_DIR.glob("models/*.onnx")
    if ".cpu.opt" not in p.name
)

DEFAULT_PIPER_BIN = os.environ.get(
    "PIPER_BIN",
    str(_LOCAL_PIPER_BIN) if _LOCAL_PIPER_BIN.exists() else "piper"
)
DEFAULT_PIPER_MODEL = os.environ.get(
    "PIPER_MODEL",
    str(_LOCAL_PIPER_MODELS[0]) if _LOCAL_PIPER_MODELS else ""
)
DEFAULT_PIPER_LANGUAGE = os.environ.get("PIPER_LANGUAGE", "ja-en-zh-es-fr-pt")
DEFAULT_PIPER_LENGTH_SCALE = os.environ.get("PIPER_LENGTH_SCALE", "1.5")
DEFAULT_PIPER_NOISE_SCALE = os.environ.get("PIPER_NOISE_SCALE", "0.667")


def detect_serial_port():
    """Auto-detect M5Stack serial port (macOS / Linux).

    Priority:
      1. STACKCHAN_PORT env var
      2. Espressif native USB CDC (VID 0x303A) — CoreS3 / AtomS3 / AtomS3R
      3. CP210x / CH340 USB-Serial bridges (VID 0x10C4 / 0x1A86) — Core / Core2
      4. Platform-specific glob fallback
      5. Hard fallback: /dev/ttyACM0
    """
    env_port = os.environ.get("STACKCHAN_PORT")
    if env_port:
        return env_port

    import serial.tools.list_ports

    # Tier 1: Espressif native USB CDC (CoreS3 / AtomS3 / AtomS3R)
    ESPRESSIF_VID = 0x303A
    # Tier 2: USB-Serial bridges used by Core / Core2
    BRIDGE_VIDS = {0x10C4, 0x1A86}

    ports = list(serial.tools.list_ports.comports())
    for p in ports:
        if p.vid == ESPRESSIF_VID:
            return p.device
    for p in ports:
        if p.vid in BRIDGE_VIDS:
            return p.device

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

    def send_wav(self, wav_data, chunk_size=1024, chunk_delay=0.005):
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

        # Send in chunks
        sent = 0
        while sent < len(wav_data):
            end = min(sent + chunk_size, len(wav_data))
            self.ser.write(wav_data[sent:end])
            sent = end
            time.sleep(chunk_delay)
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


# ---- WiFi HTTP communication ----
class StackchanHTTP:
    """WiFi HTTP API interface to stackchan-atama"""

    FACE_MAP = {
        "neutral": "neutral", "normal": "neutral",
        "happy": "happy", "sleepy": "sleepy",
        "doubt": "doubt", "sad": "sad", "angry": "angry",
    }

    def __init__(self, host=DEFAULT_WIFI_HOST):
        self.host = host
        self.base_url = f"http://{host}"

    def open(self):
        pass  # no persistent connection needed

    def close(self):
        pass

    def send_command(self, cmd):
        """Translate serial-style command to HTTP request"""
        try:
            if cmd == "STATUS":
                resp = requests.get(f"{self.base_url}/status", timeout=5)
                resp.raise_for_status()
                return resp.json()
            elif cmd.startswith("FACE:"):
                expr = cmd.split(":", 1)[1]
                mapped = self.FACE_MAP.get(expr.lower(), expr)
                resp = requests.get(f"{self.base_url}/face", params={"expression": mapped}, timeout=5)
                resp.raise_for_status()
                return resp.json()
            elif cmd.startswith("VOLUME:"):
                level = cmd.split(":", 1)[1]
                resp = requests.get(f"{self.base_url}/setting", params={"volume": level}, timeout=5)
                resp.raise_for_status()
                return resp.json()
            else:
                return {"status": "error", "error": f"unsupported WiFi command: {cmd}"}
        except requests.ConnectionError:
            return {"status": "error", "error": f"cannot connect to {self.base_url}"}
        except requests.Timeout:
            return {"status": "error", "error": "request timeout"}

    def send_wav(self, wav_data):
        """Send WAV binary via HTTP POST"""
        try:
            resp = requests.post(
                f"{self.base_url}/play_wav",
                data=wav_data,
                headers={"Content-Type": "application/octet-stream"},
                timeout=30,
            )
            resp.raise_for_status()
            return resp.json()
        except requests.HTTPError as e:
            return {"status": "error", "error": e.response.text if e.response else str(e)}
        except requests.ConnectionError:
            return {"status": "error", "error": f"cannot connect to {self.base_url}"}
        except requests.Timeout:
            return {"status": "error", "error": "send timeout"}

    def capture(self):
        """Capture JPEG image via HTTP GET"""
        try:
            resp = requests.get(f"{self.base_url}/capture", timeout=15)
            resp.raise_for_status()
            return resp.content, {"status": "ok", "size": len(resp.content)}
        except requests.HTTPError as e:
            return None, {"status": "error", "error": e.response.text if e.response else str(e)}
        except requests.ConnectionError:
            return None, {"status": "error", "error": f"cannot connect to {self.base_url}"}
        except requests.Timeout:
            return None, {"status": "error", "error": "capture timeout"}


# ---- Backend selection ----
def get_backend(args):
    """Create the appropriate backend based on --wifi flag"""
    if args.wifi:
        return StackchanHTTP(args.host)
    else:
        return StackchanSerial(args.port, args.baud)


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


def piper_synthesize(text, piper_bin=DEFAULT_PIPER_BIN, model=DEFAULT_PIPER_MODEL, speaker=0):
    """Generate WAV from text using piper-plus (local, no server required)"""
    if not model:
        raise RuntimeError("--piper-model is required when using --tts piper")

    with tempfile.NamedTemporaryFile(suffix=".wav", delete=False) as f:
        out_path = f.name

    cmd = [piper_bin, "--model", model, "-f", out_path]
    if speaker:
        cmd += ["--speaker", str(speaker)]

    # Auto-detect config file (piper expects model.onnx.json, but some models use config.json)
    model_path = Path(model)
    default_config = model_path.with_suffix(model_path.suffix + ".json")
    alt_config = model_path.parent / "config.json"
    if not default_config.exists() and alt_config.exists():
        cmd += ["--config", str(alt_config)]

    # Set OPENJTALK_PHONEMIZER_PATH if piper binary is in a known directory
    env = os.environ.copy()
    piper_dir = str(Path(piper_bin).parent)
    phonemizer = os.path.join(piper_dir, "open_jtalk_phonemizer")
    if os.path.isfile(phonemizer) and "OPENJTALK_PHONEMIZER_PATH" not in env:
        env["OPENJTALK_PHONEMIZER_PATH"] = phonemizer

    result = subprocess.run(
        cmd, input=text.encode("utf-8"), capture_output=True, env=env, timeout=30,
    )
    if result.returncode != 0:
        os.unlink(out_path)
        raise RuntimeError(f"piper failed: {result.stderr.decode('utf-8', errors='replace')}")

    wav_data = Path(out_path).read_bytes()
    os.unlink(out_path)
    return wav_data


def piper_cli_bin(piper_bin):
    """Resolve tools/piper wrapper to the underlying PiperPlus.Cli for JSONL mode."""
    path = Path(piper_bin)
    if path.name == "piper":
        root = path.parent.parent if path.parent != Path(".") else _SKILL_DIR
        cli = root / "_piper" / "PiperPlus.Cli"
        if cli.exists():
            return str(cli)
    return piper_bin


def piper_config_args(model):
    model_path = Path(model)
    default_config = model_path.with_suffix(model_path.suffix + ".json")
    alt_config = model_path.parent / "config.json"
    if not default_config.exists() and alt_config.exists():
        return ["--config", str(alt_config)]
    return []


def piper_synthesize_many(texts, piper_bin=DEFAULT_PIPER_BIN, model=DEFAULT_PIPER_MODEL, speaker=0):
    """Generate multiple WAVs in one piper-plus process to avoid repeated model loads."""
    if not model:
        raise RuntimeError("--piper-model is required when using --tts piper")
    texts = [text for text in texts if text.strip()]
    if not texts:
        return []

    with tempfile.TemporaryDirectory() as tmpdir:
        filenames = [f"chunk_{i:03}.wav" for i in range(len(texts))]
        jsonl = "\n".join(
            json.dumps({"text": text, "output_file": filename}, ensure_ascii=False)
            for text, filename in zip(texts, filenames)
        ) + "\n"
        cmd = [
            piper_cli_bin(piper_bin),
            "--model", model,
            "--json-input",
            "--output-dir", tmpdir,
            "--language", DEFAULT_PIPER_LANGUAGE,
            "--length-scale", DEFAULT_PIPER_LENGTH_SCALE,
            "--noise-scale", DEFAULT_PIPER_NOISE_SCALE,
        ] + piper_config_args(model)
        if speaker:
            cmd += ["--speaker", str(speaker)]

        result = subprocess.run(
            cmd,
            input=jsonl.encode("utf-8"),
            capture_output=True,
            env=os.environ.copy(),
            timeout=max(30, 10 * len(texts)),
        )
        if result.returncode != 0:
            raise RuntimeError(f"piper failed: {result.stderr.decode('utf-8', errors='replace')}")

        wavs = []
        for filename in filenames:
            path = Path(tmpdir) / filename
            if not path.exists():
                raise RuntimeError(
                    f"piper did not write {filename}: "
                    f"{result.stderr.decode('utf-8', errors='replace')}"
                )
            wavs.append(path.read_bytes())
        return wavs


class PiperProcess:
    """Persistent piper-plus JSONL process for low-latency repeated synthesis."""

    def __init__(self, piper_bin=DEFAULT_PIPER_BIN, model=DEFAULT_PIPER_MODEL, speaker=0):
        if not model:
            raise RuntimeError("--piper-model is required when using --tts piper")
        self.tmpdir = tempfile.TemporaryDirectory()
        self.lock = threading.Lock()
        self.counter = 0
        cmd = [
            piper_cli_bin(piper_bin),
            "--model", model,
            "--json-input",
            "--output-dir", self.tmpdir.name,
            "--language", DEFAULT_PIPER_LANGUAGE,
            "--length-scale", DEFAULT_PIPER_LENGTH_SCALE,
            "--noise-scale", DEFAULT_PIPER_NOISE_SCALE,
            "--quiet",
        ] + piper_config_args(model)
        if speaker:
            cmd += ["--speaker", str(speaker)]
        self.process = subprocess.Popen(
            cmd,
            stdin=subprocess.PIPE,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            text=True,
            bufsize=1,
        )

    def synthesize_many(self, texts, timeout=None):
        texts = [text for text in texts if text.strip()]
        if not texts:
            return []
        timeout = timeout or max(30, 10 * len(texts))
        with self.lock:
            if self.process.poll() is not None:
                raise RuntimeError(f"piper process exited with code {self.process.returncode}")
            filenames = []
            for text in texts:
                self.counter += 1
                filename = f"live_{self.counter:06}.wav"
                filenames.append(filename)
                line = json.dumps({"text": text, "output_file": filename}, ensure_ascii=False)
                self.process.stdin.write(line + "\n")
            self.process.stdin.flush()

            deadline = time.time() + timeout
            wavs = []
            for filename in filenames:
                path = Path(self.tmpdir.name) / filename
                while not path.exists():
                    if self.process.poll() is not None:
                        raise RuntimeError(f"piper process exited with code {self.process.returncode}")
                    if time.time() > deadline:
                        raise TimeoutError(f"piper timed out waiting for {filename}")
                    time.sleep(0.01)
                wavs.append(path.read_bytes())
                try:
                    path.unlink()
                except OSError:
                    pass
            return wavs

    def close(self):
        try:
            if self.process.stdin:
                self.process.stdin.close()
        except Exception:
            pass
        try:
            self.process.wait(timeout=3)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait(timeout=3)
        self.tmpdir.cleanup()


def split_text(text):
    """Split text at Japanese/English punctuation for pipeline playback"""
    parts = re.split(r"(?<=[。！？!?])", text)
    chunks = [p.strip() for p in parts if p.strip()]
    if not chunks:
        chunks = [text]
    return chunks


def normalize_xangi_stream_url(url):
    """Accept either a base xangi URL or a full /api/events/stream URL."""
    trimmed = url.strip().rstrip("/")
    if not trimmed:
        raise ValueError("xangi URL is empty")
    if trimmed.endswith("/api/events/stream"):
        return trimmed
    if trimmed.startswith("http://") or trimmed.startswith("https://"):
        return f"{trimmed}/api/events/stream"
    raise ValueError(f"xangi URL must start with http:// or https:// (got {url})")


def iter_sse_messages(url, timeout=65):
    """Yield parsed SSE events from xangi's pull stream."""
    with requests.get(
        url,
        stream=True,
        headers={"Accept": "text/event-stream"},
        timeout=(5, timeout),
    ) as resp:
        resp.raise_for_status()
        event = "message"
        data_lines = []
        for raw_line in resp.iter_lines(decode_unicode=True):
            if raw_line is None:
                continue
            line = raw_line.rstrip("\r")
            if line == "":
                if data_lines:
                    yield {"event": event, "data": "\n".join(data_lines)}
                event = "message"
                data_lines = []
                continue
            if line.startswith(":"):
                continue
            if line.startswith("event:"):
                event = line[6:].strip() or "message"
                continue
            if line.startswith("data:"):
                data_lines.append(line[5:].lstrip())
                continue


def iter_xangi_events(url, timeout=65) -> Iterator[dict]:
    """Yield JSON payloads from xangi's SSE stream."""
    for msg in iter_sse_messages(url, timeout=timeout):
        if msg["event"] == "ready":
            payload = json.loads(msg["data"])
            payload["_sse_event"] = "ready"
            yield payload
            continue
        payload = json.loads(msg["data"])
        payload["_sse_event"] = msg["event"]
        yield payload


def set_face_if_needed(sc, expression, current_face):
    if not expression or current_face[0] == expression:
        return
    result = sc.send_command(f"FACE:{expression}")
    current_face[0] = expression
    print(json.dumps({"face": expression, "result": result}, ensure_ascii=False), file=sys.stderr)


def speak_text(sc, text, args):
    text = (text or "").strip()
    if not text:
        return

    if args.pipeline:
        chunks = split_text(text)
        print(f"xangi bridge: speaking {len(chunks)} chunks", file=sys.stderr)
        wav_queue = Queue(maxsize=4)

        def tts_worker():
            for i, chunk, wav, tts_time in synthesize_chunks(chunks, args):
                wav_queue.put((i, chunk, wav, tts_time))
            wav_queue.put(None)

        executor = ThreadPoolExecutor(max_workers=1)
        executor.submit(tts_worker)

        while True:
            item = wav_queue.get()
            if item is None:
                break
            i, chunk, wav, tts_time = item
            t0 = time.time()
            if isinstance(sc, StackchanSerial):
                result = sc.send_wav(wav, chunk_size=args.serial_chunk, chunk_delay=args.serial_delay)
            else:
                result = sc.send_wav(wav)
            send_time = time.time() - t0
            print(
                json.dumps(
                    {
                        "chunk": i,
                        "chunks": len(chunks),
                        "text": chunk,
                        "tts_seconds": round(tts_time, 2),
                        "send_seconds": round(send_time, 2),
                        "bytes": len(wav),
                        "result": result,
                    },
                    ensure_ascii=False,
                ),
                file=sys.stderr,
            )
        executor.shutdown(wait=False)
        return

    wav = synthesize(text, args)
    if isinstance(sc, StackchanSerial):
        result = sc.send_wav(wav, chunk_size=args.serial_chunk, chunk_delay=args.serial_delay)
    else:
        result = sc.send_wav(wav)
    print(json.dumps({"text": text, "result": result}, ensure_ascii=False), file=sys.stderr)


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


def synthesize(text, args):
    """Generate WAV from text using the selected TTS engine"""
    if args.tts == "piper":
        return piper_synthesize(text, args.piper_bin, args.piper_model, args.piper_speaker)
    else:
        return voicevox_synthesize(text, args.voicevox_url, args.voice, args.sample_rate)


def synthesize_chunks(chunks, args):
    """Generate chunk WAVs. Piper uses one JSONL batch to load the model once."""
    if args.tts == "piper":
        t0 = time.time()
        piper_process = getattr(args, "piper_process", None)
        if piper_process:
            wavs = piper_process.synthesize_many(chunks)
        else:
            wavs = piper_synthesize_many(chunks, args.piper_bin, args.piper_model, args.piper_speaker)
        tts_time = time.time() - t0
        for i, (chunk, wav) in enumerate(zip(chunks, wavs), start=1):
            yield i, chunk, wav, tts_time if i == 1 else 0.0
        return

    for i, chunk in enumerate(chunks, start=1):
        t0 = time.time()
        wav = synthesize(chunk, args)
        tts_time = time.time() - t0
        yield i, chunk, wav, tts_time


def cmd_say(args):
    if args.tts == "voicevox":
        check_voicevox(args.voicevox_url)
    elif args.tts == "piper" and not args.piper_model:
        print("Error: --piper-model is required when using --tts piper", file=sys.stderr)
        sys.exit(1)

    sc = get_backend(args)
    sc.open()

    if args.pipeline:
        chunks = split_text(args.text)
        print(f"Pipeline ({args.tts}): {len(chunks)} chunks", file=sys.stderr)
        wav_queue = Queue(maxsize=4)

        def tts_worker():
            for i, chunk, wav, tts_time in synthesize_chunks(chunks, args):
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
            if isinstance(sc, StackchanSerial):
                result = sc.send_wav(wav, chunk_size=args.serial_chunk, chunk_delay=args.serial_delay)
            else:
                result = sc.send_wav(wav)
            send_time = time.time() - t0
            print(f"  [{i}/{len(chunks)}] TTS:{tts_time:.2f}s Send:{send_time:.2f}s ({len(wav)}B) {chunk}", file=sys.stderr)

        executor.shutdown(wait=False)
    else:
        wav = synthesize(args.text, args)
        if isinstance(sc, StackchanSerial):
            result = sc.send_wav(wav, chunk_size=args.serial_chunk, chunk_delay=args.serial_delay)
        else:
            result = sc.send_wav(wav)
        result["text"] = args.text
        result["wav_size"] = len(wav)
        result["tts"] = args.tts
        print(json.dumps(result, ensure_ascii=False))

    sc.close()


def cmd_face(args):
    sc = get_backend(args)
    sc.open()
    result = sc.send_command(f"FACE:{args.expression}")
    print(json.dumps(result, ensure_ascii=False))
    sc.close()


def cmd_status(args):
    sc = get_backend(args)
    sc.open()
    result = sc.send_command("STATUS")
    print(json.dumps(result, ensure_ascii=False))
    sc.close()


def cmd_volume(args):
    sc = get_backend(args)
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
    sc = get_backend(args)
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


def cmd_play(args):
    """Play a WAV file directly"""
    wav_path = Path(args.file)
    if not wav_path.exists():
        print(json.dumps({"status": "error", "error": f"file not found: {args.file}"}), file=sys.stderr)
        sys.exit(1)

    wav_data = wav_path.read_bytes()
    sc = get_backend(args)
    sc.open()
    result = sc.send_wav(wav_data)
    sc.close()

    result["file"] = args.file
    result["size"] = len(wav_data)
    print(json.dumps(result, ensure_ascii=False))


def cmd_xangi_bridge(args):
    if args.tts == "voicevox":
        check_voicevox(args.voicevox_url)
    elif args.tts == "piper" and not args.piper_model:
        print("Error: --piper-model is required when using --tts piper", file=sys.stderr)
        sys.exit(1)

    try:
        stream_url = normalize_xangi_stream_url(args.xangi_url)
    except ValueError as e:
        print(f"Error: {e}", file=sys.stderr)
        sys.exit(1)

    sc = get_backend(args)
    sc.open()
    args.piper_process = None
    if args.tts == "piper":
        args.piper_process = PiperProcess(args.piper_bin, args.piper_model, args.piper_speaker)
    current_face = [None]
    set_face_if_needed(sc, args.face_idle, current_face)

    backoff = max(args.retry_seconds, 1.0)
    max_backoff = max(backoff, args.max_retry_seconds)
    active_turn = None

    try:
        while True:
            try:
                print(f"xangi bridge: connecting to {stream_url}", file=sys.stderr)
                for ev in iter_xangi_events(stream_url, timeout=args.stream_timeout):
                    sse_event = ev.get("_sse_event")
                    if sse_event == "ready":
                        print(json.dumps({"ready": ev}, ensure_ascii=False), file=sys.stderr)
                        continue

                    if args.instance_id and ev.get("instance_id") != args.instance_id:
                        continue
                    if args.thread_id and ev.get("thread_id") != args.thread_id:
                        continue

                    ev_type = ev.get("type")
                    if not ev_type:
                        continue
                    print(json.dumps(ev, ensure_ascii=False), file=sys.stderr)

                    if ev_type == "turn.started":
                        active_turn = ev.get("turn_id")
                        set_face_if_needed(sc, args.face_thinking, current_face)
                    elif ev_type == "message.delta":
                        if active_turn == ev.get("turn_id"):
                            set_face_if_needed(sc, args.face_talking, current_face)
                    elif ev_type == "turn.complete":
                        active_turn = None
                        set_face_if_needed(sc, args.face_talking, current_face)
                        speak_text(sc, ev.get("text", ""), args)
                        set_face_if_needed(sc, args.face_idle, current_face)
                    elif ev_type == "turn.aborted":
                        active_turn = None
                        set_face_if_needed(sc, args.face_idle, current_face)
                    elif ev_type == "agent.error":
                        active_turn = None
                        set_face_if_needed(sc, args.face_error, current_face)
            except KeyboardInterrupt:
                raise
            except Exception as e:
                print(
                    f"xangi bridge: stream error: {e} (retry in {backoff:.1f}s)",
                    file=sys.stderr,
                )
                time.sleep(backoff)
                backoff = min(backoff * 2, max_backoff)
                continue

            backoff = max(args.retry_seconds, 1.0)
            time.sleep(backoff)
    except KeyboardInterrupt:
        print("xangi bridge: stopped", file=sys.stderr)
    finally:
        set_face_if_needed(sc, args.face_idle, current_face)
        if args.piper_process:
            args.piper_process.close()
        sc.close()


def main():
    parser = argparse.ArgumentParser(description="stackchan-atama controller")
    parser.add_argument("--port", default=DEFAULT_PORT, help=f"Serial port (default: {DEFAULT_PORT})")
    parser.add_argument("--baud", type=int, default=DEFAULT_BAUD, help="Baud rate (default: 921600)")
    parser.add_argument("--wifi", action="store_true", help="Use WiFi HTTP API instead of USB serial")
    parser.add_argument("--host", default=DEFAULT_WIFI_HOST, help=f"WiFi host IP (default: {DEFAULT_WIFI_HOST}, env: STACKCHAN_IP)")
    parser.add_argument("--voicevox-url", default=DEFAULT_VOICEVOX_URL, help="VOICEVOX Engine URL")
    parser.add_argument("--tts", choices=["voicevox", "piper"], default=DEFAULT_TTS,
                        help="TTS engine (default: piper)")
    parser.add_argument("--piper-bin", default=DEFAULT_PIPER_BIN,
                        help="Path to piper binary (default: piper, env: PIPER_BIN)")
    parser.add_argument("--piper-model", default=DEFAULT_PIPER_MODEL,
                        help="Path to piper .onnx model file (env: PIPER_MODEL)")
    parser.add_argument("--piper-speaker", type=int, default=0,
                        help="Piper speaker ID for multi-speaker models (default: 0)")
    parser.add_argument("--serial-chunk", type=int, default=1024,
                        help="Serial transfer chunk size in bytes (default: 1024, try 256 if transfer fails)")
    parser.add_argument("--serial-delay", type=float, default=0.005,
                        help="Delay between serial chunks in seconds (default: 0.005, try 0.02 if transfer fails)")
    sub = parser.add_subparsers(dest="command", required=True)

    p_say = sub.add_parser("say", help="Speak text via TTS (VOICEVOX or piper)")
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

    p_wifi_cfg = sub.add_parser("wifi-config", help="Set or clear WiFi credentials via serial (saved to NVS)")
    p_wifi_cfg.add_argument("--ssid", help="WiFi SSID")
    p_wifi_cfg.add_argument("--password", default="", help="WiFi password")
    p_wifi_cfg.add_argument("--clear", action="store_true", help="Clear saved WiFi credentials")
    p_wifi_cfg.set_defaults(func=cmd_wifi)

    p_capture = sub.add_parser("capture", help="Capture image from camera (CoreS3 only)")
    p_capture.add_argument("-o", "--output", default=None, help="Output file (default: capture.jpg)")
    p_capture.set_defaults(func=cmd_capture)

    p_play = sub.add_parser("play", help="Play a WAV file directly")
    p_play.add_argument("file", help="WAV file path")
    p_play.set_defaults(func=cmd_play)

    p_bridge = sub.add_parser("xangi-bridge", help="Subscribe to xangi SSE and speak completed responses")
    p_bridge.add_argument("--xangi-url", default=DEFAULT_XANGI_URL,
                          help="Base xangi URL or full /api/events/stream URL (env: XANGI_URL)")
    p_bridge.add_argument("--thread-id", default="",
                          help="Optional thread filter, e.g. discord:1234567890")
    p_bridge.add_argument("--instance-id", default="",
                          help="Optional xangi instance_id filter")
    p_bridge.add_argument("--retry-seconds", type=float, default=1.0,
                          help="Initial reconnect delay in seconds (default: 1)")
    p_bridge.add_argument("--max-retry-seconds", type=float, default=30.0,
                          help="Max reconnect delay in seconds (default: 30)")
    p_bridge.add_argument("--stream-timeout", type=int, default=65,
                          help="Read timeout for SSE stream in seconds (default: 65)")
    p_bridge.add_argument("--face-idle", default="neutral",
                          help="Face to use while idle (default: neutral)")
    p_bridge.add_argument("--face-thinking", default="doubt",
                          help="Face to use after turn.started (default: doubt)")
    p_bridge.add_argument("--face-talking", default="happy",
                          help="Face to use while talking / before speaking (default: happy)")
    p_bridge.add_argument("--face-error", default="sad",
                          help="Face to use on agent.error (default: sad)")
    p_bridge.add_argument("--pipeline", action="store_true",
                          help="Speak final text in sentence chunks for faster playback")
    p_bridge.add_argument("--voice", type=int, default=DEFAULT_VOICEVOX_SPEAKER,
                          help="VOICEVOX speaker ID when --tts voicevox is used")
    p_bridge.add_argument("--sample-rate", type=int, default=DEFAULT_SAMPLE_RATE,
                          help="WAV sample rate for VOICEVOX (default: 16000)")
    p_bridge.set_defaults(func=cmd_xangi_bridge)

    args = parser.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
