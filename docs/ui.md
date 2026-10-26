# UI（dc-ui）

ローカル PC とリモコンで同じ画面を使う Qt 6 Quick のアプリ。P2 レーン C の骨格（ミキサー画面）に、P3 レーン C で各レーンの画面を足していく。WebView・QtQuick.Controls は使わず、Rectangle とポインターハンドラーだけで作る（V5 の結果。RAM 4GB の Surface で常駐 150MB 以下）。

## 構成

| ファイル（apps/ui） | 役割 |
|---|---|
| `src/connection` | 制御プロトコルのクライアント（Qt のソケット、GUI スレッドだけで動く）。パラメーターツリーの写しを持つ |
| `src/strip_model` | ミキサーのストリップ（トラックとマスター）。60Hz に間引いた楽観的な編集 |
| `src/param_hub` | ストリップ以外の画面用。どのパスでも読み書きでき、QML 型 `Param`・`ParamList`（`import DcUi.Params`）とコンテキストプロパティ `params` で使う |
| `src/recorder` | 録画の開始・停止と状態 |
| `src/preview_stream` | プレビュー映像（capd の `video/preview`）の購読と受信。見ている画面があるときだけ `/lan/subscribe` し、届いたフレームを渡す。コンテキストプロパティ `previewStream` |
| `src/preview_video`・`src/preview_decoder` | プレビューの表示（QML 型 `PreviewVideo`、`import DcUi.Preview`）。Media Foundation で H.264 をデコードし（GPU、使えなければ CPU）、Qt の D3D11 デバイスのテクスチャーに写して描く。P1/V6 と同じ方式で、デコードの部品は `libs/mfvideo`。Windows の D3D11 描画のときだけ映り、それ以外は理由を表示する |
| `qml/Main.qml` | 上のバー、左のページボタン、ページ、共通のシート |
| `qml/*Page.qml` | ページ。`MixerPage` だけ常に読み込み、ほかは表示中だけ読み込む（隠れたページはメモリを使わない） |

## ページの足し方

1. `qml/XxxPage.qml` を作る。ルートは `Item` で、横にスクロールしないページは `readonly property bool swipeable: true` を持つ（横スワイプで隣のページへ移る）。
2. `Main.qml` の `pages` に `{ key, label, file, keep: false }` を足し、`CMakeLists.txt` の `DC_UI_QML` に足す。
3. `dc-ui --selftest` は全ページを一度ずつ表示し、QML の警告が出たら失敗にする。

## パラメーターの使い方（QML）

```qml
import DcUi.Params

Param { id: mode; path: "/system/audio_mode" }      // mode.value, mode.set("exclusive"), mode.choices, mode.min ...
ParamList { id: sources; prefix: "/video/source" }    // 番号の一覧（ロール num と base）。orderBy: "position" で並べ替え
params.call("/mixer/track/add", { channels: 1 }, (ok, v) => ...)
params.setMany({ "/a": 1, "/b": "x" })                // 1 回の Set でまとめて
```

編集はすぐ画面に出て、パラメーターごとに約 16ms 間隔で送る。エンジンが拒否すると元の値に戻り、トーストにエラーを出す。範囲・選択肢・型はスキーマ（libs/protocol）から取るので、ツリーに値がまだなくても既定値で描ける。

設定の行には `SettingRow` / `InfoRow` と、`ParamSegments`（少ない選択肢）・`ParamPick`（一覧から選ぶ）・`ParamNumber`（キーパッド。`factor: 100` で割合を % で入力）・`ParamSwitch`・`ParamText`（文字。`multiline` で複数行）・`ParamColor`（色見本とパレット）・`SizePick`（幅と高さを一緒に）を使う。

## 映像ページ

`VideoPage` は 4 つのタブ（シーン・ソース・出力先・プレビュー）。名前や選択肢の一覧は `Video` シングルトン（`qml/Video.qml`）にまとめている。

- シーン：シーンをタップすると「次」（`preview_scene`）になり、そのシーンを編集する。TAKE で本番と入れ替え。「…」または長押しで本番に出す・名前・並べ替え・削除。
- キャンバス（`LayerCanvas`）：レイヤーの枠だけを描く（映像は capd が合成する）。タップで手前のレイヤーを選び、選んだレイヤーはドラッグで移動、右下のつまみで大きさを変える。端と中央に 10px で吸着。ダブルタップでレイヤーの設定。
- 横幅 900px 未満では、キャンバス・シーン・レイヤーを縦に並べる。シーンのタブではキャンバスのドラッグと重ならないよう、横スワイプでのページ移動を止める。
- プレビュー：`/video/preview/mix` のミックスを capd がエンコードした映像を映す（protocol.md 10 章）。タブを開いている間だけ購読するので、誰も見ていなければ capd はエンコードしない。下の「出すミックス」で切り替え、解像度・fps・ビットレートは出力先のタブで変える。映像が来ないときは理由（ミックスがない、エンジンにつながっていない、映像を待っている）を真ん中に出す。映像をタップすると解像度・受信 fps の表示を隠す。

