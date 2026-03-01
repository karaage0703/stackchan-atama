# stackchan-atama

アタマだけスタックチャン — M5Stack単体で動く顔+音声スタックチャン（サーボ不要）

## コンセプト

体（サーボ）なし、アタマ（画面+スピーカー）だけのスタックチャン。M5Stack 1台あれば動く。

- WiFi HTTP API で PC から制御
- 外部から音声データ（WAV）を受け取って再生 + 口パク
- 表情変更（happy, sad, angry, sleepy, doubt, neutral）
- TTS サーバー（VOICEVOX 等）の URL を自由に指定可能

## 対応デバイス

M5Unified ベースなので以下のデバイスで動作：

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
| `GET /speech?say=text&tts_url=http://...` | GET | 外部 TTS で音声合成して再生 |
| `GET /setting?volume=180` | GET | 音量等の設定変更 |
| `GET /status` | GET | 状態取得 |

## 技術スタック

- [PlatformIO](https://platformio.org/) (Arduino framework)
- [M5Unified](https://github.com/m5stack/M5Unified)
- [m5stack-avatar](https://github.com/stack-chan/m5stack-avatar)
- [ESP8266Audio](https://github.com/earlephilhower/ESP8266Audio)

## セットアップ

TODO

## ライセンス

MIT License
