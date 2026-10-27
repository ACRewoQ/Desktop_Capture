<!-- このファイルは生成物です。編集せず、libs/protocol/src/schema_v1.cpp を直してから
     dc-cli schema --markdown > docs/spec/protocol-params.md で作り直してください。 -->

# パラメーター一覧（プロトコル v1）

`#` は 1 から始まる番号、`*` は任意の名前を表します。書込が ro のものはエンジンだけが変更し、local のものはエンジンの PC のクライアントだけが変更できます（リモコンの Set は `unauthorized`）。保存の P はプロファイルに、S はミキサースナップショットにも入るもの（protocol.md 5.10）です。

| パス | 型 | 書込 | 保存 | 範囲・選択肢 | 既定値 | 説明 |
|---|---|---|---|---|---|---|
| `/system/version` | string | ro |  |  | `""` | エンジンのバージョン |
| `/system/protocol` | int | ro |  | 1 〜 1000 | `1` | プロトコルのメジャーバージョン |
| `/system/sample_rate` | int | ro |  | 48000 | `48000` | サンプルレート（48kHz 固定） |
| `/system/block_size` | int | rw | P | 64 / 128 / 256 / 512 / 1024 | `256` | 音声バッファのサンプル数 |
| `/system/clock_device` | string | rw | P |  | `""` | クロック基準デバイスの ID（"" = 自動） |
| `/system/audio_mode` | string | rw | P | shared / exclusive | `"shared"` | WASAPI の動作モード（shared = 共有、exclusive = 排他。48kHz 以外のデバイスはエンジンがレート変換する） |
| `/system/clock_active` | string | ro |  |  | `""` | 実際のクロック基準（device/<id>、timer = 出力デバイスがなく内部タイマー） |
| `/system/latency_ms` | float | ro |  | 0 〜 1000 | `0` | クロック基準デバイスの出力バッファによる遅延 ms |
| `/system/cpu_load` | float | ro |  | 0 〜 1 | `0` | 音声スレッドの負荷率 |
| `/system/xruns` | int | ro |  | 0 〜 9.22337e+18 | `0` | 起動後の音切れ回数 |
| `/devices/input/#/id` | string | ro |  |  | `""` | デバイス ID（WASAPI はエンドポイント ID、ASIO は asio:<ドライバー名>。protocol.md 4.4） |
| `/devices/input/#/name` | string | ro |  |  | `""` | デバイス名 |
| `/devices/input/#/channels` | int | ro |  | 0 〜 64 | `0` | チャンネル数 |
| `/devices/input/#/connected` | bool | ro |  |  | `false` | 接続中 |
| `/devices/input/#/api` | string | ro |  | wasapi / asio / vmic | `"wasapi"` | ドライバー種別 |
| `/devices/input/#/sample_rate` | int | ro |  | 0 〜 384000 | `0` | デバイス側のサンプルレート（0 = 不明。48000 以外はエンジンが変換する） |
| `/devices/output/#/id` | string | ro |  |  | `""` | デバイス ID（WASAPI はエンドポイント ID、ASIO は asio:<ドライバー名>。protocol.md 4.4） |
| `/devices/output/#/name` | string | ro |  |  | `""` | デバイス名 |
| `/devices/output/#/channels` | int | ro |  | 0 〜 64 | `0` | チャンネル数 |
| `/devices/output/#/connected` | bool | ro |  |  | `false` | 接続中 |
| `/devices/output/#/api` | string | ro |  | wasapi / asio / vmic | `"wasapi"` | ドライバー種別 |
| `/devices/output/#/sample_rate` | int | ro |  | 0 〜 384000 | `0` | デバイス側のサンプルレート（0 = 不明。48000 以外はエンジンが変換する） |
| `/devices/process/#/exe` | string | ro |  |  | `""` | 音声を出しているアプリの実行ファイル名 |
| `/devices/process/#/pid` | int | ro |  | 0 〜 9.22337e+18 | `0` | プロセス ID |
| `/devices/process/#/title` | string | ro |  |  | `""` | ウィンドウタイトル |
| `/mixer/track_count` | int | ro |  | 0 〜 64 | `4` | 存在するトラック数 |
| `/mixer/solo_active` | bool | ro |  |  | `false` | どれかのトラックがソロ中 |
| `/cue/target` | string | rw | P |  | `""` | キューの出力デバイス（device/<id>、"" = なし） |
| `/cue/volume_db` | float | rw | P S | -144 〜 12 | `0` | キュー音量 dB |
| `/track/#/position` | int | ro | P | 1 〜 64 | `1` | 画面上の並び順（1 始まり、連番） |
| `/track/#/channels` | int | rw | P | 1 / 2 | `2` | 1 = モノ、2 = ステレオ |
| `/track/#/input/kind` | string | rw | P | none / device / process / lan / sampler | `"none"` | 入力の種類 |
| `/track/#/input/source` | string | rw | P |  | `""` | 入力元（デバイス ID、実行ファイル名（大文字小文字は区別しない）、LAN は <相手の id>/<n>。sampler は "" = 全バンク、"1"〜"8" = そのバンクだけ） |
| `/track/#/input/channel` | int | rw | P | 1 〜 64 | `1` | デバイスの何チャンネル目から使うか |
| `/track/#/input/state` | string | ro |  | none / ok / disconnected | `"none"` | 入力の接続状態 |
| `/track/#/input/lan_buffer_ms` | float | rw | P | 0 〜 200 | `0` | LAN 入力の受信バッファ目標 ms（0 = 自動） |
| `/track/#/input/lan_delay_ms` | float | ro |  | 0 〜 1000 | `0` | LAN 入力の今の受信バッファ量 ms |
| `/track/#/name` | string | rw | P |  | `""` | 表示名 |
| `/track/#/volume_db` | float | rw | P S | -144 〜 12 | `0` | フェーダー音量 dB（-144 = 無音） |
| `/track/#/pan` | float | rw | P S | -1 〜 1 | `0` | パン（-1 = 左、0 = 中央、1 = 右）。ステレオはバランス |
| `/track/#/lowcut/enabled` | bool | rw | P S |  | `false` | ローカット（ハイパス）の有効 |
| `/track/#/lowcut/freq_hz` | float | rw | P S | 20 〜 2000 | `80` | ローカット周波数 Hz |
| `/track/#/highcut/enabled` | bool | rw | P S |  | `false` | ハイカット（ローパス）の有効 |
| `/track/#/highcut/freq_hz` | float | rw | P S | 1000 〜 20000 | `18000` | ハイカット周波数 Hz |
| `/track/#/mute` | bool | rw | P S |  | `false` | ミュート |
| `/track/#/solo` | bool | rw | P S |  | `false` | ソロ |
| `/track/#/cue` | bool | rw | P |  | `false` | キュー（ヘッドホン確認）へ送る |
| `/track/#/meter_id` | int | ro |  | 0 〜 65535 | `0` | UDPメーターパケット内の識別子 |
| `/track/#/record/enabled` | bool | rw | P |  | `false` | トラック別録音（WAV/FLAC）の対象 |
| `/track/#/record/tap` | string | rw | P | pre / post | `"post"` | 録音位置（フェーダー前 / 後） |
| `/track/#/out/#/target` | string | rw | P |  | `""` | 出力先 1〜8。"" / master / track/<n> / device/<id> / vmic/<id> / lan/<n> |
| `/track/#/send/#/target` | string | rw | P |  | `""` | センド先 1〜8。"" / track/<n> / device/<id> / vmic/<id> / lan/<n> |
| `/track/#/send/#/level_db` | float | rw | P S | -144 〜 12 | `0` | センド量 dB |
| `/track/#/send/#/pre_fader` | bool | rw | P S |  | `false` | フェーダー前から送る |
| `/track/#/fx/#/type` | string | ro | P |  | `""` | エフェクト種別（eq4 / comp / reverb / gate / delay / limiter / vst3:<id>） |
| `/track/#/fx/#/name` | string | ro |  |  | `""` | エフェクトの表示名 |
| `/track/#/fx/#/bypass` | bool | rw | P S |  | `false` | バイパス |
| `/track/#/fx/#/param/*` | float | rw | P S |  | `0` | エフェクトのパラメーター。内蔵は下の「エフェクトのパラメーター」表の範囲、VST3 は p<ParamID> で 0〜1 |
| `/track/#/fx/#/status` | string | ro |  | ok / loading / failed / missing | `"ok"` | 状態（loading = 読み込み中、failed = 読み込み失敗、missing = プラグインが見つからない） |
| `/track/#/fx/#/error` | string | ro |  |  | `""` | 失敗の理由 |
| `/track/#/fx/#/latency_samples` | int | ro |  | 0 〜 1000000 | `0` | エフェクトが足す遅延（サンプル） |
| `/track/#/fx/#/param_count` | int | ro |  | 0 〜 1000000 | `0` | パラメーターの総数（VST3 はツリーに載っていないものも数える） |
| `/track/#/fx/#/editor` | bool | rw |  |  | `false` | プラグイン画面を表示する（VST3 のみ、エンジンの PC に出る） |
| `/master/name` | string | rw | P |  | `"Master"` | 表示名 |
| `/master/volume_db` | float | rw | P S | -144 〜 12 | `0` | フェーダー音量 dB（-144 = 無音） |
| `/master/pan` | float | rw | P S | -1 〜 1 | `0` | パン（-1 = 左、0 = 中央、1 = 右）。ステレオはバランス |
| `/master/lowcut/enabled` | bool | rw | P S |  | `false` | ローカット（ハイパス）の有効 |
| `/master/lowcut/freq_hz` | float | rw | P S | 20 〜 2000 | `80` | ローカット周波数 Hz |
| `/master/highcut/enabled` | bool | rw | P S |  | `false` | ハイカット（ローパス）の有効 |
| `/master/highcut/freq_hz` | float | rw | P S | 1000 〜 20000 | `18000` | ハイカット周波数 Hz |
| `/master/mute` | bool | rw | P S |  | `false` | ミュート |
| `/master/meter_id` | int | ro |  | 0 〜 65535 | `0` | UDPメーターパケット内の識別子 |
| `/master/record/enabled` | bool | rw | P |  | `false` | トラック別録音（WAV/FLAC）の対象 |
| `/master/record/tap` | string | rw | P | pre / post | `"post"` | 録音位置（フェーダー前 / 後） |
| `/master/out/#/target` | string | rw | P |  | `""` | 出力先 1〜8。"" / device/<id> / vmic/<id> / lan/<n> |
| `/master/fx/#/type` | string | ro | P |  | `""` | エフェクト種別（eq4 / comp / reverb / gate / delay / limiter / vst3:<id>） |
| `/master/fx/#/name` | string | ro |  |  | `""` | エフェクトの表示名 |
| `/master/fx/#/bypass` | bool | rw | P S |  | `false` | バイパス |
| `/master/fx/#/param/*` | float | rw | P S |  | `0` | エフェクトのパラメーター。内蔵は下の「エフェクトのパラメーター」表の範囲、VST3 は p<ParamID> で 0〜1 |
| `/master/fx/#/status` | string | ro |  | ok / loading / failed / missing | `"ok"` | 状態（loading = 読み込み中、failed = 読み込み失敗、missing = プラグインが見つからない） |
| `/master/fx/#/error` | string | ro |  |  | `""` | 失敗の理由 |
| `/master/fx/#/latency_samples` | int | ro |  | 0 〜 1000000 | `0` | エフェクトが足す遅延（サンプル） |
| `/master/fx/#/param_count` | int | ro |  | 0 〜 1000000 | `0` | パラメーターの総数（VST3 はツリーに載っていないものも数える） |
| `/master/fx/#/editor` | bool | rw |  |  | `false` | プラグイン画面を表示する（VST3 のみ、エンジンの PC に出る） |
| `/capture/state` | string | ro |  | idle / starting / recording / stopping / error | `"idle"` | 録画状態 |
| `/capture/elapsed_ms` | int | ro |  | 0 〜 9.22337e+18 | `0` | 録画経過時間 ms |
| `/capture/file` | string | ro |  |  | `""` | 録画中・直前のファイルパス |
| `/capture/error` | string | ro |  |  | `""` | 直前のエラー内容 |
| `/capture/mix` | int | rw | P | 0 〜 4 | `1` | 録画する映像ミックス（0 = なし） |
| `/capture/container` | string | rw | P | mkv / mp4 | `"mkv"` | コンテナ |
| `/capture/directory` | string | rw | P |  | `""` | 保存先フォルダー（"" = ビデオフォルダー） |
| `/capture/filename` | string | rw | P |  | `"%Y-%m-%d_%H-%M-%S"` | ファイル名パターン（strftime 形式） |
| `/capture/video/encoder` | string | rw | P | auto / nvenc / amf / qsv / x264 | `"auto"` | エンコーダー |
| `/capture/video/codec` | string | rw | P | h264 / hevc / av1 | `"h264"` | コーデック |
| `/capture/video/width` | int | rw | P | 160 〜 3840 | `1920` | 録画解像度（幅） |
| `/capture/video/height` | int | rw | P | 90 〜 2160 | `1080` | 録画解像度（高さ） |
| `/capture/video/fps` | int | rw | P | 10 〜 120 | `60` | フレームレート |
| `/capture/video/rate_control` | string | rw | P | cbr / vbr / cqp | `"vbr"` | レート制御 |
| `/capture/video/bitrate_kbps` | int | rw | P | 500 〜 150000 | `20000` | ビットレート kbps（cbr / vbr） |
| `/capture/video/quality` | int | rw | P | 0 〜 51 | `23` | 品質値（cqp、小さいほど高画質） |
| `/capture/audio/#/source` | string | rw | P |  | `""` | 音声ストリーム 1〜8 の中身（"" / master / track/<n>） |
| `/capture/audio/#/codec` | string | rw | P | aac / opus / pcm | `"aac"` | 音声コーデック |
| `/capture/audio/#/bitrate_kbps` | int | rw | P | 64 〜 512 | `192` | 音声ビットレート kbps（aac / opus） |
| `/recorder/state` | string | ro |  | idle / recording / error | `"idle"` | トラック別録音の状態 |
| `/recorder/elapsed_ms` | int | ro |  | 0 〜 9.22337e+18 | `0` | 録音経過時間 ms |
| `/recorder/folder` | string | ro |  |  | `""` | 録音中・直前のテイクのフォルダー |
| `/recorder/error` | string | ro |  |  | `""` | 直前のエラー内容 |
| `/recorder/dropped_frames` | int | ro |  | 0 〜 9.22337e+18 | `0` | 書き込みが間に合わず無音で埋めたフレーム数（テイクごと） |
| `/recorder/format` | string | rw | P | wav / flac | `"wav"` | 形式（48kHz / 24bit。WAV は 4GB を超えると RF64） |
| `/recorder/directory` | string | rw | P |  | `""` | 保存先フォルダー（"" = ミュージック\Desktop Capture） |
| `/recorder/folder_name` | string | rw | P |  | `"%Y-%m-%d_%H-%M-%S"` | テイクのフォルダー名パターン（strftime 形式） |
| `/recorder/with_capture` | bool | rw | P |  | `true` | 録画（/capture）の開始・停止に合わせて録音も開始・停止する |
| `/sampler/active_bank` | int | rw | P | 1 〜 8 | `1` | 表示・ホットキー・MIDI の対象バンク |
| `/sampler/library/folder` | string | local | P |  | `""` | ライブラリのフォルダー（エンジンの PC の絶対パス。"" = なし）。リモコンからは変えられない |
| `/sampler/bank/#/name` | string | rw | P |  | `""` | バンク名 |
| `/sampler/bank/#/pad/#/file` | string | rw | P |  | `""` | 音声ファイルのパス（WAV / FLAC / MP3 / Ogg Vorbis。"" = 空） |
| `/sampler/bank/#/pad/#/name` | string | rw | P |  | `""` | パッドの表示名（"" = ファイル名） |
| `/sampler/bank/#/pad/#/color` | string | rw | P |  | `""` | パッドの色（#rrggbb、"" = 既定） |
| `/sampler/bank/#/pad/#/mode` | string | rw | P | oneshot / toggle / hold / loop | `"oneshot"` | oneshot = 最後まで再生（押し直すと頭から）、toggle = 押すたびに再生・停止、hold = 押している間だけ、loop = 押すたびにループ再生・停止 |
| `/sampler/bank/#/pad/#/volume_db` | float | rw | P | -144 〜 12 | `0` | パッドの音量 dB |
| `/sampler/bank/#/pad/#/state` | string | ro |  | empty / loading / ready / error | `"empty"` | ファイルの読み込み状態 |
| `/sampler/bank/#/pad/#/error` | string | ro |  |  | `""` | 読み込みエラーの内容 |
| `/sampler/bank/#/pad/#/duration_ms` | int | ro |  | 0 〜 9.22337e+18 | `0` | 音声の長さ ms |
| `/sampler/bank/#/pad/#/playing` | bool | ro |  |  | `false` | 再生中 |
| `/video/mix/#/name` | string | rw | P |  | `""` | 映像ミックス名 |
| `/video/mix/#/width` | int | rw | P | 160 〜 3840 | `1920` | 合成解像度（幅） |
| `/video/mix/#/height` | int | rw | P | 90 〜 2160 | `1080` | 合成解像度（高さ） |
| `/video/mix/#/fps` | int | rw | P | 10 〜 120 | `60` | 合成フレームレート |
| `/video/mix/#/active` | bool | ro |  |  | `false` | 出力先から参照されている（/capture/mix・/video/vcam/mix・/video/monitor/mix・/video/preview/mix・/video/lan/mix） |
| `/video/mix/#/background` | int | rw | P | 0 〜 4294967295 | `4278190080` | 背景色（0xAARRGGBB） |
| `/video/mix/#/program_scene` | int | rw | P | 0 〜 32 | `0` | 本番に出しているシーン（0 = なし）。変えるとトランジションで切り替わる |
| `/video/mix/#/preview_scene` | int | rw | P | 0 〜 32 | `0` | 次に出すシーン（プレビュー、0 = なし） |
| `/video/mix/#/transition/type` | string | rw | P | cut / fade | `"fade"` | シーン切り替えの効果 |
| `/video/mix/#/transition/duration_ms` | int | rw | P | 0 〜 10000 | `300` | フェードの長さ ms |
| `/video/scene/#/name` | string | rw | P |  | `""` | シーン名 |
| `/video/scene/#/position` | int | ro | P | 1 〜 32 | `1` | シーン一覧の並び順（1 始まり、連番） |
| `/video/scene/#/layer/#/name` | string | rw | P |  | `""` | レイヤー名（"" = ソース名を表示） |
| `/video/scene/#/layer/#/position` | int | ro | P | 1 〜 32 | `1` | 重なり順（1 = 最前面、連番） |
| `/video/scene/#/layer/#/source` | int | rw | P | 0 〜 64 | `0` | 表示するソース /video/source/<n>（0 = なし） |
| `/video/scene/#/layer/#/visible` | bool | rw | P |  | `true` | 表示 |
| `/video/scene/#/layer/#/x` | float | rw | P | -4 〜 4 | `0` | 表示枠の左端（キャンバスの幅 = 1） |
| `/video/scene/#/layer/#/y` | float | rw | P | -4 〜 4 | `0` | 表示枠の上端（キャンバスの高さ = 1） |
| `/video/scene/#/layer/#/width` | float | rw | P | 0 〜 8 | `1` | 表示枠の幅（キャンバスの幅 = 1） |
| `/video/scene/#/layer/#/height` | float | rw | P | 0 〜 8 | `1` | 表示枠の高さ（キャンバスの高さ = 1） |
| `/video/scene/#/layer/#/fit` | string | rw | P | fit / fill / stretch | `"fit"` | 枠への収め方（fit = 縦横比を保って全体を表示、fill = 縦横比を保って枠を埋める、stretch = 引き伸ばす） |
| `/video/scene/#/layer/#/crop/left` | float | rw | P | 0 〜 1 | `0` | 切り取り（ソースの幅・高さに対する割合） |
| `/video/scene/#/layer/#/crop/top` | float | rw | P | 0 〜 1 | `0` | 切り取り（ソースの幅・高さに対する割合） |
| `/video/scene/#/layer/#/crop/right` | float | rw | P | 0 〜 1 | `0` | 切り取り（ソースの幅・高さに対する割合） |
| `/video/scene/#/layer/#/crop/bottom` | float | rw | P | 0 〜 1 | `0` | 切り取り（ソースの幅・高さに対する割合） |
| `/video/scene/#/layer/#/opacity` | float | rw | P | 0 〜 1 | `1` | 不透明度 |
| `/video/scene/#/layer/#/chroma/enabled` | bool | rw | P |  | `false` | クロマキー |
| `/video/scene/#/layer/#/chroma/color` | int | rw | P | 0 〜 16777215 | `65280` | 抜く色（0xRRGGBB） |
| `/video/scene/#/layer/#/chroma/similarity` | float | rw | P | 0 〜 1 | `0.4` | 抜く色の範囲（大きいほど広く抜く） |
| `/video/scene/#/layer/#/chroma/smoothness` | float | rw | P | 0 〜 1 | `0.08` | 境目のなめらかさ |
| `/video/scene/#/layer/#/chroma/spill` | float | rw | P | 0 〜 1 | `0.1` | 色かぶりの除去 |
| `/video/scene/#/layer/#/no_signal` | string | rw | P | transparent / image | `"transparent"` | ソースが途切れたときの表示（transparent = 透明、image = 「信号なし」画像） |
| `/video/source/#/name` | string | rw | P |  | `""` | ソース名 |
| `/video/source/#/kind` | string | ro | P | display / window / camera / image / media / text / color / lan | `"display"` | ソースの種類（追加時に決まる） |
| `/video/source/#/state` | string | ro |  | none / ok / disconnected / error | `"none"` | capd が報告する状態（none = どのミックスでも使っていない） |
| `/video/source/#/error` | string | ro |  |  | `""` | state = error の理由 |
| `/video/source/#/width` | int | ro |  | 0 〜 16384 | `0` | ソースの実際の幅（capd が報告） |
| `/video/source/#/height` | int | ro |  | 0 〜 16384 | `0` | ソースの実際の高さ（capd が報告） |
| `/video/source/#/cursor` | bool | rw | P |  | `true` | マウスカーソルを入れる（display / window） |
| `/video/source/#/display/id` | string | rw | P |  | `""` | モニター ID（/video/devices/display/#/id、"" = 主モニター） |
| `/video/source/#/display/method` | string | rw | P | wgc / dda | `"wgc"` | 取得方式（wgc = Windows.Graphics.Capture、dda = Desktop Duplication） |
| `/video/source/#/window/exe` | string | rw | P |  | `""` | ウィンドウの実行ファイル名（例 game.exe） |
| `/video/source/#/window/title` | string | rw | P |  | `""` | ウィンドウタイトル |
| `/video/source/#/window/match` | string | rw | P | title / exe | `"title"` | 閉じたあとにつなぎ直す条件（title = 実行ファイル名とタイトル、exe = 実行ファイル名だけ） |
| `/video/source/#/camera/id` | string | rw | P |  | `""` | カメラ ID（/video/devices/camera/#/id） |
| `/video/source/#/camera/name` | string | rw | P |  | `""` | カメラ名（ID が変わったときの照合用） |
| `/video/source/#/camera/width` | int | rw | P | 0 〜 7680 | `0` | 取り込み解像度の幅（0 = 自動：1920×1080 以下で最大のもの） |
| `/video/source/#/camera/height` | int | rw | P | 0 〜 4320 | `0` | 取り込み解像度の高さ（0 = 自動） |
| `/video/source/#/camera/fps` | int | rw | P | 0 〜 240 | `0` | 取り込みフレームレート（0 = 自動：60 以下で最大） |
| `/video/source/#/file` | string | rw | P |  | `""` | ファイルのパス（image / media） |
| `/video/source/#/media/loop` | bool | rw | P |  | `true` | 動画を繰り返す |
| `/video/source/#/text/content` | string | rw | P |  | `""` | 表示する文字（改行可） |
| `/video/source/#/text/font` | string | rw | P |  | `"Yu Gothic UI"` | フォント名 |
| `/video/source/#/text/size` | int | rw | P | 8 〜 512 | `48` | 文字の大きさ（ピクセル） |
| `/video/source/#/text/color` | int | rw | P | 0 〜 4294967295 | `4294967295` | 文字色（0xAARRGGBB） |
| `/video/source/#/text/background` | int | rw | P | 0 〜 4294967295 | `0` | 背景色（0xAARRGGBB、0 = 透明） |
| `/video/source/#/text/bold` | bool | rw | P |  | `false` | 太字 |
| `/video/source/#/text/italic` | bool | rw | P |  | `false` | 斜体 |
| `/video/source/#/text/align` | string | rw | P | left / center / right | `"left"` | 行揃え |
| `/video/source/#/text/wrap_width` | int | rw | P | 0 〜 7680 | `0` | 折り返し幅（ピクセル、0 = 折り返さない） |
| `/video/source/#/color/value` | int | rw | P | 0 〜 4294967295 | `4278190080` | 単色ソースの色（0xAARRGGBB） |
| `/video/source/#/lan/stream` | string | rw | P |  | `""` | LAN メディア伝送の受信ストリーム ID（レーン N） |
| `/video/devices/display/#/id` | string | ro |  |  | `""` | モニター ID |
| `/video/devices/display/#/name` | string | ro |  |  | `""` | モニター名 |
| `/video/devices/display/#/width` | int | ro |  | 0 〜 16384 | `0` | 幅（物理ピクセル） |
| `/video/devices/display/#/height` | int | ro |  | 0 〜 16384 | `0` | 高さ（物理ピクセル） |
| `/video/devices/display/#/primary` | bool | ro |  |  | `false` | 主モニター |
| `/video/devices/window/#/exe` | string | ro |  |  | `""` | 実行ファイル名 |
| `/video/devices/window/#/title` | string | ro |  |  | `""` | ウィンドウタイトル |
| `/video/devices/camera/#/id` | string | ro |  |  | `""` | カメラ ID |
| `/video/devices/camera/#/name` | string | ro |  |  | `""` | カメラ名 |
| `/video/vcam/mix` | int | rw | P | 0 〜 4 | `0` | 仮想カメラに出す映像ミックス（0 = なし） |
| `/video/vcam/resolution` | string | rw | P | 1080p / 720p | `"1080p"` | 仮想カメラの解像度 |
| `/video/monitor/mix` | int | rw | P | 0 〜 4 | `0` | 全画面表示に出す映像ミックス（0 = なし） |
| `/video/monitor/display` | string | rw | P |  | `""` | 全画面表示先のモニター ID |
| `/video/vcam/state` | string | ro |  | off / waiting / active / unavailable | `"off"` | 仮想カメラの状態（waiting = カメラを開いたアプリがない、unavailable = 未登録など） |
| `/video/vcam/detail` | string | ro |  |  | `""` | 仮想カメラの状態の説明（unavailable の理由など） |
| `/video/monitor/state` | string | ro |  | off / active / no_display / error | `"off"` | 全画面表示の状態（no_display = モニターが見つからない。つながれば自動で戻る） |
| `/video/monitor/detail` | string | ro |  |  | `""` | 全画面表示の状態の説明（error の内容など） |
| `/video/preview/mix` | int | rw | P | 0 〜 4 | `1` | リモコンのプレビューに出す映像ミックス |
| `/video/preview/width` | int | rw | P | 160 〜 1920 | `960` | プレビュー解像度（幅） |
| `/video/preview/height` | int | rw | P | 90 〜 1080 | `540` | プレビュー解像度（高さ） |
| `/video/preview/fps` | int | rw | P | 5 〜 60 | `30` | プレビューのフレームレート |
| `/video/preview/bitrate_kbps` | int | rw | P | 500 〜 8000 | `3000` | プレビューのビットレート kbps |
| `/video/lan/mix` | int | rw | P | 0 〜 4 | `0` | LAN 送信（映像）に出す映像ミックス（0 = なし） |
| `/video/lan/width` | int | rw | P | 160 〜 3840 | `1920` | LAN 送信の解像度（幅） |
| `/video/lan/height` | int | rw | P | 90 〜 2160 | `1080` | LAN 送信の解像度（高さ） |
| `/video/lan/fps` | int | rw | P | 5 〜 60 | `30` | LAN 送信のフレームレート |
| `/video/lan/bitrate_kbps` | int | rw | P | 500 〜 50000 | `8000` | LAN 送信のビットレート kbps |
| `/lan/name` | string | rw |  |  | `""` | LAN で名乗る名前（"" = コンピューター名） |
| `/lan/id` | string | ro |  |  | `""` | この PC のエンジンの id（Welcome.server_id と同じ。インストールごとに固定） |
| `/lan/stream/#/name` | string | rw | P |  | `""` | LAN 送信ストリーム 1〜8 の表示名（"" = LAN <n>）。出力先 lan/<n> で使う |
| `/lan/stream/#/codec` | string | rw | P | pcm16 / pcm24 / opus | `"pcm24"` | コーデック（pcm = 無圧縮・低遅延、opus = 無線向け） |
| `/lan/stream/#/opus_frames` | int | rw | P | 120 / 240 / 480 / 960 | `480` | Opus のパケット長（フレーム数、480 = 10 ms） |
| `/lan/stream/#/bitrate_kbps` | int | rw | P | 32 〜 512 | `128` | Opus のビットレート kbps |
| `/lan/stream/#/stream_id` | int | ro |  | 0 〜 4294967295 | `0` | 今のストリーム ID（パケットの stream_id。送信を開き直すと変わる。0 = 送っていない） |
| `/lan/stream/#/subscribers` | int | ro |  | 0 〜 32 | `0` | 受信している相手の数 |
| `/lan/stream/#/active` | bool | ro |  |  | `false` | 何かが出力していて、受信者がいて送信中 |
| `/lan/sub/#/stream` | string | ro |  |  | `""` | 購読しているストリーム（audio/<n> / video/preview / video/next / video/lan） |
| `/lan/sub/#/address` | string | ro |  |  | `""` | 送り先の UDP アドレス（ip:port） |
| `/lan/sub/#/client` | string | ro |  |  | `""` | 購読した接続の名前 |
| `/lan/peers/#/id` | string | ro |  |  | `""` | LAN 内の別の PC のエンジンの id |
| `/lan/peers/#/name` | string | ro |  |  | `""` | その PC の名前 |
| `/lan/peers/#/address` | string | ro |  |  | `""` | 制御の接続先（ip:port） |
| `/lan/peers/#/state` | string | ro |  | offline / unpaired / connecting / connected / refused | `"offline"` | 相手の状態（refused = ペアリングが取り消された） |
| `/lan/peers/#/manual` | bool | ro |  |  | `false` | 手動で追加した相手（mDNS で見つからない場合） |
| `/lan/peers/#/stream/#/name` | string | ro |  |  | `""` | 相手の LAN 送信ストリームの表示名（接続中だけ） |
| `/lan/peers/#/stream/#/active` | bool | ro |  |  | `false` | 相手のストリームに何かが出力している |
| `/plugins/extra_dirs` | string | rw |  |  | `""` | 標準以外の VST3 フォルダー（; 区切り） |
| `/plugins/scan/state` | string | ro |  | idle / scanning | `"idle"` | プラグインのスキャン状態 |
| `/plugins/scan/progress` | float | ro |  | 0 〜 1 | `0` | スキャンの進み具合 |
| `/plugins/scan/current` | string | ro |  |  | `""` | スキャン中のファイル |
| `/plugins/#/id` | string | ro |  |  | `""` | エフェクト種別として使う ID（vst3:<id>） |
| `/plugins/#/name` | string | ro |  |  | `""` | プラグイン名 |
| `/plugins/#/vendor` | string | ro |  |  | `""` | メーカー |
| `/plugins/#/category` | string | ro |  |  | `""` | 分類（プラグインの申告、例 Fx|EQ） |
| `/plugins/#/instrument` | bool | ro |  |  | `false` | 音源（MIDI で鳴らすもの） |
| `/plugins/#/status` | string | ro |  | ok / failed / crashed / timeout / unsupported | `"ok"` | スキャン結果（unsupported = モノ・ステレオのメイン入出力がない） |
| `/plugins/#/error` | string | ro |  |  | `""` | スキャンで失敗した理由 |
| `/plugins/#/path` | string | ro |  |  | `""` | プラグインのファイル |
| `/remote/clients` | int | ro |  | 0 〜 4 | `0` | 接続中のリモコン数 |
| `/remote/pairing/active` | bool | ro |  |  | `false` | ペアリング受付中 |
| `/remote/paired/#/id` | string | ro |  |  | `""` | ペアリング済みの相手の id（Hello.client_id） |
| `/remote/paired/#/name` | string | ro |  |  | `""` | ペアリング済みの相手の名前 |
| `/remote/paired/#/role` | string | ro |  | remote / peer | `"remote"` | 相手の種類（リモコン / 別の PC のエンジン） |
| `/remote/paired/#/online` | bool | ro |  |  | `false` | 今つながっている |
| `/control/binding/#/trigger` | string | rw |  |  | `""` | きっかけ。key:<修飾+キー>（例 key:ctrl+shift+f9）、midi:note/<ch>/<n>・midi:cc/<ch>/<n>・midi:pc/<ch>/<n>・midi:pb/<ch>（ch 1〜16、n 0〜127）。"" = 未設定 |
| `/control/binding/#/device` | string | rw |  |  | `""` | MIDI 機器名（/control/midi/device/#/name。"" = どの機器でも） |
| `/control/binding/#/action` | string | rw |  | none / record_toggle / record_start / record_stop / recorder_toggle / pad / pad_stop / take / toggle / set / step / param / snapshot | `"none"` | 動作（protocol.md 5.9） |
| `/control/binding/#/target` | string | rw |  |  | `""` | 動作の対象。pad は <パッド> か <バンク>/<パッド>、pad_stop は ""（全バンク）か <バンク>、take は <ミックス>、snapshot は <スナップショット>、toggle・set・step・param はパラメーターのパス |
| `/control/binding/#/value` | string | rw |  |  | `""` | set の値、step の増減（例 -1.5） |
| `/control/binding/#/enabled` | bool | rw |  |  | `true` | 有効 |
| `/control/binding/#/state` | string | ro |  | ok / off / invalid / conflict / unsupported | `"off"` | ok = 使える、off = 無効か未設定、invalid = きっかけ・対象・値が正しくない、conflict = 他のアプリが同じホットキーを使用中、unsupported = この PC では使えない |
| `/control/binding/#/error` | string | ro |  |  | `""` | invalid などの理由 |
| `/control/midi/device/#/name` | string | ro |  |  | `""` | MIDI 入力機器名（同じ名前が複数あれば 2 台目から " #2" を付ける） |
| `/control/midi/device/#/enabled` | bool | rw |  |  | `true` | この機器を開く（false = 他のアプリに譲る。名前で覚える） |
| `/control/midi/device/#/state` | string | ro |  | ok / off / busy / error | `"off"` | ok = 受信中、off = 使わない、busy = 他のアプリが使用中、error = 開けない |
| `/control/midi/last/trigger` | string | ro |  |  | `""` | 最後に受けた MIDI のきっかけ（trigger の形） |
| `/control/midi/last/device` | string | ro |  |  | `""` | 最後に受けた MIDI の機器名 |
| `/control/midi/last/value` | int | ro |  | 0 〜 16383 | `0` | 最後に受けた MIDI の値（ベロシティ・CC 値、pb は 0〜16383） |
| `/control/learn/binding` | int | ro |  | 0 〜 128 | `0` | 学習中の割り当て（0 = 学習していない） |
| `/control/learn/source` | string | ro |  | any / key / midi | `"any"` | 学習で待っているもの |
| `/profile/current` | string | ro |  |  | `""` | 使用中のプロファイル名（自動保存と /profile/save の書き先） |
| `/profile/autosave` | bool | rw |  |  | `true` | 変更を自動で使用中のプロファイルに保存する（PC 共通の設定） |
| `/profile/modified` | bool | ro |  |  | `false` | 使用中のプロファイルに保存していない変更がある |
| `/profile/directory` | string | ro |  |  | `""` | プロファイルのフォルダー |
| `/profile/error` | string | ro |  |  | `""` | 直前の読み込み・保存の失敗の理由（"" = なし） |
| `/profile/list/#/name` | string | ro |  |  | `""` | フォルダーにあるプロファイル（名前順） |
| `/profile/list/#/modified` | int | ro |  | 0 〜 9.22337e+18 | `0` | 最後に保存した時刻（UNIX 秒） |
| `/snapshot/#/name` | string | rw |  |  | `""` | ミキサースナップショットの名前（使用中のプロファイルに入る） |
| `/snapshot/#/modified` | int | ro |  | 0 〜 9.22337e+18 | `0` | スナップショットを撮った時刻（UNIX 秒） |
| `/snapshot/last` | int | ro |  | 0 〜 64 | `0` | 最後に呼び出した・撮ったスナップショット（0 = なし） |