## サンプラーと録画のページ

- サンプラー：8 バンク × 16 パッド。パッドは押した瞬間に鳴らし（`/sampler/trigger`）、「押す間」のパッドは離したときに止める（`/sampler/release`）。指ごとに別のパッドを押せる。「編集」を押している間は、タップでパッドの設定（ファイル・名前・色・押したときの動き・音量）を開く。
- 録画：録画ファイルの形式・保存先・映像（エンコーダー、解像度、ビットレートなど）・音声ストリーム 1〜8 と、トラック別録音（開始・停止、録音するトラック、形式、保存先）。録画の設定は録画を始めた時点で capd が読むので、録画中に変えると次の録画から効く。

## LAN ページとリモコンのペアリング

- LAN ページ：この PC の名前、ペアリングの受付（6 桁の数字を大きく表示）とペアリング済みの機器（取り消し）、見つかった別の PC（ペアリング・忘れる・IP アドレスで追加、相手の送信ストリーム）、LAN 送信 1〜8（名前・コーデック・Opus の設定・受信数）、受け取っている相手。
- リモコン（`--remote`）は、初めてのエンジンに断られると数字のキーパッドを出す。6 桁を入れるとつなぎ直し、もらったトークンを次から使う。インストールごとの ID とトークンは `QSettings`（Windows はレジストリ `HKCU\Software\DesktopCapture\dc-ui`）に保存する。エンジンで取り消されたトークンは消して、もう一度ペアリングを求める。
- 同じ PC からのループバック接続はペアリング不要（protocol.md 8.2）。試験ではエンジンを LAN のアドレスで指定する。

## キー・MIDI のページ

ホットキーと MIDI の割り当て（protocol.md 5.9）。名前と選択肢は `Bindings` シングルトン（`qml/Bindings.qml`）。

- 一覧：きっかけ（`Ctrl + Shift + R`、`MIDI CC 7（ch 1）`）、動作、状態（使えます・オフ・設定が正しくありません・ほかのアプリが使用中）。タップで `BindingSheet` を開く。
- きっかけは「学習」（エンジンの PC でキーを押すか MIDI 機器を動かす）か「入力する」（`ctrl+shift+f9`、`midi:cc/1/7`）。シートを閉じると学習をやめる。
- 動作はエンジンの `action` に「シーン切替」（`set` で `/video/mix/<n>/program_scene`）と「バンク切替」（`set` で `/sampler/active_bank`）を足した一覧から選ぶ。オン・オフ・上げ下げ・MIDI で動かす対象は、トラック（またはマスター）→ パラメーターの順に選ぶ。
- MIDI 機器の一覧と、機器ごとのオン・オフ（DAW に譲る）、最後に受けた MIDI。

## タッチ操作の約束

| 操作 | 動き |
|---|---|
| フェーダー | 指を置いた位置から相対で動く（飛ばない）。複数の指で別々に動かせる。ダブルタップで 0dB |
| 長押し | 数値をキーパッドで入力（音量の dB 表示、LC / HC） |
| 横スワイプ | ページの切り替え（ミキサーではストリップのスクロール）。40px 動くまではスライダーや一覧が優先 |
| 取り消せない操作 | つまみを右端までスライドして確定（トラックの削除など） |
| シート | 外側をタップすると閉じる。開いている間は下の画面に触れない |

タッチの目標は最小 44px。画面幅が狭いとき（リモコンを縦に持ったとき）は設定の行が縦に積まれる。

## 試験用のオプション

| オプション | 内容 |
|---|---|
| `--page KEY` | 最初に開くページ（`mixer`、`settings`、…） |
| `--screenshot FILE [--size WxH]` | つないで描画したあと PNG に保存して終わる（`QT_QPA_PLATFORM=offscreen` でも可） |
| `--pair CODE` | `--remote` で、キーパッドの代わりにペアリングの数字を渡す |
| `--demo NAME` | シートを見本の内容で開く（`number`、`text`、`color`、ミキサーの `menu`・`master`・`input`・`output`・`fx`・`fxedit`、映像の `layer`・`source`・`sources`・`outputs`・`preview`・`textsource`、サンプラーの `pad`・`edit`、LAN の `pairing`、キー・MIDI の `binding`（`binding3` で 3 番）・`learn`） |
| `--selftest` / `--exercise` | 自動の確認（[build-and-test.md](build-and-test.md)） |
