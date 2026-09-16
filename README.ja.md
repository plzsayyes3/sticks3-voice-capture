# StickS3 Voice Capture

*[English README](README.md)*

M5Stack StickS3をベースにした、PLAUD的なオフラインファーストの音声メモ端末です。
首から下げてボタンを押して話すだけ — 録音にスマホもネットワークも不要です。
後で任意のタイミングでWi-Fi同期すれば、要約なしのverbatim文字起こしが自動でノートに入ります。

```text
StickS3                          Mac(local-receiver)
  トップボタン → 録音             POST /v1/recordings
  Opus/Ogg → 内蔵Flash            durable write + SHA-256 idempotency
                                        ↓
  横ボタン(短押し)                 whisper.cpp + VAD → verbatim transcript
    → Wi-Fi Sync ───────────────→      ↓
      LAN(mDNSホスト名)           mynotebook/00_inbox(要約なし)
      → 圏外時はTailscale Funnel(HTTPS)
```

## 現状

端末内録音 → Wi-Fi Sync → ローカル文字起こし → ノート投入のパイプラインは、
実機でend-to-endに動作確認済みです。Device Recording Acceptance Gate全11項目
(build、OTAサイズ、パーティションレイアウト、短時間/複数録音、`.ogg` finalize、
実再生、reboot後保持、20分連続録音、強制reset耐性、`.part`回収)に合格しています。

既知の制約:
- firmwareイメージは2MiB OTAスロットの残り約2%まで来ており、今後の機能追加前に
  `sdkconfig`での容量最適化が必要になる見込みです。
- `light_sleep_enable`はUSB-Serial-JTAGのdebug安定性のため強制的に`false`にして
  います。正確なバッテリー駆動時間を測るには本番向けに戻す必要があります。
- 外出先からの同期は、自分で用意したTailscale FunnelのURLに依存します(下記参照)。
  同梱の中継サービスはありません。

## リポジトリ構成

- `firmware/` — ESP-IDF v5.5.1ファームウェア(ESP32-S3 / StickS3)
  - `components/audio_pipeline` — I2Sキャプチャ→Opusエンコード→書き込みキュー
  - `components/recording_store` — FAT-on-flashストレージ、`.part`→`.ogg`
    finalize、使用容量の問い合わせ
  - `components/wifi_sync` — 横ボタンのWi-Fi Syncクライアント(登録済みネット
    ワークのスキャン、未送信録音のアップロード、LAN優先+圏外時フォールバック)
  - `components/ui_status` — LVGL状態画面(アイコン、バッテリー、ヒント表示)
  - `components/stick_s3_board`、`components/voice_ble` — StickS3基板の
    bring-upとBLE。VoiceStickベースから継承
- `local-receiver/` — アップロードを受信し、whisper.cppで文字起こしして
  `mynotebook/00_inbox`へpushするFlaskサーバー(詳細は個別のREADME参照)
- `scripts/` — flash抽出(`extract-recordings.sh`)とUIアイコン変換
  (`convert-icon.py`)のツール

## ファームウェアのビルド

```bash
cd firmware
cp components/wifi_sync/include/secrets.h.example components/wifi_sync/include/secrets.h
# secrets.hを編集: Wi-Fiネットワーク、STICKS3_DEVICE_TOKEN、STICKS3_RECEIVER_URL(_FALLBACK)
idf.py build
idf.py -p <port> flash
```

`secrets.h`はgitignore対象です — 実際のWi-Fi認証情報と端末共有トークンを保持
するため、コミットされることはありません。CIはプレースホルダーの
`secrets.h.example`に対してビルドするため、実credentialなしでもコンパイル
確認とOTAサイズ予算のチェックができます。

### 外出先からの同期(Tailscale Funnel)

`STICKS3_RECEIVER_URL`(LAN、mDNSホスト名)を先に試し、失敗した場合のみ
`STICKS3_RECEIVER_URL_FALLBACK`を試します。フォールバックのセットアップ:

```bash
tailscale funnel --bg --https=10000 8090
```

これによりMac上のlocal-receiver(ポート8090)が、自分のTailscaleアカウント
経由で公開HTTPSとして公開されます。このエンドポイントを実際に保護している
のは既存のBearer token認証であり、URL自体はそれを持つ誰からでも到達可能な点
に注意してください。

## 受信サーバーの実行

セットアップ、必要環境(ffmpeg、whisper.cpp + VADモデル)、durability/
idempotencyの契約については[local-receiver/README.md](local-receiver/README.md)
を参照してください。

## ソース

インポートしたファームウェアは[`78/voicestick`](https://github.com/78/voicestick)
のcommit `e865d68c1d96411571cbe1501a301ebe3c98f3b3`由来です。元のMITライセンス
とcopyright表記は[LICENSE](LICENSE)にそのまま残しています。

Sync queueとサーバー契約の設計は[`guzus/open-plaud`](https://github.com/guzus/open-plaud)
を参考に検討しました。そのOgg/Opus writerを設計リファレンスとして使い、本
リポジトリでは既存のVoiceStickエンコーダーを中心に独自にlocal packet writer
を実装しています。
