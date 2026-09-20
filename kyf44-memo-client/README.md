# KYF44 Memo Capture

Minimal Android client for using the KYOCERA KYF44 as a dedicated Japanese
text-capture device.

The phone does not contain a GitHub token. It only knows the receiver URL and a
device-specific bearer token. The Mac receiver owns GITHUB_TOKEN and writes to
plzsayyes3/mynotebook/00_inbox.

## Capture flow

1. Open the app. The memo field is focused immediately.
2. Type with the KYF44 Japanese IME.
3. Move focus to Send and press the center key, or press the call key while the
   app is open.
4. The text is first written and fsynced under the app private storage.
5. The app POSTs it to /v1/memos.
6. If offline, WorkManager keeps the local memo and retries when networking is
   available.
7. The receiver fsyncs another durable copy, replies 201, then pushes the
   verbatim text to mynotebook/00_inbox in the background.

A memo is therefore never dependent on GitHub being reachable at capture time.

## Receiver setup

In local-receiver/.env:

    KYF44_DEVICE_TOKEN=<random device-only secret>
    GITHUB_TOKEN=<fine-grained token held on the Mac only>

Generate the device token, for example:

    python3 -c 'import secrets; print(secrets.token_urlsafe(32))'

Restart local-receiver/app.py after changing .env.

For the app Receiver URL, use the same externally reachable HTTPS receiver base
URL used by the StickS3 off-LAN path. The app appends /v1/memos automatically.

## Build

Open kyf44-memo-client in Android Studio, let Gradle sync, then build the debug
APK. The project intentionally uses standard Android widgets rather than a
touch-first UI framework.

Command-line build if Gradle 8.9 is installed:

    cd kyf44-memo-client
    gradle :app:assembleDebug

APK:

    app/build/outputs/apk/debug/app-debug.apk

## Install on KYF44

Enable developer options / USB debugging, connect by USB, then:

    adb devices
    adb install -r app/build/outputs/apk/debug/app-debug.apk

On first launch, enter:

- Receiver: an https:// base URL or full /v1/memos URL
- Device token: the value of KYF44_DEVICE_TOKEN

The token is encrypted with an AES/GCM key held by Android Keystore before it is
written to SharedPreferences. It is not compiled into the APK.

## Current keypad behavior

- Japanese entry: standard KYF44 IME in the memo field
- D-pad: move between memo / send / settings
- Center key: activate the focused button
- Call key while the app is foreground: capture/send
- Menu key: open receiver settings

The call-key shortcut is intentionally separate from the center key so Japanese
conversion/selection is not disrupted.
