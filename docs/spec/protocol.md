# 制御プロトコル仕様 v1

- 状態：**v1 ドラフト（G0 で Reo の確認待ち）**
- 対象：エンジン（mixd）と、ローカル UI・リモコン・キャプチャー（capd）・テスト用ツールとの間の制御通信
- 実装：`libs/protocol`（メッセージ・パラメーターツリー・スキーマ）、`libs/control`（TCP サーバー／クライアント）
- パラメーター一覧：[protocol-params.md](protocol-params.md)（スキーマから生成。テストで食い違いを検出）

この仕様書とスキーマ（`libs/protocol/src/schema_v1.cpp`）を正とします。変更は単独の PR で行い、互換性のない変更ではメジャーバージョン（`kProtocolVersion`）を上げます。

## 1. 全体像

全設定を「パラメーターツリー」として**エンジンだけが**持ちます。クライアントは接続時にツリー全体（スナップショット）を受け取り、以降は変更差分（デルタ）だけを受け取って手元の写しを最新に保ちます。クライアントが値を変えたいときは要求（Set / Call）を送り、エンジンが検証して反映した結果がデルタとして**全クライアント**（要求した本人も含む）に配られます。これにより、複数のクライアントが同時につながっても表示がずれません。

| 経路 | 用途 | 方式 |
|---|---|---|
| 制御 | 設定の取得・変更、コマンド、通知 | TCP、既定ポート 47800 |
| メーター | レベルメーター | UDP、約 30Hz、クライアントが指定したポートへ |
| 検出 | LAN 内のエンジンの自動検出 | mDNS（`_dcmix._tcp`、8 章） |
| メディア | LAN の音声・映像ストリーム | UDP（[lan-media.md](lan-media.md)、購読は 10 章） |

## 2. TCP の枠組み

- 1 メッセージ = 4 バイトの長さ（リトルエンディアン、符号なし）＋ MessagePack のペイロード。
- 長さは 1 以上 16 MiB 以下。範囲外はプロトコル違反として `Error` を送って切断します。
- ペイロードは 2 要素の配列 `[type, body]`。`type` は下表の番号、`body` は文字列キーのマップです。
- 受信側は**知らないキーを無視**します（後方互換のため）。足りないキーは既定値（0、空文字、nil）として扱います。
- `TCP_NODELAY` を有効にします。

## 3. 値の型

パラメーターの値はスカラーだけです。リストは番号付きのパスで表します（例：`/track/3/out/1/target`, `/track/3/out/2/target`）。

| 型 | MessagePack | 備考 |
|---|---|---|
| nil | nil | |
| bool | true / false | |
| int | 整数 | 64 ビット符号付き |
| float | float64 | **常に float64 で送る**。受信側は float32 も受け付ける |
| string | str（UTF-8） | |

float のパラメーターへ int を送ってもよく、エンジンは float に変換して保存・配信します。逆（int のパラメーターへ float）は `type_mismatch` です。

## 4. パス

- `/` で始まり、`/` で区切ったセグメントの並び。末尾の `/` と空セグメントは不可。
- セグメントは小文字英数字と `_` `-` `.` のみ。
- 番号セグメントは 1 から始まり、先頭に 0 を付けません（`/track/1`、`/track/64`）。
- スキーマでは番号セグメントを `#`、任意の名前を `*` と書きます（`/track/#/fx/#/param/*`）。

主なツリー（詳細は [protocol-params.md](protocol-params.md)）：

| 接頭辞 | 内容 | 主な担当レーン |
|---|---|---|
| `/system` | バージョン、サンプルレート、バッファサイズ、負荷 | A |
| `/devices` | 入出力デバイス・音声を出しているアプリの一覧（読み取り専用） | A |
| `/mixer`, `/track/#`, `/master`, `/cue` | ミキサー、トラック、ルーティング、エフェクト | A |
| `/capture` | 出力先「デスクトップキャプチャ」：録画設定・状態・音声ストリーム割り当て | B |
| `/recorder` | トラック別録音（WAV / FLAC）の設定・状態 | A3 |
| `/sampler` | サンプラー（効果音パッド）のバンク・パッド | A3 |
| `/video` | 映像ミックス、仮想カメラ、全画面表示、プレビュー | B |
| `/remote` | リモコンの接続数・ペアリング | N / C |
| `/lan` | LAN 送信ストリーム、購読、LAN 内の別の PC（相手） | N |
| `/control` | ホットキーと MIDI の割り当て、MIDI 入力機器、学習 | C7 |

### 4.1 上限値

| 項目 | 値 |
|---|---|
| 通常トラック | 最大 64（初期 4）。マスターは別 |
| トラックの出力先 | 1 トラックあたり 8（`out/1`〜`out/8`） |
| センド | 1 トラックあたり 8（`send/1`〜`send/8`） |
| エフェクト | 1 トラックあたり 16 |
| 映像ミックス | 最大 4 |
| 映像ソース | 最大 64 |
| シーン | 最大 32 |
| レイヤー | 1 シーンあたり 32 |
| 録画の音声ストリーム | 8（`/capture/audio/1`〜`8`） |
| サンプラー | 8 バンク × 16 パッド（`/sampler/bank/1/pad/1`〜`/sampler/bank/8/pad/16`） |
| リモコン同時接続 | 4 |
| LAN 送信ストリーム（音声） | 8（`/lan/stream/1`〜`8`、出力先 `lan/1`〜`lan/8`） |
| 購読 | 32（`/lan/sub/#`） |
| 相手の PC の一覧 | 16（`/lan/peers/#`） |
| ペアリング済みの相手 | 16（`/remote/paired/#`） |
| ホットキー・MIDI の割り当て | 128（`/control/binding/#`、固定 ID） |
| MIDI 入力機器 | 16（`/control/midi/device/#`） |

### 4.2 トラック番号

トラック番号は**固定 ID** です。削除しても他のトラックの番号は詰めません（ルーティングの参照 `track/3` がずれないようにするため）。画面上の並びは `/track/#/position`（1 始まりの連番、エンジンが管理）で表します。追加時は空いている最小の番号を使います。

### 4.3 出力先の書き方

`/track/#/out/#/target`、`/track/#/send/#/target`、`/master/out/#/target` の値は次のどれかです。

| 値 | 意味 | トラックの出力 | センド | マスターの出力 |
|---|---|---|---|---|
| `""` | 未使用 | ○ | ○ | ○ |
| `master` | マスター | ○ | × | × |
| `track/<n>` | 別のトラックの入力に足す | ○ | ○ | × |
| `device/<id>` | 出力デバイス（`/devices/output/#/id`） | ○ | ○ | ○ |
| `vmic/<id>` | 仮想マイク（VB-CABLE など） | ○ | ○ | ○ |
| `lan/<n>` | LAN 送信ストリーム `/lan/stream/<n>`（1〜8） | ○ | ○ | ○ |

