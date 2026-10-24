<!-- このファイルは生成物です。編集せず、libs/protocol/src/schema_v1.cpp を直してから
     dc-cli schema --markdown > docs/spec/protocol-params.md で作り直してください。 -->

# パラメーター一覧（プロトコル v1）

`#` は 1 から始まる番号、`*` は任意の名前を表します。書込が ro のものはエンジンだけが変更します。

| パス | 型 | 書込 | 範囲・選択肢 | 既定値 | 説明 |
|---|---|---|---|---|---|
| `/system/version` | string | ro |  | `""` | エンジンのバージョン |
| `/system/protocol` | int | ro | 1 〜 1000 | `1` | プロトコルのメジャーバージョン |
| `/system/sample_rate` | int | ro | 48000 | `48000` | サンプルレート（48kHz 固定） |
| `/system/block_size` | int | rw | 64 / 128 / 256 / 512 / 1024 | `256` | 音声バッファのサンプル数 |
| `/system/clock_device` | string | rw |  | `""` | クロック基準デバイスの ID（"" = 自動） |
| `/system/audio_mode` | string | rw | shared / exclusive | `"shared"` | WASAPI の動作モード（shared = 共有、exclusive = 排他。排他は 48kHz で開けるデバイスだけ） |
| `/system/clock_active` | string | ro |  | `""` | 実際のクロック基準（device/<id>、timer = 出力デバイスがなく内部タイマー） |
| `/system/latency_ms` | float | ro | 0 〜 1000 | `0` | クロック基準デバイスの出力バッファによる遅延 ms |
| `/system/cpu_load` | float | ro | 0 〜 1 | `0` | 音声スレッドの負荷率 |
| `/system/xruns` | int | ro | 0 〜 9.22337e+18 | `0` | 起動後の音切れ回数 |
| `/devices/input/#/id` | string | ro |  | `""` | デバイス ID（WASAPI エンドポイント ID） |
| `/devices/input/#/name` | string | ro |  | `""` | デバイス名 |
| `/devices/input/#/channels` | int | ro | 0 〜 64 | `0` | チャンネル数 |
| `/devices/input/#/connected` | bool | ro |  | `false` | 接続中 |
| `/devices/input/#/api` | string | ro | wasapi / asio / vmic | `"wasapi"` | ドライバー種別 |
| `/devices/output/#/id` | string | ro |  | `""` | デバイス ID（WASAPI エンドポイント ID） |
| `/devices/output/#/name` | string | ro |  | `""` | デバイス名 |
| `/devices/output/#/channels` | int | ro | 0 〜 64 | `0` | チャンネル数 |
| `/devices/output/#/connected` | bool | ro |  | `false` | 接続中 |
| `/devices/output/#/api` | string | ro | wasapi / asio / vmic | `"wasapi"` | ドライバー種別 |
| `/devices/process/#/exe` | string | ro |  | `""` | 音声を出しているアプリの実行ファイル名 |
| `/devices/process/#/pid` | int | ro | 0 〜 9.22337e+18 | `0` | プロセス ID |
| `/devices/process/#/title` | string | ro |  | `""` | ウィンドウタイトル |
| `/mixer/track_count` | int | ro | 0 〜 64 | `4` | 存在するトラック数 |
| `/mixer/solo_active` | bool | ro |  | `false` | どれかのトラックがソロ中 |
| `/cue/target` | string | rw |  | `""` | キューの出力デバイス（device/<id>、"" = なし） |
| `/cue/volume_db` | float | rw | -144 〜 12 | `0` | キュー音量 dB |
| `/track/#/position` | int | ro | 1 〜 64 | `1` | 画面上の並び順（1 始まり、連番） |
| `/track/#/channels` | int | rw | 1 / 2 | `2` | 1 = モノ、2 = ステレオ |
| `/track/#/input/kind` | string | rw | none / device / process / lan / sampler | `"none"` | 入力の種類 |
| `/track/#/input/source` | string | rw |  | `""` | 入力元（デバイス ID、実行ファイル名、LAN ストリーム ID） |
| `/track/#/input/channel` | int | rw | 1 〜 64 | `1` | デバイスの何チャンネル目から使うか |
| `/track/#/input/state` | string | ro | none / ok / disconnected | `"none"` | 入力の接続状態 |
| `/track/#/name` | string | rw |  | `""` | 表示名 |
| `/track/#/volume_db` | float | rw | -144 〜 12 | `0` | フェーダー音量 dB（-144 = 無音） |
| `/track/#/pan` | float | rw | -1 〜 1 | `0` | パン（-1 = 左、0 = 中央、1 = 右）。ステレオはバランス |
| `/track/#/lowcut/enabled` | bool | rw |  | `false` | ローカット（ハイパス）の有効 |
| `/track/#/lowcut/freq_hz` | float | rw | 20 〜 2000 | `80` | ローカット周波数 Hz |
| `/track/#/highcut/enabled` | bool | rw |  | `false` | ハイカット（ローパス）の有効 |
| `/track/#/highcut/freq_hz` | float | rw | 1000 〜 20000 | `18000` | ハイカット周波数 Hz |
| `/track/#/mute` | bool | rw |  | `false` | ミュート |
| `/track/#/solo` | bool | rw |  | `false` | ソロ |
| `/track/#/cue` | bool | rw |  | `false` | キュー（ヘッドホン確認）へ送る |
| `/track/#/meter_id` | int | ro | 0 〜 65535 | `0` | UDPメーターパケット内の識別子 |
| `/track/#/record/enabled` | bool | rw |  | `false` | トラック別録音（WAV/FLAC）の対象 |
| `/track/#/record/tap` | string | rw | pre / post | `"post"` | 録音位置（フェーダー前 / 後） |
| `/track/#/out/#/target` | string | rw |  | `""` | 出力先 1〜8。"" / master / track/<n> / device/<id> / vmic/<id> / lan/<stream> |
| `/track/#/send/#/target` | string | rw |  | `""` | センド先 1〜8。"" / track/<n> / device/<id> / vmic/<id> / lan/<stream> |
| `/track/#/send/#/level_db` | float | rw | -144 〜 12 | `0` | センド量 dB |
| `/track/#/send/#/pre_fader` | bool | rw |  | `false` | フェーダー前から送る |
| `/track/#/fx/#/type` | string | ro |  | `""` | エフェクト種別（eq4 / comp / reverb / gate / delay / limiter / vst3:<id>） |
| `/track/#/fx/#/name` | string | ro |  | `""` | エフェクトの表示名 |
| `/track/#/fx/#/bypass` | bool | rw |  | `false` | バイパス |
| `/track/#/fx/#/param/*` | float | rw |  | `0` | エフェクトのパラメーター。範囲はエフェクト種別ごとに P3 で定義 |
| `/master/name` | string | rw |  | `"Master"` | 表示名 |
| `/master/volume_db` | float | rw | -144 〜 12 | `0` | フェーダー音量 dB（-144 = 無音） |
| `/master/pan` | float | rw | -1 〜 1 | `0` | パン（-1 = 左、0 = 中央、1 = 右）。ステレオはバランス |
| `/master/lowcut/enabled` | bool | rw |  | `false` | ローカット（ハイパス）の有効 |
| `/master/lowcut/freq_hz` | float | rw | 20 〜 2000 | `80` | ローカット周波数 Hz |
| `/master/highcut/enabled` | bool | rw |  | `false` | ハイカット（ローパス）の有効 |
| `/master/highcut/freq_hz` | float | rw | 1000 〜 20000 | `18000` | ハイカット周波数 Hz |
| `/master/mute` | bool | rw |  | `false` | ミュート |
| `/master/meter_id` | int | ro | 0 〜 65535 | `0` | UDPメーターパケット内の識別子 |
| `/master/record/enabled` | bool | rw |  | `false` | トラック別録音（WAV/FLAC）の対象 |
| `/master/record/tap` | string | rw | pre / post | `"post"` | 録音位置（フェーダー前 / 後） |
| `/master/out/#/target` | string | rw |  | `""` | 出力先 1〜8。"" / device/<id> / vmic/<id> / lan/<stream> |
| `/master/fx/#/type` | string | ro |  | `""` | エフェクト種別（eq4 / comp / reverb / gate / delay / limiter / vst3:<id>） |
| `/master/fx/#/name` | string | ro |  | `""` | エフェクトの表示名 |
| `/master/fx/#/bypass` | bool | rw |  | `false` | バイパス |
| `/master/fx/#/param/*` | float | rw |  | `0` | エフェクトのパラメーター。範囲はエフェクト種別ごとに P3 で定義 |
| `/capture/state` | string | ro | idle / starting / recording / stopping / error | `"idle"` | 録画状態 |
| `/capture/elapsed_ms` | int | ro | 0 〜 9.22337e+18 | `0` | 録画経過時間 ms |
| `/capture/file` | string | ro |  | `""` | 録画中・直前のファイルパス |
| `/capture/error` | string | ro |  | `""` | 直前のエラー内容 |
| `/capture/mix` | int | rw | 0 〜 4 | `1` | 録画する映像ミックス（0 = なし） |
| `/capture/container` | string | rw | mkv / mp4 | `"mkv"` | コンテナ |
| `/capture/directory` | string | rw |  | `""` | 保存先フォルダー（"" = ビデオフォルダー） |
| `/capture/filename` | string | rw |  | `"%Y-%m-%d_%H-%M-%S"` | ファイル名パターン（strftime 形式） |
| `/capture/video/encoder` | string | rw | auto / nvenc / amf / qsv / x264 | `"auto"` | エンコーダー |
| `/capture/video/codec` | string | rw | h264 / hevc / av1 | `"h264"` | コーデック |
| `/capture/video/width` | int | rw | 160 〜 3840 | `1920` | 録画解像度（幅） |
| `/capture/video/height` | int | rw | 90 〜 2160 | `1080` | 録画解像度（高さ） |
| `/capture/video/fps` | int | rw | 10 〜 120 | `60` | フレームレート |
| `/capture/video/rate_control` | string | rw | cbr / vbr / cqp | `"vbr"` | レート制御 |
| `/capture/video/bitrate_kbps` | int | rw | 500 〜 150000 | `20000` | ビットレート kbps（cbr / vbr） |
| `/capture/video/quality` | int | rw | 0 〜 51 | `23` | 品質値（cqp、小さいほど高画質） |
| `/capture/audio/#/source` | string | rw |  | `""` | 音声ストリーム 1〜8 の中身（"" / master / track/<n>） |
| `/capture/audio/#/codec` | string | rw | aac / opus / pcm | `"aac"` | 音声コーデック |
| `/capture/audio/#/bitrate_kbps` | int | rw | 64 〜 512 | `192` | 音声ビットレート kbps（aac / opus） |
| `/video/mix/#/name` | string | rw |  | `""` | 映像ミックス名 |
| `/video/mix/#/width` | int | rw | 160 〜 3840 | `1920` | 合成解像度（幅） |
| `/video/mix/#/height` | int | rw | 90 〜 2160 | `1080` | 合成解像度（高さ） |
| `/video/mix/#/fps` | int | rw | 10 〜 120 | `60` | 合成フレームレート |
| `/video/mix/#/active` | bool | ro |  | `false` | 出力先から参照されている（/capture/mix・/video/vcam/mix・/video/monitor/mix・/video/preview/mix） |
| `/video/mix/#/background` | int | rw | 0 〜 4294967295 | `4278190080` | 背景色（0xAARRGGBB） |
| `/video/mix/#/program_scene` | int | rw | 0 〜 32 | `0` | 本番に出しているシーン（0 = なし）。変えるとトランジションで切り替わる |
| `/video/mix/#/preview_scene` | int | rw | 0 〜 32 | `0` | 次に出すシーン（プレビュー、0 = なし） |
| `/video/mix/#/transition/type` | string | rw | cut / fade | `"fade"` | シーン切り替えの効果 |
| `/video/mix/#/transition/duration_ms` | int | rw | 0 〜 10000 | `300` | フェードの長さ ms |
| `/video/scene/#/name` | string | rw |  | `""` | シーン名 |
| `/video/scene/#/position` | int | ro | 1 〜 32 | `1` | シーン一覧の並び順（1 始まり、連番） |
| `/video/scene/#/layer/#/name` | string | rw |  | `""` | レイヤー名（"" = ソース名を表示） |
| `/video/scene/#/layer/#/position` | int | ro | 1 〜 32 | `1` | 重なり順（1 = 最前面、連番） |
| `/video/scene/#/layer/#/source` | int | rw | 0 〜 64 | `0` | 表示するソース /video/source/<n>（0 = なし） |
| `/video/scene/#/layer/#/visible` | bool | rw |  | `true` | 表示 |
| `/video/scene/#/layer/#/x` | float | rw | -4 〜 4 | `0` | 表示枠の左端（キャンバスの幅 = 1） |
| `/video/scene/#/layer/#/y` | float | rw | -4 〜 4 | `0` | 表示枠の上端（キャンバスの高さ = 1） |
| `/video/scene/#/layer/#/width` | float | rw | 0 〜 8 | `1` | 表示枠の幅（キャンバスの幅 = 1） |
| `/video/scene/#/layer/#/height` | float | rw | 0 〜 8 | `1` | 表示枠の高さ（キャンバスの高さ = 1） |
| `/video/scene/#/layer/#/fit` | string | rw | fit / fill / stretch | `"fit"` | 枠への収め方（fit = 縦横比を保って全体を表示、fill = 縦横比を保って枠を埋める、stretch = 引き伸ばす） |
| `/video/scene/#/layer/#/crop/left` | float | rw | 0 〜 1 | `0` | 切り取り（ソースの幅・高さに対する割合） |
| `/video/scene/#/layer/#/crop/top` | float | rw | 0 〜 1 | `0` | 切り取り（ソースの幅・高さに対する割合） |
| `/video/scene/#/layer/#/crop/right` | float | rw | 0 〜 1 | `0` | 切り取り（ソースの幅・高さに対する割合） |
| `/video/scene/#/layer/#/crop/bottom` | float | rw | 0 〜 1 | `0` | 切り取り（ソースの幅・高さに対する割合） |
| `/video/scene/#/layer/#/opacity` | float | rw | 0 〜 1 | `1` | 不透明度 |
| `/video/scene/#/layer/#/chroma/enabled` | bool | rw |  | `false` | クロマキー |
| `/video/scene/#/layer/#/chroma/color` | int | rw | 0 〜 16777215 | `65280` | 抜く色（0xRRGGBB） |
| `/video/scene/#/layer/#/chroma/similarity` | float | rw | 0 〜 1 | `0.4` | 抜く色の範囲（大きいほど広く抜く） |
| `/video/scene/#/layer/#/chroma/smoothness` | float | rw | 0 〜 1 | `0.08` | 境目のなめらかさ |
| `/video/scene/#/layer/#/chroma/spill` | float | rw | 0 〜 1 | `0.1` | 色かぶりの除去 |
| `/video/scene/#/layer/#/no_signal` | string | rw | transparent / image | `"transparent"` | ソースが途切れたときの表示（transparent = 透明、image = 「信号なし」画像） |
| `/video/source/#/name` | string | rw |  | `""` | ソース名 |
| `/video/source/#/kind` | string | ro | display / window / camera / image / media / text / color / lan | `"display"` | ソースの種類（追加時に決まる） |
| `/video/source/#/state` | string | ro | none / ok / disconnected / error | `"none"` | capd が報告する状態（none = どのミックスでも使っていない） |
| `/video/source/#/error` | string | ro |  | `""` | state = error の理由 |
| `/video/source/#/width` | int | ro | 0 〜 16384 | `0` | ソースの実際の幅（capd が報告） |
| `/video/source/#/height` | int | ro | 0 〜 16384 | `0` | ソースの実際の高さ（capd が報告） |
| `/video/source/#/cursor` | bool | rw |  | `true` | マウスカーソルを入れる（display / window） |
| `/video/source/#/display/id` | string | rw |  | `""` | モニター ID（/video/devices/display/#/id、"" = 主モニター） |
| `/video/source/#/display/method` | string | rw | wgc / dda | `"wgc"` | 取得方式（wgc = Windows.Graphics.Capture、dda = Desktop Duplication） |
| `/video/source/#/window/exe` | string | rw |  | `""` | ウィンドウの実行ファイル名（例 game.exe） |
| `/video/source/#/window/title` | string | rw |  | `""` | ウィンドウタイトル |
| `/video/source/#/window/match` | string | rw | title / exe | `"title"` | 閉じたあとにつなぎ直す条件（title = 実行ファイル名とタイトル、exe = 実行ファイル名だけ） |
| `/video/source/#/camera/id` | string | rw |  | `""` | カメラ ID（/video/devices/camera/#/id） |
| `/video/source/#/camera/name` | string | rw |  | `""` | カメラ名（ID が変わったときの照合用） |
| `/video/source/#/camera/width` | int | rw | 0 〜 7680 | `0` | 取り込み解像度の幅（0 = 自動：1920×1080 以下で最大のもの） |
| `/video/source/#/camera/height` | int | rw | 0 〜 4320 | `0` | 取り込み解像度の高さ（0 = 自動） |
| `/video/source/#/camera/fps` | int | rw | 0 〜 240 | `0` | 取り込みフレームレート（0 = 自動：60 以下で最大） |
| `/video/source/#/file` | string | rw |  | `""` | ファイルのパス（image / media） |
| `/video/source/#/media/loop` | bool | rw |  | `true` | 動画を繰り返す |
| `/video/source/#/text/content` | string | rw |  | `""` | 表示する文字（改行可） |
| `/video/source/#/text/font` | string | rw |  | `"Yu Gothic UI"` | フォント名 |
| `/video/source/#/text/size` | int | rw | 8 〜 512 | `48` | 文字の大きさ（ピクセル） |
| `/video/source/#/text/color` | int | rw | 0 〜 4294967295 | `4294967295` | 文字色（0xAARRGGBB） |
| `/video/source/#/text/background` | int | rw | 0 〜 4294967295 | `0` | 背景色（0xAARRGGBB、0 = 透明） |
| `/video/source/#/text/bold` | bool | rw |  | `false` | 太字 |
| `/video/source/#/text/italic` | bool | rw |  | `false` | 斜体 |
| `/video/source/#/text/align` | string | rw | left / center / right | `"left"` | 行揃え |
| `/video/source/#/text/wrap_width` | int | rw | 0 〜 7680 | `0` | 折り返し幅（ピクセル、0 = 折り返さない） |
| `/video/source/#/color/value` | int | rw | 0 〜 4294967295 | `4278190080` | 単色ソースの色（0xAARRGGBB） |
| `/video/source/#/lan/stream` | string | rw |  | `""` | LAN メディア伝送の受信ストリーム ID（レーン N） |
| `/video/devices/display/#/id` | string | ro |  | `""` | モニター ID |
| `/video/devices/display/#/name` | string | ro |  | `""` | モニター名 |
| `/video/devices/display/#/width` | int | ro | 0 〜 16384 | `0` | 幅（物理ピクセル） |
| `/video/devices/display/#/height` | int | ro | 0 〜 16384 | `0` | 高さ（物理ピクセル） |
| `/video/devices/display/#/primary` | bool | ro |  | `false` | 主モニター |
| `/video/devices/window/#/exe` | string | ro |  | `""` | 実行ファイル名 |
| `/video/devices/window/#/title` | string | ro |  | `""` | ウィンドウタイトル |
| `/video/devices/camera/#/id` | string | ro |  | `""` | カメラ ID |
| `/video/devices/camera/#/name` | string | ro |  | `""` | カメラ名 |
| `/video/vcam/mix` | int | rw | 0 〜 4 | `0` | 仮想カメラに出す映像ミックス（0 = なし） |
| `/video/vcam/resolution` | string | rw | 1080p / 720p | `"1080p"` | 仮想カメラの解像度 |
| `/video/monitor/mix` | int | rw | 0 〜 4 | `0` | 全画面表示に出す映像ミックス（0 = なし） |
| `/video/monitor/display` | string | rw |  | `""` | 全画面表示先のモニター ID |
| `/video/preview/mix` | int | rw | 0 〜 4 | `1` | リモコンのプレビューに出す映像ミックス |
| `/video/preview/width` | int | rw | 160 〜 1920 | `960` | プレビュー解像度（幅） |
| `/video/preview/height` | int | rw | 90 〜 1080 | `540` | プレビュー解像度（高さ） |
| `/video/preview/fps` | int | rw | 5 〜 60 | `30` | プレビューのフレームレート |
| `/video/preview/bitrate_kbps` | int | rw | 500 〜 8000 | `3000` | プレビューのビットレート kbps |
| `/remote/clients` | int | ro | 0 〜 4 | `0` | 接続中のリモコン数 |
| `/remote/pairing/active` | bool | ro |  | `false` | ペアリング受付中 |

| メソッド | 引数 | 戻り値 | 実装フェーズ | 説明 |
|---|---|---|---|---|
| `/session/resync` | なし | nil | P0 | スナップショットを送り直す（rev の飛びを検出したとき） |
| `/mixer/track/add` | channels:int?, name:string?, position:int? | int（新しいトラック番号） | P0 | トラックを追加。番号は空いている最小の番号 |
| `/mixer/track/remove` | track:int | nil | P0 | トラックを削除。番号は詰めない |
| `/mixer/track/move` | track:int, position:int | nil | P0 | 並び順を変える |
| `/capture/record/start` | なし | nil | P0 | 録画開始 |
| `/capture/record/stop` | なし | nil | P0 | 録画停止 |
| `/capture/report` | state:string, file:string?, error:string?, duration_ms:int? | nil | P2 | capd が録画の状態をエンジンに知らせる（role = サービスの接続だけ） |
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
| `/devices/refresh` | なし | nil | P2 | デバイス・プロセス一覧の更新 |
| `/remote/pairing/start` | なし | string（6桁コード） | P4 | ペアリング受付開始 |
| `/remote/pairing/stop` | なし | nil | P4 | ペアリング受付終了 |
