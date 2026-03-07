---
name: xs:stackchan-atama
description: スタックチャン・アタマ（M5Stack単体版）をUSBシリアルまたはWiFi HTTP API経由で制御するスキル。テキスト読み上げ・表情変更・音量調整・カメラ撮影・WAV再生をPCからコマンド実行。パイプライン再生で高速応答。borotの返答をスタックチャンに喋らせる連携にも対応。「スタックチャンに喋らせて」「stackchan-atama」で使用。
---

# スタックチャン・アタマ制御スキル

USBシリアルまたはWiFi HTTP API経由でスタックチャン・アタマ（M5Stack単体、サーボ不要版）を制御する。テキスト読み上げ、表情変更、音量調整、カメラ撮影、WAVファイル再生が可能。

## 通常のスタックチャンとの違い

- **stackchan（既存）**: WiFi HTTP API、AI_StackChan2ファームウェア、サーボ付き
- **stackchan-atama（本スキル）**: USBシリアル or WiFi、専用ファームウェア、アタマだけ（サーボなし）

## スキル構成

- `tools/stackchan_atama.py` - 制御CLI（pyserial + requests）
- `src/main.cpp` - M5Stack用ファームウェア（PlatformIO）
- `platformio.ini` - ビルド設定（CoreS3 / Core / Core2）

## ファームウェア書き込み

ファームウェアの更新が必要な場合（初回セットアップ、コード変更後など）。

### 前提条件

- PlatformIO CLI（`pio`）がインストール済み（なければ `uv tool install platformio`）
- USBケーブルでデバイスを接続

### 書き込みコマンド

```bash
cd [SKILL_DIR]

# CoreS3の場合（推奨）
pio run -e m5stack-cores3 -t upload

# Core2の場合
pio run -e m5stack-core2 -t upload

# Core（初代）の場合
pio run -e m5stack-core -t upload
```

- 画面にアバターの顔が出れば成功
- 初回はライブラリダウンロードで時間がかかる（2回目以降は高速）
- 書き込み後、自動的にデバイスがリセットされる

## 接続モード

### USBシリアル（デフォルト）

- ポート: 自動検出（macOS: `/dev/cu.usbmodem*`, Linux: `/dev/ttyACM*`）。`--port` で手動指定も可能
- ボーレート: 921600
- プロトコル: USBシリアル（テキストコマンド + バイナリWAV転送）

### WiFi HTTP API（`--wifi`）

- IP: 環境変数 `STACKCHAN_IP` またはデフォルト `192.168.1.9`。`--host` で指定も可能
- プロトコル: HTTP (port 80)
- USBケーブル不要。別のPCからでも操作可能

### 共通要件

TTSエンジンが必要（どちらか一方でOK）：

#### VOICEVOX（デフォルト）
- VOICEVOX Engine がローカルで起動していること（port 50021）
  - Dockerで起動: `docker run --rm -p 50021:50021 voicevox/voicevox_engine:cpu-latest`
  - macOSでは `open -a VOICEVOX` でも起動可能
  - 確認: `curl http://localhost:50021/version`
  - 起動していない場合、`say` コマンドはエラーメッセージを表示して終了する