`lan/<n>` は 1〜8 の番号だけです（それ以外は `out_of_range`）。同じストリームへ複数のトラックが出力すると、出力デバイスと同じように足し合わせて送ります。

エンジンは次を拒否します（`conflict`）：自分自身への出力、ループ（例：1→2→1）、マスターからトラックへの出力、センドからマスターへの送り。存在しないトラックを指すと `not_found` です。処理順は出力先のつながりから自動で決めます（`processingOrder()`）。トラックを削除すると、そのトラックを指していた出力先・センド・録画音声ストリームは `""` に戻ります。

録画の音声ストリーム `/capture/audio/#/source` は `""`、`master`、`track/<n>` のどれかです。

### 4.4 デバイス ID と入力元

`/devices/input/#/id`・`/devices/output/#/id` の形は API によって違います。UI は中身を解釈せず、そのまま `/track/#/input/source` や `device/<id>` に使います。

| api | 入力の ID | 出力の ID |
|---|---|---|
| `wasapi` / `vmic` | WASAPI のエンドポイント ID（`{0.0.1.00000000}.{…}`） | 同じ |
| `asio` | `asio:<ドライバー名>`（全入力チャンネルで 1 台。トラックは `/track/#/input/channel` で選ぶ） | `asio:<ドライバー名>:<n>-<n+1>`（ステレオの組ごとに 1 台。チャンネル数が奇数なら最後は `:<n>-<n>` のモノ） |

- `vmic` は仮想オーディオケーブル（VB-CABLE など）です。WASAPI のデバイスと同じに扱い、`device/<id>` と `vmic/<id>` のどちらで指しても同じデバイスに出力します。
- ASIO はドライバーの仕組み上、同時に 1 つのドライバーしか開けません。2 つ目のドライバーを指した入出力は開かれず、`device.error` で知らせます。ASIO のデバイスがクロック基準に使える出力のとき、エンジンは ASIO のコールバックで動きます。
- `/devices/*/#/sample_rate` はデバイス側のレートです。48000 以外（44.1kHz など）は共有・排他・ASIO のどれでもエンジンが自前で 48kHz に変換します。

**ID が変わったときの再照合**：Windows はサンプルレートの変更や USB の差し替えでデバイスに新しい ID を付けることがあります。設定（入力元、出力先、センド先、キュー、`/system/clock_device`）が指す ID が一覧から消え、エンジンがその ID の名前を覚えていて、同じ向き・同じ名前（なければ Windows が付け直す番号を除いた名前。`Line (2- Steinberg UR22)` は `Line (Steinberg UR22)` とみなす）の接続中デバイスがちょうど 1 台あり、そのデバイスがまだどこからも指されていなければ、エンジンは設定の ID を新しいものに書き換えます（エンジン発のデルタ、`device.rematched` イベント）。名前の記憶はエンジンの起動中だけです（プロファイル保存は P5）。

**プロセス音声**：`/track/#/input/kind` = `process` のとき、`source` は実行ファイル名（`chrome.exe` など、大文字小文字は区別しない）です。

- エンジンはその名前で動いているプロセスのうち、親が同じ名前でないもの（プロセスツリーの根）を選び、子プロセスごと 48kHz ステレオで取り込みます。根が複数あるときは音声セッションを持つもの、次に起動の早いものを選びます。
- 同じアプリを指すトラックが何本あっても取り込みは 1 本で、各トラックへ配ります（Windows 10 は 1 プロセス内で同じ PID を 2 回開けないため）。
- アプリが終了すると `input/state` = `disconnected`（無音）になり、同じ実行ファイル名のアプリが起動したら自動でつなぎ直します。
- `/devices/process/#` は音声セッションを持つアプリの一覧で、実行ファイル名ごとに 1 行（`pid` は根のプロセス）です。`/devices/refresh` か数秒ごとの見直しで更新されます。

### 4.5 ビデオミキサー

映像の設定もエンジンのツリーが持ち、実際の取り込みと合成は capd が行います（レーン B1）。

| 部分木 | 中身 | 番号 |
|---|---|---|
| `/video/source/#` | ソース（モニター、ウィンドウ、カメラ、画像、動画、文字、単色、LAN）。全ミックス・全シーンで共有し、同じソースを何枚のレイヤーに出しても取り込みは 1 回 | `/video/source/add` で追加 |
| `/video/scene/#` | シーン。並び順は `position` | `/video/scene/add` |
| `/video/scene/#/layer/#` | シーンに重ねるレイヤー。`source` でソースを指す。重なり順は `position`（1 = 最前面） | `/video/scene/#/layer/add` |
| `/video/mix/#` | 映像ミックス（最大 4）。解像度・fps・背景色、本番シーン `program_scene` とプレビュー `preview_scene`、トランジション | `/video/mix/add` |
| `/video/devices/display` ほか | capd が見つけたモニター・ウィンドウ・カメラの一覧（読み取り専用） | capd が報告 |

- 番号はトラックと同じく**固定 ID** で、削除しても詰めません。追加は空いている最小の番号です。
- シーンは全ミックスで共有します。ミックスごとに解像度が違ってもよいように、レイヤーの表示枠（`x`, `y`, `width`, `height`）はキャンバスの幅・高さを 1 とした割合で表します（全体 = `0, 0, 1, 1`）。はみ出し（負の値、1 超え）も可。ソースは枠の中に `fit`（縦横比を保って全体）、`fill`（縦横比を保って枠を埋める）、`stretch` で収めます。ソースの解像度が変わっても枠は変わりません。
- クロップはソースの幅・高さに対する割合（左右・上下それぞれ 0〜1）で、収める前に切り取ります。
- 色は整数で、背景・文字は `0xAARRGGBB`、クロマキーの色は `0xRRGGBB` です。
- エンジンは次を拒否します（`not_found`）：存在しないソースを指すレイヤー、存在しないシーンを指す `program_scene` / `preview_scene`、存在しないミックスを指す出力先（`/capture/mix`、`/video/vcam/mix`、`/video/monitor/mix`、`/video/preview/mix`、`/video/lan/mix`）。0 は「なし」です。
- ソースを削除すると、そのソースを出していたレイヤーも消えます。シーンを削除すると、それを指していた `program_scene` / `preview_scene` は 0 に、ミックスを削除すると、それを指していた出力先は 0 に戻ります。
- `/video/mix/#/active` は、出力先のどれかがそのミックスを指している間 true です（エンジンが計算）。capd は出力先があるミックスだけを合成します。
- 起動直後のツリー：ソース 1 = 主モニター、シーン 1 = そのソースをキャンバス全体に出すレイヤー 1 枚、ミックス 1 の本番・プレビュー = シーン 1。

