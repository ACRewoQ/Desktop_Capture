# ビルドとテスト（ローカル専用）

このリポジトリには GitHub Actions などの自動テスト（CI）を置きません。ビルドとテストはすべて手元の Windows PC で行います。PR を出す前と、ゲート（G0〜G5）の確認時に、この手順を実行してください。

## 1. 初回だけ行う準備

1. **Visual Studio 2022 または 2026**（Community で可）をインストールし、ワークロード「C++ によるデスクトップ開発」を選びます。MSVC と Windows SDK が入ります。プリセットは見つかった中で最も新しい Visual Studio を使います。
2. **CMake 4.2 以上**をインストールします（VS 2026 を使うには 4.2 以上が必要です）。`winget install Kitware.CMake` で入ります。Visual Studio の「C++ CMake tools for Windows」でも構いません。
3. **Git for Windows** をインストールします。
4. **vcpkg** を任意の場所に置き、環境変数 `VCPKG_ROOT` を設定します。PowerShell で：

   ```powershell
   git clone https://github.com/microsoft/vcpkg C:\dev\vcpkg
   C:\dev\vcpkg\bootstrap-vcpkg.bat -disableMetrics
   [Environment]::SetEnvironmentVariable('VCPKG_ROOT', 'C:\dev\vcpkg', 'User')
   ```

   設定後は PowerShell を開き直してください。
   ※「Developer PowerShell for VS」は `VCPKG_ROOT` を Visual Studio 内蔵の vcpkg に書き換えることがあるので、**通常の PowerShell** を使ってください（Visual Studio ジェネレーターは通常の PowerShell から使えます）。

5. リポジトリを取得します。

   ```powershell
   git clone https://github.com/ACRewoQ/Desktop_Capture C:\dev\Desktop_Capture
   ```

## 2. 一括テスト（推奨）

```powershell
cd C:\dev\Desktop_Capture
.\scripts\test.ps1
```

実行ポリシーで止められる場合は `powershell -ExecutionPolicy Bypass -File .\scripts\test.ps1` で実行します。

スクリプトは次を順に行い、最後に `ALL CHECKS PASSED` と表示されれば合格です。

| 段階 | 内容 |
|---|---|
| Configure | `cmake --preset windows`。初回は vcpkg が asio・msgpack・Catch2・Opus などを取得・ビルドします（数分） |
| Build | 全ターゲットをビルド（既定は Debug。`-Config Release` も可） |
| ctest | 単体テストと結合テスト。モックサーバーと複数クライアントの TCP 通信、共有メモリのリング、LAN メディア伝送（ネットワーク模擬と UDP ループバック）を含む |
| Smoke | 別プロセス同士の確認：3 プロセスの起動、モックサーバー ⇔ dc-cli、dc-shm-tone の書き手 ⇔ 読み手、dc-lan-tone の送信 ⇔ 受信（UDP） |

## 3. 手で動かす場合

```powershell
cmake --preset windows
cmake --build --preset windows-debug
ctest --preset windows-debug
```

Visual Studio で開く場合は、フォルダーを「フォルダーを開く」で開くと `CMakePresets.json` が読み込まれます。

### 生成物の場所

`build\windows\<フォルダー>\Debug\` の下にできます。

| 実行ファイル | 内容 |
|---|---|
| `apps\mixd\Debug\mixd.exe` | 音声エンジン（P0 では起動して待つだけ） |
| `apps\capd\Debug\capd.exe` | キャプチャー（P2 骨格：`capd list`、`capd record`。[capd.md](capd.md)） |
| `apps\ui\Debug\dc-ui.exe` | UI（Qt 6 Quick。Qt が見つからないときは仮の実行ファイル） |
| `tools\mock_server\Debug\dc-mock-server.exe` | モックサーバー（偽エンジン） |
| `tools\dc_cli\Debug\dc-cli.exe` | テスト用クライアント |
| `tools\shm_tone\Debug\dc-shm-tone.exe` | 共有メモリ音声の書き手・読み手 |
| `tools\lan_sim\Debug\dc-lan-sim.exe` | LAN 音声伝送のネットワーク模擬シミュレーター |
| `tools\lan_tone\Debug\dc-lan-tone.exe` | LAN 音声伝送の送受信テスト（実 UDP、PC 間でも使える） |
| `tools\audio_check\Debug\dc-audio-check.exe` | 録音ファイル（WAV / FLAC / MP3 / Ogg）の長さ・レベル・テストトーンの途切れを調べる |
| `tests\Debug\dc_tests.exe` | テスト本体 |

### モックサーバーで遊ぶ

ターミナルを 2 つ開きます。

```powershell
# 1つ目：モックサーバー（受信したメッセージを表示）
.\build\windows\tools\mock_server\Debug\dc-mock-server.exe