| メソッド | 引数 | 戻り値 | 実装フェーズ | 説明 |
|---|---|---|---|---|
| `/session/resync` | なし | nil | P0 | スナップショットを送り直す（rev の飛びを検出したとき） |
| `/session/clock` | なし | int（マイクロ秒） | P4 | エンジンの PC の時計（QPC、LAN メディアの時刻と同じ）。リモコンがプレビューの遅延を測るのに使う |
| `/mixer/track/add` | channels:int?, name:string?, position:int? | int（新しいトラック番号） | P0 | トラックを追加。番号は空いている最小の番号 |
| `/mixer/track/remove` | track:int | nil | P0 | トラックを削除。番号は詰めない |
| `/mixer/track/move` | track:int, position:int | nil | P0 | 並び順を変える |
| `/capture/record/start` | なし | nil | P0 | 録画開始 |
| `/capture/record/stop` | なし | nil | P0 | 録画停止 |
| `/capture/report` | state:string, file:string?, error:string?, duration_ms:int?, reason:string? | nil | P2 | capd が録画の状態をエンジンに知らせる（role = サービスの接続だけ。reason は録画中のファイル切り替え） |
| `/recorder/start` | なし | string（テイクのフォルダー） | P3 | トラック別録音を開始。録音対象がなければ conflict |
| `/recorder/stop` | なし | nil | P3 | トラック別録音を停止 |
| `/sampler/trigger` | pad:int, bank:int? | nil | P3 | パッドを押す（bank の既定は /sampler/active_bank）。動きは mode による |
| `/sampler/release` | pad:int, bank:int? | nil | P3 | パッドを離す（hold のパッドが止まる） |
| `/sampler/library/list` | dir:string?, offset:int?, count:int? | string（JSON） | P5 | ライブラリのフォルダー 1 段分（dir はライブラリからの相対パス、/ 区切り）。サブフォルダーと読める音声ファイル |
| `/sampler/stop` | bank:int? | nil | P3 | 再生中のパッドを止める（bank なし = 全バンク） |
| `/video/source/add` | kind:string, name:string? | int（新しいソース番号） | P3 | ソースを追加。番号は空いている最小の番号 |
| `/video/source/remove` | source:int | nil | P3 | ソースを削除。そのソースを表示していたレイヤーも消える |
| `/video/source/report` | source:int, state:string, width:int?, height:int?, error:string? | nil | P3 | capd がソースの状態を知らせる（role = サービスの接続だけ） |
| `/video/scene/add` | name:string?, position:int? | int（新しいシーン番号） | P3 | シーンを追加 |
| `/video/scene/remove` | scene:int | nil | P3 | シーンを削除。参照していた program / preview は 0 に戻る |
| `/video/scene/move` | scene:int, position:int | nil | P3 | シーンの並び順を変える |
| `/video/scene/#/layer/add` | source:int, name:string?, position:int? | int（新しいレイヤー番号） | P3 | レイヤーを追加（既定は最前面、キャンバス全体） |
| `/video/scene/#/layer/remove` | layer:int | nil | P3 | レイヤーを削除 |
| `/video/scene/#/layer/move` | layer:int, position:int | nil | P3 | 重なり順を変える（1 = 最前面） |
| `/video/mix/add` | name:string? | int（新しいミックス番号） | P3 | 映像ミックスを追加（最大 4） |
| `/video/mix/remove` | mix:int | nil | P3 | 映像ミックスを削除。参照していた出力先は 0 に戻る |
| `/video/mix/#/take` | なし | nil | P3 | プレビューのシーンをトランジションで本番に出し、直前の本番シーンをプレビューに入れる |
| `/video/devices/refresh` | なし | nil | P3 | モニター・ウィンドウ・カメラ一覧の更新を capd に頼む（イベント video.devices.refresh） |
| `/video/devices/report` | kind:string, <n>/<field>:値… | nil | P3 | capd がモニター・ウィンドウ・カメラの一覧を知らせる（role = サービスの接続だけ） |
| `/track/#/fx/add` | type:string, index:int? | int（スロット番号） | P3 | エフェクト追加 |
| `/track/#/fx/remove` | index:int | nil | P3 | エフェクト削除 |
| `/track/#/fx/move` | from:int, to:int | nil | P3 | エフェクトの順序変更 |
| `/master/fx/add` | type:string, index:int? | int（スロット番号） | P3 | エフェクト追加 |
| `/master/fx/remove` | index:int | nil | P3 | エフェクト削除 |
| `/master/fx/move` | from:int, to:int | nil | P3 | エフェクトの順序変更 |
| `/track/#/fx/#/params` | offset:int?, count:int?, keys:string? | string（JSON 配列） | P3 | パラメーターの名前・範囲・表示文字列を取得。VST3 は取得したものをツリーに載せる |
| `/master/fx/#/params` | offset:int?, count:int?, keys:string? | string（JSON 配列） | P3 | パラメーターの名前・範囲・表示文字列を取得。VST3 は取得したものをツリーに載せる |
| `/plugins/scan` | rescan:bool? | nil | P3 | VST3 プラグインを探す（rescan = 前回の結果を使わずに全部） |
| `/devices/refresh` | なし | nil | P2 | デバイス・プロセス一覧の更新 |
| `/video/output/report` | output:string, state:string, detail:string? | nil | P3 | capd が出力（vcam / monitor）の状態をエンジンに知らせる（role = サービスの接続だけ） |
| `/remote/pairing/start` | なし | string（6桁コード） | P3 | ペアリング受付開始（2 分、1 台つながるまで） |
| `/remote/pairing/stop` | なし | nil | P3 | ペアリング受付終了 |
| `/remote/unpair` | id:string | nil | P3 | ペアリングを取り消す（つながっていれば切断） |
| `/lan/subscribe` | stream:string, port:int | int（購読番号） | P3 | ストリームを受け取る。送り先は接続元の IP の port。接続が切れると自動で解除 |
| `/lan/unsubscribe` | id:int | nil | P3 | 購読をやめる |
| `/lan/peers/pair` | id:string, code:string | nil | P3 | 相手の PC に表示された 6 桁コードでペアリングする |
| `/lan/peers/forget` | id:string | nil | P3 | 相手のペアリング情報を消す（手動で追加した相手は一覧からも消す） |
| `/lan/peers/add` | address:string | nil | P3 | 相手を IP アドレス（ip または ip:port）で手動追加 |
| `/control/binding/add` | trigger:string?, device:string?, action:string?, target:string?, value:string? | int（新しい割り当て番号） | P4 | ホットキー・MIDI の割り当てを追加（最大 128） |
| `/control/binding/remove` | binding:int | nil | P4 | 割り当てを削除。番号は詰めない |
| `/control/learn` | binding:int, source:string? | nil | P4 | 次に押されたキーの組み合わせ（エンジンの PC）か MIDI を割り当てのきっかけにする（source = any / key / midi、30 秒で終わる） |
| `/control/learn/cancel` | なし | nil | P4 | 学習をやめる |
| `/profile/save` | name:string? | string（保存したプロファイル名） | P5 | 今の状態を使用中のプロファイルに保存。name を付けるとその名前で保存して、以後はそれを使う（別名で保存） |
| `/profile/load` | name:string | nil | P5 | プロファイルに切り替える（自動保存が有効なら今のプロファイルを先に保存）。録画中・録音中は conflict |
| `/profile/new` | name:string | nil | P5 | 初期状態の新しいプロファイルを作って切り替える。録画中・録音中は conflict |
| `/profile/rename` | name:string, new_name:string | nil | P5 | プロファイルの名前を変える |
| `/profile/remove` | name:string | nil | P5 | プロファイルを削除（使用中のものは conflict） |
| `/snapshot/save` | name:string?, snapshot:int? | int（スナップショット番号） | P5 | 今のミキサーの値をスナップショットに撮る（snapshot を付けるとそれを上書き）。最大 64 |
| `/snapshot/recall` | snapshot:int | nil | P5 | スナップショットのミキサーの値に戻す |
| `/snapshot/remove` | snapshot:int | nil | P5 | スナップショットを削除。番号は詰めない |

