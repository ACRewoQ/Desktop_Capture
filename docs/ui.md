# UI（dc-ui）

ローカル PC とリモコンで同じ画面を使う Qt 6 Quick のアプリ。P2 レーン C の骨格（ミキサー画面）に、P3 レーン C で各レーンの画面を足していく。WebView・QtQuick.Controls は使わず、Rectangle とポインターハンドラーだけで作る（V5 の結果。RAM 4GB の Surface で常駐 150MB 以下）。

## 構成

| ファイル（apps/ui） | 役割 |
|---|---|
| `src/connection` | 制御プロトコルのクライアント（Qt のソケット、GUI スレッドだけで動く）。パラメーターツリーの写しを持つ |
| `src/strip_model` | ミキサーのストリップ（トラックとマスター）。60Hz に間引いた楽観的な編集 |
| `src/param_hub` | ストリップ以外の画面用。どのパスでも読み書きでき、QML 型 `Param`・`ParamList`（`import DcUi.Params`）とコンテキストプロパティ `params` で使う |
| `src/recorder` | 録画の開始・停止と状態 |
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

`VideoPage` は 3 つのタブ（シーン・ソース・出力先）。名前や選択肢の一覧は `Video` シングルトン（`qml/Video.qml`）にまとめている。

- シーン：シーンをタップすると「次」（`preview_scene`）になり、そのシーンを編集する。TAKE で本番と入れ替え。「…」または長押しで本番に出す・名前・並べ替え・削除。
- キャンバス（`LayerCanvas`）：レイヤーの枠だけを描く（映像は capd が合成する）。タップで手前のレイヤーを選び、選んだレイヤーはドラッグで移動、右下のつまみで大きさを変える。端と中央に 10px で吸着。ダブルタップでレイヤーの設定。
- 横幅 900px 未満では、キャンバス・シーン・レイヤーを縦に並べる。シーンのタブではキャンバスのドラッグと重ならないよう、横スワイプでのページ移動を止める。

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
| `--demo NAME` | シートを見本の内容で開く（`number`、`text`、`color`、ミキサーの `menu`・`master`・`input`・`output`・`fx`・`fxedit`、映像の `layer`・`source`・`sources`・`outputs`・`textsource`） |
| `--selftest` / `--exercise` | 自動の確認（[build-and-test.md](build-and-test.md)） |