# 2つ目：クライアント
$cli = '.\build\windows\tools\dc_cli\Debug\dc-cli.exe'
& $cli snapshot /track/1              # トラック1の全パラメーター
& $cli set /track/1/volume_db -6      # 値の変更
& $cli set /track/2/out/2/target track/1   # ルーティング
& $cli call /mixer/track/add channels=1 name=Guest
& $cli call /capture/record/start
& $cli call /sampler/trigger pad=1   # サンプラー（モックは playing を切り替えるだけ）
& $cli watch --meters                 # 差分・イベント・メーターを表示し続ける（Ctrl+C で終了）
& $cli schema --markdown              # パラメーター一覧（docs/spec/protocol-params.md の元）
```

`watch` を開いたまま別のターミナルから `set` すると、差分がすぐ届くのを確認できます。

### UI をモックサーバーで動かす

`dc-ui` は Qt の DLL を `DC_QT_DIR\bin` から読みます（`scripts\test.ps1` は自動で PATH に足します）。

```powershell
$env:PATH = "$env:DC_QT_DIR\bin;$env:PATH"
.\build\windows\tools\mock_server\Debug\dc-mock-server.exe          # 1つ目
.\build\windows\apps\ui\Debug\dc-ui.exe                             # 2つ目（既定は 127.0.0.1:47800）
.\build\windows\apps\ui\Debug\dc-ui.exe --connect 192.168.1.10 --remote --fullscreen   # リモコンとして
```

| 操作 | 方法 |
|---|---|
| フェーダー | 縦にドラッグ（指を置いた位置から相対）。ダブルタップで 0 dB |
| パン | 横にドラッグ。ダブルタップで中央 |
| LC / HC（ローカット・ハイカット） | タップで入切、縦ドラッグで周波数 |
| M / S | ミュート / ソロ |
| トラックの追加 | 一番右の「+ ステレオ」「+ モノ」（最大 64） |
| 入力の選択 | トラック名をタップしてメニューの「入力を選ぶ」。エンジンのデバイス一覧から選ぶ（多チャンネルのデバイスはモノトラックならチャンネルごと、ステレオなら 2 チャンネルずつ） |
| マスターの出力先 | マスターの名前をタップ |
| 移動・削除 | トラック名をタップしてメニュー（削除は 2 回押し） |
| 録画 | 右上の「録画」ボタン。録画中は経過時間を表示し、もう一度押すと停止 |
| クリップ表示 | メーター上の赤い印をタップで消す |

`dc-ui --selftest --connect 127.0.0.1:<port>` は、つないだ先でトラックの追加・編集・削除、録画の開始・停止、メーター受信、画面の描画を自動で確かめて終了します（つないだ先の状態を変えるので、モックサーバー相手に使います）。

### 本物のエンジンで 3 プロセスを動かす（G2 結合）

`scripts\g2-integration.ps1` が mixd・capd・dc-ui を同時に起動して確かめます。mixd はマスターに同期パルス（`--test-pulse`、QPC 時刻の 2 秒ごとに 50ms の 1kHz）を流し、`dc-av-flash` が同じ瞬間に主モニターの左上を白くします。dc-ui は `--exercise` で、トラック1の入力を一覧から選び、録画を始め、全トラックのフェーダー・パン・フィルターを動かし続けてから録画を止めます。その間の `/system/xruns`、capd の落ちたフレーム数、録画ファイルの音ズレ（`capd avsync`）を見て、`build\g2\integration_<日時>.md` に表を作ります。

```powershell
.\scripts\g2-integration.ps1 -Minutes 5                       # 予行
.\scripts\g2-integration.ps1 -Minutes 60 -Mode exclusive -Block 128
```

試験中は 2 秒ごとにビープが鳴るのでスピーカーを絞り、主モニターの左上を空けておきます。

### 共有メモリ音声を試す

```powershell
# 1つ目：エンジンの代わりに正弦波を書く（マスター 440Hz、トラックN = 220×(N+1) Hz）
.\build\windows\tools\shm_tone\Debug\dc-shm-tone.exe write

