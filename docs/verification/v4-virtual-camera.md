# V4 技術検証：仮想カメラ

開発計画フェーズ1の V4。「MF 仮想カメラ（Win11）と DirectShow フィルター（Win10）でテスト映像が Zoom 等に映るか」を確かめる。

試作は `prototypes/v4-vcam/`（本体のビルドとは別の CMake プロジェクト）。

## 結論（2026-10-23）：合格

| 項目 | 結果 |
|---|---|
| MF 仮想カメラ（Win11）が MF アプリから見える・映る | ✅ 30fps、1080p NV12、遅延 平均10ms、欠落0 |
| MF 仮想カメラが DirectShow 専用アプリからも見える | ✅ Win11 の互換経路で見える。30fps、遅延 平均13ms |
| DirectShow 仮想カメラが DirectShow アプリから見える・映る | ✅ Win11 で 30fps・平均5ms、Win10 で 30fps・平均14ms、欠落0 |
| 送り手なしでも映像が止まらない（信号なし画像） | ✅ MF。DirectShow は softcam を修正して対応（下記8） |
| 実アプリで映る：Win11 | ✅ Chrome（webcamtests.com）で「Desktop Capture Camera (Windows 仮想カメラ)」にテスト映像が映った |
| 実アプリで映る：Win10 | ✅ Chrome で「Desktop Capture Camera (DirectShow)」にテスト映像が映った（修正後） |

- Zoom は両機とも未導入のため Chrome で代用した（Web 版の Zoom・Teams・Meet は Chrome の中で動くので同じ経路）。デスクトップ版の Zoom・Teams・Discord は未確認で、B2 以降の実機確認に回す。
- Win10 内蔵の「カメラ」アプリは MF 専用で、DirectShow カメラは原理的に出ない（想定どおり）。Win10 で対応できるのは DirectShow でカメラを探すアプリに限られる（Chrome・Edge は確認済み。主要な会議アプリも DirectShow 対応とされるが未確認）。計画の代替案「Win10 は対応アプリを限定」の範囲で収まる。
- 測定環境：RewoQsGALLERIA2（Windows 11 Home 26200、日本語）、DESKTOP-ORDNPM7（Windows 10 Pro 22H2 19045、日本語）。

## 仕組み

```
vcam-feed（本番では capd）
  ├─ MF 用：共有メモリ Global\DesktopCapture.VCam.v1 に BGRA を書く ＋ イベント .Frame を立てる
  │     └─ vcam_mf.dll（IMFMediaSource）… Windows Camera Frame Server（LOCAL SERVICE, セッション0）内で動く
  │            └─ MF アプリ（カメラ、Chrome、Teams 等）／ DirectShow アプリ（Win11 の互換経路）
  └─ DirectShow 用：softcam の送信 API（セッション内の共有メモリ）
        └─ vcam_ds.dll（DirectShow ソースフィルター）… カメラアプリのプロセス内で動く
```

- **MF 方式**：`MFCreateVirtualCamera` に自作メディアソースの CLSID を渡す。カメラの作成自体は管理者権限不要（現在のユーザー用）だが、ソース DLL の COM 登録は Frame Server から見える HKLM が必要で、ここだけ管理者権限が要る。
- **DirectShow 方式**：softcam（MIT）のフィルターを自前の CLSID・名前・共有メモリ名でビルド。登録は regsvr32（管理者）。
- 各フレームの左上に、送り手の時刻（QPC）とフレーム番号を白黒のマスで埋め込み、`vcam-probe` が読み取って fps・欠落・遅延を測る。

## 分かったこと（設計への反映候補）

