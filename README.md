# stackchan-atama

アタマだけスタックチャン — M5Stack 単体で動く顔+音声スタックチャン（サーボ不要）

## コンセプト

体（サーボ）なし、アタマ（画面+スピーカー）だけのスタックチャン。M5Stack 1 台と USB ケーブルがあれば動く。

- **USB シリアルで PC から制御**（WiFi 不要）
- PC から WAV データを送信 → スピーカー再生 + 口パク
- 表情変更（happy, sad, angry, sleepy, doubt, neutral）
- TTS は PC 側で自由に選択（piper-plus / VOICEVOX 等）
- パイプライン再生対応（文を分割して順次送信、最初のチャンクを即座に再生開始）

### 通常のスタックチャンとの違い

- **stackchan（既存）**: WiFi HTTP API、AI_StackChan2ファームウェア、サーボ付き
- **stackchan-atama（本プロジェクト）**: USBシリアル or WiFi、専用ファームウェア、アタマだけ（サーボなし）

### スキル構成

- `tools/stackchan_atama.py` - 制御CLI（pyserial + requests）
- `src/main.cpp` - M5Stack用ファームウェア（PlatformIO）
- `platformio.ini` - ビルド設定（CoreS3 / Core / Core2 / AtomS3R）

## 動作確認済みデバイス

| デバイス | PSRAM | WAV上限 | カメラ | 備考 |
|----------|-------|---------|--------|------|
| M5Stack CoreS3 | あり | 512KB | あり | 推奨。USB CDC |
| M5Stack Core（初代/Basic） | なし | 80KB | なし | 短い発話向け。CP2104 UART |
| M5Stack Core2 | あり | 未テスト | なし | |
| M5Stack AtomS3R + Atomic Echo Base | あり | 512KB | なし | 小型構成。USB CDC。スピーカーはEcho Base必須 |

> **PSRAMなしデバイスの制約**: M5Stack Core（初代）はPSRAMがないため、80KBを超えるWAVデータは受け付けません（HTTP 400 / シリアル `invalid size`）。長い文章は `--pipeline` オプションで分割送信してください。

## シリアルコマンド

USB シリアル経由で以下のテキストコマンドを送信できます（改行区切り）：

| コマンド | 説明 | レスポンス例 |
|----------|------|-------------|
| `STATUS` | 状態取得 | `{"status":"online","wifi":false,"playing":false,"queued":0}` |
| `FACE:happy` | 表情変更 | `{"status":"ok","expression":"happy"}` |
| `VOLUME:200` | 音量変更(0-255) | `{"status":"ok","volume":200}` |
| `WAV:12345` | WAV バイナリ受信 | `READY` → バイナリ送信 → `{"status":"ok","size":12345,"queued":1}` |
| `WIFI:ssid:pass` | WiFi 接続（NVS に保存、再起動後も自動接続） | `{"status":"ok","ip":"192.168.x.x"}` |
| `WIFI:CLEAR` | 保存済み WiFi 認証情報を消去 | `{"status":"ok","message":"wifi credentials cleared"}` |
| `CAPTURE` | カメラキャプチャ（CoreS3 のみ） | Base64 JPEG データ |

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
| `GET /capture` | GET | カメラキャプチャ（JPEG 画像を返す、CoreS3 のみ） |

## セットアップ

### 必要なもの