# 2つ目：capd の代わりに読む（0.5 秒ごとにレベルを表示）
.\build\windows\tools\shm_tone\Debug\dc-shm-tone.exe read
```

書き手を止めると `writer STALLED`、再起動すると `restarts` が増えて読み続けることを確認できます。

## 4. テストの書き方

- テストは `tests/` に Catch2 で書き、`tests/CMakeLists.txt` の `dc_tests` に追加します。`ctest` が自動で拾います。
- プロトコルにパラメーターを足したら `dc-cli schema --markdown` の出力で `docs/spec/protocol-params.md` を作り直します（`schema_doc_up_to_date` テストが食い違いを検出します）。
- 実機が必要な試験（オーディオデバイス、GPU、RAM 4GB 端末など）は自動テストにせず、手順と測定スクリプトを `docs/` と `scripts/` に置きます。

## 5. Linux での開発（任意）

プロトコル・モックサーバー・共有メモリのライブラリは Windows API に依存しないので、Linux でもビルドとテストができます（Claude のクラウド環境での確認用）。Windows 専用の機能（WASAPI、WGC など）が入るフェーズ 2 以降は Windows が正です。

```bash
sudo apt install cmake ninja-build g++ pkg-config libasio-dev libmsgpack-cxx-dev catch2 libopus-dev libflac-dev
cmake --preset linux && cmake --build --preset linux && ctest --preset linux
```

## 6. 依存ライブラリのバージョン固定（任意）

`vcpkg.json` は現在、手元の vcpkg のポートをそのまま使います。バージョンを固定したくなったら、vcpkg を更新したうえで次を実行し、変更をコミットします。

```powershell
& "$env:VCPKG_ROOT\vcpkg.exe" x-update-baseline --add-initial-baseline
```

FFmpeg（avcodec・avformat と NVENC・AMF・QSV）は V3 で追加しました（Windows のみ、初回ビルドは約 10 分）。Qt 6 は V5 で導入しました（vcpkg ではなく `scripts\install-qt.ps1` でビルド済みを `C:\dev\Qt` に入れ、環境変数 `DC_QT_DIR` で見つけます。見つからなければ Qt を使うターゲットは飛ばします）。JUCE は V7 で導入方法を確かめました（下記の 7 章）。

## 7. JUCE を使うターゲット（VST3 ホスト）

JUCE 9 は vcpkg ではなく CMake の FetchContent で取得します（`cmake/DcJuce.cmake`）。mixd の VST3 ホスト（P3 A1）が使うので、Windows ではオプション `DC_WITH_JUCE` が既定で ON です（`windows` プリセットが `DC_WITH_JUCE=ON` を渡すので、前からある build\windows でも `scripts\test.ps1` でビルドされ、初回は取得とビルドに 10 分ほどかかります）。Linux では既定 OFF で、試すときは次のようにします（X11 などの開発パッケージが必要、プラグインの画面を開く `vst3_probe_test_plugin` は xvfb では通りません）。

```bash
cmake -S . -B build/linux-juce -G Ninja -DDC_WITH_JUCE=ON && cmake --build build/linux-juce
cd build/linux-juce && xvfb-run -a ctest -E vst3_probe_test_plugin
```

V7 の検証用プローブは今もプリセット `windows-juce` で動かせます。

```powershell
.\scripts\vst3-probe.ps1 -SkipScan     # 取得・ビルド・ctest（vst3_* の 3 件）
.\scripts\vst3-probe.ps1               # さらに手元の VST3 プラグインを全部試して out\vst3\report.md に出す
```

検証の結果と申し送りは [docs/verification/v7-vst3-host.md](verification/v7-vst3-host.md) にあります。
