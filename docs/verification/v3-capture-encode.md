# V3 技術検証：WGC + ハードウェアエンコード

開発計画フェーズ 1 の V3「GPU 上のまま 1080p60 をエンコードし、CPU 5% 以下か。Win10 の Desktop Duplication も確認」の結果です。

## 結論

- **合格（WGC）**：Windows.Graphics.Capture → GPU 上で NV12 変換 → NVENC の経路で、1080p60 も 4K60 も CPU 使用率は **マシン全体の 0.25% 未満**（1 コア換算で 2〜5%）。目標の 5% を大きく下回ります。フレーム落ちは 0 でした。
- **Desktop Duplication（Win10 用）**：コードは動きますが、計測時は Reo の PC でフレームが 1 枚も来ませんでした（深夜でモニターが省電力状態だったのが原因とみています）。モニターを起こした状態で再試験します（下の「未完了」）。
- 設計の変更は不要です。解像度・コーデックの既定値もそのままでよいと判断します（既定 1080p60 H.264、4K60 は HEVC/AV1 推奨）。

## 試験環境

| 項目 | 値 |
|---|---|
| PC | RewoQsGALLERIA2（Windows 11 Home 25H2, build 26200） |
| CPU | Intel Core Ultra 9 285K（24 コア） |
| GPU | NVIDIA GeForce RTX 5090（ドライバー 616.56）。iGPU は無効 |
| モニター | 3840×2160 60Hz、拡大率 150% |
| FFmpeg | 9.0.2（vcpkg、avcodec/avformat/nvcodec/amf/qsv） |
| ツール | `tools/capbench`（dc-capbench）、`scripts/v3-bench.ps1` |

## 計測方法

`dc-capbench run` は 60fps の固定周期で次を回し、1 秒ごとに負荷を記録します。画素が CPU を通ることはありません。

```
WGC / Desktop Duplication の画面 --CopyResource--> BGRA テクスチャ
  --ID3D11VideoProcessor（縮小・BT.709 変換）--> NV12 テクスチャ（FFmpeg の D3D11 フレーム）
  --FFmpeg エンコーダー（NVENC / AMF / MF は D3D11 のまま、QSV はマップ）--> パケット --> MKV または破棄
```

- 画面が変わらない周期でも直前の画像を変換・エンコードします（レーン B のコンポジターと同じ動き）。
- `--pattern` は別プロセスで動く試験パターン（毎フレーム動く窓）を出し、エンコーダーに実際の動きを与えます。そのプロセスの負荷は計測に含みません。
- CPU はプロセスの CPU 時間 ÷ 経過時間 ÷ 論理コア数。GPU はタスクマネージャーと同じ「GPU Engine」カウンターです。
- 各ケース 30 秒（最初の 3 秒は除外）。

## 結果（2026-10-23 01:48、各 30 秒）

| ケース | 取込 fps | 出力 fps | 落ち | CPU 平均 | CPU 最大 | 1 コア換算 | GPU エンコード | ビットレート | 作業メモリ |
|---|---|---|---|---|---|---|---|---|---|
| WGC H.264 1080p | 53.9 | 60.0 | 0 | 0.16% | 0.33% | 3.8% | 20% | 7.6 Mbps | 93 MB |
| WGC HEVC 1080p | 57.5 | 60.0 | 0 | 0.21% | 0.46% | 5.0% | 23% | 8.2 Mbps | 90 MB |
| WGC AV1 1080p | 57.5 | 60.0 | 0 | 0.02% | 0.20% | 0.5% | 11% | 10.7 Mbps | 92 MB |
| WGC H.264 1080p 低遅延（p1, ull） | 57.8 | 60.0 | 0 | 0.08% | 0.46% | 1.8% | 10% | 9.1 Mbps | 93 MB |
| WGC H.264 1080p 静止画面 | 5.0 | 60.0 | 0 | 0.04% | 0.20% | 0.9% | 23% | 3.4 Mbps | 92 MB |
| WGC H.264 1080p 全画面パターン | 57.9 | 60.0 | 0 | 0.17% | 0.59% | 4.1% | 17% | 6.6 Mbps | 93 MB |
| WGC H.264 1080p MKV 書き出し | 57.4 | 60.0 | 0 | 0.19% | 0.46% | 4.6% | 19% | 7.0 Mbps | 95 MB |
| WGC 変換のみ（エンコードなし） | 57.3 | 60.0 | 0 | 0.08% | 0.39% | 1.9% | 0% | — | 60 MB |
| WGC H.264 4K | 57.8 | 60.0 | 0 | 0.22% | 0.46% | 5.3% | 57% | 11.1 Mbps | 165 MB |
| WGC HEVC 4K | 46.7 | 60.0 | 0 | 0.15% | 0.46% | 3.7% | 62% | 10.9 Mbps | 150 MB |
| WGC Media Foundation H.264 1080p | 49.7 | 60.0 | 0 | 0.17% | 0.39% | 4.0% | 13% | 10.1 Mbps | 85 MB |
| DDA H.264 / HEVC 1080p、H.264 4K | 0 | 0 | 0 | — | — | — | — | — | — |