## エフェクトのパラメーター（内蔵）

`/track/#/fx/#/param/<キー>`（マスターは `/master/fx/#/param/<キー>`）。値は単位そのまま（float）。対数は UI のスライダーを対数で動かすもの。

### `eq4` 4バンドEQ

| キー | 名前 | 単位 | 範囲 | 既定値 | 備考 |
|---|---|---|---|---|---|
| `low_freq_hz` | Low 周波数 | Hz | 20 〜 1000 | 100 | 対数 |
| `low_gain_db` | Low ゲイン | dB | -18 〜 18 | 0 |  |
| `mid1_freq_hz` | Mid1 周波数 | Hz | 40 〜 10000 | 400 | 対数 |
| `mid1_gain_db` | Mid1 ゲイン | dB | -18 〜 18 | 0 |  |
| `mid1_q` | Mid1 Q |  | 0.1 〜 10 | 1 | 対数 |
| `mid2_freq_hz` | Mid2 周波数 | Hz | 100 〜 16000 | 2500 | 対数 |
| `mid2_gain_db` | Mid2 ゲイン | dB | -18 〜 18 | 0 |  |
| `mid2_q` | Mid2 Q |  | 0.1 〜 10 | 1 | 対数 |
| `high_freq_hz` | High 周波数 | Hz | 1000 〜 20000 | 8000 | 対数 |
| `high_gain_db` | High ゲイン | dB | -18 〜 18 | 0 |  |
| `output_db` | 出力 | dB | -18 〜 18 | 0 |  |