**切り替え**：`program_scene` が変わると、capd はそのミックスの `transition/type`（`cut` / `fade`）と `transition/duration_ms` で前のシーンから新しいシーンへ切り替えます。リモコンなどは `program_scene` を直接変えてもよく、プレビュー式の操作では `/video/mix/#/take` で `preview_scene` を本番に出し、直前の本番シーンをプレビューに入れます。切り替え中にさらに変わったら、その時点の画から新しいシーンへ切り替えます。

**ソースの状態**：capd はソースを開くたび・状態が変わるたびに `/video/source/report`（`source`, `state` = `ok` / `disconnected` / `error` / `none`, `width`, `height`, `error`）を送り、エンジンは `/video/source/#/state` などに反映します。どのミックスの本番・プレビュー・切り替え中のシーンにも出ていないソースは閉じて `none` になります。

**一覧**：capd はモニター・カメラの一覧を起動時と変化時に、ウィンドウの一覧は `video.devices.refresh` イベントを受けたときに、`/video/devices/report` で送ります。引数は `kind`（`display` / `window` / `camera`）と、`"<番号>/<項目>"` をキーにした値（例 `"1/id"`, `"1/name"`）で、エンジンは `/video/devices/<kind>` を丸ごと置き換えます。UI はウィンドウ選択を開くときに `/video/devices/refresh` を呼びます。

## 5. メッセージ一覧

| 番号 | 名前 | 向き | body のキー |
|---|---|---|---|
| 1 | Hello | C→S | `proto` int, `client` str, `client_id` str, `role` int（0 = ローカル、1 = リモコン、2 = サービス、3 = 相手のエンジン）, `meter_port` int, `pair_code` str, `token` str |
| 2 | Welcome | S→C | `proto` int, `server` str, `server_id` str, `session` int, `token` str |
| 3 | Snapshot | S→C | `rev` int, `params` map（パス→値） |
| 4 | Delta | S→C | `rev` int, `origin` int, `origin_client` str, `changes` map（パス→値）, `removed` [str] |
| 5 | Set | C→S | `id` int, `changes` map（パス→値） |
| 6 | Call | C→S | `id` int, `method` str, `args` map（名前→値） |
| 7 | Result | S→C | `id` int, `code` int, `message` str, `path` str, `value` 値 |
| 8 | Event | S→C | `name` str, `data` map（名前→値） |
| 9 | Ping | 双方 | `t` int |
| 10 | Pong | 双方 | `t` int（Ping の値を返す） |
| 11 | Error | S→C | `code` int, `message` str（送信後に切断） |
| 12 | Bye | 双方 | `reason` str（送信後に切断） |

### 5.1 接続の流れ

```
C: Hello{proto=1, client="ui", role=0, meter_port=50123}
S: Welcome{proto=1, server="mixd", server_id, session}
S: Snapshot{rev=120, params={...全パラメーター...}}
   …以後、変更があるたびに Delta が届く…
```

- 最初のメッセージは必ず `Hello`。それ以外が来たら `Error(bad_request)` で切断します。
- `proto` が違えば `Error(unsupported_version)` で切断します。
- `session` はエンジンの起動ごとに変わります。クライアントは再接続時に値が変わっていたら、手元の状態をすべて捨ててスナップショットから作り直します。
- ループバック（127.0.0.1 / ::1）以外からの接続は、role がリモコン（1）か相手のエンジン（3）で、`token` が有効か、受付中のペアリングコードが合っているときだけ受け付けます（8 章）。それ以外は `unauthorized` です。開発用の `mixd --allow-remote` はこの検査を省きます。リモコンの 5 台目は `limit_reached` で拒否します（相手のエンジンは数えません）。
- `client_id` はクライアントのインストールごとに固定の id です（相手のエンジンは自分の `server_id`）。ペアリングの記録に使います。空でも接続できますが、ペアリングはできません。
- role = 3（相手のエンジン）の接続は、ツリーを読むことと `/lan/subscribe`・`/lan/unsubscribe`・`/session/resync` だけができます。Set と他の Call は `unauthorized` です。

### 5.2 リビジョン（rev）とデルタ

- ツリーは `rev`（整数）を持ち、デルタ 1 つごとに必ず 1 増えます。
- クライアントは `delta.rev == 手元の rev + 1` のときだけ適用します。小さければ無視、飛んでいれば `/session/resync` を呼び、届いたスナップショットで作り直します。
- 1 つのデルタの中では、`removed`（部分木ごと削除。`/track/3` なら `/track/3` と `/track/3/...` すべて）を先に、`changes` を後に適用します。
- `origin` と `origin_client` は、そのデルタを起こした要求の `id` と送り主の名前です（エンジン自身の変更なら 0 と空文字）。UI は自分の操作の反映かどうかの判断に使えます。

### 5.3 Set（値の変更）

- 1 つの Set に含めた変更は**まとめて成功するか、まとめて失敗**します。
- 各パスについて、構文 → 存在 → スキーマ（型・範囲・選択肢・読み取り専用）の順に検査し、全部通ったら仮に反映して全体の検査（ルーティングのループなど）を行います。失敗したら元に戻します。
- 値が今と同じ変更はデルタに含めません。何も変わらなければデルタは出ず、`Result(ok)` だけ返ります。
- サーバーはデルタを全員へ送ったあとで、要求者に `Result` を返します。

フェーダー操作のように連続で送る場合、クライアントは 1 本のパラメーターにつき 60Hz 程度に間引いて送ってください。表示は楽観的に先に動かし、`Result` がエラーならツリーの値に戻します。

### 5.4 Call（コマンド）

トラックの追加や録画開始のように、値の変更では表せない操作です。メソッド名もパスの形をとります。結果は `Result.value` に入ります。コマンドによるツリーの変更は、`Result` より前にデルタとして配られます。