#### piper-plus（VOICEVOX不要、Raspberry Pi対応）
- [piper-plus](https://github.com/ayutaz/piper-plus) のバイナリとモデルファイルが必要
- サーバー不要、バイナリ単体で動作。ARM64（Raspberry Pi / DGX Spark）対応
- 日本語モデル例: `ayousanz/piper-plus-tsukuyomi-chan`（HuggingFace）
- `--tts piper --piper-bin <path> --piper-model <path>` で指定

## 対応デバイス

| デバイス | 音声再生 | カメラ | 備考 |
|----------|----------|--------|------|
| M5Stack CoreS3 | OK（大容量WAV可） | OK | PSRAM搭載、推奨 |
| M5Stack Core（初代） | OK（80KB以下） | なし | PSRAMなし、短い発話向け |
| M5Stack Core2 | 未テスト | なし | PSRAM搭載 |

## 実行フロー

### Step 1: 接続確認

```bash
# USBシリアル
cd [SKILL_DIR] && uv run tools/stackchan_atama.py status

# WiFi
cd [SKILL_DIR] && uv run tools/stackchan_atama.py --wifi status
cd [SKILL_DIR] && uv run tools/stackchan_atama.py --wifi --host 192.168.1.5 status
```

### Step 2: テキスト読み上げ（TTS）

```bash
# 通常モード（全文を一括送信）
cd [SKILL_DIR] && uv run tools/stackchan_atama.py say "こんにちは"

# パイプラインモード（文を分割、最初のチャンクを即座に再生開始）
cd [SKILL_DIR] && uv run tools/stackchan_atama.py say "こんにちは！今日もいい天気ですね。散歩に行きましたか？" --pipeline

# 話者変更
cd [SKILL_DIR] && uv run tools/stackchan_atama.py say "おはよう" --voice 3

# WiFi経由
cd [SKILL_DIR] && uv run tools/stackchan_atama.py --wifi say "こんにちは" --pipeline

# piper-plus でオフライン音声合成（VOICEVOX 不要、Raspberry Pi 対応）
cd [SKILL_DIR] && uv run tools/stackchan_atama.py --tts piper \
  --piper-bin /path/to/piper \
  --piper-model /path/to/model.onnx \
  say "こんにちは" --pipeline
```

- デフォルト: VOICEVOX ローカルエンジン経由
- `--tts piper` で piper-plus に切替（サーバー不要、ARM64対応）
- `--pipeline` で句読点区切り＋順次送信（長文推奨）
- `--voice` で話者ID変更（デフォルト: 1 = ずんだもん、VOICEVOX時のみ）

### Step 3: 表情変更

```bash
cd [SKILL_DIR] && uv run tools/stackchan_atama.py face happy
cd [SKILL_DIR] && uv run tools/stackchan_atama.py --wifi face happy
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

- CoreS3のGC0308カメラでJPEG撮影
- USBシリアル・WiFiどちらでも利用可能

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

- USBシリアルモード（デフォルト）はUSBケーブル接続が必要
- WiFiモード（`--wifi`）はネットワーク接続が必要。IPアドレスは `--host` か環境変数 `STACKCHAN_IP` で指定
- VOICEVOX Engine がローカルで動いている必要がある
- シリアルポートが他のプロセスに掴まれていると接続できない
- 長文は `--pipeline` オプションで分割送信すると体感速度が上がる
- パイプライン再生はキュー（4スロット）に順次投入される
- **M5Stack Core（初代）はPSRAMがないため、80KB超のWAVは受け付けない**
  - シリアル: `WAV:` コマンドは100KB制限（`invalid size`）
  - HTTP: `/play_wav` は80KB制限（HTTP 400）
  - 長文は `--pipeline` で分割送信すれば問題なし

## トラブルシューティング

- **Permission denied（Linux）**: `sudo usermod -aG dialout $USER` して再ログイン
- **ポートが見つからない**: USBケーブルがデータ転送対応か確認（充電専用ケーブルはNG）
- **VOICEVOX接続エラー**: `docker ps` でコンテナ起動確認、または `curl http://localhost:50021/version` で疎通確認
- **音が鳴らない**: `uv run tools/stackchan_atama.py volume 255` で最大音量に設定
- **シリアルポートが掴まれている**: `lsof /dev/ttyACM0` で確認、`fuser -k /dev/ttyACM0` で解放

## 使用例

```
スタックチャンにこんにちはって言って
スタックチャンを嬉しい顔にして
stackchan-atamaの状態確認して
スタックチャンに「今日もがんばろう」って喋らせて
スタックチャンで写真撮って
スタックチャンの音量上げて
```
