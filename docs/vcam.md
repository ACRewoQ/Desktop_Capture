# 仮想カメラ（apps/vcam）— P3 レーン B2

V4 の試作（`prototypes/v4-vcam/`、結果は `docs/verification/v4-virtual-camera.md`）を本体のビルドに移したものです。

## 構成

| もの | 役割 |
|---|---|
| `vcam_mf.dll` | Media Foundation の仮想カメラのソース（Windows 11）。Windows Camera Frame Server（LOCAL SERVICE、セッション0）が読み込む |
| `vcam_ds.dll` | DirectShow のソースフィルター（Windows 10）。softcam（MIT）を取得して、自前の CLSID・名前・共有メモリ名で作る |
| `dc-vcam-ctl` | MF カメラを現在のユーザー用に作る・消す（`install` / `uninstall`、試験用の `run`） |
| `dc_vcam_sender`（capd に入る） | 送り手。Win11 は共有メモリ（bridge）へ、Win10 は softcam の送信 API へ |
| `capd/src/vcam_output.*` | 映像ミックスのキャンバス → 仮想カメラ（30fps） |
| `dc-vcam-probe` | カメラアプリの代わりに開いて fps・欠落・遅延を測る |

- **Windows 11 は MF カメラだけ、Windows 10 は DirectShow カメラだけ**を登録します（Win11 は MF カメラ1つで DirectShow アプリにも届く。V4 の分かったこと 6）。表示名は「Desktop Capture Camera」。Win11 では Windows が「(Windows 仮想カメラ)」を付けます。
- 受け渡し（bridge v2）は **NV12**：capd が GPU で NV12（1920×1080 / 1280×720）にしてから CPU に読み戻し、MF ソースはアプリが同じ大きさを求めればそのまま写すだけです。RGB32 を求めるアプリにはソースが変換します。
- Win10 の DirectShow カメラは常に 1920×1080 30fps を提示するので、capd は BGRA 1920×1080 を作り、BGR24 に詰めて送ります。
- capd が送っていない間（出力にミックスが割り当てられていない、capd が止まっている）は、どちらのカメラも「信号なし」の画像を出し続けます。カメラアプリの一覧から消えたり止まったりしません。

## インストール（一度だけ、UAC 1回）

```powershell
cmake --build --preset windows-release
.\scripts\vcam-register.ps1            # %ProgramData%\DesktopCapture\vcam\bin-<日時> にコピーして登録
.\scripts\vcam-register.ps1 -Unregister
```

再インストールは毎回新しいフォルダーに置きます（カメラアプリや Frame Server が古い DLL を掴んでいても失敗しない。V4 の分かったこと 7）。古いフォルダーは使われなくなった後の登録で消えます。

## 試験

```powershell
$capd = '.\build\windows\apps\capd\Release\capd.exe'
& $capd vcam --seconds 30                         # 主モニターをカメラへ（エンジンなしで出力だけ試す）
& $capd vcam --test-pattern --seconds 20          # 時刻スタンプ入りの試験画像
.\build\windows\tools\vcam_probe\Release\dc-vcam-probe.exe mf --name "Desktop Capture" --seconds 10
```

`scripts/vcam-test.ps1` がこの組み合わせ（信号なし・試験画像の MF / DirectShow 経路・モニター）を自動で回して表にします。

MF ソースのログは `%ProgramData%\DesktopCapture\vcam-logs\vcam-mf.log` です。