| メソッド | 引数 | 戻り値 | 実装 | 説明 |
|---|---|---|---|---|
| `/session/resync` | なし | nil | P0 | スナップショットを送り直す（Result より前に届く） |
| `/mixer/track/add` | `channels`:int?（1/2、既定 2）, `name`:str?, `position`:int?（既定 末尾） | int（トラック番号） | P0 | トラック追加。64 本あれば `limit_reached` |
| `/mixer/track/remove` | `track`:int | nil | P0 | トラック削除。参照していた出力先は `""` に戻る |
| `/mixer/track/move` | `track`:int, `position`:int | nil | P0 | 並び順の変更 |
| `/capture/record/start` | なし | nil | P0 | 録画開始。録画中なら `conflict` |
| `/capture/record/stop` | なし | nil | P0 | 録画停止。録画中でなければ `conflict` |
| `/capture/report` | `state`:str, `file`:str?, `error`:str?, `duration_ms`:int?, `reason`:str? | nil | P2 | capd から録画の状態を知らせる（5.4.1）。role = サービス以外は `unauthorized` |
| `/recorder/start` | なし | str（テイクのフォルダー） | P3 | トラック別録音の開始（5.4.2） |
| `/recorder/stop` | なし | nil | P3 | トラック別録音の停止。録音中でなければ `conflict` |
| `/sampler/trigger` | `pad`:int（1〜16）, `bank`:int?（既定 `/sampler/active_bank`） | nil | P3 | パッドを押す（5.4.3） |
| `/sampler/release` | `pad`:int, `bank`:int? | nil | P3 | パッドを離す |
| `/sampler/stop` | `bank`:int?（なし = 全バンク） | nil | P3 | 再生中のパッドを止める |
| `/track/#/fx/add` ほか | 一覧参照 | | P3 | エフェクトの追加・削除・並べ替え・パラメーター情報（5.7） |
| `/plugins/scan` | `rescan`:bool? | nil | P3 | VST3 プラグインのスキャン（5.8） |
| `/devices/refresh` | なし | nil | P2 | デバイス一覧の更新 |
| `/video/source/add` ほか | 一覧参照 | | P3 | ビデオミキサーのソース・シーン・レイヤー・ミックスの追加・削除・並べ替え、`/video/mix/#/take`（4.5） |
| `/video/source/report`, `/video/devices/report` | 4.5 参照 | nil | P3 | capd からの報告。role = サービス以外は `unauthorized` |
| `/video/output/report` | `output`:str（`vcam` / `monitor`）, `state`:str, `detail`:str? | nil | P3 | capd から映像出力の状態を知らせる（5.4.4）。role = サービス以外は `unauthorized` |
| `/remote/pairing/start` / `stop` | なし | str（6 桁）/ nil | P3 | ペアリング受付（8 章） |
| `/remote/unpair` | `id`:str | nil | P3 | ペアリングの取り消し。つながっていれば切断する |
| `/lan/subscribe` / `unsubscribe` | `stream`:str, `port`:int / `id`:int | int（購読番号）/ nil | P3 | ストリームの購読（10 章） |
| `/lan/peers/pair` | `id`:str, `code`:str | nil | P3 | 相手の PC とペアリングする（8 章） |
| `/lan/peers/forget` / `add` | `id`:str / `address`:str | nil | P3 | 相手の記録を消す / IP アドレスで手動追加 |
| `/control/binding/add` | `trigger`, `device`, `action`, `target`, `value`（どれも str?） | int（割り当て番号） | P4 | ホットキー・MIDI の割り当てを追加（5.9）。128 個あれば `limit_reached` |
| `/control/binding/remove` | `binding`:int | nil | P4 | 割り当てを削除。番号は詰めない |
| `/control/learn` | `binding`:int, `source`:str?（`any` / `key` / `midi`、既定 `any`） | nil | P4 | 学習の開始（5.9.3） |
| `/control/learn/cancel` | なし | nil | P4 | 学習をやめる |

未実装・未知のメソッドは `unknown_method` を返します。

#### 5.4.1 録画の開始・停止（エンジンと capd）

録画の設定と状態はエンジンが持ち、実際の録画は capd が行います。capd はエンジンに role = 2（サービス）、`client` = `"capd"` で接続し、`/capture/*` と `/video/*` をツリーで追います。

| きっかけ | エンジン | capd |
|---|---|---|
| `/capture/record/start`（UI・リモコン） | capd が未接続なら `conflict`（"capture process not running"）。`/capture/state` が `idle` か `error` でなければ `conflict`。それ以外は `/capture/state` = `starting` | `starting` を見て録画を始め、`/capture/report` `state="recording"`, `file` |
| `report recording` | `/capture/state` = `recording`、`/capture/file`、`/capture/error` = `""`、`capture.started`。以後 `/capture/elapsed_ms` を数える | |
| `/capture/record/stop` | `recording` でなければ `conflict`。それ以外は `/capture/state` = `stopping` | ファイルを閉じて `/capture/report` `state="idle"`, `file`, `duration_ms` |
| `report idle` | `/capture/state` = `idle`、`capture.stopped` | |
| capd 側の失敗（エンコーダーを開けない、ディスク満杯で安全停止など） | `/capture/state` = `error`、`/capture/error` = `error` | `/capture/report` `state="error"`, `error` |
| 録画中（`starting` / `recording` / `stopping`）に capd の接続が切れた | `/capture/state` = `error`、`/capture/error` = "capture process disconnected" | |
| capd が録画中にファイルを切り替えた（GPU リセットなど、P3） | `/capture/file` を新しいファイルに。`/capture/elapsed_ms` は数え続け、`capture.started` は送らない。`capture.segment` `file`, `reason` | 前のファイルを閉じ、新しいファイルを開いて `/capture/report` `state="recording"`, `file`, `reason` |

`reason` は、録画中（`/capture/state` = `recording`）の `report recording` に付けたときだけ「ファイルの切り替え」の意味になります（`gpu_reset` など。UI は表示に使うだけ）。`recording` でないときの `reason` は無視します。`state` に `recording` / `idle` / `error` 以外が来たら `bad_request` です。エンジンの共有メモリ音声は録画中でなくても常に書かれているので、capd は `starting` を見た時点から読み始められます。

#### 5.4.2 トラック別録音（レコーダー）

エンジンが自分でファイルに書きます（capd は関係しません）。`/track/#/record/enabled`・`/master/record/enabled` が true のものを、`record/tap` の位置（pre = フェーダー前でトラックのチャンネル数、post = フェーダー後のステレオ）で、1 つのテイクフォルダーに 1 ファイルずつ書きます。

