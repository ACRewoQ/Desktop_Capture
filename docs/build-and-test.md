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
| Configure | `cmake --preset windows`。初回は vcpkg が asio・msgpack・Catch2 を取得・ビルドします（数分） |
| Build | 全ターゲットをビルド（既定は Debug。`-Config Release` も可） |
| ctest | 単体テストと結合テスト（47 件）。モックサーバーと複数クライアントの TCP 通信、共有メモリのリングを含む |
| Smoke | 別プロセス同士の確認：3 プロセスの起動、モックサーバー ⇔ dc-cli、dc-shm-tone の書き手 ⇔ 読み手 |

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
| `apps\capd\Debug\capd.exe` | キャプチャー（P0 では起動して待つだけ） |
| `apps\ui\Debug\dc-ui.exe` | UI（P0 では Qt なしの仮の実行ファイル） |
| `tools\mock_server\Debug\dc-mock-server.exe` | モックサーバー（偽エンジン） |
| `tools\dc_cli\Debug\dc-cli.exe` | テスト用クライアント |
| `tools\shm_tone\Debug\dc-shm-tone.exe` | 共有メモリ音声の書き手・読み手 |
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
& $cli watch --meters                 # 差分・イベント・メーターを表示し続ける（Ctrl+C で終了）
& $cli schema --markdown              # パラメーター一覧（docs/spec/protocol-params.md の元）
```

`watch` を開いたまま別のターミナルから `set` すると、差分がすぐ届くのを確認できます。

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
sudo apt install cmake ninja-build g++ libasio-dev libmsgpack-cxx-dev catch2
cmake --preset linux && cmake --build --preset linux && ctest --preset linux
```

## 6. 依存ライブラリのバージョン固定（任意）

`vcpkg.json` は現在、手元の vcpkg のポートをそのまま使います。バージョンを固定したくなったら、vcpkg を更新したうえで次を実行し、変更をコミットします。

```powershell
& "$env:VCPKG_ROOT\vcpkg.exe" x-update-baseline --add-initial-baseline
```

FFmpeg（avcodec・avformat と NVENC・AMF・QSV）は V3 で追加しました（Windows のみ、初回ビルドは約 10 分）。JUCE・Qt は V5・V7 で導入方法を確かめてから追加します。
