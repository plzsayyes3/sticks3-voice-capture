# TF HAT SDカード保存 — 調査・実装記録（2026-09-26）

StickS3 + M5Stack TF HAT（SKU 9551）で録音をSDカードに保存できるようにした作業の記録。
実機が手元にない状態で実装まで進めたため、**実機での確認が済んでいない項目**を最後にまとめている。

## 1. 発端: ArduinoではSDカードに新規ファイルを作れなかった

- Arduino（M5Stack core 3.2.5 / Arduino-ESP32 3.2.1 / IDF 858a988d）では、読み取りはできるが
  `SD.open(path, FILE_WRITE)` が失敗し、`errno=19`（ENODEV）になった。
- `errno=19` はFatFsの `FR_NOT_READY` / `FR_NOT_ENABLED` / `FR_NO_FILESYSTEM` を
  ESP-IDFのVFSが変換した値（FatFsのエラー番号19とは別物）。
- Arduinoの `sd_diskio.cpp` は、`SD.begin(-1, …)` のCS=-1を `uint8_t` の255として扱うため、
  CSピンの切り替え操作は何もしない。CSをHAT側で固定する構成との相性が疑わしいが、根本原因は特定していない。

## 2. ESP-IDFのSDSPIドライバでは作成・書き込みできた（実機確認済み）

テストファーム: [`tools/sd-create-test/`](../tools/sd-create-test/)（ESP-IDF v5.5.1）

- 設定: SPI（SCK=8 / MISO=1 / MOSI=0）、`SDSPI_SLOT_NO_CS`、1MHz、`format_if_mount_failed=false`
- 結果:
  - マウントOK（SDHC 29,818MB）、読み取りOK、既存ファイルの追記オープンOK、`mkdir` OK
  - POSIX API（`open` / `write` / `fsync` / `close`）での新規作成と読み戻しは7回とも成功
  - FatFsを直接呼ぶ場合、`FIL` をスタックに置くと `f_sync` がCRCエラー（`0x109`）で毎回失敗する。
    静的領域に置けば成功する。**POSIX API経由で使う限りは問題ない**（`recording_store` はPOSIX経由）。
- 電源: PM1の5V昇圧（BOOST_EN）をオフにしてもSDカードは動いた。HATは3.3Vで給電されている。

## 3. 録音ファームへの組み込み（ブランチ構成）

push済みのブランチはない（すべてローカル）。

```
origin/main (9e7926a)
└ feature/sd-card-storage            SDカード保存＋内蔵フラッシュへのフォールバック
  └ feature/smaller-internal-storage  本体の録音領域を2.56MBに縮小し、OTAスロットを拡張
    └ feature/split-on-silence        30分を過ぎたら次の無音でファイルを区切る
```

### feature/sd-card-storage

- `recording_store`:
  - SDカードをSPI3（LCDがSPI2を使用）、CSなし、20MHzでマウントし、`/sdcard/REC` に保存する。
  - SDカードは絶対にフォーマットしない。
  - 起動時にSDカードを使えなければ、今までどおり内蔵フラッシュ（`/recordings`）に保存する。
  - 切り替えは起動時の1回だけ。録音の途中でSDの書き込みに失敗した場合、その録音は中断し、`.part` が残る。
- `wifi_sync`:
  - まずSDカードの未送信分を送る。
  - SDカードに未送信が1件もないときだけ、内蔵フラッシュを一時的にマウントして、SD導入前の未送信分を送る。
    送り終わったらアンマウントする。
- `audio_pipeline_state.c`: 録音番号をファイル名から読むように修正した（SD保存時にBLEへ送る番号が0になっていた）。

### feature/smaller-internal-storage

| 領域 | 変更前 | 変更後 |
|---|---|---|
| ota_0 / ota_1 | 0x200000（2.00MB） | 0x2b0000（2.69MB） |
| storage | 0x3f0000（3.94MB） | 0x290000（2.56MB、20kbpsで約17分） |
| アプリの空き | 約5KB | 約710KB |

- CIのサイズ上限、READMEの注意書き、`wifi_sync.c` のコメントもあわせて更新した。

### feature/split-on-silence

- 録音が30分を過ぎたら、最初の無音（約0.6秒）でOggファイルを確定し、すぐ次のファイルで録音を続ける。
  35分までに無音がなければ強制的に区切る。
- 無音判定: 60msごとのRMSが、追跡している雑音の基準の2倍＋40以下なら「静か」とする。
  基準は、音量が下がればすぐ追従し、上がるときはゆっくり追従する（`NOISE_FLOOR_RISE=0.001`、時定数約60秒）。
- 実際の24分の録音で、区切りを2分後にしたシミュレーションをした。10回区切って強制は0回、
  すべての区切りが無音（区切り直前0.6秒の音量が前後30秒の中央値の半分以下）だった。
  `0.004` だと小さな声の途中で区切ることがあったため、`0.001` にした。
