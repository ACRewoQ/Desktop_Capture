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
| `/video` | 映像ミックス、仮想カメラ、全画面表示、プレビュー | B |
| `/remote` | リモコンの接続数・ペアリング | N / C |
| `/lan` | LAN 送信ストリーム、購読、LAN 内の別の PC（相手） | N |

### 4.1 上限値

| 項目 | 値 |
|---|---|
| 通常トラック | 最大 64（初期 4）。マスターは別 |
| トラックの出力先 | 1 トラックあたり 8（`out/1`〜`out/8`） |
| センド | 1 トラックあたり 8（`send/1`〜`send/8`） |
| エフェクト | 1 トラックあたり 16 |
| 映像ミックス | 最大 4 |
| 録画の音声ストリーム | 8（`/capture/audio/1`〜`8`） |
| リモコン同時接続 | 4 |
| LAN 送信ストリーム（音声） | 8（`/lan/stream/1`〜`8`、出力先 `lan/1`〜`lan/8`） |
| 購読 | 32（`/lan/sub/#`） |
| 相手の PC の一覧 | 16（`/lan/peers/#`） |
| ペアリング済みの相手 | 16（`/remote/paired/#`） |

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
| `/capture/report` | `state`:str, `file`:str?, `error`:str?, `duration_ms`:int? | nil | P2 | capd から録画の状態を知らせる（5.4.1）。role = サービス以外は `unauthorized` |
| `/track/#/fx/add` ほか | 一覧参照 | | P3 | エフェクトの追加・削除・並べ替え |
| `/devices/refresh` | なし | nil | P2 | デバイス一覧の更新 |
| `/remote/pairing/start` / `stop` | なし | str（6 桁）/ nil | P3 | ペアリング受付（8 章） |
| `/remote/unpair` | `id`:str | nil | P3 | ペアリングの取り消し。つながっていれば切断する |
| `/lan/subscribe` / `unsubscribe` | `stream`:str, `port`:int / `id`:int | int（購読番号）/ nil | P3 | ストリームの購読（10 章） |
| `/lan/peers/pair` | `id`:str, `code`:str | nil | P3 | 相手の PC とペアリングする（8 章） |
| `/lan/peers/forget` / `add` | `id`:str / `address`:str | nil | P3 | 相手の記録を消す / IP アドレスで手動追加 |

未実装・未知のメソッドは `unknown_method` を返します。

#### 5.4.1 録画の開始・停止（エンジンと capd）

録画の設定と状態はエンジンが持ち、実際の録画は capd が行います。capd はエンジンに role = 2（サービス）、`client` = `"capd"` で接続し、`/capture/*` と `/video/mix/#` をツリーで追います。

| きっかけ | エンジン | capd |
|---|---|---|
| `/capture/record/start`（UI・リモコン） | capd が未接続なら `conflict`（"capture process not running"）。`/capture/state` が `idle` か `error` でなければ `conflict`。それ以外は `/capture/state` = `starting` | `starting` を見て録画を始め、`/capture/report` `state="recording"`, `file` |
| `report recording` | `/capture/state` = `recording`、`/capture/file`、`/capture/error` = `""`、`capture.started`。以後 `/capture/elapsed_ms` を数える | |
| `/capture/record/stop` | `recording` でなければ `conflict`。それ以外は `/capture/state` = `stopping` | ファイルを閉じて `/capture/report` `state="idle"`, `file`, `duration_ms` |
| `report idle` | `/capture/state` = `idle`、`capture.stopped` | |
| capd 側の失敗（エンコーダーを開けない、ディスク満杯で安全停止など） | `/capture/state` = `error`、`/capture/error` = `error` | `/capture/report` `state="error"`, `error` |
| 録画中（`starting` / `recording` / `stopping`）に capd の接続が切れた | `/capture/state` = `error`、`/capture/error` = "capture process disconnected" | |

`state` に `recording` / `idle` / `error` 以外が来たら `bad_request` です。エンジンの共有メモリ音声は録画中でなくても常に書かれているので、capd は `starting` を見た時点から読み始められます。

### 5.5 Event（通知）

パラメーターではない出来事を知らせます。状態そのものは必ずツリーにも載せ、Event は「起きたこと」の通知だけに使います（再接続したクライアントはツリーから状態を復元できるようにするため）。

| 名前 | data | 送るとき |
|---|---|---|
| `capture.started` | `file` | 録画開始 |
| `capture.stopped` | `file`, `duration_ms` | 録画停止 |
| `capture.segment` | `file`, `reason` | GPU リセットなどで新しいファイルに切り替えた（P3） |
| `device.lost` / `device.restored` | `id`, `name` | デバイスの切断・復帰（P3） |

### 5.6 生存確認

- クライアントは 5 秒ごとに `Ping` を送ります。サーバーはすぐ `Pong` を返します。
- サーバーは 15 秒間何も受信しなかった接続を切ります。
- 切断されたクライアントは再接続し、スナップショットから作り直します。

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
| `video/lan` | 映像ミックスの出力先「LAN 送信」（`/video/lan/*`） | capd（UDP 47811） |

- `/lan/subscribe` `stream`, `port` で申し込みます。送り先は「制御の接続元の IP アドレス」の `port`（受け取る側の UDP ポート）です。戻り値の購読番号は `/lan/sub/<番号>` に対応し、`/lan/unsubscribe` `id` でやめます。購読は**その制御接続が切れると自動で消えます**。同じ接続から同じストリーム・同じポートへの申し込みは、前の番号を返します。32 件を超えると `limit_reached` です。
- エンジンは購読を `/lan/sub/#`（`stream`、`address`、`client`）に載せます。capd は `video/*` の購読をツリーで追い、プレビュー・LAN 送信の映像をエンコードしてその宛先へ送ります。`/lan/stream/#/subscribers` は音声ストリームごとの購読数です。
- 受信レポート（lan-media.md 2.4）はパケットの送り元へ返すので、映像のキーフレーム要求は capd に、音声の統計は mixd に届きます。

### 10.1 別の PC の音声をトラックに入れる

- `/track/#/input/kind` = `lan`、`/track/#/input/source` = `<相手の id>/<n>`（相手の `/lan/peers/#/id` と、相手の `audio/<n>`）。
- エンジンはペアリング済みで見つかっている相手に自分から接続し（role = 3）、相手のストリーム名を `/lan/peers/#/stream/#` に写します。トラックが参照しているストリームだけを購読し、参照がなくなればやめます。
- 相手から同じ UDP の送り元で複数のストリームが届くので、受け取る側は相手の `/lan/stream/<n>/stream_id` を見てパケットを振り分けます（コーデックの変更などで送信を開き直すと変わります）。
- 受け取った音声は相手との時計のずれを補正して（lan-media.md 3 章）、エンジンのクロックで鳴らします。`/track/#/input/lan_buffer_ms` は受信バッファの目標（0 = 自動）、`/track/#/input/lan_delay_ms` は今のバッファ量です。
- 相手がいない・受信が途絶えた間は無音で、`/track/#/input/state` = `disconnected` になります。相手が戻れば自動で購読し直します（設計書「デバイス切断時の動作」の LAN 入力）。
