#include "dc/protocol/schema.hpp"

#include <limits>

namespace dc::proto {

namespace {

using V = Value;
constexpr auto RO = Access::ReadOnly;
constexpr auto RW = Access::ReadWrite;
constexpr std::int64_t kI64Max = std::numeric_limits<std::int64_t>::max();
constexpr std::int64_t kArgbMax = 0xFFFFFFFFLL;

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
             isMaster ? "出力先 1〜8。\"\" / device/<id> / vmic/<id> / lan/<stream>"
                      : "出力先 1〜8。\"\" / master / track/<n> / device/<id> / vmic/<id> / lan/<stream>"));
    if (!isMaster) {
        sc.add(s(base + "/send/#/target", "", "センド先 1〜8。\"\" / track/<n> / device/<id> / vmic/<id> / lan/<stream>"));
        sc.add(f(base + "/send/#/level_db", kMinVolumeDb, kMaxVolumeDb, 0.0, "センド量 dB"));
        sc.add(b(base + "/send/#/pre_fader", false, "フェーダー前から送る"));
    }
    sc.add(s(base + "/fx/#/type", "", "エフェクト種別（eq4 / comp / reverb / gate / delay / limiter / vst3:<id>）", RO));
    sc.add(s(base + "/fx/#/name", "", "エフェクトの表示名", RO));
    sc.add(b(base + "/fx/#/bypass", false, "バイパス"));
    sc.add(fAny(base + "/fx/#/param/*", "エフェクトのパラメーター。範囲はエフェクト種別ごとに P3 で定義"));
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
             "WASAPI の動作モード（shared = 共有、exclusive = 排他。排他は 48kHz で開けるデバイスだけ）"));
    sc.add(s("/system/clock_active", "", "実際のクロック基準（device/<id>、timer = 出力デバイスがなく内部タイマー）", RO));
    sc.add(f("/system/latency_ms", 0.0, 1000.0, 0.0, "クロック基準デバイスの出力バッファによる遅延 ms", RO));
    sc.add(f("/system/cpu_load", 0.0, 1.0, 0.0, "音声スレッドの負荷率", RO));
    sc.add(i("/system/xruns", 0, kI64Max, 0, "起動後の音切れ回数", RO));

    // --- devices (engine-managed lists) ---
    for (const char* dir : {"input", "output"}) {
        const std::string base = std::string("/devices/") + dir + "/#";
        sc.add(s(base + "/id", "", "デバイス ID（WASAPI エンドポイント ID）", RO));
        sc.add(s(base + "/name", "", "デバイス名", RO));
        sc.add(i(base + "/channels", 0, 64, 0, "チャンネル数", RO));
        sc.add(b(base + "/connected", false, "接続中", RO));
        sc.add(e(base + "/api", {"wasapi", "asio", "vmic"}, "wasapi", "ドライバー種別", RO));
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
    sc.add(s("/track/#/input/source", "", "入力元（デバイス ID、実行ファイル名、LAN ストリーム ID）"));
    sc.add(i("/track/#/input/channel", 1, 64, 1, "デバイスの何チャンネル目から使うか"));
    sc.add(e("/track/#/input/state", {"none", "ok", "disconnected"}, "none", "入力の接続状態", RO));
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

    // --- video mixer (lane B1): sources, scenes with layers, mixes with program/preview and transitions ---
    // Sources, scenes and layers are added and removed by methods; numbers are fixed IDs like tracks.
    sc.add(s("/video/mix/#/name", "", "映像ミックス名"));
    sc.add(i("/video/mix/#/width", 160, 3840, 1920, "合成解像度（幅）"));
    sc.add(i("/video/mix/#/height", 90, 2160, 1080, "合成解像度（高さ）"));
    sc.add(i("/video/mix/#/fps", 10, 120, 60, "合成フレームレート"));
    sc.add(b("/video/mix/#/active", false, "出力先から参照されている（/capture/mix・/video/vcam/mix・/video/monitor/mix・/video/preview/mix）", RO));
    sc.add(i("/video/mix/#/background", 0, kArgbMax, 0xFF000000LL, "背景色（0xAARRGGBB）"));
    sc.add(i("/video/mix/#/program_scene", 0, kMaxVideoScenes, 0,
             "本番に出しているシーン（0 = なし）。変えるとトランジションで切り替わる"));
    sc.add(i("/video/mix/#/preview_scene", 0, kMaxVideoScenes, 0, "次に出すシーン（プレビュー、0 = なし）"));
    sc.add(e("/video/mix/#/transition/type", {"cut", "fade"}, "fade", "シーン切り替えの効果"));
    sc.add(i("/video/mix/#/transition/duration_ms", 0, 10000, 300, "フェードの長さ ms"));

    sc.add(s("/video/scene/#/name", "", "シーン名"));
    sc.add(i("/video/scene/#/position", 1, kMaxVideoScenes, 1, "シーン一覧の並び順（1 始まり、連番）", RO));
    {
        const std::string l = "/video/scene/#/layer/#";
        sc.add(s(l + "/name", "", "レイヤー名（\"\" = ソース名を表示）"));
        sc.add(i(l + "/position", 1, kMaxLayersPerScene, 1, "重なり順（1 = 最前面、連番）", RO));
        sc.add(i(l + "/source", 0, kMaxVideoSources, 0, "表示するソース /video/source/<n>（0 = なし）"));
        sc.add(b(l + "/visible", true, "表示"));
        sc.add(f(l + "/x", -4.0, 4.0, 0.0, "表示枠の左端（キャンバスの幅 = 1）"));
        sc.add(f(l + "/y", -4.0, 4.0, 0.0, "表示枠の上端（キャンバスの高さ = 1）"));
        sc.add(f(l + "/width", 0.0, 8.0, 1.0, "表示枠の幅（キャンバスの幅 = 1）"));
        sc.add(f(l + "/height", 0.0, 8.0, 1.0, "表示枠の高さ（キャンバスの高さ = 1）"));
        sc.add(e(l + "/fit", {"fit", "fill", "stretch"}, "fit",
                 "枠への収め方（fit = 縦横比を保って全体を表示、fill = 縦横比を保って枠を埋める、stretch = 引き伸ばす）"));
        for (const char* side : {"left", "top", "right", "bottom"})
            sc.add(f(l + "/crop/" + side, 0.0, 1.0, 0.0, "切り取り（ソースの幅・高さに対する割合）"));
        sc.add(f(l + "/opacity", 0.0, 1.0, 1.0, "不透明度"));
        sc.add(b(l + "/chroma/enabled", false, "クロマキー"));
        sc.add(i(l + "/chroma/color", 0, 0xFFFFFF, 0x00FF00, "抜く色（0xRRGGBB）"));
        sc.add(f(l + "/chroma/similarity", 0.0, 1.0, 0.4, "抜く色の範囲（大きいほど広く抜く）"));
        sc.add(f(l + "/chroma/smoothness", 0.0, 1.0, 0.08, "境目のなめらかさ"));
        sc.add(f(l + "/chroma/spill", 0.0, 1.0, 0.1, "色かぶりの除去"));
        sc.add(e(l + "/no_signal", {"transparent", "image"}, "transparent",
                 "ソースが途切れたときの表示（transparent = 透明、image = 「信号なし」画像）"));
    }

    {
        const std::string src = "/video/source/#";
        sc.add(s(src + "/name", "", "ソース名"));
        sc.add(e(src + "/kind", {"display", "window", "camera", "image", "media", "text", "color", "lan"}, "display",
                 "ソースの種類（追加時に決まる）", RO));
        sc.add(e(src + "/state", {"none", "ok", "disconnected", "error"}, "none",
                 "capd が報告する状態（none = どのミックスでも使っていない）", RO));
        sc.add(s(src + "/error", "", "state = error の理由", RO));
        sc.add(i(src + "/width", 0, 16384, 0, "ソースの実際の幅（capd が報告）", RO));
        sc.add(i(src + "/height", 0, 16384, 0, "ソースの実際の高さ（capd が報告）", RO));
        sc.add(b(src + "/cursor", true, "マウスカーソルを入れる（display / window）"));
        sc.add(s(src + "/display/id", "", "モニター ID（/video/devices/display/#/id、\"\" = 主モニター）"));
        sc.add(e(src + "/display/method", {"wgc", "dda"}, "wgc",
                 "取得方式（wgc = Windows.Graphics.Capture、dda = Desktop Duplication）"));
        sc.add(s(src + "/window/exe", "", "ウィンドウの実行ファイル名（例 game.exe）"));
        sc.add(s(src + "/window/title", "", "ウィンドウタイトル"));
        sc.add(e(src + "/window/match", {"title", "exe"}, "title",
                 "閉じたあとにつなぎ直す条件（title = 実行ファイル名とタイトル、exe = 実行ファイル名だけ）"));
        sc.add(s(src + "/camera/id", "", "カメラ ID（/video/devices/camera/#/id）"));
        sc.add(s(src + "/camera/name", "", "カメラ名（ID が変わったときの照合用）"));
        sc.add(i(src + "/camera/width", 0, 7680, 0, "取り込み解像度の幅（0 = 自動：1920×1080 以下で最大のもの）"));
        sc.add(i(src + "/camera/height", 0, 4320, 0, "取り込み解像度の高さ（0 = 自動）"));
        sc.add(i(src + "/camera/fps", 0, 240, 0, "取り込みフレームレート（0 = 自動：60 以下で最大）"));
        sc.add(s(src + "/file", "", "ファイルのパス（image / media）"));
        sc.add(b(src + "/media/loop", true, "動画を繰り返す"));
        sc.add(s(src + "/text/content", "", "表示する文字（改行可）"));
        sc.add(s(src + "/text/font", "Yu Gothic UI", "フォント名"));
        sc.add(i(src + "/text/size", 8, 512, 48, "文字の大きさ（ピクセル）"));
        sc.add(i(src + "/text/color", 0, kArgbMax, 0xFFFFFFFFLL, "文字色（0xAARRGGBB）"));
        sc.add(i(src + "/text/background", 0, kArgbMax, 0, "背景色（0xAARRGGBB、0 = 透明）"));
        sc.add(b(src + "/text/bold", false, "太字"));
        sc.add(b(src + "/text/italic", false, "斜体"));
        sc.add(e(src + "/text/align", {"left", "center", "right"}, "left", "行揃え"));
        sc.add(i(src + "/text/wrap_width", 0, 7680, 0, "折り返し幅（ピクセル、0 = 折り返さない）"));
        sc.add(i(src + "/color/value", 0, kArgbMax, 0xFF000000LL, "単色ソースの色（0xAARRGGBB）"));
        sc.add(s(src + "/lan/stream", "", "LAN メディア伝送の受信ストリーム ID（レーン N）"));
    }

    // what capd can capture on this PC (capd reports these with /video/devices/report)
    sc.add(s("/video/devices/display/#/id", "", "モニター ID", RO));
    sc.add(s("/video/devices/display/#/name", "", "モニター名", RO));
    sc.add(i("/video/devices/display/#/width", 0, 16384, 0, "幅（物理ピクセル）", RO));
    sc.add(i("/video/devices/display/#/height", 0, 16384, 0, "高さ（物理ピクセル）", RO));
    sc.add(b("/video/devices/display/#/primary", false, "主モニター", RO));
    sc.add(s("/video/devices/window/#/exe", "", "実行ファイル名", RO));
    sc.add(s("/video/devices/window/#/title", "", "ウィンドウタイトル", RO));
    sc.add(s("/video/devices/camera/#/id", "", "カメラ ID", RO));
    sc.add(s("/video/devices/camera/#/name", "", "カメラ名", RO));

    sc.add(i("/video/vcam/mix", 0, kMaxVideoMixes, 0, "仮想カメラに出す映像ミックス（0 = なし）"));
    sc.add(e("/video/vcam/resolution", {"1080p", "720p"}, "1080p", "仮想カメラの解像度"));
    sc.add(i("/video/monitor/mix", 0, kMaxVideoMixes, 0, "全画面表示に出す映像ミックス（0 = なし）"));
    sc.add(s("/video/monitor/display", "", "全画面表示先のモニター ID"));
    sc.add(i("/video/preview/mix", 0, kMaxVideoMixes, 1, "リモコンのプレビューに出す映像ミックス"));
    sc.add(i("/video/preview/width", 160, 1920, 960, "プレビュー解像度（幅）"));
    sc.add(i("/video/preview/height", 90, 1080, 540, "プレビュー解像度（高さ）"));
    sc.add(i("/video/preview/fps", 5, 60, 30, "プレビューのフレームレート"));
    sc.add(i("/video/preview/bitrate_kbps", 500, 8000, 3000, "プレビューのビットレート kbps"));

    // --- remote ---
    sc.add(i("/remote/clients", 0, 4, 0, "接続中のリモコン数", RO));
    sc.add(b("/remote/pairing/active", false, "ペアリング受付中", RO));

    // --- methods ---
    sc.addMethod({"/session/resync", "", "nil", "スナップショットを送り直す（rev の飛びを検出したとき）", "P0"});
    sc.addMethod({"/mixer/track/add", "channels:int?, name:string?, position:int?", "int（新しいトラック番号）",
                  "トラックを追加。番号は空いている最小の番号", "P0"});
    sc.addMethod({"/mixer/track/remove", "track:int", "nil", "トラックを削除。番号は詰めない", "P0"});
    sc.addMethod({"/mixer/track/move", "track:int, position:int", "nil", "並び順を変える", "P0"});
    sc.addMethod({"/capture/record/start", "", "nil", "録画開始", "P0"});
    sc.addMethod({"/capture/record/stop", "", "nil", "録画停止", "P0"});
    sc.addMethod({"/capture/report", "state:string, file:string?, error:string?, duration_ms:int?", "nil",
                  "capd が録画の状態をエンジンに知らせる（role = サービスの接続だけ）", "P2"});
    sc.addMethod({"/video/source/add", "kind:string, name:string?", "int（新しいソース番号）",
                  "ソースを追加。番号は空いている最小の番号", "P3"});
    sc.addMethod({"/video/source/remove", "source:int", "nil", "ソースを削除。そのソースを表示していたレイヤーも消える", "P3"});
    sc.addMethod({"/video/source/report", "source:int, state:string, width:int?, height:int?, error:string?", "nil",
                  "capd がソースの状態を知らせる（role = サービスの接続だけ）", "P3"});
    sc.addMethod({"/video/scene/add", "name:string?, position:int?", "int（新しいシーン番号）", "シーンを追加", "P3"});
    sc.addMethod({"/video/scene/remove", "scene:int", "nil", "シーンを削除。参照していた program / preview は 0 に戻る", "P3"});
    sc.addMethod({"/video/scene/move", "scene:int, position:int", "nil", "シーンの並び順を変える", "P3"});
    sc.addMethod({"/video/scene/#/layer/add", "source:int, name:string?, position:int?", "int（新しいレイヤー番号）",
                  "レイヤーを追加（既定は最前面、キャンバス全体）", "P3"});
    sc.addMethod({"/video/scene/#/layer/remove", "layer:int", "nil", "レイヤーを削除", "P3"});
    sc.addMethod({"/video/scene/#/layer/move", "layer:int, position:int", "nil", "重なり順を変える（1 = 最前面）", "P3"});
    sc.addMethod({"/video/mix/add", "name:string?", "int（新しいミックス番号）", "映像ミックスを追加（最大 4）", "P3"});
    sc.addMethod({"/video/mix/remove", "mix:int", "nil", "映像ミックスを削除。参照していた出力先は 0 に戻る", "P3"});
    sc.addMethod({"/video/mix/#/take", "", "nil",
                  "プレビューのシーンをトランジションで本番に出し、直前の本番シーンをプレビューに入れる", "P3"});
    sc.addMethod({"/video/devices/refresh", "", "nil", "モニター・ウィンドウ・カメラ一覧の更新を capd に頼む（イベント video.devices.refresh）", "P3"});
    sc.addMethod({"/video/devices/report", "kind:string, <n>/<field>:値…", "nil",
                  "capd がモニター・ウィンドウ・カメラの一覧を知らせる（role = サービスの接続だけ）", "P3"});
    sc.addMethod({"/track/#/fx/add", "type:string, index:int?", "int（スロット番号）", "エフェクト追加", "P3"});
    sc.addMethod({"/track/#/fx/remove", "index:int", "nil", "エフェクト削除", "P3"});
    sc.addMethod({"/track/#/fx/move", "from:int, to:int", "nil", "エフェクトの順序変更", "P3"});
    sc.addMethod({"/master/fx/add", "type:string, index:int?", "int（スロット番号）", "エフェクト追加", "P3"});
    sc.addMethod({"/master/fx/remove", "index:int", "nil", "エフェクト削除", "P3"});
    sc.addMethod({"/devices/refresh", "", "nil", "デバイス・プロセス一覧の更新", "P2"});
    sc.addMethod({"/remote/pairing/start", "", "string（6桁コード）", "ペアリング受付開始", "P4"});
    sc.addMethod({"/remote/pairing/stop", "", "nil", "ペアリング受付終了", "P4"});

    return sc;
}

} // namespace

const Schema& schemaV1() {
    static const Schema schema = buildV1();
    return schema;
}

} // namespace dc::proto