- 開始：`/recorder/start`、または `/recorder/with_capture` が true のとき `/capture/state` が `recording` になった時点。録音対象がなければ `conflict`（"no track armed for recording"）、録音中なら `conflict`。録画に連動して始めたときに録音対象がなければ、何もしません。
- 停止：`/recorder/stop`、または録画に連動して始めた録音は `/capture/state` が `recording` でなくなった時点（`stopping`・`idle`・`error`）。
- フォルダー：`/recorder/directory`（`""` = ミュージック\Desktop Capture）の下に `/recorder/folder_name` の名前で作ります。同じ名前があれば `_2`、`_3` を付けます。ファイル名は `<番号2桁>_<表示名>.wav`（マスターは `00_Master`、トラックは `/track/#/` の番号。ファイル名に使えない文字は `_`）。
- 形式：48kHz / 24bit の WAV か FLAC。WAV が 4GB を超えると RF64 に切り替えます。
- 全ファイルは同じ音声ブロックから始まり、同じ長さで終わります（サンプル単位でそろう）。録音中に対象・タップ・チャンネル数を変えても次のテイクまで反映しません。録音中に削除したトラックや切断した入力は無音で続きます。
- ディスク書き込みは別スレッドです。書き込みが間に合わずバッファがあふれた分は無音で埋めて長さを保ち、`/recorder/dropped_frames` に数えます。書き込みに失敗したら（ディスク満杯など）そこまでのファイルを正しく閉じて `/recorder/state` = `error`、`/recorder/error` に理由を入れます。

#### 5.4.3 サンプラー

8 バンク × 16 パッドの効果音パッドです。サンプラーの音は、`/track/#/input/kind` = `sampler` のトラックに入ります（`input/source` が `""` なら全バンク、`"1"`〜`"8"` ならそのバンクだけ）。音量・パン・エフェクト・出力先はそのトラックで決めます。

- `/sampler/bank/#/pad/#/file` を設定すると、エンジンは別スレッドで読み込みます（`state`：`loading` → `ready` か `error`）。WAV・FLAC・MP3・Ogg Vorbis を読み、48kHz のステレオに変換してメモリに置きます。1 ファイル 10 分まで。読み込み中や空のパッドの `trigger` は何もしません（エラーにもしません）。
- `mode`：`oneshot` は押すと最後まで再生し、再生中に押すと頭から。`toggle` は押すたびに再生と停止。`hold` は押している間（`release` まで）だけ。`loop` は押すたびにループ再生と停止。停止と頭出しは数 ms のフェードでつなぎます。
- 同時に鳴らせるのは 32 音まで。足りないときは一番古い音を止めます。
- 再生中は `playing` が true になります（約 30Hz で更新）。再生中にファイルを差し替えるとその音は止まります。
- `pad`・`bank` が範囲外なら `out_of_range` です。

#### 5.4.4 映像出力の状態（仮想カメラ・全画面表示）

仮想カメラ（`/video/vcam/*`）と全画面表示（`/video/monitor/*`）は capd が出します。capd は出力の状態が変わるたびに `/video/output/report` で知らせ、エンジンは `/video/<output>/state`・`/video/<output>/detail` に書きます（`detail` 省略 = `""`）。

| `output` | `state` | 意味 |
|---|---|---|
| `vcam` | `off` | 出していない（`/video/vcam/mix` = 0、またはミックスがない） |
| | `waiting` | 出しているが、カメラを開いているアプリがない |
| | `active` | アプリがカメラを読んでいる |
| | `unavailable` | 出せない（仮想カメラが未登録など）。`detail` に理由 |
| `monitor` | `off` | 出していない（`/video/monitor/mix` = 0、またはミックスがない） |
| | `active` | 表示中 |
| | `no_display` | `/video/monitor/display` のモニターが見つからない。つながれば capd が自動で戻して `active` を送る |
| | `error` | 表示できない。`detail` に理由。ほかの出力（録画・仮想カメラ）は止まらない |

`output` か `state` が上の表にない値なら `bad_request` です。capd の接続が切れたら、エンジンは両方を `off`・`""` に戻します（capd は再接続したら今の状態を送り直します）。

### 5.5 Event（通知）

パラメーターではない出来事を知らせます。状態そのものは必ずツリーにも載せ、Event は「起きたこと」の通知だけに使います（再接続したクライアントはツリーから状態を復元できるようにするため）。

| 名前 | data | 送るとき |
|---|---|---|
| `capture.started` | `file` | 録画開始 |
| `capture.stopped` | `file`, `duration_ms` | 録画停止 |
| `recorder.started` | `folder` | トラック別録音の開始 |
| `recorder.stopped` | `folder`, `duration_ms`, `error`? | トラック別録音の停止 |
| `capture.segment` | `file`, `reason` | GPU リセットなどで新しいファイルに切り替えた（P3） |
| `device.lost` / `device.restored` | `id`, `name`, `kind`（`input` / `output` / `process`） | 設定で使っているデバイス・アプリの切断と復帰。プロセスの `id` は実行ファイル名 |
| `device.rematched` | `old_id`, `id`, `name` | ID が変わったデバイスを名前で照合し、設定を書き換えた（4.4） |
| `device.error` | `message` | デバイスを開けなかった（他のアプリが排他で使用中、2 つ目の ASIO ドライバーなど）。同じ理由は続けて送らない |
| `video.devices.refresh` | なし | `/video/devices/refresh` が呼ばれた。capd が一覧を送り直す（P3） |
| `control.learned` | `binding`, `trigger`, `device` | 学習でその割り当てのきっかけが決まった（5.9.3） |

### 5.6 生存確認と時計

- クライアントは 5 秒ごとに `Ping` を送ります。サーバーはすぐ `Pong` を返します。
- `/session/clock`（P4）はエンジンの PC の時計（QPC をマイクロ秒にしたもの。LAN メディアのパケットの時刻と同じ時計）を返します。リモコンは呼ぶ前後の自分の時計との差から時計のずれを推定し（往復時間が最も短かった回を使う）、プレビュー映像の「合成してから表示するまで」の遅延を測ります。
- サーバーは 15 秒間何も受信しなかった接続を切ります。
- 切断されたクライアントは再接続し、スナップショットから作り直します。

### 5.7 エフェクト

各トラックとマスターに最大 16 個のエフェクトを挿入できます。処理順は `fx/1` → `fx/2` → …で、置き場所は Lo-Hi フィルターの後・録画の pre タップとフェーダーの前です（入力 → ローカット → ハイカット → エフェクト → [pre] → フェーダー → パン → [post]）。

スロット `/track/#/fx/#`（マスターは `/master/fx/#`）の中身：