1. **MF ソースは別セッションの別アカウントで動く。** Frame Server（LOCAL SERVICE、セッション0）内で動くので、送り手との受け渡しは `Global\` 名前空間が必要。一般ユーザーは `Global\` にオブジェクトを作れないため、**作るのはソース側、送り手は開くだけ**にする（送り手はカメラが使われるまで開けないので再試行する）。DACL で認証済みユーザーに読み書きを許可する。
2. **DLL の置き場所。** LOCAL SERVICE はユーザープロファイル配下を読めないので、ソース DLL は `%ProgramData%` 等に置く（試作は `%ProgramData%\DesktopCapture\vcam\bin-<日時>`）。そのフォルダーは一般ユーザーが書き換えられないこと（サービスが読み込むため）。インストーラーで配置・登録する前提になる。
3. **登録時の一時インスタンス。** `MFCreateVirtualCamera` の呼び出し時にユーザープロセス内でもソースが一度作られてすぐ破棄される（`Global\` 作成は権限不足で失敗する）。実害はないが、初期化は軽く、失敗しても落ちないようにする。
4. **2つの 30fps 時計のうなり。** ソースが自分の 30fps 時計で送り出すと、送り手の 30fps とずれて約4枚に1枚の重複・欠落が出た（MF 直接読み取り）。送り手がフレームごとにイベントを立て、ソースはそれを待って送る方式に変更（送り手がいないか2周期以上遅れたら自前の時計で信号なし画像）。修正後は 301枚中301枚が送り手のフレームで、欠落0。
5. **デバイス名。** Windows は MF 仮想カメラ名に「(Windows 仮想カメラ)」（英語版は "(Windows Virtual Camera)"）を付け足す。UI やドキュメントではこの表示名を前提にする。
6. **Win11 では MF カメラ1つで DirectShow アプリにも届く。** Win11 では DirectShow フィルターは不要で、両方登録すると同じ映像のカメラが2つ並ぶ。→ B2 では **Win11 は MF のみ、Win10 は DirectShow のみ登録**とするのがよい。
7. **使用中の DLL は上書きできない。** Frame Server やカメラアプリが DLL を掴んだままだと、再インストール時のコピーが失敗する。試作の最初の登録スクリプトは「解除→コピー→登録」の順だったため、コピー失敗で未登録のまま終わった。インストーラーは**毎回新しいフォルダーに置いて登録し直し、古いフォルダーは使われなくなってから消す**方式にする（試作もそう変更）。
8. **softcam の制約。** softcam のフィルターは、カメラアプリがフィルターを開いた時点で送り手が動いていないと形式を返せず失敗する（信号なし画像を出せない）。Chrome は形式を返さないカメラを一覧から外すとみられる（Win10 の Chrome で一覧に出なかった）。試作ではビルド時に softcam を書き換え、送り手がいなくても 1080p30 を提示して信号なしの単色画像を出すようにした。B2 の vcam-ds も、固定の形式を常に提示し、送り手がいなければ信号なし画像を出す自前フィルターにする（BaseClasses は MIT で流用可）。

## 手順

ビルド・登録・測定はすべて `prototypes/v4-vcam/scripts/` のスクリプトで行う。

| スクリプト | 内容 | 管理者 |
|---|---|---|
| `v4-build.ps1` | Release ビルド（`build\bin`）とパターンの往復テスト | 不要 |
| `v4-register.ps1` | `%ProgramData%\DesktopCapture\vcam\bin-<日時>` へコピーして DLL を登録（Win10 は DirectShow のみ）。`-Unregister` で解除 | UAC 1回 |
| `v4-test.ps1` | 自動測定：一覧、信号なし、MF 直接、MF→DirectShow、DirectShow | 不要 |
| `v4-demo.ps1` | 実アプリで目視確認するため、両カメラにテスト映像を流し続ける | 不要 |

### 測定結果の生データ（2026-10-23、最終）

```
Win11  mf (送り手なし)        frames>60 stamped=0
Win11  mf (送り手あり)        fps=30.0 unique=301/301 dropped=0  latency avg=10.3  NV12_1920x1080
Win11  MF→DirectShow 互換経路  fps=30.0 dropped=0                 latency avg=12.8  RGB24_1920x1080
Win11  DirectShow カメラ      fps=30.0 dropped=0                 latency avg=5.2   RGB24_1920x1080
Win10  DirectShow カメラ      fps=30.0 dropped=0                 latency avg=14.3  RGB24_1920x1080
```

修正前（2026-10-22）は MF 直接で 301枚中69枚の重複（上記4）、遅延 平均26ms だった。

## 片付け

`v4-register.ps1 -Unregister`（UAC）で登録を解除し、`%ProgramData%\DesktopCapture` を削除すれば元に戻る。
