---
name: xs:stackchan-atama
description: スタックチャン・アタマ（M5Stack単体版）をUSBシリアルまたはWiFi HTTP API経由で制御するスキル。テキスト読み上げ・表情変更・音量調整・カメラ撮影・WAV再生をPCからコマンド実行。パイプライン再生で高速応答。borotの返答をスタックチャンに喋らせる連携にも対応。「スタックチャンに喋らせて」「stackchan-atama」で使用。
---

# スタックチャン・アタマ制御スキル

USBシリアルまたはWiFi HTTP API経由でスタックチャン・アタマ（M5Stack単体、サーボ不要版）を制御する。

## 接続モード

### USBシリアル（デフォルト）

ポート自動検出。`--port` で手動指定も可能。

### WiFi HTTP API（`--wifi`）

IP: 環境変数 `STACKCHAN_IP` で指定（`--host` でも可）。

## TTS（音声合成）

- **デフォルト: piper-plus**（環境変数 `STACKCHAN_TTS` で変更可能）
- piper-plus: サーバー不要、バイナリ単体で動作。`tools/setup_piper.sh` でセットアップ
- VOICEVOX: `--tts voicevox` で切替。ローカルエンジン（port 50021）が必要

## Step 0: 初回セットアップ（piper-plusのバイナリ・モデルが無い場合のみ）

```bash
cd [SKILL_DIR] && tools/setup_piper.sh
```

OS/ARCH を自動判定し、piper-plus の C# CLI とつくよみちゃんモデルをダウンロード、`tools/piper` ラッパーを生成する。
macOS arm64/x64・Linux arm64/x64 対応。インストール済みならスキップ。

## 実行フロー

### Step 1: 接続確認

```bash
# USBシリアル
cd [SKILL_DIR] && uv run tools/stackchan_atama.py status

# WiFi
cd [SKILL_DIR] && uv run tools/stackchan_atama.py --wifi status
```

### Step 2: テキスト読み上げ（TTS）

```bash
# 通常モード（全文を一括送信）
cd [SKILL_DIR] && uv run tools/stackchan_atama.py say "こんにちは"

# パイプラインモード（文を分割、最初のチャンクを即座に再生開始）— 長文推奨
cd [SKILL_DIR] && uv run tools/stackchan_atama.py say "こんにちは！今日もいい天気ですね。散歩に行きましたか？" --pipeline

# VOICEVOX で話者変更（VOICEVOX時のみ）
cd [SKILL_DIR] && uv run tools/stackchan_atama.py --tts voicevox say "おはよう" --voice 3

# WiFi経由
cd [SKILL_DIR] && uv run tools/stackchan_atama.py --wifi say "こんにちは" --pipeline
```

### Step 3: 表情変更

```bash
cd [SKILL_DIR] && uv run tools/stackchan_atama.py face happy
```

表情一覧:
- neutral / 0 — 普通
- happy / 1 — 嬉しい
- sleepy / 2 — 眠い
- doubt / 3 — 疑問
- sad / 4 — 悲しい
- angry / 5 — 怒り

### Step 4: 音量調整

```bash
cd [SKILL_DIR] && uv run tools/stackchan_atama.py volume 200
```

音量: 0〜255（デフォルト: 255）

### Step 5: カメラ撮影（CoreS3のみ）

```bash
cd [SKILL_DIR] && uv run tools/stackchan_atama.py capture -o /tmp/photo.jpg
cd [SKILL_DIR] && uv run tools/stackchan_atama.py --wifi capture -o /tmp/photo.jpg
```

### Step 6: WAVファイル直接再生

```bash
cd [SKILL_DIR] && uv run tools/stackchan_atama.py play /tmp/audio.wav
cd [SKILL_DIR] && uv run tools/stackchan_atama.py --wifi play /tmp/audio.wav
```

### Step 7: WiFi設定（シリアル経由のみ）

```bash
cd [SKILL_DIR] && uv run tools/stackchan_atama.py wifi-config --ssid MyNetwork --password mypass
cd [SKILL_DIR] && uv run tools/stackchan_atama.py wifi-config --clear
```

## borot連携

borotの返答をスタックチャンに喋らせたい場合:

1. borotが返答テキストを生成
2. テキストを `stackchan_atama.py say --pipeline` で送信
3. 表情も文脈に合わせて変更

**例: からあげに挨拶する**
```bash
cd [SKILL_DIR]
uv run tools/stackchan_atama.py face happy
uv run tools/stackchan_atama.py say "からあげさん、おはよう！" --pipeline
```

**例: 悲しいニュースを伝える**
```bash
cd [SKILL_DIR]
uv run tools/stackchan_atama.py face sad
uv run tools/stackchan_atama.py say "残念だけど、雨みたいだよ"
```

**例: WiFi経由で操作**
```bash
cd [SKILL_DIR]
uv run tools/stackchan_atama.py --wifi face happy
uv run tools/stackchan_atama.py --wifi say "ケーブルなしで喋れるよ！" --pipeline
uv run tools/stackchan_atama.py --wifi capture -o /tmp/photo.jpg
```

## 注意点

- 長文は `--pipeline` で分割送信すると体感速度が上がる
- M5Stack Core（初代）はPSRAMがないため80KB超のWAVは不可 → `--pipeline` で回避
- シリアル転送が失敗する場合: `--serial-chunk 256 --serial-delay 0.02` で速度を落とす

## 使用例

```
スタックチャンにこんにちはって言って
スタックチャンを嬉しい顔にして
stackchan-atamaの状態確認して
スタックチャンに「今日もがんばろう」って喋らせて
スタックチャンで写真撮って
スタックチャンの音量上げて
```
