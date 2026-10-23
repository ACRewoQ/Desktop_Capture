# V4 技術検証：仮想カメラ

開発計画フェーズ1の V4。「MF 仮想カメラ（Win11）と DirectShow フィルター（Win10）でテスト映像が Zoom 等に映るか」を確かめる。

試作は `prototypes/v4-vcam/`（本体のビルドとは別の CMake プロジェクト）。

## 結論（暫定・2026-10-22 時点）

| 項目 | 結果 |
|---|---|
| MF 仮想カメラ（Win11）が MF アプリから見える・映る | ✅ 30fps、1080p NV12、遅延 平均14〜26ms |
| MF 仮想カメラが DirectShow 専用アプリからも見える | ✅ Win11 の互換経路で見える。30fps、遅延 平均37〜45ms |
| DirectShow 仮想カメラが DirectShow アプリから見える・映る | ✅ 30fps、1080p RGB24、遅延 平均5〜6ms |
| 送り手なしでも映像が止まらない（信号なし画像） | ✅ MF で確認 |
| Zoom 等の実アプリで映る | ⏳ 調査中（Win11 の Teams で「ほかのアプリで利用中」、Chrome・Discord で出ない。再登録の失敗が疑わしい） |
| Win10 実機 | ⏳ 未実施 |

測定環境：RewoQsGALLERIA2、Windows 11 Home 26200（日本語）。

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
4. **2つの 30fps 時計のうなり。** ソースが自分の 30fps 時計で送り出すと、送り手の 30fps とずれて約4枚に1枚の重複・欠落が出た（MF 直接読み取り）。送り手がフレームごとにイベントを立て、ソースはそれを待って送る方式に変更（送り手がいないか2周期以上遅れたら自前の時計で信号なし画像）。→ 再測定待ち。
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

### 測定結果の生データ（2026-10-22、修正前）

```
mf (送り手なし)        frames=121 fps=30.0 stamped=0
mf (送り手あり)        frames=301 fps=30.0 unique=231 dropped=69  latency avg=26.1 p95=44.0 max=44.9  NV12_1920x1080
MF→DirectShow 互換経路  frames=300 fps=30.0 unique=300 dropped=0   latency avg=44.6 p95=46.0 max=48.0  RGB24_1920x1080
DirectShow カメラ      frames=301 fps=30.0 unique=301 dropped=0   latency avg=5.8  p95=6.9  max=20.2  RGB24_1920x1080
```

## 片付け

`v4-register.ps1 -Unregister`（UAC）で登録を解除し、`%ProgramData%\DesktopCapture` を削除すれば元に戻る。
