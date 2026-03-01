#!/usr/bin/env python3
"""
stackchan-atama controller

PC側からstackchan-atamaを制御するスクリプト。
ローカルVOICEVOXで音声合成してWAVをスタックチャンに送信。

Usage:
    uv run stackchan_atama.py say "こんにちは"
    uv run stackchan_atama.py face happy
    uv run stackchan_atama.py status
"""

import argparse
import json
import sys
import urllib.request
import urllib.error
import urllib.parse

STACKCHAN_IP = None  # Set via env or --ip
VOICEVOX_URL = "http://127.0.0.1:50021"
VOICEVOX_SPEAKER = 3  # ずんだもん


def get_stackchan_ip():
    import os
    return STACKCHAN_IP or os.environ.get("STACKCHAN_IP", "192.168.50.128")


def stackchan_request(path, method="GET", data=None, content_type=None, timeout=15):
    ip = get_stackchan_ip()
    url = f"http://{ip}{path}"
    req = urllib.request.Request(url, data=data, method=method)
    if content_type:
        req.add_header("Content-Type", content_type)
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            return json.loads(resp.read().decode("utf-8"))
    except Exception as e:
        return {"status": "error", "error": str(e)}


def voicevox_synthesize(text, speaker=VOICEVOX_SPEAKER):
    """VOICEVOXで音声合成してWAVバイナリを返す"""
    # Step 1: audio_query
    query_url = f"{VOICEVOX_URL}/audio_query?text={urllib.parse.quote(text)}&speaker={speaker}"
    req = urllib.request.Request(query_url, method="POST")
    try:
        with urllib.request.urlopen(req, timeout=30) as resp:
            audio_query = resp.read()
    except Exception as e:
        print(json.dumps({"status": "error", "error": f"audio_query failed: {e}"}))
        sys.exit(1)

    # Step 2: synthesis
    synth_url = f"{VOICEVOX_URL}/synthesis?speaker={speaker}"
    req = urllib.request.Request(synth_url, data=audio_query, method="POST")
    req.add_header("Content-Type", "application/json")
    try:
        with urllib.request.urlopen(req, timeout=60) as resp:
            return resp.read()
    except Exception as e:
        print(json.dumps({"status": "error", "error": f"synthesis failed: {e}"}))
        sys.exit(1)


def cmd_say(args):
    text = args.text
    speaker = args.voice

    # Synthesize with VOICEVOX
    wav_data = voicevox_synthesize(text, speaker)

    # Send to stackchan-atama
    result = stackchan_request("/play_wav", method="POST", data=wav_data,
                               content_type="application/octet-stream", timeout=30)
    result["text"] = text
    result["voice"] = speaker
    result["wav_size"] = len(wav_data)
    print(json.dumps(result, ensure_ascii=False))


def cmd_face(args):
    expr = args.expression
    result = stackchan_request(f"/face?expression={expr}")
    print(json.dumps(result, ensure_ascii=False))


def cmd_status(args):
    result = stackchan_request("/status")
    print(json.dumps(result, ensure_ascii=False))


def cmd_setting(args):
    params = []
    if args.volume is not None:
        params.append(f"volume={args.volume}")
    query = "&".join(params)
    result = stackchan_request(f"/setting?{query}")
    print(json.dumps(result, ensure_ascii=False))


def main():
    parser = argparse.ArgumentParser(description="stackchan-atama controller")
    parser.add_argument("--ip", help="stackchan-atama IP address")
    parser.add_argument("--voicevox-url", default=VOICEVOX_URL, help="VOICEVOX Engine URL")
    sub = parser.add_subparsers(dest="command", required=True)

    p_say = sub.add_parser("say", help="Speak text via VOICEVOX")
    p_say.add_argument("text", help="Text to speak")
    p_say.add_argument("--voice", type=int, default=VOICEVOX_SPEAKER, help="VOICEVOX speaker ID")
    p_say.set_defaults(func=cmd_say)

    p_face = sub.add_parser("face", help="Change face expression")
    p_face.add_argument("expression", help="Expression: neutral/happy/sleepy/doubt/sad/angry")
    p_face.set_defaults(func=cmd_face)

    p_status = sub.add_parser("status", help="Check connection status")
    p_status.set_defaults(func=cmd_status)

    p_setting = sub.add_parser("setting", help="Change settings")
    p_setting.add_argument("--volume", type=int, help="Volume (0-255)")
    p_setting.set_defaults(func=cmd_setting)

    args = parser.parse_args()

    global STACKCHAN_IP, VOICEVOX_URL
    if args.ip:
        STACKCHAN_IP = args.ip
    VOICEVOX_URL = args.voicevox_url

    args.func(args)


if __name__ == "__main__":
    main()
