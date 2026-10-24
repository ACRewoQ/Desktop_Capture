# capd（キャプチャープロセス）— P2 骨格

開発計画フェーズ 2 レーン B「録画骨格」の実装メモです。設計書「技術検証の結果（フェーズ1）」の V3 の決定に従っています。

## できること（P2）

- 映像ミックス 1 つ・シーン 1 つ・レイヤー 1 枚（モニター全体をキャンバスに収める）
- 取得：Windows.Graphics.Capture（既定）、Desktop Duplication（`--source dda`、Win10 の黄色枠を避けたいとき）
- 合成：D3D11 シェーダーで BGRA キャンバスに描く → ID3D11VideoProcessor で録画解像度の NV12 へ。画素は CPU を通りません
- エンコーダー自動選択：`auto` は NVENC → AMF → QSV → Media Foundation → x264 の順に開いて、最初に開けたものを使う。2560×1440 を超える H.264 は HEVC に切り替える（V3：4K H.264 は NVENC の 57% を使う）
- x264（ソフトウェア、H.264 のみ、preset veryfast）：GPU の NV12 面までは同じで、エンコードスレッドが `av_hwframe_transfer_data` で CPU に下ろしてから渡す。vcpkg の FFmpeg は `gpl` と `x264` を有効にしている（個人用なので GPL で問題ない）
- エンコーダーの共通オプション（プレビュー配信用）：`VideoSettings::gopFrames`（キーフレーム間隔、0 = 2 秒）、`lowLatency`（NVENC tune=ull、AMF ultralowlatency、QSV async_depth=1、x264 zerolatency）、`submit(..., keyframe=true)` でそのフレームを IDR にする（NVENC・QSV・x264 は forced-idr）
- NVENC は `delay=0`（遅延 34ms → 約 5ms）。そのぶん呼び出しが GPU 待ちになるので、エンコードは専用スレッドで行う
- 音声：エンジンの共有メモリ（`docs/spec/shm-audio.md`）から最大 8 ストリーム（マスター / トラック n）、AAC・Opus・PCM 24bit
- 保存：MKV（既定、途中で止まっても再生できる）または MP4。書き込みは専用スレッド
- レート制御：VBR / CBR / CQP（エンコーダーの種類ごとのオプションに対応づけ）
- エンジン追従（`capd` を引数なしで起動）：エンジンにサービスとして接続し、`/capture/state` の `starting` で録画を始めて `/capture/report` で `recording` を返し、`stopping` でファイルを閉じて `idle` を返す。失敗は `error` と理由を返す（`docs/spec/protocol.md` 5.4.1）。接続は1秒ごとに再試行し、録画中に切れたら録画を閉じる

## 時刻のそろえ方

録画開始の瞬間の QPC を時刻 0 にします。

- 映像：ミックスは自前のタイマーで一定周期に回り、各周期の予定時刻（QPC）から `pts = (時刻 − 開始) × fps` を決めます。画面が変わらなくても最後の画像を出し続けます。ミックスの fps が録画より高いときは周期を間引きます。
- 音声：出力サンプル番号 = 開始からの経過時間 × 48kHz。共有メモリの時刻アンカーで「その時刻のエンジンのフレーム」を求めて読みます。エンジンが止まっている・遅れている間は無音で埋め、再起動したらアンカーで合わせ直します。エンジンの時計（出力デバイス）と QPC のずれは、時刻アンカーで測った位置の誤差を libs/clocksync の PI 制御（DriftController）に入れ、可変レートの sinc 補間（VariableResampler、ストリームごとに同じ比率）で追従します（P3）。600ppm ずれた時計でも 3 分間で飛び・重複なし、ズレ 2ms 未満（`tests/test_capd.cpp`）。40ms を超える段差（エンジンの停止・再起動）だけは合わせ直し、そのときも補間器の履歴を本物の音で埋めてから再開するので、ブツッという音になりません。
- 読み取りは実時間より 100ms 遅らせ、エンジンが書き終えたブロックだけを読みます。

## 使い方

```powershell
$capd = '.\build\windows\apps\capd\Release\capd.exe'
& $capd                                       # エンジン（127.0.0.1:47800）に従う。--port で変更
& $capd list                                  # モニターと、この PC で開けるハードウェアエンコーダー
& $capd record --seconds 10                   # 1080p60 H.264、マスター音声、ビデオフォルダーへ
& $capd record --size 3840x2160 --bitrate 40000 --audio master,track/1,track/2
& $capd record --source dda --container mp4 --audio-codec opus
& $capd probe <ファイル>                       # 各ストリームのコーデック・パケット数・先頭/末尾時刻
```

mixd ができるまでは `dc-mock-server --capd` がエンジンの代わりになります（`--capd` なしのモックは今までどおり自分で録画したふりをします）。`dc-cli call /capture/record/start` / `stop` で録画を操作できます。

エンジンの代わりに `dc-shm-tone write` を動かすと、マスター 440Hz・トラック N = 220×(N+1)Hz の音が入ります。

実機での確認は `scripts/b-record.ps1`（7 ケースを録って `capd probe` で読み直し、`build\b-record\<日時>\summary.md` に表を作る）。

## まだないこと

| 項目 | 予定 |
|---|---|
| mixd 本体との接続試験 | G2（mixd 側の record start/stop と `/capture/report` の実装後） |
| ソース（モニター番号、WGC / DDA）の設定 | スキーマにまだないので既定（主モニター、WGC）。P3（B1）のシーン設定で入る |
| シーン・複数レイヤー・ウィンドウ / カメラのソース | P3（B1） |
| GPU リセット時に新しいファイルで続ける | P3。P2 はエラーで録画を止める |
| プレビュー配信・仮想カメラ・全画面出力 | P3 / P4（ミックスの「出力先」として足す） |

## 構成

| ファイル | 役割 |
|---|---|
| `src/settings.*` | 録画設定とパラメーターツリーからの変換、保存先のパス |
| `src/video_mix.*` | ミックスの周期処理（MMCSS「Capture」）、ソース、コンポジター、出力先への受け渡し |
| `src/compositor.*` | レイヤーを D3D11 で描く |
| `src/source_wgc.cpp` / `src/source_dda.cpp` / `src/converter.*` / `src/d3d.*` | V3 の dc-capbench から移したもの |
| `src/video_encoder.*` | FFmpeg ハードウェアエンコーダー、専用スレッド、自動選択 |
| `src/audio_capture.*` | 共有メモリ → 録画の時間軸（無音埋め・合わせ直し） |
| `src/audio_encoder.*` | AAC / Opus / PCM |
| `src/muxer.*` | MKV / MP4 書き出し（専用スレッド、書き込み失敗で録画を止める） |
| `src/recorder.*` | 1 回の録画の組み立て |
| `src/engine_follower.*` | エンジンに従う通常モード（接続、状態の追跡、`/capture/report`） |

映像以外（設定・音声・エンコード・MKV）は Linux でもビルドでき、`tests/test_capd.cpp` で試験しています。
