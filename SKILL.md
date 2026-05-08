---
name: xs:stackchan-atama
description: スタックチャン・アタマ（M5Stack単体版）をUSBシリアルまたはWiFi HTTP API経由で制御するスキル。テキスト読み上げ・表情変更・音量調整・カメラ撮影・WAV再生をPCからコマンド実行。パイプライン再生で高速応答。borotの返答をスタックチャンに喋らせる連携にも対応。「スタックチャンに喋らせて」「stackchan-atama」で使用。
---

# スタックチャン・アタマ制御スキル

USBシリアルまたはWiFi HTTP API経由でスタックチャン・アタマ（M5Stack単体、サーボ不要版）を制御する。
xangi 連携はオプションで、xangi の返答イベントを購読して stackchan-atama に喋らせることもできる。

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

OS/ARCH を自動判定し、piper-plus CLI（macOS/Linux arm64/x64）または native piper-plus（Linux armv7）と、つくよみちゃん 6-language モデルをダウンロード、`tools/piper` ラッパーを生成する。

`tools/piper` はモデルカードの推奨どおり `--language ja-en-zh-es-fr-pt` と
`--length-scale 1.5` を既定で使う。上書きする場合は `PIPER_LANGUAGE` /
`PIPER_LENGTH_SCALE` / `PIPER_NOISE_SCALE` を指定する。
macOS arm64/x64・Linux arm64/x64・Linux armv7 対応。インストール済みならスキップ。

**`say` 実行時に `piper failed: ... Opset 5 ... opset 3 only` エラーが出たら:** 初回実行で生成された最適化キャッシュが onnxruntime と互換性が無い状態。`rm [SKILL_DIR]/models/*.cpu.opt.onnx*` で解消。

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

### Step 8: xangi の返答をそのまま喋らせる（オプション）

```bash
# xangi の pull 型 SSE 接続
cd [SKILL_DIR] && uv run tools/stackchan_atama.py xangi-bridge --xangi-url http://127.0.0.1:18888 --pipeline

# 特定チャンネルだけに絞る
cd [SKILL_DIR] && uv run tools/stackchan_atama.py xangi-bridge \
  --xangi-url http://127.0.0.1:18888 \
  --thread-id discord:1478428157932605480 \
  --pipeline
```

既定動作:
- `turn.started` → `doubt`
- `message.delta` → `happy`
- `turn.complete` → 最終テキストを発話して `neutral`
- `agent.error` → `sad`

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
- `xangi-bridge` では piper-plus を JSONL 入力の常駐プロセスとして保持し、初回だけモデルロードする。2回目以降のTTSはロードなしで低遅延になる
- 発話速度を優先する場合は `PIPER_LENGTH_SCALE=1.2` のように小さくできる（既定はモデルカード推奨の `1.5`）。品質とのトレードオフ
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
xangi の返答を stackchan-atama で読ませて
```