### `comp` コンプレッサー

| キー | 名前 | 単位 | 範囲 | 既定値 | 備考 |
|---|---|---|---|---|---|
| `threshold_db` | スレッショルド | dB | -60 〜 0 | -18 |  |
| `ratio` | レシオ | :1 | 1 〜 20 | 4 | 対数 |
| `attack_ms` | アタック | ms | 0.1 〜 200 | 10 | 対数 |
| `release_ms` | リリース | ms | 10 〜 2000 | 150 | 対数 |
| `knee_db` | ニー | dB | 0 〜 24 | 6 |  |
| `makeup_db` | メイクアップ | dB | 0 〜 24 | 0 |  |

### `gate` ゲート

| キー | 名前 | 単位 | 範囲 | 既定値 | 備考 |
|---|---|---|---|---|---|
| `threshold_db` | スレッショルド | dB | -90 〜 0 | -50 |  |
| `floor_db` | 閉じたときの音量 | dB | -90 〜 0 | -90 |  |
| `attack_ms` | アタック | ms | 0.1 〜 50 | 0.5 | 対数 |
| `hold_ms` | ホールド | ms | 0 〜 500 | 50 |  |
| `release_ms` | リリース | ms | 5 〜 2000 | 150 | 対数 |

### `reverb` リバーブ