- 書き込みキューを約10秒（170パケット）に増やしてPSRAMに置いた。書き込みタスクのスタックは8KBにした。

## 4. Mac側の受信サーバー（local-receiver）

### 見つかった問題と対処（2026-09-26）

- **9/23 22時（JST）以降、文字起こしが1件も進んでいなかった**（92件中26件が未処理）。
  - 原因: 受信サーバーを `launchctl submit` で起動しており、PATHが `/usr/bin:/bin:/usr/sbin:/sbin` だけだった。
    そのため `/opt/homebrew/bin` の `ffmpeg` と `whisper-cli` が見つからなかった。
  - エラーは標準出力・標準エラーとも `/dev/null` に捨てられていて、見えていなかった。
- 対処: LaunchAgentとして登録し直した。
  - `~/Library/LaunchAgents/com.plzsayyes3.sticks3-localreceiver.plist`（リポジトリには入れていない）
  - PATHに `/opt/homebrew/bin` を追加、`RunAtLoad` と `KeepAlive` を設定
  - ログは `~/sticks3-voice-capture-data/receiver.log` に出す
  - `MAX_UPLOAD_BYTES=67108864`（64MB、約7時間ぶん。以前の上限8MBは約53分ぶん）
  - 起動時に未処理の録音を自動で拾い直す（`reprocess_pending_recordings`）

操作:

```bash
tail -f ~/sticks3-voice-capture-data/receiver.log
```

```bash
launchctl kickstart -k gui/$(id -u)/com.plzsayyes3.sticks3-localreceiver
```

### 受信後の流れ

受信（トークン確認、`recordings/` に保存してディスクへ確定、SHA-256で重複を判定）→ 201を返し、本体は録音を削除する
→ 文字起こし（ffmpegで16kHzのWAVに変換 → whisper-cli large-v3-turbo＋VAD、1件ずつ処理）
→ `notes/` にMarkdownを作成 → GitHub `plzsayyes3/mynotebook` の `00_inbox/` へ追加 → `done/` に完了を記録する。

## 5. 実機が戻ったらやること

### 書き込み手順（パーティション変更を含む）

1. **今のファームのうちに**、Wi-Fi同期で内蔵フラッシュの録音を送り切る（次の手順で消える）。
2. USBで書き込む（OTAではできない）。

   ```bash
   idf.py -p /dev/cu.usbmodem101 flash
   ```

3. 新しい録音領域を消去する。消さないと旧データの一部が残ってマウントに失敗し、SDカードがないときに録音できない。

   ```bash
   python -m esptool --chip esp32s3 -p /dev/cu.usbmodem101 erase_region 0x570000 0x290000
   ```

### 未確認の項目

- [ ] 実際の録音がSDカードの `REC/` に `.ogg` として保存されるか（20MHzでの連続書き込み、`.part` → `.ogg` の名前変更）
- [ ] Wi-Fi同期でSDカードからアップロードして削除できるか
- [ ] SDカードに未送信がないとき、内蔵フラッシュの未送信分を送れるか
- [ ] 30分超の録音が無音で区切られるか、区切り目で音が欠けたりノイズが入ったりしないか
      （短時間で試すなら、区切りを数分にしたテスト用ビルドを作る）
- [ ] 区切った各ファイルがMacで別々のノートになるか
- [ ] SDカードのFirst Aidと、テスト用ファイル（`IDFT0926.TST`、`IDFS0926.TST`、`IDFF0926.TST`、`IDFD0926/`）の削除

## 6. 関連ファイル・場所

- ワークツリー: `/private/tmp/sticks3-sd-card-storage`（`/private/tmp` なのでMacの再起動で消えることがある。
  ブランチとコミットはメインリポジトリの `.git` にあるので残る）
- SD対応前のフラッシュのバックアップ（0x0〜0x410000。旧パーティション表、ブートローダー、両OTAスロット）:
  `~/sticks3-voice-capture-data/firmware-backups/2026-09-26_pre-sd_flash_0x0_0x410000.bin`

  ```bash
  python -m esptool --chip esp32s3 -p PORT write_flash 0x0 ~/sticks3-voice-capture-data/firmware-backups/2026-09-26_pre-sd_flash_0x0_0x410000.bin
  ```

  このバックアップは旧パーティション表なので、書き戻すと `storage` は旧位置（0x410000〜）の扱いに戻る。
- テストファーム `tools/sd-create-test/` は、フルの `flash` ではパーティション表も書き込む。
  このブランチの表と同じなので、新しい割り当てにしてから使うこと。
- メインのチェックアウト（`feature/sticks3-audio-state-ux`）の作業途中の変更には触れていない。
  そちらには `wifi_sync.c` の変更もあるので、マージするときは衝突に注意する。
