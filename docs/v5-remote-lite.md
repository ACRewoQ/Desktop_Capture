# V5 リモコンの軽さ（技術検証）

開発計画フェーズ1の V5。Qt Quick の最小アプリで、RAM 4GB の Win11 端末での常駐メモリと、マルチタッチでの同時フェーダー操作を確かめます。G4 の目標は「常駐メモリ 150MB 以下」です。

## 検証アプリ dc-v5-probe

`tools/v5_remote_probe/`。Qt 6 が見つかったときだけビルドされます（`cmake/DcDependencies.cmake` の `DC_HAVE_QT`）。

| 項目 | 内容 |
|---|---|
| 画面 | 横に並んだフェーダーストリップ（名前・メーター・フェーダー・dB 表示・ミュート）と、下端の計測バー |
| `--ui custom` | 既定。Rectangle / Text とポインターハンドラーだけで描いた軽量版 |
| `--ui controls` | 比較用。QtQuick.Controls（Basic スタイル）の Slider / Button / Label で同じ画面 |
| `--ui minimal` | 下限の目安。計測バーだけのウィンドウ |
| `--tracks N` | ストリップ数（1〜64） |
| `--connect HOST[:PORT]` | 模擬ではなく mixd / dc-mock-server に接続。足りない分は `/mixer/track/add` で増やし、フェーダー操作を Set で送り、UDP メーターを受ける |
| `--backend` | 描画方式。`default`（Windows では D3D11）/ `d3d11` / `software` / `opengl` |
| `--touch-test` | 合成タッチによる自動試験を行って終了（下記） |
| `--duration SEC` / `--report FILE` | 指定秒数で終了し、JSON の計測結果を書く |

フェーダーは指を置いた位置からの相対移動で動き（触れた瞬間に値が飛ばない）、ダブルタップで 0 dB に戻ります。横スワイプはストリップ一覧のスクロールになり、フェーダーは動きません。

### タッチ自動試験（`--touch-test`）

Windows では `InjectTouchInput` で本物のタッチ入力を合成するので、タッチパネルのない PC でも確認できます。

1. 10 本の指で 10 本のフェーダーを同時に、指ごとに違う量だけ下げる。全フェーダーが動き、量の順に並び、同時操作数が 10 に達すれば合格
2. フェーダー 1 をダブルタップして 0 dB に戻る
3. フェーダーの上から始めた横スワイプで一覧がスクロールし、どのフェーダーも動かない

試験中はウィンドウを最前面にし、前面でなくなったら合成入力を止めます（他のアプリを誤って操作しないため）。Linux では QTest のタッチイベントで同じ試験を行い、QML 側の動きだけを確かめます。

## 手順

### 1. Qt の導入（初回だけ）

```powershell
.\scripts\install-qt.ps1          # C:\dev\Qt に Qt 6.10 系、環境変数 DC_QT_DIR を設定
```

管理者権限は不要です。aqt 3.3 は Qt 6.11 以降のダウンロード配置を読めないため、既定は 6.10 系です。

### 2. 開発 PC で計測

```powershell
.\scripts\v5-package.ps1          # Release ビルド + 配布用フォルダーと zip
.\build\v5-package\dc-v5-probe\measure.cmd
```

`measure.cmd`（中身は `v5-measure.ps1`）は各構成を 20 秒ずつ起動し、外からワーキングセットと専用メモリ（タスクマネージャーと同じ値）を 1 秒ごとに測ります。後半の平均を定常値とします。結果は `v5-results-日時\summary.csv` と `summary.json` に出ます。

### 3. RAM 4GB 端末で計測

1. `build\v5-package\dc-v5-probe.zip` を端末にコピーして展開（インストール不要。MSVC ランタイムも同梱）
2. `measure.cmd` をダブルクリック。合成タッチ試験の間（約 10 秒×2）は画面に触れない
3. `touch-try.cmd` で全画面表示にし、実際に指で操作する
   - 4〜5 本の指で同時にフェーダーを動かし、計測バーの「最大同時」が指の本数になるか
   - 動きが指に遅れずについてくるか、ダブルタップで 0 dB、横スワイプでページ送り
4. `v5-results-日時` フォルダーを返送

## 結果

（計測後に記入）