| パス | 内容 |
|---|---|
| `type` | `eq4` / `comp` / `gate` / `reverb` / `delay` / `limiter`（内蔵）、`vst3:<id>`（`/plugins/#/id`） |
| `name` | 表示名。内蔵は日本語名、VST3 はプラグイン名 |
| `bypass` | バイパス（10ms で切り替え、バイパス中の VST3 は処理しない） |
| `param/<キー>` | パラメーター（float）。内蔵はキーと範囲が決まっている（[protocol-params.md](protocol-params.md) の「エフェクトのパラメーター」表）。VST3 はキー `p<ParamID>`（VST3 の ParamID を 10 進で）、値は 0〜1 の正規化値 |
| `status` | `ok`、`loading`（VST3 を読み込み中）、`failed`（読み込み失敗、理由は `error`）、`missing`（その VST3 がこの PC にない） |
| `latency_samples` | エフェクトが足す遅延。トラック間の遅延補正はしない（ライブ用途のため。遅延のあるエフェクトを並列の経路に挿すと位相がずれる） |
| `param_count` | パラメーターの総数 |
| `editor` | VST3 のプラグイン画面をエンジンの PC に開く（true）・閉じる（false）。利用者が画面を閉じると false に戻る。内蔵エフェクトで true は `conflict` |

コマンド：

| メソッド | 引数 | 戻り値 | 説明 |
|---|---|---|---|
| `/track/#/fx/add` | `type`:str, `index`:int?（既定 末尾） | int（スロット番号） | 追加。`index` 以降は 1 つずつ後ろへずれる。16 個あれば `limit_reached`、知らない種別は `not_found`。VST3 は `status = loading` で追加され、読み込めたら `ok` とパラメーターが届く |
| `/track/#/fx/remove` | `index`:int | nil | 削除。後ろのスロットは前に詰める |
| `/track/#/fx/move` | `from`:int, `to`:int | nil | 並べ替え。番号は付け直す |
| `/track/#/fx/#/params` | `offset`:int?（既定 0）, `count`:int?（既定 64、最大 256）, `keys`:str?（`,` 区切り） | str（JSON 配列） | パラメーターの情報。`keys` があればそのキーだけ、なければプラグインの並び順で `offset` から `count` 個 |

マスターも同じ（`/master/fx/add` など）。スロット番号はトラック番号と違い固定 ID ではなく、削除・並べ替えのたびに 1 から振り直します（スロットの部分木を消して作り直すデルタが届きます）。

`params` の戻り値は JSON の配列で、要素は `{"key", "name", "unit", "min", "max", "default", "value", "text", "steps", "log"}` です。`text` はプラグインが表示する値の文字列（例 `"-6.0 dB"`）、`steps` は 0 = 連続、2 以上 = 段階数、`log` は内蔵エフェクトの対数スライダー。VST3 の `min`/`max` は 0 と 1、`unit` はプラグインの申告するラベルです。

VST3 のパラメーターは数万個あるものがあるので（V7）、ツリーには全部は載せません。載るのは、読み込み時のプラグインの並びで最初の 128 個、`params` で取得されたもの、プラグイン画面で動かされたもので、1 つのエフェクトにつき最大 1024 個です。ツリーにないパラメーターは `params` で取得してから Set します（取得したキーはそのデルタが `Result` より前に届きます）。プラグインの中で変わった値（画面での操作、プリセットの切り替え）もデルタで届きます。ホストが自動化用に用意しただけの空きスロット（名前が空や `#` で始まるもの）と MIDI CC 用のものは出しません。

### 5.8 プラグイン（VST3）

エンジンは起動時に前回のスキャン結果（キャッシュ）を読み、`/plugins/#` に一覧を載せます。`/plugins/scan` は標準の VST3 フォルダーと `/plugins/extra_dirs` を探し、新しいファイルと更新されたファイルだけを調べます（`rescan = true` で全部）。プラグインはスキャン用の子プロセスで 1 つずつ読み込むので、読み込みで落ちる・止まるプラグインがあってもエンジンは止まりません（`status` = `crashed` / `timeout`）。進み具合は `/plugins/scan/state`・`progress`・`current` で分かります。スキャン中にもう一度呼ぶと `conflict` です。

`/plugins/#` の番号はスキャンのたびに名前順で振り直します。エフェクトの `type` には番号ではなく `id` を使います。

### 5.9 ホットキーと MIDI（割り当て）

キーボードのショートカットと MIDI 機器（パッド・フェーダー・つまみ）で、録画やサンプラー、シーン切替、ミュート、フェーダーなどを操作します。割り当てはエンジンが持ち、受け付けもエンジンの PC で行います（エンジンは常駐しているので、UI を閉じていても、ゲームなど他のアプリが前面にあっても効きます）。リモコンのキーボードや MIDI 機器は使いません。

割り当て 1 つが `/control/binding/<n>` です。`/control/binding/add` で追加し（番号はトラックと同じく固定 ID）、中身は Set で変えます。

| パス | 内容 |
|---|---|
| `trigger` | きっかけ（5.9.1）。`""` = 未設定 |
| `device` | MIDI のとき、受け付ける機器の名前（`""` = どの機器でも） |
| `action` / `target` / `value` | 動作（5.9.2） |
| `enabled` | 有効 |
| `state` / `error` | エンジンが書く。`ok`、`off`（無効・未設定・`action` = `none`）、`invalid`（きっかけ・対象・値が正しくない、対象のトラックがないなど。理由は `error`）、`conflict`（他のアプリが同じホットキーを登録済み）、`unsupported`（この PC ではホットキーか MIDI が使えない） |

Set はフィールドごとに受け付け、組み合わせの誤りは拒否せずに `state` = `invalid` で知らせます（UI が `action` と `target` を別々に送れるように）。対象のトラックや映像ミックスが後から作られれば `ok` に戻ります。同じきっかけに複数の割り当てがあれば、番号順に全部実行します。

割り当てと MIDI 機器の `enabled` は `%APPDATA%\DesktopCapture\mixd-control.txt` に保存し、エンジンの起動時に戻します（プロファイル（P5）とは別。どのプロファイルでも同じキー・同じ機器を使うため）。

#### 5.9.1 きっかけ

| 形 | 意味 |
|---|---|
| `key:<修飾>+…+<キー>` | ホットキー。修飾は `ctrl`・`alt`・`shift`・`win` をこの順で 0 個以上、最後にキーを 1 つ。例 `key:ctrl+shift+f9`、`key:f13`、`key:alt+num5` |
| `midi:note/<ch>/<n>` | ノート。ノートオン（ベロシティ 1 以上）で押す、ノートオフ（ベロシティ 0 のノートオンも）で離す |
| `midi:cc/<ch>/<n>` | コントロールチェンジ。ボタンとしては 64 以上になったときに押す、64 未満になったときに離す。`param` では値 0〜127 |
| `midi:pc/<ch>/<n>` | プログラムチェンジ。押すだけ |
| `midi:pb/<ch>` | ピッチベンド。`param` だけ（値 0〜16383） |

