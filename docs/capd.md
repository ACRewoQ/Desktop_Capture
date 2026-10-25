# capd（キャプチャープロセス）

開発計画フェーズ 2 レーン B「録画骨格」と、フェーズ 3 レーン B1「ビデオミキサー」の実装メモです。設計書「技術検証の結果（フェーズ1）」の V3 の決定に従っています。

## ビデオミキサー（P3 B1）

設定はエンジンのツリー `/video/*`（`docs/spec/protocol.md` 4.4）にあり、capd はそれに従って合成します。

- `VideoEngine`（`src/video_mix.*`）：D3D11 デバイス 1 つと映像スレッド 1 本。ミックス（最大 4）はそれぞれ自分の fps の予定時刻でこのスレッド上で回る。ソースは全ミックスで共有し、同じカメラを 2 つのミックスに出しても開くのは 1 回
- 出力先（録画・仮想カメラ・全画面・プレビュー）は `acquire(ミックス番号)` でリースを取り、`addFrameConsumer` で毎フレームのキャンバス（premultiplied BGRA）を受け取る。リースがあるミックスだけを合成する
- 各ミックスは本番シーン・プレビューシーン・フェード中の前のシーンのソースを開いておく（テイクやレイヤーの表示切り替えですぐ映るように）。どこからも使われなくなったソースは 3 秒後に閉じる
- 切り替え：`program_scene` が変わると cut か fade。フェード中にさらに変わったら、その瞬間の画を凍結して新しいシーンへフェードする
- ソース（`src/video_source.*`、`src/source_mf.cpp`、`src/text_renderer.*`）：モニター（WGC / DDA、ID はモニターのデバイスパス）、ウィンドウ（WGC、実行ファイル名＋タイトルで探してつなぎ直す）、カメラと動画ファイル（Media Foundation のソースリーダー、できれば GPU で RGB32 へ変換）、画像（WIC）、文字（DirectWrite）、単色（シェーダーで塗る）。対象がないときは `disconnected`、開けないときは `error` を報告し、自分で再試行する
- 合成（`src/compositor.*`）：レイヤーごとに 1 枚の四角形。クロップはテクスチャ座標、fit / fill / stretch は `video_config.cpp` の `layoutLayer`、クロマキーは OBS と同じ CbCr 距離方式
- GPU のリセット・取り外し：デバイス・ソース・コンポジターを作り直して続ける。フレームの `generation` が上がるので、出力先はそれを見て自分の GPU オブジェクトを作り直す。ミックスの解像度・fps を変えたときも `generation` が上がる
- 報告：ソースの状態は `/video/source/report`、モニター・カメラの一覧は変化時に、ウィンドウの一覧は `video.devices.refresh` を受けたときに `/video/devices/report` で送る
- 動画ファイルの音声は使わない（音声はエンジンの担当）

実機確認：`scripts/b1-video.ps1`（`capd mixtest` で画素を確かめる自己試験と、`dc-mock-server --capd` 相手のシーン切り替え入り録画）。

## できること（P2）

- 映像：上の「ビデオミキサー」のミックスの1つ（`/capture/mix`）を録画する
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

録画の音ズレは `capd avsync <ファイル>` で測ります。`dc-av-flash`（主モニターの左上 1/4 を QPC 時刻の一定間隔で白くする）と `mixd --test-pulse`（同じ瞬間にマスターへ 50ms のビープ）を動かしたまま録画したファイルから、白くなったフレームとビープの頭の時刻を拾って組にし、「音 − 映像」の平均・中央値・最小・最大と、最初の 1 割と最後の 1 割の差（ずれの変化）を出します。`--csv` で組ごとの値、`--region X,Y,W,H` で見る場所（画面に対する割合、既定 0.06,0.06,0.10,0.10）を変えられます。mixd・capd・dc-ui をまとめて動かす G2 の試験は `scripts/g2-integration.ps1`。

実機での確認は `scripts/b-record.ps1`（7 ケースを録って `capd probe` で読み直し、`build\b-record\<日時>\summary.md` に表を作る）。

## まだないこと

| 項目 | 予定 |
|---|---|
| GPU リセット時に新しいファイルで続ける | P3（B2+B3）。ミキサー側の作り直しは B1 でできている |
| LAN 映像ストリームのソース | レーン N（今は `error` を報告する） |
| プレビュー配信・仮想カメラ・全画面出力 | P3 / P4（ミックスの「出力先」として足す） |

## 構成

| ファイル | 役割 |
|---|---|
| `src/settings.*` | 録画設定とパラメーターツリーからの変換、保存先のパス |
| `src/video_mix.*` | VideoEngine とミックスのリース：映像スレッド（MMCSS「Capture」）、ソースの開閉、トランジション、出力先への受け渡し、GPU リセット |
| `src/video_config.*` | `/video/*` の読み取り、レイヤーの配置、トランジションの状態（ポータブル、Linux で試験） |
| `src/video_source.*` / `src/source_mf.cpp` / `src/text_renderer.*` | ソース全種とモニター・ウィンドウ・カメラの一覧 |
| `src/compositor.*` | レイヤーを D3D11 で描く（クロップ、不透明度、クロマキー、単色、フェード） |
| `src/mix_test.*` | `capd mixtest`（実機の自己試験） |
| `src/source_wgc.cpp` / `src/source_dda.cpp` / `src/converter.*` / `src/d3d.*` | V3 の dc-capbench から移したもの |
| `src/video_encoder.*` | FFmpeg ハードウェアエンコーダー、専用スレッド、自動選択 |
| `src/audio_capture.*` | 共有メモリ → 録画の時間軸（無音埋め・合わせ直し） |
| `src/audio_encoder.*` | AAC / Opus / PCM |
| `src/muxer.*` | MKV / MP4 書き出し（専用スレッド、書き込み失敗で録画を止める） |
| `src/recorder.*` | 1 回の録画の組み立て |
| `src/engine_follower.*` | エンジンに従う通常モード（接続、状態の追跡、`/capture/report`） |

映像以外（設定・音声・エンコード・MKV）は Linux でもビルドでき、`tests/test_capd.cpp` で試験しています。
