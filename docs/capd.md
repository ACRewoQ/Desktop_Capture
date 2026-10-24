# capd（キャプチャープロセス）— P2 骨格

開発計画フェーズ 2 レーン B「録画骨格」の実装メモです。設計書「技術検証の結果（フェーズ1）」の V3 の決定に従っています。

## できること（P2）

- 映像ミックス 1 つ・シーン 1 つ・レイヤー 1 枚（モニター全体をキャンバスに収める）
- 取得：Windows.Graphics.Capture（既定）、Desktop Duplication（`--source dda`、Win10 の黄色枠を避けたいとき）
- 合成：D3D11 シェーダーで BGRA キャンバスに描く → ID3D11VideoProcessor で録画解像度の NV12 へ。画素は CPU を通りません
- エンコーダー自動選択：`auto` は NVENC → AMF → QSV → Media Foundation の順に開いて、最初に開けたものを使う。2560×1440 を超える H.264 は HEVC に切り替える（V3：4K H.264 は NVENC の 57% を使う）
- NVENC は `delay=0`（遅延 34ms → 約 5ms）。そのぶん呼び出しが GPU 待ちになるので、エンコードは専用スレッドで行う
- 音声：エンジンの共有メモリ（`docs/spec/shm-audio.md`）から最大 8 ストリーム（マスター / トラック n）、AAC・Opus・PCM 24bit
- 保存：MKV（既定、途中で止まっても再生できる）または MP4。書き込みは専用スレッド
- レート制御：VBR / CBR / CQP（エンコーダーの種類ごとのオプションに対応づけ）
- エンジン追従（`capd` を引数なしで起動）：エンジンにサービスとして接続し、`/capture/state` の `starting` で録画を始めて `/capture/report` で `recording` を返し、`stopping` でファイルを閉じて `idle` を返す。失敗は `error` と理由を返す（`docs/spec/protocol.md` 5.4.1）。接続は1秒ごとに再試行し、録画中に切れたら録画を閉じる

## 時刻のそろえ方

録画開始の瞬間の QPC を時刻 0 にします。

- 映像：ミックスは自前のタイマーで一定周期に回り、各周期の予定時刻（QPC）から `pts = (時刻 − 開始) × fps` を決めます。画面が変わらなくても最後の画像を出し続けます。ミックスの fps が録画より高いときは周期を間引きます。
- 音声：出力サンプル番号 = 開始からの経過時間 × 48kHz。共有メモリの時刻アンカーで「その時刻のエンジンのフレーム」を求めて読みます。エンジンが止まっている・遅れている間は無音で埋め、再起動したらアンカーで合わせ直します。エンジンの時計と QPC のずれが 40ms を超えたときも合わせ直します（P3 で libs/clocksync の可変レート補正に置き換える候補）。
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

録画の音ズレは `capd avsync <ファイル>` で測ります。`dc-av-flash`（主モニターの左上 1/4 を QPC 時刻の一定間隔で白くする）と `mixd --test-pulse`（同じ瞬間にマスターへ 50ms のビープ）を動かしたまま録画したファイルから、白くなったフレームとビープの頭の時刻を拾って組にし、「音 − 映像」の平均・中央値・最小・最大と、最初の 1 割と最後の 1 割の差（ずれの変化）を出します。`--csv` で組ごとの値、`--region X,Y,W,H` で見る場所（画面に対する割合、既定 0.06,0.06,0.10,0.10）を変えられます。mixd・capd・dc-ui をまとめて動かす G2 の試験は `scripts/g2-integration.ps1`。

実機での確認は `scripts/b-record.ps1`（7 ケースを録って `capd probe` で読み直し、`build\b-record\<日時>\summary.md` に表を作る）。

## まだないこと

| 項目 | 予定 |
|---|---|
| ソース（モニター番号、WGC / DDA）の設定 | スキーマにまだないので既定（主モニター、WGC）。P3（B1）のシーン設定で入る |
| シーン・複数レイヤー・ウィンドウ / カメラのソース | P3（B1） |
| GPU リセット時に新しいファイルで続ける | P3。P2 はエラーで録画を止める |
| プレビュー配信・仮想カメラ・全画面出力 | P3 / P4（ミックスの「出力先」として足す） |
| x264（ソフトウェア） | vcpkg の FFmpeg に x264 を入れていないため未対応。`auto` はハードウェアだけを試す |

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