`<ch>` は 1〜16、`<n>` は 0〜127。キーの名前は `a`〜`z`、`0`〜`9`、`f1`〜`f24`、`num0`〜`num9`、`num_add`・`num_sub`・`num_mul`・`num_div`・`num_dec`、`space`・`enter`・`tab`・`backspace`・`escape`・`insert`・`delete`・`home`・`end`・`pageup`・`pagedown`・`up`・`down`・`left`・`right`・`pause`・`printscreen`・`scrolllock`、`minus`・`equal`・`comma`・`period`・`semicolon`・`slash`・`backslash`・`quote`・`bracketleft`・`bracketright`・`backquote`、`media_play`・`media_stop`・`media_next`・`media_prev` です（日本語キーボードでも位置ではなく Windows の仮想キーコードで決まる）。

- ホットキーは Windows の `RegisterHotKey` で登録します。他のアプリが登録済みの組み合わせは `conflict` です。ホットキーとして登録した組み合わせは他のアプリには届きません。
- キーを押し続けたときの自動の繰り返しは、`step` だけ受け付けます（押しっぱなしで音量を下げ続けるなど）。
- MIDI のシステムメッセージ（クロック、アクティブセンシング、SysEx）は無視します。

#### 5.9.2 動作

| `action` | `target` | `value` | 押したとき | 離したとき |
|---|---|---|---|---|
| `none` | | | 何もしない | |
| `record_toggle` | | | 録画していなければ `/capture/record/start`、録画中なら `/capture/record/stop` | |
| `record_start` / `record_stop` | | | `/capture/record/start` / `stop` | |
| `recorder_toggle` | | | トラック別録音の開始・停止の切り替え | |
| `pad` | `<パッド>`（今のバンク）か `<バンク>/<パッド>` | | `/sampler/trigger` | `/sampler/release`（押す間のパッドが止まる） |
| `pad_stop` | `""`（全バンク）か `<バンク>` | | `/sampler/stop` | |
| `take` | `<ミックス>` | | `/video/mix/<ミックス>/take` | |
| `toggle` | bool のパラメーター（例 `/track/3/mute`） | | true と false を入れ替える | |
| `set` | パラメーター | 値の文字列（パラメーターの型で読む） | その値にする | |
| `step` | 数値のパラメーター | 増減（例 `-1`） | 値に足して範囲に収める | |
| `param` | パラメーター（MIDI だけ） | | 5.9.4 | |

- シーン切替は `set` で `/video/mix/<n>/program_scene` に、サンプラーのバンク切替は `set` で `/sampler/active_bank` に、ミュートは `toggle` で `/track/<n>/mute` にします。UI はこれを「シーン切替」「バンク切替」「ミュート」として見せます。
- 動作はクライアントの Set・Call と同じ検査を通ります（`origin_client` = `"control"`）。失敗（録画中の `record_start` など）は無視し、ログに残すだけです。
- `toggle`・`set`・`step`・`param` の対象は書き込みできるパラメーターだけです（読み取り専用や存在しないパスは `invalid`）。`/control/...` は対象にできません。

#### 5.9.3 学習

`/control/learn` を呼ぶと、エンジンは次の入力を待ちます（`/control/learn/binding` = その番号、`/control/learn/source`）。

- `key`：エンジンの PC で押されたキーの組み合わせ（修飾キー以外のキーが押された時点の修飾の状態）。待っている間に押したキーは他のアプリに届きません。`escape` だけを押すと学習をやめます。
- `midi`：ノートオン、コントロールチェンジ、プログラムチェンジ、ピッチベンドのどれか。
- `any`：どちらか早い方。

決まったら `trigger`（MIDI なら `device` も）に書き、`control.learned` を送って学習を終えます（`/control/learn/binding` = 0）。30 秒たつか、`/control/learn/cancel`、別の割り当ての学習で終わります。存在しない割り当ては `not_found` です。

UI でパラメーターを選んで MIDI で動かすようにするときは、UI が `action` = `param`・`target` = そのパスの割り当てを `/control/binding/add` で作り、`/control/learn` を `source` = `midi` で呼びます。

#### 5.9.4 MIDI の値をパラメーターへ（`param`）

| 対象 | 値の対応 |
|---|---|
| `volume_db`・`level_db` で終わるパス | UI のフェーダーと同じ目盛り。0 = 無音（-144）、1 以上は -60 dB〜+12 dB に直線（CC の 106 ≒ 0 dB） |
| そのほかの float・int | 最小〜最大に直線（int は四捨五入） |
| bool | 半分以上で true |
| 選択肢のある string・int | 選択肢の数で等分 |

CC は 0〜127、ピッチベンドは 0〜16383 を 0〜1 として扱います。ノートは押したとき最大、離したとき最小です。つまみを速く回したときのために、1 つの割り当てにつき約 60Hz に間引いて最後の値を Set します。

#### 5.9.5 MIDI 入力機器

エンジンは MIDI 入力機器（Windows の MIDI 入力、`winmm`）を数秒ごとと `/devices/refresh` で探し、`/control/midi/device/#` に載せて、`enabled` のものを開きます。Windows の MIDI 入力は同時に 1 つのアプリしか開けないので、DAW などで使う機器は `enabled` = false にします（名前で覚える）。他のアプリが開いていれば `state` = `busy` で、数秒ごとに開き直します。

最後に受けたメッセージは `/control/midi/last/*` に約 10Hz で載せます（UI の確認表示用）。

## 6. エラーコード

| 番号 | 名前 | 意味 |
|---|---|---|
| 0 | ok | 成功 |
| 1 | bad_request | 形式の誤り、引数不足 |
| 2 | not_found | パスやトラックが存在しない |
| 3 | type_mismatch | 型が違う |
| 4 | out_of_range | 範囲外、選択肢にない |
| 5 | read_only | エンジン管理のパラメーター |
| 6 | unsupported_version | プロトコルのバージョン違い |
| 7 | unauthorized | ペアリングされていない |
| 8 | conflict | 状態と矛盾（ループ、録画中に開始など） |
| 9 | internal | エンジン内部のエラー |
| 10 | unknown_method | 未知・未実装のメソッド |
| 11 | limit_reached | 上限に達した |

## 7. メーター（UDP）

`Hello.meter_port` に 0 以外を指定したクライアントへ、エンジンは約 30Hz でメーターパケットを送ります。取りこぼしは再送しません。すべてリトルエンディアンです。

| オフセット | サイズ | 内容 |
|---|---|---|
| 0 | 4 | マジック `DCM1` |
| 4 | 4 | 連番（パケットごとに +1） |
| 8 | 8 | エンジン時刻（マイクロ秒） |
| 16 | 2 | エントリー数（最大 128） |
| 18 | 2 | 予約（0） |
| 20〜 | | エントリーの並び |

