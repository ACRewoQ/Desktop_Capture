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

## プレビューと LAN 映像の送出（P3）

リモコンのプレビュー（`/video/preview/*`、ストリーム `video/preview`）、シーン画面用の「次」のシーン（ストリーム `video/next`）と映像ミックスの出力先「LAN 送信」（`/video/lan/*`、ストリーム `video/lan`）を、エンジンが `/lan/sub/#` に並べる購読者へ送ります（`docs/spec/protocol.md` 10 章、`docs/spec/lan-media.md` 7 章）。

- 送り口：`video/preview` は UDP 47811、`video/next` は UDP 47813、`video/lan` は UDP 47812（使われていたら空いているポート。受信レポートは送り元へ返るので、見る側の設定は要らない）
- 見る人がいる間だけ動く：最初の購読でミックスのリースを取ってエンコーダーを開き、最後の購読が消えたら閉じる（ほかの出力先がなければミックスも止まる）。エンジンとの接続が切れたら送るのをやめる
- 映像：ミックスのキャンバスを GPU で設定の解像度の NV12 にし（縦横は偶数に丸める）、H.264・CBR・低遅延の設定（`lowLatency`）でエンコード。フレームレートはミックスと別に、QPC 時刻の 1/fps ごとに 1 枚
- キーフレーム：最初のフレーム、見る人が 0 人から増えたとき、見る側の要求（欠け・途中参加）のとき、ほかは 2 秒ごと。どのキーフレームにも SPS/PPS を付ける（エンコーダーが付けなければ前のものを入れる）
- GPU リセット：フレームが別のデバイスから来たらエンコーダーを作り直し、新しいストリームとして送り直す（見る側はストリーム開始で切り替える）
- 設定（ミックス・解像度・fps・ビットレート）を変えるとエンコーダーを開き直す
- `video/next`：`/video/preview/mix` の `preview_scene` だけを、トランジションなしで同じキャンバスに描いた映像（設定は `/video/preview/*`）。capd の中では別のミックス（番号 100 + ミックス）として、見る人がいる間だけ動く。次のシーンが変わるとすぐ切り替わる
- ログ：見る人の数が変わったとき、送り始めたとき（エンコーダー名）、送っている間は 30 秒ごとに枚数・キーフレーム要求・往復時間

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
- 出力先（エンジン追従のとき）：`/video/vcam/mix`（仮想カメラ、`docs/vcam.md`）と `/video/monitor/mix`・`display`（全画面表示）のミックスに出力をつなぎ、状態を `/video/output/report` で返す（`protocol.md` 5.4.4。仮想カメラは `active` / `waiting` / `unavailable`、全画面は `active` / `no_display` / `error`）。モニターが外れたら全画面だけ止まり、戻れば自動で再開する
- GPU リセット：映像エンジンがデバイスを作り直す間に録画中のファイルを閉じ、新しいデバイスで新しいファイル（同じ名前があれば `_2` を付ける）を開いて `/capture/report` `recording` + `reason="gpu_reset"` を返す（エンジンは `capture.segment` を配る）。15 秒たっても GPU が戻らなければ `error`
- ディスク：録画の 30 秒分（最低 256MB）を予備として残す。空きがその 2 倍ないと開始を断り、録画中に予備を切ったら安全に止めてファイルを閉じる

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

録画の音ズレは `capd avsync <ファイル>` で測ります。`dc-av-flash`（主モニターの左上 1/4 を QPC 時刻の一定間隔で白くする）と `mixd --test-pulse`（同じ瞬間にマスターへ 50ms のビープ）を動かしたまま録画したファイルから、白くなったフレームとビープの頭の時刻を拾って組にし、「音 − 映像」の平均・中央値・最小・最大と、最初の 1 割と最後の 1 割の差（ずれの変化）を出します。`--csv` で組ごとの値、`--region X,Y,W,H` で見る場所（画面に対する割合、既定 0.06,0.06,0.10,0.10）を変えられます。mixd・capd・dc-ui をまとめて動かす G2 の試験は `scripts/g2-integration.ps1`。映像側の段差を調べるときは、環境変数 `DC_CAPD_FRAME_LOG=<CSV ファイル>` を付けて capd を動かすと、映像ミックスの刻みごとに、キャンバスに載った画面がいつ取り込まれたものか（`age_ms` = 刻みの予定時刻 − OS が取り込んだ時刻）と、刻みの遅れ（`tick_late_ms`）を書き出します。

録画の音声ストリームごとの中身は `capd levels <ファイル> [--tone HZ]` で見ます。ストリームごとに名前・長さ・全体のレベルと、指定した周波数の音の大きさ、1 秒ごとの区切りのうちその音が主に鳴っている数・無音（-80 dBFS 未満）の数を出します（G3 で「トラックごとに別の音が入っている」ことを確かめるため）。

G3 の切断試験（`scripts/g3-integration.ps1`）のために、環境変数 `DC_CAPD_TEST_DIR=<フォルダー>` を付けると試験用のスイッチが効きます。そのフォルダーに `gpu_reset` という名前のファイルを置くと capd が消して、GPU を失ったときと同じ作り直しをします（本物の GPU リセットは管理者権限の `dxcap -forcetdr` が要るため）。`disk_free` というファイルにバイト数を書くと、それを空き容量として扱います。通常の運用ではこの変数を付けません。

実機での確認は `scripts/b-record.ps1`（7 ケースを録って `capd probe` で読み直し、`build\b-record\<日時>\summary.md` に表を作る）。

## まだないこと

| 項目 | 予定 |
|---|---|
| LAN 映像ストリームのソース | レーン N（今は `error` を報告する） |

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
| `src/lan_stream.*` | LAN 映像ストリームの設定・宛先・SPS/PPS の付け足し（ポータブル、Linux で試験） |
| `src/lan_video_output.*` | LAN 映像の出力先（ミックスのフレーム → NV12 → H.264 → `lan::VideoOutput`） |
| `src/engine_follower.*` | エンジンに従う通常モード（接続、状態の追跡、`/capture/report`、出力先と LAN 映像の購読の追跡） |

映像以外（設定・音声・エンコード・MKV）は Linux でもビルドでき、`tests/test_capd.cpp` で試験しています。
