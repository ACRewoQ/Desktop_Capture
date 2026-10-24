#include "dc/protocol/schema.hpp"

#include <limits>

namespace dc::proto {

namespace {

using V = Value;
constexpr auto RO = Access::ReadOnly;
constexpr auto RW = Access::ReadWrite;
constexpr std::int64_t kI64Max = std::numeric_limits<std::int64_t>::max();

ParamSpec b(std::string p, bool def, std::string desc, Access a = RW) {
    return {std::move(p), ValueType::Bool, a, {}, {}, {}, {}, V{def}, std::move(desc)};
}
ParamSpec i(std::string p, std::int64_t lo, std::int64_t hi, std::int64_t def, std::string desc, Access a = RW) {
    return {std::move(p), ValueType::Int, a, double(lo), double(hi), {}, {}, V{def}, std::move(desc)};
}
ParamSpec ic(std::string p, std::vector<std::int64_t> choices, std::int64_t def, std::string desc, Access a = RW) {
    return {std::move(p), ValueType::Int, a, {}, {}, {}, std::move(choices), V{def}, std::move(desc)};
}
ParamSpec f(std::string p, double lo, double hi, double def, std::string desc, Access a = RW) {
    return {std::move(p), ValueType::Float, a, lo, hi, {}, {}, V{def}, std::move(desc)};
}
ParamSpec fAny(std::string p, std::string desc) {
    return {std::move(p), ValueType::Float, RW, {}, {}, {}, {}, V{0.0}, std::move(desc)};
}
ParamSpec s(std::string p, std::string def, std::string desc, Access a = RW) {
    return {std::move(p), ValueType::String, a, {}, {}, {}, {}, V{std::move(def)}, std::move(desc)};
}
ParamSpec e(std::string p, std::vector<std::string> choices, std::string def, std::string desc, Access a = RW) {
    return {std::move(p), ValueType::String, a, {}, {}, std::move(choices), {}, V{std::move(def)}, std::move(desc)};
}

// Parameters shared by /track/# and /master.
void addStrip(Schema& sc, const std::string& base, bool isMaster) {
    sc.add(s(base + "/name", isMaster ? "Master" : "", "表示名"));
    sc.add(f(base + "/volume_db", kMinVolumeDb, kMaxVolumeDb, 0.0, "フェーダー音量 dB（-144 = 無音）"));
    sc.add(f(base + "/pan", -1.0, 1.0, 0.0, "パン（-1 = 左、0 = 中央、1 = 右）。ステレオはバランス"));
    sc.add(b(base + "/lowcut/enabled", false, "ローカット（ハイパス）の有効"));
    sc.add(f(base + "/lowcut/freq_hz", 20.0, 2000.0, 80.0, "ローカット周波数 Hz"));
    sc.add(b(base + "/highcut/enabled", false, "ハイカット（ローパス）の有効"));
    sc.add(f(base + "/highcut/freq_hz", 1000.0, 20000.0, 18000.0, "ハイカット周波数 Hz"));
    sc.add(b(base + "/mute", false, "ミュート"));
    if (!isMaster) {
        sc.add(b(base + "/solo", false, "ソロ"));
        sc.add(b(base + "/cue", false, "キュー（ヘッドホン確認）へ送る"));
    }
    sc.add(i(base + "/meter_id", 0, 65535, 0, "UDPメーターパケット内の識別子", RO));
    sc.add(b(base + "/record/enabled", false, "トラック別録音（WAV/FLAC）の対象"));
    sc.add(e(base + "/record/tap", {"pre", "post"}, "post", "録音位置（フェーダー前 / 後）"));
    sc.add(s(base + "/out/#/target", "",
             isMaster ? "出力先 1〜8。\"\" / device/<id> / vmic/<id> / lan/<n>"
                      : "出力先 1〜8。\"\" / master / track/<n> / device/<id> / vmic/<id> / lan/<n>"));
    if (!isMaster) {
        sc.add(s(base + "/send/#/target", "", "センド先 1〜8。\"\" / track/<n> / device/<id> / vmic/<id> / lan/<n>"));
        sc.add(f(base + "/send/#/level_db", kMinVolumeDb, kMaxVolumeDb, 0.0, "センド量 dB"));
        sc.add(b(base + "/send/#/pre_fader", false, "フェーダー前から送る"));
    }
    sc.add(s(base + "/fx/#/type", "", "エフェクト種別（eq4 / comp / reverb / gate / delay / limiter / vst3:<id>）", RO));
    sc.add(s(base + "/fx/#/name", "", "エフェクトの表示名", RO));
    sc.add(b(base + "/fx/#/bypass", false, "バイパス"));
    sc.add(fAny(base + "/fx/#/param/*",
                "エフェクトのパラメーター。内蔵は下の「エフェクトのパラメーター」表の範囲、VST3 は p<ParamID> で 0〜1"));
    sc.add(e(base + "/fx/#/status", {"ok", "loading", "failed", "missing"}, "ok",
             "状態（loading = 読み込み中、failed = 読み込み失敗、missing = プラグインが見つからない）", RO));
    sc.add(s(base + "/fx/#/error", "", "失敗の理由", RO));
    sc.add(i(base + "/fx/#/latency_samples", 0, 1000000, 0, "エフェクトが足す遅延（サンプル）", RO));
    sc.add(i(base + "/fx/#/param_count", 0, 1000000, 0, "パラメーターの総数（VST3 はツリーに載っていないものも数える）", RO));
    sc.add(b(base + "/fx/#/editor", false, "プラグイン画面を表示する（VST3 のみ、エンジンの PC に出る）"));
}

Schema buildV1() {
    Schema sc;

    // --- system ---
    sc.add(s("/system/version", "", "エンジンのバージョン", RO));
    sc.add(i("/system/protocol", 1, 1000, 1, "プロトコルのメジャーバージョン", RO));
    sc.add(ic("/system/sample_rate", {48000}, 48000, "サンプルレート（48kHz 固定）", RO));
    sc.add(ic("/system/block_size", {64, 128, 256, 512, 1024}, 256, "音声バッファのサンプル数"));
    sc.add(s("/system/clock_device", "", "クロック基準デバイスの ID（\"\" = 自動）"));
    sc.add(e("/system/audio_mode", {"shared", "exclusive"}, "shared",
             "WASAPI の動作モード（shared = 共有、exclusive = 排他。48kHz 以外のデバイスはエンジンがレート変換する）"));
    sc.add(s("/system/clock_active", "", "実際のクロック基準（device/<id>、timer = 出力デバイスがなく内部タイマー）", RO));
    sc.add(f("/system/latency_ms", 0.0, 1000.0, 0.0, "クロック基準デバイスの出力バッファによる遅延 ms", RO));
    sc.add(f("/system/cpu_load", 0.0, 1.0, 0.0, "音声スレッドの負荷率", RO));
    sc.add(i("/system/xruns", 0, kI64Max, 0, "起動後の音切れ回数", RO));

    // --- devices (engine-managed lists) ---
    for (const char* dir : {"input", "output"}) {
        const std::string base = std::string("/devices/") + dir + "/#";
        sc.add(s(base + "/id", "", "デバイス ID（WASAPI はエンドポイント ID、ASIO は asio:<ドライバー名>。protocol.md 4.4）", RO));
        sc.add(s(base + "/name", "", "デバイス名", RO));
        sc.add(i(base + "/channels", 0, 64, 0, "チャンネル数", RO));
        sc.add(b(base + "/connected", false, "接続中", RO));
        sc.add(e(base + "/api", {"wasapi", "asio", "vmic"}, "wasapi", "ドライバー種別", RO));
        sc.add(i(base + "/sample_rate", 0, 384000, 0, "デバイス側のサンプルレート（0 = 不明。48000 以外はエンジンが変換する）", RO));
    }
    sc.add(s("/devices/process/#/exe", "", "音声を出しているアプリの実行ファイル名", RO));
    sc.add(i("/devices/process/#/pid", 0, kI64Max, 0, "プロセス ID", RO));
    sc.add(s("/devices/process/#/title", "", "ウィンドウタイトル", RO));

    // --- mixer ---
    sc.add(i("/mixer/track_count", 0, kMaxTracks, kInitialTracks, "存在するトラック数", RO));
    sc.add(b("/mixer/solo_active", false, "どれかのトラックがソロ中", RO));
    sc.add(s("/cue/target", "", "キューの出力デバイス（device/<id>、\"\" = なし）"));
    sc.add(f("/cue/volume_db", kMinVolumeDb, kMaxVolumeDb, 0.0, "キュー音量 dB"));

    // --- tracks ---
    sc.add(i("/track/#/position", 1, kMaxTracks, 1, "画面上の並び順（1 始まり、連番）", RO));
    sc.add(ic("/track/#/channels", {1, 2}, 2, "1 = モノ、2 = ステレオ"));
    sc.add(e("/track/#/input/kind", {"none", "device", "process", "lan", "sampler"}, "none", "入力の種類"));
    sc.add(s("/track/#/input/source", "",
             "入力元（デバイス ID、実行ファイル名（大文字小文字は区別しない）、LAN は <相手の id>/<n>。sampler は \"\" = 全バンク、\"1\"〜\"8\" = そのバンクだけ）"));
    sc.add(i("/track/#/input/channel", 1, 64, 1, "デバイスの何チャンネル目から使うか"));
    sc.add(e("/track/#/input/state", {"none", "ok", "disconnected"}, "none", "入力の接続状態", RO));
    sc.add(f("/track/#/input/lan_buffer_ms", 0.0, 200.0, 0.0, "LAN 入力の受信バッファ目標 ms（0 = 自動）"));
    sc.add(f("/track/#/input/lan_delay_ms", 0.0, 1000.0, 0.0, "LAN 入力の今の受信バッファ量 ms", RO));
    addStrip(sc, "/track/#", false);
    addStrip(sc, "/master", true);

    // --- capture (output destination "デスクトップキャプチャ") ---
    sc.add(e("/capture/state", {"idle", "starting", "recording", "stopping", "error"}, "idle", "録画状態", RO));
    sc.add(i("/capture/elapsed_ms", 0, kI64Max, 0, "録画経過時間 ms", RO));
    sc.add(s("/capture/file", "", "録画中・直前のファイルパス", RO));
    sc.add(s("/capture/error", "", "直前のエラー内容", RO));
    sc.add(i("/capture/mix", 0, kMaxVideoMixes, 1, "録画する映像ミックス（0 = なし）"));
    sc.add(e("/capture/container", {"mkv", "mp4"}, "mkv", "コンテナ"));
    sc.add(s("/capture/directory", "", "保存先フォルダー（\"\" = ビデオフォルダー）"));
    sc.add(s("/capture/filename", "%Y-%m-%d_%H-%M-%S", "ファイル名パターン（strftime 形式）"));
    sc.add(e("/capture/video/encoder", {"auto", "nvenc", "amf", "qsv", "x264"}, "auto", "エンコーダー"));
    sc.add(e("/capture/video/codec", {"h264", "hevc", "av1"}, "h264", "コーデック"));
    sc.add(i("/capture/video/width", 160, 3840, 1920, "録画解像度（幅）"));
    sc.add(i("/capture/video/height", 90, 2160, 1080, "録画解像度（高さ）"));
    sc.add(i("/capture/video/fps", 10, 120, 60, "フレームレート"));
    sc.add(e("/capture/video/rate_control", {"cbr", "vbr", "cqp"}, "vbr", "レート制御"));
    sc.add(i("/capture/video/bitrate_kbps", 500, 150000, 20000, "ビットレート kbps（cbr / vbr）"));
    sc.add(i("/capture/video/quality", 0, 51, 23, "品質値（cqp、小さいほど高画質）"));
    sc.add(s("/capture/audio/#/source", "", "音声ストリーム 1〜8 の中身（\"\" / master / track/<n>）"));
    sc.add(e("/capture/audio/#/codec", {"aac", "opus", "pcm"}, "aac", "音声コーデック"));
    sc.add(i("/capture/audio/#/bitrate_kbps", 64, 512, 192, "音声ビットレート kbps（aac / opus）"));

    // --- recorder: per-track WAV / FLAC (tracks and master opt in with record/enabled, record/tap) ---
    sc.add(e("/recorder/state", {"idle", "recording", "error"}, "idle", "トラック別録音の状態", RO));
    sc.add(i("/recorder/elapsed_ms", 0, kI64Max, 0, "録音経過時間 ms", RO));
    sc.add(s("/recorder/folder", "", "録音中・直前のテイクのフォルダー", RO));
    sc.add(s("/recorder/error", "", "直前のエラー内容", RO));
    sc.add(i("/recorder/dropped_frames", 0, kI64Max, 0, "書き込みが間に合わず無音で埋めたフレーム数（テイクごと）", RO));
    sc.add(e("/recorder/format", {"wav", "flac"}, "wav", "形式（48kHz / 24bit。WAV は 4GB を超えると RF64）"));
    sc.add(s("/recorder/directory", "", "保存先フォルダー（\"\" = ミュージック\\Desktop Capture）"));
    sc.add(s("/recorder/folder_name", "%Y-%m-%d_%H-%M-%S", "テイクのフォルダー名パターン（strftime 形式）"));
    sc.add(b("/recorder/with_capture", true, "録画（/capture）の開始・停止に合わせて録音も開始・停止する"));

    // --- sampler: sound effect pads, played into the tracks whose input kind is sampler ---
    sc.add(i("/sampler/active_bank", 1, kSamplerBanks, 1, "表示・ホットキー・MIDI の対象バンク"));
    sc.add(s("/sampler/bank/#/name", "", "バンク名"));
    sc.add(s("/sampler/bank/#/pad/#/file", "", "音声ファイルのパス（WAV / FLAC / MP3 / Ogg Vorbis。\"\" = 空）"));
    sc.add(s("/sampler/bank/#/pad/#/name", "", "パッドの表示名（\"\" = ファイル名）"));
    sc.add(s("/sampler/bank/#/pad/#/color", "", "パッドの色（#rrggbb、\"\" = 既定）"));
    sc.add(e("/sampler/bank/#/pad/#/mode", {"oneshot", "toggle", "hold", "loop"}, "oneshot",
             "oneshot = 最後まで再生（押し直すと頭から）、toggle = 押すたびに再生・停止、hold = 押している間だけ、loop = 押すたびにループ再生・停止"));
    sc.add(f("/sampler/bank/#/pad/#/volume_db", kMinVolumeDb, kMaxVolumeDb, 0.0, "パッドの音量 dB"));
    sc.add(e("/sampler/bank/#/pad/#/state", {"empty", "loading", "ready", "error"}, "empty", "ファイルの読み込み状態", RO));
    sc.add(s("/sampler/bank/#/pad/#/error", "", "読み込みエラーの内容", RO));
    sc.add(i("/sampler/bank/#/pad/#/duration_ms", 0, kI64Max, 0, "音声の長さ ms", RO));
    sc.add(b("/sampler/bank/#/pad/#/playing", false, "再生中", RO));

    // --- video (minimal in v1; scenes and layers are added by lane B1 in P3) ---
    sc.add(s("/video/mix/#/name", "", "映像ミックス名"));
    sc.add(i("/video/mix/#/width", 160, 3840, 1920, "合成解像度（幅）"));
    sc.add(i("/video/mix/#/height", 90, 2160, 1080, "合成解像度（高さ）"));
    sc.add(i("/video/mix/#/fps", 10, 120, 60, "合成フレームレート"));
    sc.add(b("/video/mix/#/active", false, "出力先があり合成中", RO));
    sc.add(i("/video/vcam/mix", 0, kMaxVideoMixes, 0, "仮想カメラに出す映像ミックス（0 = なし）"));
    sc.add(e("/video/vcam/resolution", {"1080p", "720p"}, "1080p", "仮想カメラの解像度"));
    sc.add(i("/video/monitor/mix", 0, kMaxVideoMixes, 0, "全画面表示に出す映像ミックス（0 = なし）"));
    sc.add(s("/video/monitor/display", "", "全画面表示先のモニター ID"));
    // what capd's outputs are doing (capd reports these with /video/output/report)
    sc.add(e("/video/vcam/state", {"off", "waiting", "active", "unavailable"}, "off",
             "仮想カメラの状態（waiting = カメラを開いたアプリがない、unavailable = 未登録など）", RO));
    sc.add(s("/video/vcam/detail", "", "仮想カメラの状態の説明（unavailable の理由など）", RO));
    sc.add(e("/video/monitor/state", {"off", "active", "no_display", "error"}, "off",
             "全画面表示の状態（no_display = モニターが見つからない。つながれば自動で戻る）", RO));
    sc.add(s("/video/monitor/detail", "", "全画面表示の状態の説明（error の内容など）", RO));
    sc.add(i("/video/preview/mix", 0, kMaxVideoMixes, 1, "リモコンのプレビューに出す映像ミックス"));
    sc.add(i("/video/preview/width", 160, 1920, 960, "プレビュー解像度（幅）"));
    sc.add(i("/video/preview/height", 90, 1080, 540, "プレビュー解像度（高さ）"));
    sc.add(i("/video/preview/fps", 5, 60, 30, "プレビューのフレームレート"));
    sc.add(i("/video/preview/bitrate_kbps", 500, 8000, 3000, "プレビューのビットレート kbps"));
    sc.add(i("/video/lan/mix", 0, kMaxVideoMixes, 0, "LAN 送信（映像）に出す映像ミックス（0 = なし）"));
    sc.add(i("/video/lan/width", 160, 3840, 1920, "LAN 送信の解像度（幅）"));
    sc.add(i("/video/lan/height", 90, 2160, 1080, "LAN 送信の解像度（高さ）"));
    sc.add(i("/video/lan/fps", 5, 60, 30, "LAN 送信のフレームレート"));
    sc.add(i("/video/lan/bitrate_kbps", 500, 50000, 8000, "LAN 送信のビットレート kbps"));

    // --- LAN media transport (docs/spec/protocol.md 10, docs/spec/lan-media.md) ---
    sc.add(s("/lan/name", "", "LAN で名乗る名前（\"\" = コンピューター名）"));
    sc.add(s("/lan/id", "", "この PC のエンジンの id（Welcome.server_id と同じ。インストールごとに固定）", RO));
    sc.add(s("/lan/stream/#/name", "", "LAN 送信ストリーム 1〜8 の表示名（\"\" = LAN <n>）。出力先 lan/<n> で使う"));
    sc.add(e("/lan/stream/#/codec", {"pcm16", "pcm24", "opus"}, "pcm24", "コーデック（pcm = 無圧縮・低遅延、opus = 無線向け）"));
    sc.add(ic("/lan/stream/#/opus_frames", {120, 240, 480, 960}, 480, "Opus のパケット長（フレーム数、480 = 10 ms）"));
    sc.add(i("/lan/stream/#/bitrate_kbps", 32, 512, 128, "Opus のビットレート kbps"));
    sc.add(i("/lan/stream/#/stream_id", 0, 4294967295LL, 0,
             "今のストリーム ID（パケットの stream_id。送信を開き直すと変わる。0 = 送っていない）", RO));
    sc.add(i("/lan/stream/#/subscribers", 0, kMaxLanSubscriptions, 0, "受信している相手の数", RO));
    sc.add(b("/lan/stream/#/active", false, "何かが出力していて、受信者がいて送信中", RO));
    sc.add(s("/lan/sub/#/stream", "", "購読しているストリーム（audio/<n> / video/preview / video/lan）", RO));
    sc.add(s("/lan/sub/#/address", "", "送り先の UDP アドレス（ip:port）", RO));
    sc.add(s("/lan/sub/#/client", "", "購読した接続の名前", RO));
    sc.add(s("/lan/peers/#/id", "", "LAN 内の別の PC のエンジンの id", RO));
    sc.add(s("/lan/peers/#/name", "", "その PC の名前", RO));
    sc.add(s("/lan/peers/#/address", "", "制御の接続先（ip:port）", RO));
    sc.add(e("/lan/peers/#/state", {"offline", "unpaired", "connecting", "connected", "refused"}, "offline",
             "相手の状態（refused = ペアリングが取り消された）", RO));
    sc.add(b("/lan/peers/#/manual", false, "手動で追加した相手（mDNS で見つからない場合）", RO));
    sc.add(s("/lan/peers/#/stream/#/name", "", "相手の LAN 送信ストリームの表示名（接続中だけ）", RO));
    sc.add(b("/lan/peers/#/stream/#/active", false, "相手のストリームに何かが出力している", RO));

    // --- plugins (VST3, lane A1) ---
    sc.add(s("/plugins/extra_dirs", "", "標準以外の VST3 フォルダー（; 区切り）"));
    sc.add(e("/plugins/scan/state", {"idle", "scanning"}, "idle", "プラグインのスキャン状態", RO));
    sc.add(f("/plugins/scan/progress", 0.0, 1.0, 0.0, "スキャンの進み具合", RO));
    sc.add(s("/plugins/scan/current", "", "スキャン中のファイル", RO));
    sc.add(s("/plugins/#/id", "", "エフェクト種別として使う ID（vst3:<id>）", RO));
    sc.add(s("/plugins/#/name", "", "プラグイン名", RO));
    sc.add(s("/plugins/#/vendor", "", "メーカー", RO));
    sc.add(s("/plugins/#/category", "", "分類（プラグインの申告、例 Fx|EQ）", RO));
    sc.add(b("/plugins/#/instrument", false, "音源（MIDI で鳴らすもの）", RO));
    sc.add(e("/plugins/#/status", {"ok", "failed", "crashed", "timeout", "unsupported"}, "ok",
             "スキャン結果（unsupported = モノ・ステレオのメイン入出力がない）", RO));
    sc.add(s("/plugins/#/error", "", "スキャンで失敗した理由", RO));
    sc.add(s("/plugins/#/path", "", "プラグインのファイル", RO));

    // --- remote ---
    sc.add(i("/remote/clients", 0, 4, 0, "接続中のリモコン数", RO));
    sc.add(b("/remote/pairing/active", false, "ペアリング受付中", RO));
    sc.add(s("/remote/paired/#/id", "", "ペアリング済みの相手の id（Hello.client_id）", RO));
    sc.add(s("/remote/paired/#/name", "", "ペアリング済みの相手の名前", RO));
    sc.add(e("/remote/paired/#/role", {"remote", "peer"}, "remote", "相手の種類（リモコン / 別の PC のエンジン）", RO));
    sc.add(b("/remote/paired/#/online", false, "今つながっている", RO));

    // --- methods ---
    sc.addMethod({"/session/resync", "", "nil", "スナップショットを送り直す（rev の飛びを検出したとき）", "P0"});
    sc.addMethod({"/mixer/track/add", "channels:int?, name:string?, position:int?", "int（新しいトラック番号）",
                  "トラックを追加。番号は空いている最小の番号", "P0"});
    sc.addMethod({"/mixer/track/remove", "track:int", "nil", "トラックを削除。番号は詰めない", "P0"});
    sc.addMethod({"/mixer/track/move", "track:int, position:int", "nil", "並び順を変える", "P0"});
    sc.addMethod({"/capture/record/start", "", "nil", "録画開始", "P0"});
    sc.addMethod({"/capture/record/stop", "", "nil", "録画停止", "P0"});
    sc.addMethod({"/capture/report", "state:string, file:string?, error:string?, duration_ms:int?, reason:string?", "nil",
                  "capd が録画の状態をエンジンに知らせる（role = サービスの接続だけ。reason は録画中のファイル切り替え）", "P2"});
    sc.addMethod({"/recorder/start", "", "string（テイクのフォルダー）",
                  "トラック別録音を開始。録音対象がなければ conflict", "P3"});
    sc.addMethod({"/recorder/stop", "", "nil", "トラック別録音を停止", "P3"});
    sc.addMethod({"/sampler/trigger", "pad:int, bank:int?", "nil",
                  "パッドを押す（bank の既定は /sampler/active_bank）。動きは mode による", "P3"});
    sc.addMethod({"/sampler/release", "pad:int, bank:int?", "nil", "パッドを離す（hold のパッドが止まる）", "P3"});
    sc.addMethod({"/sampler/stop", "bank:int?", "nil", "再生中のパッドを止める（bank なし = 全バンク）", "P3"});
    sc.addMethod({"/track/#/fx/add", "type:string, index:int?", "int（スロット番号）", "エフェクト追加", "P3"});
    sc.addMethod({"/track/#/fx/remove", "index:int", "nil", "エフェクト削除", "P3"});
    sc.addMethod({"/track/#/fx/move", "from:int, to:int", "nil", "エフェクトの順序変更", "P3"});
    sc.addMethod({"/master/fx/add", "type:string, index:int?", "int（スロット番号）", "エフェクト追加", "P3"});
    sc.addMethod({"/master/fx/remove", "index:int", "nil", "エフェクト削除", "P3"});
    sc.addMethod({"/master/fx/move", "from:int, to:int", "nil", "エフェクトの順序変更", "P3"});
    sc.addMethod({"/track/#/fx/#/params", "offset:int?, count:int?, keys:string?", "string（JSON 配列）",
                  "パラメーターの名前・範囲・表示文字列を取得。VST3 は取得したものをツリーに載せる", "P3"});
    sc.addMethod({"/master/fx/#/params", "offset:int?, count:int?, keys:string?", "string（JSON 配列）",
                  "パラメーターの名前・範囲・表示文字列を取得。VST3 は取得したものをツリーに載せる", "P3"});
    sc.addMethod({"/plugins/scan", "rescan:bool?", "nil", "VST3 プラグインを探す（rescan = 前回の結果を使わずに全部）", "P3"});
    sc.addMethod({"/devices/refresh", "", "nil", "デバイス・プロセス一覧の更新", "P2"});
    sc.addMethod({"/video/output/report", "output:string, state:string, detail:string?", "nil",
                  "capd が出力（vcam / monitor）の状態をエンジンに知らせる（role = サービスの接続だけ）", "P3"});
    sc.addMethod({"/remote/pairing/start", "", "string（6桁コード）", "ペアリング受付開始（2 分、1 台つながるまで）", "P3"});
    sc.addMethod({"/remote/pairing/stop", "", "nil", "ペアリング受付終了", "P3"});
    sc.addMethod({"/remote/unpair", "id:string", "nil", "ペアリングを取り消す（つながっていれば切断）", "P3"});
    sc.addMethod({"/lan/subscribe", "stream:string, port:int", "int（購読番号）",
                  "ストリームを受け取る。送り先は接続元の IP の port。接続が切れると自動で解除", "P3"});
    sc.addMethod({"/lan/unsubscribe", "id:int", "nil", "購読をやめる", "P3"});
    sc.addMethod({"/lan/peers/pair", "id:string, code:string", "nil", "相手の PC に表示された 6 桁コードでペアリングする", "P3"});
    sc.addMethod({"/lan/peers/forget", "id:string", "nil", "相手のペアリング情報を消す（手動で追加した相手は一覧からも消す）", "P3"});
    sc.addMethod({"/lan/peers/add", "address:string", "nil", "相手を IP アドレス（ip または ip:port）で手動追加", "P3"});

    return sc;
}

} // namespace

const Schema& schemaV1() {
    static const Schema schema = buildV1();
    return schema;
}

} // namespace dc::proto
