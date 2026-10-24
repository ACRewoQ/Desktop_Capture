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
| `/track/#/input/source` | string | rw |  | `""` | 入力元（デバイス ID、実行ファイル名、LAN は <相手の id>/<n>） |
| `/track/#/input/channel` | int | rw | 1 〜 64 | `1` | デバイスの何チャンネル目から使うか |
| `/track/#/input/state` | string | ro | none / ok / disconnected | `"none"` | 入力の接続状態 |
| `/track/#/input/lan_buffer_ms` | float | rw | 0 〜 200 | `0` | LAN 入力の受信バッファ目標 ms（0 = 自動） |
| `/track/#/input/lan_delay_ms` | float | ro | 0 〜 1000 | `0` | LAN 入力の今の受信バッファ量 ms |
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
| `/track/#/out/#/target` | string | rw |  | `""` | 出力先 1〜8。"" / master / track/<n> / device/<id> / vmic/<id> / lan/<n> |
| `/track/#/send/#/target` | string | rw |  | `""` | センド先 1〜8。"" / track/<n> / device/<id> / vmic/<id> / lan/<n> |
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
| `/master/out/#/target` | string | rw |  | `""` | 出力先 1〜8。"" / device/<id> / vmic/<id> / lan/<n> |
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
| `/video/mix/#/active` | bool | ro |  | `false` | 出力先があり合成中 |
| `/video/vcam/mix` | int | rw | 0 〜 4 | `0` | 仮想カメラに出す映像ミックス（0 = なし） |
| `/video/vcam/resolution` | string | rw | 1080p / 720p | `"1080p"` | 仮想カメラの解像度 |
| `/video/monitor/mix` | int | rw | 0 〜 4 | `0` | 全画面表示に出す映像ミックス（0 = なし） |
| `/video/monitor/display` | string | rw |  | `""` | 全画面表示先のモニター ID |
| `/video/preview/mix` | int | rw | 0 〜 4 | `1` | リモコンのプレビューに出す映像ミックス |
| `/video/preview/width` | int | rw | 160 〜 1920 | `960` | プレビュー解像度（幅） |
| `/video/preview/height` | int | rw | 90 〜 1080 | `540` | プレビュー解像度（高さ） |
| `/video/preview/fps` | int | rw | 5 〜 60 | `30` | プレビューのフレームレート |
| `/video/preview/bitrate_kbps` | int | rw | 500 〜 8000 | `3000` | プレビューのビットレート kbps |
| `/video/lan/mix` | int | rw | 0 〜 4 | `0` | LAN 送信（映像）に出す映像ミックス（0 = なし） |
| `/video/lan/width` | int | rw | 160 〜 3840 | `1920` | LAN 送信の解像度（幅） |
| `/video/lan/height` | int | rw | 90 〜 2160 | `1080` | LAN 送信の解像度（高さ） |
| `/video/lan/fps` | int | rw | 5 〜 60 | `30` | LAN 送信のフレームレート |
| `/video/lan/bitrate_kbps` | int | rw | 500 〜 50000 | `8000` | LAN 送信のビットレート kbps |
| `/lan/name` | string | rw |  | `""` | LAN で名乗る名前（"" = コンピューター名） |
| `/lan/id` | string | ro |  | `""` | この PC のエンジンの id（Welcome.server_id と同じ。インストールごとに固定） |
| `/lan/stream/#/name` | string | rw |  | `""` | LAN 送信ストリーム 1〜8 の表示名（"" = LAN <n>）。出力先 lan/<n> で使う |
| `/lan/stream/#/codec` | string | rw | pcm16 / pcm24 / opus | `"pcm24"` | コーデック（pcm = 無圧縮・低遅延、opus = 無線向け） |
| `/lan/stream/#/opus_frames` | int | rw | 120 / 240 / 480 / 960 | `480` | Opus のパケット長（フレーム数、480 = 10 ms） |
| `/lan/stream/#/bitrate_kbps` | int | rw | 32 〜 512 | `128` | Opus のビットレート kbps |
| `/lan/stream/#/subscribers` | int | ro | 0 〜 32 | `0` | 受信している相手の数 |
| `/lan/stream/#/active` | bool | ro |  | `false` | 何かが出力していて、受信者がいて送信中 |
| `/lan/sub/#/stream` | string | ro |  | `""` | 購読しているストリーム（audio/<n> / video/preview / video/lan） |
| `/lan/sub/#/address` | string | ro |  | `""` | 送り先の UDP アドレス（ip:port） |
| `/lan/sub/#/client` | string | ro |  | `""` | 購読した接続の名前 |
| `/lan/peers/#/id` | string | ro |  | `""` | LAN 内の別の PC のエンジンの id |
| `/lan/peers/#/name` | string | ro |  | `""` | その PC の名前 |
| `/lan/peers/#/address` | string | ro |  | `""` | 制御の接続先（ip:port） |
| `/lan/peers/#/state` | string | ro | offline / unpaired / connecting / connected / refused | `"offline"` | 相手の状態（refused = ペアリングが取り消された） |
| `/lan/peers/#/manual` | bool | ro |  | `false` | 手動で追加した相手（mDNS で見つからない場合） |
| `/lan/peers/#/stream/#/name` | string | ro |  | `""` | 相手の LAN 送信ストリームの表示名（接続中だけ） |
| `/lan/peers/#/stream/#/active` | bool | ro |  | `false` | 相手のストリームに何かが出力している |
| `/remote/clients` | int | ro | 0 〜 4 | `0` | 接続中のリモコン数 |
| `/remote/pairing/active` | bool | ro |  | `false` | ペアリング受付中 |
| `/remote/paired/#/id` | string | ro |  | `""` | ペアリング済みの相手の id（Hello.client_id） |
| `/remote/paired/#/name` | string | ro |  | `""` | ペアリング済みの相手の名前 |
| `/remote/paired/#/role` | string | ro | remote / peer | `"remote"` | 相手の種類（リモコン / 別の PC のエンジン） |
| `/remote/paired/#/online` | bool | ro |  | `false` | 今つながっている |

