# Desktop Capture

デスクトップキャプチャー付きソフトウェアミキサー（個人用）。Windows 10 / 11 向けの C++20 ネイティブアプリです。

- 最大 64 トラックのミキサー（音量・パン・Lo-Hi フィルター、内蔵エフェクト、VST3、LAN 入出力、録音、サンプラー）
- デスクトップキャプチャー（ビデオミキサー、GPU エンコード、トラック別音声の録画、仮想カメラ）
- 別 PC からのリモートコントローラー（プレビュー、タッチ操作）

設計と計画は Claude Docs の「全体アーキテクチャ設計書」と「開発計画」にあります。現在は **フェーズ 0（開発基盤と共通仕様）** です。

## 構成

| ディレクトリ | 内容 |
|---|---|
| `apps/mixd` | 音声エンジン（設定の正本を持つ）。P0 は空の骨格 |
| `apps/capd` | キャプチャー（画面取得・合成・エンコード・録画）。P0 は空の骨格 |
| `apps/ui` | UI（ローカル／リモート共通、Qt 6 Quick 予定）。P0 は仮の実行ファイル |
| `libs/protocol` | 制御プロトコル：メッセージ、MessagePack コーデック、パラメーターツリー、スキーマ、ルーティング検証、メーターパケット |
| `libs/control` | 制御プロトコルの TCP サーバー／クライアント（asio） |
| `libs/shmaudio` | エンジン→キャプチャーの共有メモリ音声リング |
| `tools/mock_server` | モックサーバー（プロトコルを話す偽エンジン） |
| `tools/dc_cli` | テスト用クライアント（コマンドライン） |
| `tools/shm_tone` | 共有メモリ音声の試験用の書き手・読み手 |
| `tools/capbench` | P1-V3 検証ツール：画面キャプチャー（WGC / Desktop Duplication）→ GPU 上で NV12 変換 → FFmpeg ハードウェアエンコードの負荷計測（Windows のみ）。結果は [docs/verification/v3-capture-encode.md](docs/verification/v3-capture-encode.md) |
| `tools/loopback_probe` | P1-V1 検証ツール：アプリ単位の音声キャプチャー（WASAPI Process Loopback）の自己テストと手動キャプチャー（Windows のみ）。結果は [docs/verification/v1-process-loopback.md](docs/verification/v1-process-loopback.md) |
| `tests` | 単体テスト・結合テスト（Catch2） |
| `docs/spec` | [制御プロトコル仕様](docs/spec/protocol.md)、[パラメーター一覧](docs/spec/protocol-params.md)、[共有メモリ音声フォーマット](docs/spec/shm-audio.md) |

## ビルドとテスト

CI は使いません。テストはすべてローカルで行います。手順は [docs/build-and-test.md](docs/build-and-test.md) を参照してください。

```powershell
.\scripts\test.ps1
```

## ライセンス

個人用（配布予定なし）。
