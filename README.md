# stackchan-atama

アタマだけスタックチャン — M5Stack 単体で動く顔+音声スタックチャン（サーボ不要）

## コンセプト

体（サーボ）なし、アタマ（画面+スピーカー）だけのスタックチャン。M5Stack 1 台あれば動く。

- WiFi HTTP API で PC から制御
- **外部から音声データ（WAV）を受け取って再生 + 口パク**
- 表情変更（happy, sad, angry, sleepy, doubt, neutral）
- TTS サーバー（VOICEVOX 等）は PC 側で自由に選択

### AI_StackChan2 との違い

| | AI_StackChan2 | stackchan-atama |
|---|---|---|
| TTS | クラウド API（ttsquest）に依存 | PC 側で自由に選択（ローカル VOICEVOX 等） |
| 音声 | M5Stack がクラウドから取得 | PC が生成した WAV を M5Stack に送信 |
| ChatGPT | M5Stack から直接 API 呼び出し | PC 側で処理（AI エージェント連携可） |
| サーボ | あり | なし（アタマだけ） |
| 設定 | SD カード / Web UI | SD カード / SmartConfig |

## 対応デバイス

[M5Unified](https://github.com/m5stack/M5Unified) ベースなので以下のデバイスで動作：

| デバイス | 画面 | スピーカー | マイク | 備考 |
|----------|------|-----------|--------|------|
| M5Stack Core (Basic) | 320x240 | あり | なし | |
| M5Stack Core2 | 320x240 | あり | あり | |
| **M5 CoreS3** | 320x240 | あり | あり | **推奨** |
| ATOM S3 | 128x128 | なし | なし | 顔表示のみ |

## HTTP API

| エンドポイント | メソッド | 説明 |
|---------------|---------|------|
| `GET /` | GET | 接続確認 |
| `POST /play_wav` | POST | WAV バイナリを受け取りスピーカー再生 + 口パク |
| `GET /face?expression=happy` | GET | 表情変更 |
| `GET /setting?volume=180` | GET | 音量等の設定変更 |
| `GET /status` | GET | 状態取得（JSON） |

## セットアップ

### 必要なもの

- M5Stack Core / Core2 / CoreS3 のいずれか
- USB-C ケーブル
- [PlatformIO CLI](https://docs.platformio.org/en/stable/core/installation.html)

### ビルド & 書き込み

```bash
# リポジトリをクローン
git clone https://github.com/karaage0703/stackchan-atama.git
cd stackchan-atama

# M5 CoreS3 の場合
pio run -e m5stack-cores3 -t upload

# M5Stack Core (初代) の場合
pio run -e m5stack-core -t upload

# M5Stack Core2 の場合
pio run -e m5stack-core2 -t upload
```

### WiFi 設定

**方法 1: SD カード**

SD カードに `wifi.txt` を作成（1 行目: SSID、2 行目: パスワード）：
```
MyWiFiSSID
MyWiFiPassword
```

**方法 2: SmartConfig**

SD カードがない場合、[ESP-Touch](https://www.espressif.com/en/products/software/esp-touch/overview) アプリで設定。

### PC 側の使い方

PC 側の制御スクリプトは `tools/` にあります。依存ライブラリなし（Python 標準ライブラリのみ）。

```bash
cd tools

# ローカル VOICEVOX で音声合成してスタックチャンで再生
uv run stackchan_atama.py say "こんにちは"

# 表情変更
uv run stackchan_atama.py face happy

# 状態確認
uv run stackchan_atama.py status

# 音量変更
uv run stackchan_atama.py setting --volume 200

# IP アドレスを指定
uv run stackchan_atama.py --ip 192.168.1.100 say "テスト"
```

PC 側で VOICEVOX Engine が動いている必要があります：
```bash
# Docker で VOICEVOX Engine を起動
docker run --rm -p 50021:50021 voicevox/voicevox_engine:cpu-latest
```

## 技術スタック

- [PlatformIO](https://platformio.org/) (Arduino framework)
- [M5Unified](https://github.com/m5stack/M5Unified) — M5Stack デバイス統合ライブラリ
- [m5stack-avatar](https://github.com/stack-chan/m5stack-avatar) — アバター顔描画ライブラリ
- [ESP8266Audio](https://github.com/earlephilhower/ESP8266Audio) — 音声再生ライブラリ

## 謝辞

このプロジェクトは、以下の素晴らしいプロジェクトと開発者の方々の成果に基づいています：

- **[ｽﾀｯｸﾁｬﾝ (stack-chan)](https://github.com/stack-chan/stack-chan)** — ししかわさん ([@meganetaaan](https://github.com/meganetaaan)) が生み出した、手乗りサイズのコミュニケーションロボット。スタックチャンというコンセプトと m5stack-avatar ライブラリがなければ、このプロジェクトは存在しません
- **[AI_StackChan2](https://github.com/robo8080/AI_StackChan2)** / **[M5Unified_StackChan](https://github.com/robo8080/M5Unified_StackChan)** — robo8080 さんによる AI スタックチャン実装。HTTP API 設計、AudioOutputM5Speaker の実装パターン、口パク（リップシンク）の仕組みなど、多くの部分を参考にしました
- **[M5Unified](https://github.com/m5stack/M5Unified)** — らびやんさん ([@lovyan03](https://github.com/lovyan03)) による M5Stack デバイス統合ライブラリ。複数デバイスへの対応を実現する基盤です
- **[VOICEVOX](https://voicevox.hiroshiba.jp/)** — ヒホさん ([@Hiroshiba](https://github.com/Hiroshiba)) による無料の音声合成ソフトウェア
- **スタックチャンコミュニティ** — アイデア、作例、知見を共有してくださっている全ての方々

## ライセンス

MIT License