- 「取込 fps」はキャプチャー API から新しい画像が届いた数です。モニターが 60Hz なので約 58 が上限です。46.7〜53.9 のケースは、同じ時間に別スレッド（V2・V6）が同じ PC で計測しており、その影響とみています（同じケースの 1 回目は 57.4〜57.7）。出力 fps はどのケースも 60.0 でした。
- 4K H.264 は NVENC の能力の 57% を使います。4K60 の録画は HEVC か AV1 を既定にするのが安全です。
- 作業メモリは 1080p で約 90MB、4K で約 160MB（FFmpeg と D3D11 を含む試作ツール全体）。

### エンコード遅延（プレビュー・LAN 伝送向け）

FFmpeg の NVENC は既定でパケットを 2 フレームぶん貯めるため、投入からパケットまで約 34ms かかります。`delay=0` で解消します。

| 設定 | 平均 | 最大 |
|---|---|---|
| H.264 1080p 既定 | 34.1 ms | 36.2 ms |
| H.264 1080p `delay=0` | 4.7 ms | 7.0 ms |
| H.264 1080p `delay=0 zerolatency=1 preset=p1 tune=ull` | 3.2 ms | 8.3 ms |
| HEVC 4K `delay=0` | 10.7 ms | 17.3 ms |
| Media Foundation H.264 1080p | 3.6 ms | 6.6 ms |

`delay=0` にするとエンコードの完了待ちが呼び出し側のスレッドで起きます（4K HEVC で 1 フレームあたり平均 10.7ms）。本番ではエンコーダーへの投入と受け取りを専用スレッドで行い、キャプチャー・合成の周期を止めないようにします。

## 分かったこと（レーン B への申し送り）

1. **NV12 の配列テクスチャ**：RTX 5090（ドライバー 616.56）では、NV12 で ArraySize > 1 のテクスチャを RENDER_TARGET 付きで作れません（BIND_DECODER のときだけ可）。FFmpeg の固定プール（配列テクスチャ 1 枚）は作れないので、1 枚ずつ別テクスチャのプールにする。`encoder.cpp` に判定と切り替えがあります。
2. **エンコーダーの自動選択**：この PC で使えたのは `h264/hevc/av1_nvenc` と `h264/hevc/av1_mf`（NVIDIA の MFT）。AMF（AMD）と QSV（Intel）は GPU がないため開けません。自動選択は「NVENC → AMF → QSV → MF」の順に実際に開いて試す方式でよいです（`dc-capbench list` と同じ方法）。
3. **MF エンコーダー**は NVENC が使えない環境の代わりになりますが、レート制御などの細かい設定が少ないので既定にはしません。
4. **GPU カウンター**：NVIDIA では VideoProcessor の負荷が「VideoProcessing」エンジンに出ません（常に 0）。GPU 負荷の表示には 3D とエンコードを使います。
5. **WGC**：枠なし（IsBorderRequired = false）、カーソル込みで問題なく取れました。画面が変わらないときは 1 秒あたり数枚しか届かないので、出力周期はキャプチャーと切り離す必要があります（今回の作り方どおり）。
6. **作業メモリ**：4K60 で約 160MB。キャプチャープロセスの目安になります。

## 未完了

- **Desktop Duplication の再試験**：計測時（深夜、無操作 35 分以上）はフレームが届きませんでした。DuplicateOutput と DuplicateOutput1 はどちらも成功し、エラーも出ず、最初の 1 枚すら来ない状態です。WGC は同じ時刻に普通に取れていました。モニターが省電力状態だったのが最も疑わしいので、Reo が PC を使っているときに `scripts/v3-bench.ps1 -Only dda-h264-1080p,dda-hevc-1080p,dda-h264-native` で再試験します。それでも取れない場合は Meta の仮想ディスプレイドライバーとの干渉を疑います。
- **Win10 実機**：手元に Win10 の PC がないため、Win10 は「Desktop Duplication が Win11 上で動くこと」の確認で代えます。WGC 自体は Win10 1903 以降で動く API です。

## 再現手順

```powershell
cmake --preset windows
cmake --build --preset windows-release --target dc-capbench
.\build\windows\tools\capbench\Release\dc-capbench.exe list
.\scripts\v3-bench.ps1 -Seconds 30
```

結果は `build\v3-results\<日時>\summary.md` にできます。ほかの重い処理（他スレッドの計測を含む）と同時に走らせないでください。
