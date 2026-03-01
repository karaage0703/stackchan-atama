# stackchan-atama

アタマだけスタックチャン — M5Stack 単体で動く顔+音声スタックチャン（サーボ不要）

## コンセプト

体（サーボ）なし、アタマ（画面+スピーカー）だけのスタックチャン。M5Stack 1 台と USB ケーブルがあれば動く。

- **USB シリアルで PC から制御**（WiFi 不要）
- PC から WAV データを送信 → スピーカー再生 + 口パク
- 表情変更（happy, sad, angry, sleepy, doubt, neutral）
- TTS は PC 側で自由に選択（ローカル VOICEVOX 等）
- パイプライン再生対応（文を分割して順次送信、最初のチャンクを即座に再生開始）

## 動作確認済みデバイス

- **M5Stack CoreS3** — 推奨。USB CDC、PSRAM搭載
- **M5Stack Core（初代/Basic）** — 動作確認済み。CP2104 UART、PSRAM無し

## シリアルコマンド

USB シリアル経由で以下のテキストコマンドを送信できます（改行区切り）：

| コマンド | 説明 | レスポンス例 |
|----------|------|-------------|
| `STATUS` | 状態取得 | `{"status":"online","wifi":false,"playing":false,"queued":0}` |
| `FACE:happy` | 表情変更 | `{"status":"ok","expression":"happy"}` |
| `VOLUME:200` | 音量変更(0-255) | `{"status":"ok","volume":200}` |
| `WAV:12345` | WAV バイナリ受信 | `READY` → バイナリ送信 → `{"status":"ok","size":12345,"queued":1}` |
| `WIFI:ssid:pass` | WiFi 接続 | `{"status":"ok","ip":"192.168.x.x"}` |

### WAV 送信プロトコル

1. `WAV:<バイト数>\n` を送信
2. ESP32 が `READY\n` を返す
3. WAV バイナリを 1KB チャンク + 5ms 間隔で送信
4. 受信完了で `{"status":"ok","size":<バイト数>,"queued":<キュー数>}` が返る
5. キュー（4 スロット）に入り順次再生される

## HTTP API（WiFi 接続時）

WiFi 接続時は HTTP API も使用可能：

| エンドポイント | メソッド | 説明 |
|---------------|---------|------|
| `GET /status` | GET | 状態取得 |
| `POST /play_wav` | POST | WAV 再生 |
| `GET /face?expression=happy` | GET | 表情変更 |
| `GET /setting?volume=180` | GET | 設定変更 |

## セットアップ

### 必要なもの

- M5Stack Core / Core2 / CoreS3 のいずれか
- USB-C ケーブル
- [PlatformIO CLI](https://docs.platformio.org/en/stable/core/installation.html)（`uv tool install platformio` でもOK）
- [VOICEVOX Engine](https://voicevox.hiroshiba.jp/)（音声合成用、PC 側で起動）

### ビルド & 書き込み

```bash
git clone https://github.com/karaage0703/stackchan-atama.git
cd stackchan-atama

# M5 CoreS3（推奨）
pio run -e m5stack-cores3 -t upload

# M5Stack Core2
pio run -e m5stack-core2 -t upload

# M5Stack Core (初代)
pio run -e m5stack-core -t upload
```

書き込み後、M5Stack の画面にアバターの顔が表示されれば成功。

### PC 側ツール

```bash
cd tools

# 接続確認
uv run stackchan_atama.py status

# 音声合成してスタックチャンで再生
uv run stackchan_atama.py say "こんにちは"

# パイプラインモード（長文を分割して高速再生）
uv run stackchan_atama.py say "こんにちは！今日もいい天気ですね。" --pipeline

# 表情変更
uv run stackchan_atama.py face happy

# 音量変更
uv run stackchan_atama.py volume 200

# 話者変更
uv run stackchan_atama.py say "おはよう" --voice 3

# シリアルポート指定
uv run stackchan_atama.py --port /dev/ttyACM1 status
```

### VOICEVOX Engine の起動

PC 側で VOICEVOX Engine が動いている必要があります：

```bash
# Docker
docker run --rm -p 50021:50021 voicevox/voicevox_engine:cpu-latest

# または直接インストール
# https://voicevox.hiroshiba.jp/ からダウンロード
```

### WiFi 設定（オプション・動作未確認）

WiFi は必須ではありません。USB シリアルだけで全機能が使えます。

WiFi を使いたい場合はシリアルコマンドで接続：
```
WIFI:MySSID:MyPassword
```

> **注意**: WiFi 機能は実装済みですが、まだ動作確認を行っていません。

## AI エージェント連携

SKILL.md を参照してください。Claude Code や borot 等の AI エージェントから `stackchan_atama.py` を呼び出してスタックチャンを制御できます。

## 技術スタック

- [PlatformIO](https://platformio.org/) (Arduino framework)
- [M5Unified](https://github.com/m5stack/M5Unified) — M5Stack デバイス統合ライブラリ
- [m5stack-avatar](https://github.com/stack-chan/m5stack-avatar) — アバター顔描画ライブラリ
- [ESP8266Audio](https://github.com/earlephilhower/ESP8266Audio) — 音声再生ライブラリ

## 謝辞

このプロジェクトは、以下の素晴らしいプロジェクトと開発者の方々の成果に基づいています：

- **[ｽﾀｯｸﾁｬﾝ (stack-chan)](https://github.com/stack-chan/stack-chan)** — ししかわさん ([@meganetaaan](https://github.com/meganetaaan))
- **[AI_StackChan2](https://github.com/robo8080/AI_StackChan2)** — robo8080 さん
- **[M5Unified](https://github.com/m5stack/M5Unified)** — らびやんさん ([@lovyan03](https://github.com/lovyan03))
- **[VOICEVOX](https://voicevox.hiroshiba.jp/)** — ヒホさん ([@Hiroshiba](https://github.com/Hiroshiba))
- **スタックチャンコミュニティ** — アイデア・知見を共有してくださっている全ての方々

## ライセンス

MIT License