| メソッド | 引数 | 戻り値 | 実装フェーズ | 説明 |
|---|---|---|---|---|
| `/session/resync` | なし | nil | P0 | スナップショットを送り直す（rev の飛びを検出したとき） |
| `/mixer/track/add` | channels:int?, name:string?, position:int? | int（新しいトラック番号） | P0 | トラックを追加。番号は空いている最小の番号 |
| `/mixer/track/remove` | track:int | nil | P0 | トラックを削除。番号は詰めない |
| `/mixer/track/move` | track:int, position:int | nil | P0 | 並び順を変える |
| `/capture/record/start` | なし | nil | P0 | 録画開始 |
| `/capture/record/stop` | なし | nil | P0 | 録画停止 |
| `/capture/report` | state:string, file:string?, error:string?, duration_ms:int? | nil | P2 | capd が録画の状態をエンジンに知らせる（role = サービスの接続だけ） |
| `/track/#/fx/add` | type:string, index:int? | int（スロット番号） | P3 | エフェクト追加 |
| `/track/#/fx/remove` | index:int | nil | P3 | エフェクト削除 |
| `/track/#/fx/move` | from:int, to:int | nil | P3 | エフェクトの順序変更 |
| `/master/fx/add` | type:string, index:int? | int（スロット番号） | P3 | エフェクト追加 |
| `/master/fx/remove` | index:int | nil | P3 | エフェクト削除 |
| `/devices/refresh` | なし | nil | P2 | デバイス・プロセス一覧の更新 |
| `/remote/pairing/start` | なし | string（6桁コード） | P3 | ペアリング受付開始（2 分、1 台つながるまで） |
| `/remote/pairing/stop` | なし | nil | P3 | ペアリング受付終了 |
| `/remote/unpair` | id:string | nil | P3 | ペアリングを取り消す（つながっていれば切断） |
| `/lan/subscribe` | stream:string, port:int | int（購読番号） | P3 | ストリームを受け取る。送り先は接続元の IP の port。接続が切れると自動で解除 |
| `/lan/unsubscribe` | id:int | nil | P3 | 購読をやめる |
| `/lan/peers/pair` | id:string, code:string | nil | P3 | 相手の PC に表示された 6 桁コードでペアリングする |
| `/lan/peers/forget` | id:string | nil | P3 | 相手のペアリング情報を消す（手動で追加した相手は一覧からも消す） |
| `/lan/peers/add` | address:string | nil | P3 | 相手を IP アドレス（ip または ip:port）で手動追加 |
