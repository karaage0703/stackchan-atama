# stackchan-atama セットアップ用プロンプト

以下のプロンプトをClaude Code等のAIエージェントに入力すると、初回セットアップを対話的に進められます。

---

## プロンプト

```
stackchan-atama（アタマだけスタックチャン）のセットアップを手伝ってください。

### 環境
- デバイス: M5Stack CoreS3（または Core2 / Core）
- OS: （自分のOSを書く: Ubuntu / macOS / Windows）
- USB接続済み

### やりたいこと
1. PlatformIO CLIのインストール確認
2. ファームウェアのビルド＆書き込み
3. シリアル接続の確認
4. VOICEVOX Engineの起動確認
5. 音声テスト

### 手順

#### Step 1: PlatformIO CLI
`pio --version` で確認。なければ `uv tool install platformio` でインストール。

#### Step 2: ファームウェア書き込み
```bash
cd stackchan-atama

# CoreS3の場合
pio run -e m5stack-cores3 -t upload

# Core2の場合
pio run -e m5stack-core2 -t upload

# Core（初代）の場合
pio run -e m5stack-core -t upload
```
画面にアバターの顔が出れば成功。

#### Step 3: 接続確認
```bash
cd tools
uv run stackchan_atama.py status
```
`{"status":"online",...}` が返ればOK。

ポートが見つからない場合:
- Linux: `ls /dev/ttyACM*` または `ls /dev/ttyUSB*`
- macOS: `ls /dev/cu.usbmodem*`
- Windows: デバイスマネージャーでCOMポート確認
- ポート指定: `uv run stackchan_atama.py --port /dev/ttyACM1 status`

#### Step 4: VOICEVOX Engine
```bash
# Dockerで起動（推奨）
docker run --rm -p 50021:50021 voicevox/voicevox_engine:cpu-latest

# 起動確認
curl http://localhost:50021/version
```

#### Step 5: 音声テスト
```bash
cd tools

# シンプルな発話
uv run stackchan_atama.py say "こんにちは"

# パイプライン再生（長文向け）
uv run stackchan_atama.py say "こんにちは！今日もいい天気ですね。" --pipeline

# 表情変更
uv run stackchan_atama.py face happy

# 音量調整
uv run stackchan_atama.py volume 200
```

### トラブルシューティング

- **Permission denied（Linux）**: `sudo usermod -aG dialout $USER` して再ログイン
- **ポートが見つからない**: USBケーブルがデータ転送対応か確認（充電専用ケーブルはNG）
- **VOICEVOX接続エラー**: `docker ps` でコンテナが起動しているか確認
- **音が鳴らない**: `uv run stackchan_atama.py volume 255` で最大音量に設定
- **シリアルポートが他プロセスに掴まれている**: `lsof /dev/ttyACM0` で確認、`fuser -k /dev/ttyACM0` で解放

各ステップの結果を見せてくれれば、問題があれば一緒にデバッグします。
```