エントリー：`meter_id` u16、`channels` u8（1 / 2）、`flags` u8（bit0 = 前回からクリップあり、bit1 = 入力切断中）、続けてチャンネルごとに `peak` i16 と `rms` i16（単位は 0.01 dB、-14400 = 無音）。

`meter_id` は `/track/#/meter_id`、`/master/meter_id`（= 0）で対応づけます。

## 8. 検出とペアリング

### 8.1 検出（mDNS / DNS-SD）

- エンジンはサービス `_dcmix._tcp`（ポート = 制御の TCP ポート）を広告します。インスタンス名は `/lan/name`（空ならコンピューター名）。TXT レコード：`proto=1`、`id=<server_id>`、`name=<表示名>`。
- エンジンは同じサービスを探し、見つけた相手（自分以外）を `/lan/peers/#` に載せます。mDNS が届かない相手（別のサブネット、VPN など）は `/lan/peers/add` で IP アドレスから追加できます（手動の相手は再起動後も残ります）。
- `server_id` はインストールごとに 1 度だけ乱数で作り、設定フォルダーに保存します（`/lan/id` にも出します）。
- Windows では OS の DNS-SD（`DnsServiceRegister` / `DnsServiceBrowse`）を使います。UDP 5353 を自分で開かないので、OS の mDNS とぶつからず、ファイアウォールの追加の許可も要りません。

### 8.2 ペアリング

ペアリングは「受け付ける側」と「つなぐ側」の 1 方向です。つないだ側（リモコン、または別の PC のエンジン）が、受け付けた側に接続し、そのストリームを受け取れるようになります。2 台の PC で双方向に送り合うときは、両方の PC でペアリングします。

1. 受け付ける側の画面で `/remote/pairing/start` を呼ぶと 6 桁の数字が返り、`/remote/pairing/active` = true になります。受付は 2 分で終わり、1 台成功するか `/remote/pairing/stop` でも終わります。
2. つなぐ側は Hello に `pair_code` と `client_id` を入れて接続します（相手のエンジンは role = 3。エンジンの UI からは `/lan/peers/pair` `id`, `code` を呼ぶと、エンジンが代わりに接続します）。
3. コードが合えば、エンジンはトークン（128 ビットの乱数を 16 進 32 文字）を作って `Welcome.token` で返し、`client_id`・名前・role と一緒に保存して `/remote/paired/#` に載せます。つなぐ側はトークンを保存し、以後は `Hello.token` で接続します。
4. コードが違うと `unauthorized` で切断します。5 回間違えると受付を終わります。
5. `/remote/unpair` で取り消すと、そのトークンは使えなくなり、つながっていれば切断します。つなぐ側の相手一覧では `refused` になります。

ローカル UI と capd はループバック接続なのでペアリング不要です。制御の TCP は暗号化しません（個人用の LAN での利用が前提）。

## 9. 互換性のルール

- キーやパラメーターの**追加**は互換とみなします（受信側は知らないものを無視）。v1 のまま追加できます。
- パラメーターの意味・型・範囲の変更、削除、メッセージ形式の変更は非互換です。`kProtocolVersion` を上げ、この仕様書と `protocol-params.md` を同じ PR で更新します。
- Hello の role = 3（相手のエンジン）と `client_id` は P3 で足しました。古いエンジンは知らない role を `bad_request` で拒否するので、LAN でつなぐ PC は同じ版にそろえてください。
- 各レーンがパラメーターを足すときは `schema_v1.cpp` に追加し、`protocol-params.md` を生成し直します（`schema_doc_up_to_date` テストが食い違いを検出します）。

## 10. LAN ストリームの購読

LAN の音声・映像は、受け取る側が送る側のエンジンに制御プロトコルで申し込み、送る側がその宛先へ UDP で送ります（パケットの形式は [lan-media.md](lan-media.md)）。受信者のいないストリームは送らず、映像はエンコードもしません。

| ストリーム | 中身 | 送るプロセス（UDP の送り元） |
|---|---|---|
| `audio/<n>` | 出力先 `lan/<n>` に出された音声（`/lan/stream/<n>` の設定） | mixd（UDP 47810） |
| `video/preview` | リモコンのプレビュー（`/video/preview/*`） | capd（UDP 47811） |
| `video/lan` | 映像ミックスの出力先「LAN 送信」（`/video/lan/*`） | capd（UDP 47812） |

- `/lan/subscribe` `stream`, `port` で申し込みます。送り先は「制御の接続元の IP アドレス」の `port`（受け取る側の UDP ポート）です。戻り値の購読番号は `/lan/sub/<番号>` に対応し、`/lan/unsubscribe` `id` でやめます。購読は**その制御接続が切れると自動で消えます**。同じ接続から同じストリーム・同じポートへの申し込みは、前の番号を返します。32 件を超えると `limit_reached` です。
- エンジンは購読を `/lan/sub/#`（`stream`、`address`、`client`）に載せます。capd は `video/*` の購読をツリーで追い、プレビュー・LAN 送信の映像をエンコードしてその宛先へ送ります。`/lan/stream/#/subscribers` は音声ストリームごとの購読数です。
- 受信レポート（lan-media.md 2.4）はパケットの送り元へ返すので、映像のキーフレーム要求は capd に、音声の統計は mixd に届きます。

### 10.1 別の PC の音声をトラックに入れる

- `/track/#/input/kind` = `lan`、`/track/#/input/source` = `<相手の id>/<n>`（相手の `/lan/peers/#/id` と、相手の `audio/<n>`）。
- エンジンはペアリング済みで見つかっている相手に自分から接続し（role = 3）、相手のストリーム名を `/lan/peers/#/stream/#` に写します。トラックが参照しているストリームだけを購読し、参照がなくなればやめます。
- 相手から同じ UDP の送り元で複数のストリームが届くので、受け取る側は相手の `/lan/stream/<n>/stream_id` を見てパケットを振り分けます（コーデックの変更などで送信を開き直すと変わります）。
- 受け取った音声は相手との時計のずれを補正して（lan-media.md 3 章）、エンジンのクロックで鳴らします。`/track/#/input/lan_buffer_ms` は受信バッファの目標（0 = 自動）、`/track/#/input/lan_delay_ms` は今のバッファ量です。
- 相手がいない・受信が途絶えた間は無音で、`/track/#/input/state` = `disconnected` になります。相手が戻れば自動で購読し直します（設計書「デバイス切断時の動作」の LAN 入力）。