| キー | 名前 | 単位 | 範囲 | 既定値 | 備考 |
|---|---|---|---|---|---|
| `size` | 広さ |  | 0 〜 1 | 0.5 |  |
| `damping` | 高域の減衰 |  | 0 〜 1 | 0.5 |  |
| `predelay_ms` | プリディレイ | ms | 0 〜 200 | 10 |  |
| `width` | ステレオ幅 |  | 0 〜 1 | 1 |  |
| `mix` | ミックス |  | 0 〜 1 | 0.25 |  |

### `delay` ディレイ

| キー | 名前 | 単位 | 範囲 | 既定値 | 備考 |
|---|---|---|---|---|---|
| `time_ms` | ディレイタイム | ms | 1 〜 2000 | 300 | 対数 |
| `feedback` | フィードバック |  | 0 〜 0.95 | 0.35 |  |
| `highcut_hz` | フィードバックのハイカット | Hz | 1000 〜 20000 | 8000 | 対数 |
| `ping_pong` | ピンポン |  | 0 / 1 | 0 | スイッチ |
| `mix` | ミックス |  | 0 〜 1 | 0.25 |  |

### `limiter` リミッター（遅延 48 サンプル）

| キー | 名前 | 単位 | 範囲 | 既定値 | 備考 |
|---|---|---|---|---|---|
| `input_db` | 入力ゲイン | dB | 0 〜 24 | 0 |  |
| `ceiling_db` | シーリング | dB | -24 〜 0 | -1 |  |
| `release_ms` | リリース | ms | 1 〜 1000 | 50 | 対数 |
