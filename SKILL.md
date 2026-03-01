---
name: xs:stackchan-atama
description: スタックチャン・アタマ（M5Stack単体版）をUSBシリアル経由で制御するスキル。テキスト読み上げ・表情変更・音量調整をPCからコマンド実行。パイプライン再生で高速応答。borotの返答をスタックチャンに喋らせる連携にも対応。「スタックチャンに喋らせて」「stackchan-atama」で使用。
---

# スタックチャン・アタマ制御スキル

USBシリアル経由でスタックチャン・アタマ（M5Stack単体、サーボ不要版）を制御する。テキスト読み上げ、表情変更、音量調整が可能。WiFi不要。

## 通常のスタックチャンとの違い

- **stackchan（既存）**: WiFi HTTP API、AI_StackChan2ファームウェア、サーボ付き
- **stackchan-atama（本スキル）**: USBシリアル、専用ファームウェア、アタマだけ（サーボなし）

## スキル構成

- `tools/stackchan_atama.py` - 制御CLI（pyserial + requests）

## 接続情報

- ポート: `/dev/ttyACM0`（環境変数やオプションで変更可能）
- ボーレート: 921600
- プロトコル: USBシリアル（テキストコマンド + バイナリWAV転送）
- 要件: VOICEVOX Engine がローカルで起動していること（port 50021）

## 実行フロー

### Step 1: 接続確認

```bash
cd [SKILL_DIR] && uv run tools/stackchan_atama.py status
```

`{"status":"online","wifi":false,...}` が返れば接続OK。

### Step 2: テキスト読み上げ（TTS）

```bash
# 通常モード（全文を一括送信）
cd [SKILL_DIR] && uv run tools/stackchan_atama.py say "こんにちは"

# パイプラインモード（文を分割、最初のチャンクを即座に再生開始）
cd [SKILL_DIR] && uv run tools/stackchan_atama.py say "こんにちは！今日もいい天気ですね。散歩に行きましたか？" --pipeline

# 話者変更
cd [SKILL_DIR] && uv run tools/stackchan_atama.py say "おはよう" --voice 3
```

- VOICEVOX ローカルエンジン経由
- `--pipeline` で句読点区切り＋順次送信（長文推奨）
- `--voice` で話者ID変更（デフォルト: 1 = ずんだもん）

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

## 注意点

- USBケーブルで接続する必要がある（WiFiは不要）
- VOICEVOX Engine がローカルで動いている必要がある
- シリアルポートが他のプロセスに掴まれていると接続できない
- 長文は `--pipeline` オプションで分割送信すると体感速度が上がる
- パイプライン再生はキュー（4スロット）に順次投入される

## 使用例

```
スタックチャンにこんにちはって言って
スタックチャンを嬉しい顔にして
stackchan-atamaの状態確認して
スタックチャンに「今日もがんばろう」って喋らせて
```