- M5Stack Core / Core2 / CoreS3 / AtomS3R + Atomic Echo Base のいずれか
- USB-C ケーブル
- [PlatformIO CLI](https://docs.platformio.org/en/stable/core/installation.html)（`uv tool install platformio` でもOK）
- TTS エンジン（以下のいずれか）:
  - [piper-plus](https://github.com/ayutaz/piper-plus)（デフォルト。サーバー不要、ARM64対応）
  - [VOICEVOX Engine](https://voicevox.hiroshiba.jp/)（`--tts voicevox` で切替）

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

# M5Stack AtomS3R + Atomic Echo Base
pio run -e m5stack-atoms3r -t upload
```

書き込み後、M5Stack の画面にアバターの顔が表示されれば成功（AtomS3R は小画面にアバターが表示されます）。

### udev ルール（Linux、オプション）

USB シリアルのデバイス名を `/dev/stackchan` に固定できます。挿し順でデバイス名が変わる問題を防止。

```bash
sudo cp udev/99-stackchan.rules /etc/udev/rules.d/
sudo udevadm control --reload
# USB ケーブルを抜き差し
ls -la /dev/stackchan  # -> /dev/ttyACMx へのシンボリックリンク
```

設定後は `--port /dev/stackchan` でアクセスできます：
```bash
uv run stackchan_atama.py --port /dev/stackchan status
pio run -e m5stack-cores3 -t upload --upload-port /dev/stackchan
```

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

# piper-plus でオフライン音声合成（VOICEVOX 不要、Raspberry Pi 対応）
uv run stackchan_atama.py --tts piper \
  --piper-bin /path/to/piper \
  --piper-model /path/to/model.onnx \
  say "こんにちは"

# WiFi 設定（NVS に保存される）
uv run stackchan_atama.py wifi --ssid MySSID --password MyPassword

# WiFi 認証情報消去
uv run stackchan_atama.py wifi --clear

# カメラキャプチャ（CoreS3 のみ）
uv run stackchan_atama.py capture -o photo.jpg

# シリアルポート指定
uv run stackchan_atama.py --port /dev/ttyACM1 status
```

### TTS エンジンの設定

デフォルトは piper-plus。環境変数 `STACKCHAN_TTS` で切替可能。

#### piper-plus（デフォルト）

サーバー不要、バイナリ単体で動作。ARM64（Raspberry Pi / DGX Spark）対応。

スクリプトは `tools/piper`（バイナリ）と `models/*.onnx`（モデル）を自動検出します。
シンボリックリンクまたはコピーで配置してください：

```bash
# piper-plus バイナリ
ln -s /path/to/piper-plus/piper/bin/piper tools/piper

# モデルファイル + config
mkdir -p models
ln -s /path/to/piper-plus/models/tsukuyomi-chan-6lang-fp16.onnx models/
ln -s /path/to/piper-plus/models/config.json models/
```

環境変数 `PIPER_BIN` / `PIPER_MODEL` での指定も可能（自動検出より優先）。

#### VOICEVOX（オプション）

```bash
export STACKCHAN_TTS=voicevox

# Docker で起動
docker run --rm -p 50021:50021 voicevox/voicevox_engine:cpu-latest

# 確認
curl http://localhost:50021/version
```

### 環境変数一覧

| 変数 | デフォルト | 説明 |
|------|----------|------|
| `STACKCHAN_TTS` | `piper` | TTS エンジン（`piper` or `voicevox`） |
| `STACKCHAN_IP` | `192.168.1.100` | WiFi モード時の IP アドレス |
| `PIPER_BIN` | `piper` | piper バイナリのパス |
| `PIPER_MODEL` | （なし） | piper モデルファイル（.onnx）のパス |

### WiFi 設定（オプション）

WiFi は必須ではありません。USB シリアルだけで全機能が使えます。

WiFi を使いたい場合はシリアルコマンドまたは PC ツールで接続：
```bash
# シリアルコマンド
WIFI:MySSID:MyPassword

# PC ツール
uv run stackchan_atama.py wifi --ssid MySSID --password MyPassword
```

認証情報は NVS（不揮発メモリ）に保存され、再起動後も自動接続します。消去したい場合は `WIFI:CLEAR` を送信。

> **注意**: ESP32 は 2.4GHz のみ対応です。5GHz の SSID には接続できません。

### カメラキャプチャ（CoreS3 のみ）

CoreS3 に GC0308 カメラユニットを接続すると、画像キャプチャが使えます。

```bash
# シリアル経由
uv run stackchan_atama.py capture -o photo.jpg

# HTTP 経由（WiFi 接続時）
curl -o photo.jpg http://<CoreS3のIP>/capture
```

解像度は QVGA (320x240)。Vision API に送る用途に十分な画質です。

## トラブルシューティング

- **Permission denied（Linux）**: `sudo usermod -aG dialout $USER` して再ログイン
- **ポートが見つからない**: USBケーブルがデータ転送対応か確認（充電専用ケーブルはNG）
- **VOICEVOX接続エラー**: `docker ps` でコンテナ起動確認、または `curl http://localhost:50021/version` で疎通確認
- **音が鳴らない**: `uv run stackchan_atama.py volume 255` で最大音量に設定
- **シリアルポートが掴まれている**: `lsof /dev/ttyACM0` で確認、`fuser -k /dev/ttyACM0` で解放
- **シリアルでWAV転送が失敗する**: `--serial-chunk 256 --serial-delay 0.02` で転送速度を落とす

## AI エージェント連携

SKILL.md を参照してください。Claude Code や borot 等の AI エージェントから `stackchan_atama.py` を呼び出してスタックチャンを制御できます。

## 技術スタック

- [PlatformIO](https://platformio.org/) (Arduino framework)
- [M5Unified](https://github.com/m5stack/M5Unified) — M5Stack デバイス統合ライブラリ
- [m5stack-avatar](https://github.com/stack-chan/m5stack-avatar) — アバター顔描画ライブラリ
- 自前WAVプレーヤー（M5.Speaker.playRaw ベース、ESP8266Audio不要）

## 謝辞

このプロジェクトは、以下の素晴らしいプロジェクトと開発者の方々の成果に基づいています：

- **[ｽﾀｯｸﾁｬﾝ (stack-chan)](https://github.com/stack-chan/stack-chan)** — ししかわさん ([@meganetaaan](https://github.com/meganetaaan))
- **[AI_StackChan2](https://github.com/robo8080/AI_StackChan2)** — robo8080 さん
- **[M5Unified](https://github.com/m5stack/M5Unified)** — らびやんさん ([@lovyan03](https://github.com/lovyan03))
- **[VOICEVOX](https://voicevox.hiroshiba.jp/)** — ヒホさん ([@Hiroshiba](https://github.com/Hiroshiba))
- **[@starf555](https://github.com/starf555)** — AtomS3R + Atomic Echo Base 対応の実装
- **スタックチャンコミュニティ** — アイデア・知見を共有してくださっている全ての方々

## ライセンス

MIT License
