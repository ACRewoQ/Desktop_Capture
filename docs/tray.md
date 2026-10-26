# 常駐アプリ（dc-tray）

P5-B。通知領域（タスクバーの右下）に出る小さな常駐アプリで、エンジン（mixd）とキャプチャ（capd）をウィンドウなしで起動して見張り、ミキサーの画面（dc-ui）を開き、ログオン時の起動を切り替えます。Win32 だけで作り、Qt も WebView も使いません（常駐の負担を小さくするため）。

## 使い方

`scripts\package.ps1` で Release をビルドし、実行に要るもの一式を `build\package\DesktopCapture` にまとめます（`-Zip` で zip も、`-Install` で `%LOCALAPPDATA%\Programs\DesktopCapture` へ入れ替え）。フォルダーの中の `dc-tray.exe` を起動すると、mixd と capd を起動してミキサーの画面を開きます。

| 操作 | 動き |
|---|---|
| アイコンを左クリック | ミキサーの画面を開く（開いていれば前に出す） |
| 右クリック | メニュー：mixd・capd の状態、エンジンとキャプチャを再起動、ログオン時に起動、ログのフォルダーを開く、設定ファイルを開く、終了 |
| 通知をクリック | ログのフォルダーを開く |
| `dc-tray.exe` をもう一度起動 | 動いている方がミキサーの画面を開く（2 つ目の常駐はしない） |

アイコンは 3 本のフェーダーで、つまみの色が状態を表します（緑 = 動作中、黄 = 起動中・立ち上げ直し中、赤 = 起動できない、灰 = 停止）。

## 見張りと立ち上げ直し

- mixd・capd が頼んでいないのに終わったら、通知（終了コード。0xC0000005 などは言葉も添える）を出して立ち上げ直します。待ち時間は 1 秒、3 秒、10 秒と延び、4 回続けて落ちたら（30 秒以上動いた回は数え直し）あきらめて赤にします。メニューの「再起動」で数え直して起動します。
- capd はエンジンにつなぎ直す作りなので、mixd だけ立ち上げ直しても capd はそのまま動き続けます（録画中なら、その録画はそこで閉じます。G3 で決めたとおり）。
- 前の dc-tray が mixd・capd を止めずに終わっていたら（強制終了など）、同じフォルダーの mixd.exe・capd.exe を引き継いで見張ります。別の場所の mixd.exe（ビルドフォルダーのテスト用など）が動いているときは、ポートがぶつかるので起動せず「別の場所から起動されています」と知らせます。

## 止め方

コンソールがないので Ctrl+C は送れません。代わりに名前付きイベント `Local\DesktopCapture.stop.<mixd|capd>` を mixd・capd に渡し（`--stop-event`）、それをセットすると Ctrl+C と同じように止まります。順番は capd（録画中なら録画ファイルを閉じる）、mixd の順で、15 秒で止まらなければ強制終了します。Windows のシャットダウン・サインアウトのときも同じ順で止めます（各 4 秒）。終了時は dc-tray が開いたミキサーの画面も閉じます。

## ログオン時の起動

メニューの「ログオン時に起動」、または `dc-tray --autostart on|off|status`。`HKCU\...\CurrentVersion\Run` の `DesktopCapture` に `"<フォルダー>\dc-tray.exe" --background` を書きます（管理者の権限は不要、ログオン時はミキサーの画面を開かない）。Windows の「設定 > アプリ > スタートアップ」でオフにされると `Explorer\StartupApproved\Run` に印が付くので、メニューには「Windows の設定でオフ」と出し、チェックを付け直すと印も消します。フォルダーを動かしたら付け直します（メニューに「別のフォルダーのもの」と出ます）。

## ファイル

| 場所 | 中身 |
|---|---|
| `%LOCALAPPDATA%\DesktopCapture\logs\mixd.log`・`capd.log` | 標準出力とエラー。起動のたびに `.1.log`〜`.4.log` へずらし、前の 4 回分を残す |
| `%LOCALAPPDATA%\DesktopCapture\logs\tray.log` | dc-tray の記録（起動、終了コード、立ち上げ直し、停止） |
| `%LOCALAPPDATA%\DesktopCapture\tray-status.txt` | 今の状態（`tray_pid`、`mixd.state`・`.pid`・`.restarts`・`.last_exit`）。試験スクリプトが読む |
| `%APPDATA%\DesktopCapture\tray.txt` | 任意の設定。`mixd_args`・`capd_args`・`ui_args`（足す引数）、`capd = off`（音声だけ）。「再起動」で反映 |

## コマンドライン

```
dc-tray [--background] [--bin-dir DIR] [--mixd-args "..."] [--capd-args "..."] [--no-capd]
dc-tray --autostart on|off|status
dc-tray --open | --quit
```

`--quit` は動いている dc-tray に終了を頼み、mixd・capd まで止まるのを待って戻ります（`package.ps1 -Install` が入れ替えの前に使う）。実行ファイルは dc-tray と同じフォルダー、なければビルドフォルダーの並び（`build\windows\apps\<mixd|capd|ui>\<構成>\`）から探します。

## 構成（apps/tray）

| ファイル | 役割 |
|---|---|
| `src/tray_core` | Windows に依存しない判断：立ち上げ直しの間隔とあきらめ、コマンドラインの引用符、tray.txt、StartupApproved の値、ログのずらし、状態ファイル。`tests/test_tray.cpp` |
| `src/child` | 1 つの子プロセス：ログへの出力、スレッドプールでの終了待ち（ウィンドウにメッセージを送る）、停止イベント、引き継ぎ |
| `src/autostart` | Run キーと StartupApproved |
| `src/icon` | アイコンを実行時に描く（画像ファイルなし） |
| `main.cpp` | 通知領域のアイコン、メニュー、通知、二重起動の受け渡し、コマンドライン |

実機の試験は `scripts\p5b-tray.ps1`（起動、二重起動、UI、mixd・capd を強制終了して立ち上げ直し、録画中の終了でファイルが壊れないこと、きれいな停止、起動できない mixd であきらめること、ログオン時の起動の切り替え）。
