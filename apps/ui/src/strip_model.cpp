#include "strip_model.hpp"

#include "connection.hpp"
#include "route_choices.hpp"

#include "dc/protocol/schema.hpp"

#include <algorithm>
#include <cmath>
#include <memory>
#include <set>

namespace dc::ui {

using namespace dc::proto;

namespace {

constexpr double kFaderMinDb = -60.0;
constexpr double kFaderMaxDb = kMaxVolumeDb;
constexpr double kMeterMinDb = -60.0;
constexpr double kMeterMaxDb = 6.0;
constexpr int kSendIntervalMs = 16; // ~60 Hz per parameter (protocol.md 5.3)

bool startsWith(const std::string& s, const char* prefix) { return s.rfind(prefix, 0) == 0; }

// "/track/12/volume_db" -> 12 (0 when the path is not below a track).
int trackOfPath(const std::string& path) {
    if (!startsWith(path, "/track/")) return 0;
    int n = 0;
    for (std::size_t i = 7; i < path.size() && path[i] != '/'; ++i) {
        if (path[i] < '0' || path[i] > '9') return 0;
        n = n * 10 + (path[i] - '0');
    }
    return n;
}

double meterScale(std::int16_t cdb) {
    const double db = cdb / 100.0;
    return std::clamp((db - kMeterMinDb) / (kMeterMaxDb - kMeterMinDb), 0.0, 1.0);
}

} // namespace

StripModel::StripModel(Connection& connection, Kind kind, QObject* parent)
    : QAbstractListModel(parent), connection_(connection), kind_(kind) {
    flushTimer_.setSingleShot(true);
    flushTimer_.setTimerType(Qt::PreciseTimer);
    connect(&flushTimer_, &QTimer::timeout, this, &StripModel::flush);
    connect(&connection_, &Connection::reset, this, [this] {
        overrides_.clear();
        outgoing_.clear();
        inFlight_.clear();
        rebuild(true);
    });
    connect(&connection_, &Connection::deltaApplied, this, &StripModel::onDelta);
    connect(&connection_, &Connection::metersReceived, this, &StripModel::onMeters);
    rebuild(true);
}

// --- reading -------------------------------------------------------------------------------------

int StripModel::rowCount(const QModelIndex& parent) const { return parent.isValid() ? 0 : count(); }

bool StripModel::canAdd() const { return kind_ == Kind::Tracks && count() < kMaxTracks; }

int StripModel::maxTracks() const { return kMaxTracks; }

int StripModel::trackIdAt(int row) const { return row >= 0 && row < count() ? rows_[static_cast<std::size_t>(row)] : -1; }

int StripModel::rowOfTrack(int trackId) const {
    const auto it = std::find(rows_.begin(), rows_.end(), trackId);
    return it == rows_.end() ? -1 : static_cast<int>(it - rows_.begin());
}

std::string StripModel::base(int row) const {
    if (kind_ == Kind::Master) return "/master";
    return "/track/" + std::to_string(rows_[static_cast<std::size_t>(row)]);
}

const Value* StripModel::value(int row, const char* leaf) const {
    const std::string path = base(row) + "/" + leaf;
    if (auto it = overrides_.find(path); it != overrides_.end()) return &it->second;
    return connection_.tree().get(path);
}

double StripModel::number(int row, const char* leaf, double fallback) const {
    const Value* v = value(row, leaf);
    return v ? asNumber(*v).value_or(fallback) : fallback;
}

bool StripModel::flag(int row, const char* leaf) const {
    const Value* v = value(row, leaf);
    const bool* b = v ? std::get_if<bool>(v) : nullptr;
    return b && *b;
}

QString StripModel::text(int row, const char* leaf) const {
    const Value* v = value(row, leaf);
    const std::string* s = v ? std::get_if<std::string>(v) : nullptr;
    return s ? QString::fromStdString(*s) : QString();
}

QString StripModel::inputText(int row) const {
    const auto& tree = connection_.tree();
    // Name of the device whose id matches, from /devices/<dir>/#/id.
    auto deviceName = [&](const char* dir, const std::string& id) -> QString {
        for (int i = 1;; ++i) {
            const std::string devBase = std::string("/devices/") + dir + "/" + std::to_string(i);
            const auto devId = tree.getAs<std::string>(devBase + "/id");
            if (!devId) return QString::fromStdString(id);
            if (*devId == id) return QString::fromStdString(tree.getAs<std::string>(devBase + "/name").value_or(id));
        }
    };
    if (kind_ == Kind::Master) {
        // First used output, and how many more.
        QString first;
        int more = 0;
        for (int i = 1; i <= kMaxTrackOutputs; ++i) {
            const QString target = text(row, ("out/" + std::to_string(i) + "/target").c_str());
            if (target.isEmpty()) continue;
            if (first.isEmpty()) first = RouteChoices::describe(tree, target);
            else ++more;
        }
        if (first.isEmpty()) return QStringLiteral("出力なし");
        return more ? QStringLiteral("%1 他%2").arg(first).arg(more) : first;
    }
    const QString kind = text(row, "input/kind");
    const std::string source = text(row, "input/source").toStdString();
    if (kind == QLatin1String("device")) return deviceName("input", source);
    if (kind == QLatin1String("process")) return QStringLiteral("アプリ: %1").arg(QString::fromStdString(source));
    if (kind == QLatin1String("lan")) {
        // "<peer id>/<n>" -> "<peer name> LAN <n>"
        const auto slash = source.rfind('/');
        const std::string peerId = source.substr(0, slash);
        QString peer = QString::fromStdString(peerId);
        for (int i = 1;; ++i) {
            const std::string base = "/lan/peers/" + std::to_string(i);
            const auto id = tree.getAs<std::string>(base + "/id");
            if (!id) break;
            if (*id == peerId) peer = QString::fromStdString(tree.getAs<std::string>(base + "/name").value_or(peerId));
        }
        return QStringLiteral("LAN: %1 %2").arg(peer, slash == std::string::npos ? QString() : QString::fromStdString(source.substr(slash + 1)));
    }
    if (kind == QLatin1String("sampler"))
        return source.empty() ? QStringLiteral("サンプラー") : QStringLiteral("サンプラー B%1").arg(QString::fromStdString(source));
    return QStringLiteral("入力なし");
}

QVariant StripModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() >= count()) return {};
    const int row = index.row();
    switch (role) {
    case TrackIdRole: return rows_[static_cast<std::size_t>(row)];
    case NameRole: {
        QString n = text(row, "name");
        if (n.isEmpty()) n = kind_ == Kind::Master ? QStringLiteral("Master") : QStringLiteral("Track %1").arg(rows_[row]);
        return n;
    }
    case ChannelsRole: return kind_ == Kind::Master ? 2 : static_cast<int>(number(row, "channels", 2));
    case InputTextRole: return inputText(row);
    case InputOkRole: return kind_ == Kind::Master || text(row, "input/state") != QLatin1String("disconnected");
    case VolumeDbRole: return number(row, "volume_db");
    case FaderRole: return dbToFader(number(row, "volume_db"));
    case PanRole: return number(row, "pan");
    case MuteRole: return flag(row, "mute");
    case SoloRole: return flag(row, "solo");
    case LowcutOnRole: return flag(row, "lowcut/enabled");
    case LowcutHzRole: return number(row, "lowcut/freq_hz", 80);
    case HighcutOnRole: return flag(row, "highcut/enabled");
    case HighcutHzRole: return number(row, "highcut/freq_hz", 18000);
    case LevelLRole: return meters_[static_cast<std::size_t>(row)].level[0];
    case LevelRRole: return meters_[static_cast<std::size_t>(row)].level[1];
    case ClipRole: return meters_[static_cast<std::size_t>(row)].clip;
    case CueRole: return kind_ == Kind::Tracks && flag(row, "cue");
    case RecordRole: return flag(row, "record/enabled");
    case FxCountRole: {
        int n = 0;
        while (n < kMaxFxPerTrack && value(row, ("fx/" + std::to_string(n + 1) + "/type").c_str())) ++n;
        return n;
    }
    default: return {};
    }
}

QHash<int, QByteArray> StripModel::roleNames() const {
    return {{TrackIdRole, "trackId"},   {NameRole, "name"},           {ChannelsRole, "channels"},
            {InputTextRole, "inputText"}, {InputOkRole, "inputOk"},   {VolumeDbRole, "volumeDb"},
            {FaderRole, "fader"},       {PanRole, "pan"},             {MuteRole, "mute"},
            {SoloRole, "solo"},         {LowcutOnRole, "lowcutOn"},   {LowcutHzRole, "lowcutHz"},
            {HighcutOnRole, "highcutOn"}, {HighcutHzRole, "highcutHz"}, {LevelLRole, "levelL"},
            {LevelRRole, "levelR"},     {ClipRole, "clip"},
            {CueRole, "cue"},           {RecordRole, "recordArmed"}, {FxCountRole, "fxCount"}};
}

double StripModel::faderToDb(double fader) {
    if (fader <= 0.0) return kMinVolumeDb;
    return kFaderMinDb + std::min(fader, 1.0) * (kFaderMaxDb - kFaderMinDb);
}

double StripModel::dbToFader(double db) {
    if (db <= kFaderMinDb) return 0.0;
    return std::min((db - kFaderMinDb) / (kFaderMaxDb - kFaderMinDb), 1.0);
}

QString StripModel::dbText(double db) {
    if (db <= kFaderMinDb) return QStringLiteral("-∞");
    return QStringLiteral("%1%2").arg(db > 0.05 ? QStringLiteral("+") : QString()).arg(db, 0, 'f', 1);
}

QString StripModel::hzText(double hz) {
    if (hz >= 1000) return QStringLiteral("%1k").arg(hz / 1000.0, 0, 'f', hz >= 10000 ? 0 : 1);
    return QString::number(std::lround(hz));
}

// --- editing -------------------------------------------------------------------------------------

void StripModel::edit(int row, const char* leaf, Value v) {
    if (row < 0 || row >= count()) return;
    const std::string path = base(row) + "/" + leaf;
    const Value* now = value(row, leaf);
    if (now && *now == v) return;
    overrides_[path] = v;
    outgoing_[path] = std::move(v);
    emit dataChanged(index(row), index(row));
    if (flushTimer_.isActive()) return;
    const qint64 since = sinceFlush_.isValid() ? sinceFlush_.elapsed() : kSendIntervalMs;
    flushTimer_.start(static_cast<int>(std::max<qint64>(0, kSendIntervalMs - since)));
}

void StripModel::flush() {
    sinceFlush_.start();
    auto outgoing = std::move(outgoing_);
    outgoing_.clear();
    for (auto& [path, v] : outgoing) {
        // One Set per parameter: a rejected value must not take the others with it.
        const std::uint64_t id = connection_.set({{path, v}}, [this, path](const Result& r) {
            auto it = inFlight_.find(path);
            if (it == inFlight_.end() || it->second != r.id) return; // a newer value is on its way
            inFlight_.erase(it);
            if (!outgoing_.count(path)) overrides_.erase(path);
            if (r.code != ErrorCode::Ok) emit failed(Connection::describe(r));
            emitPathChanged(path);
        });
        inFlight_[path] = id;
    }
}

void StripModel::editTogether(int row, std::vector<std::pair<const char*, Value>> leaves) {
    if (row < 0 || row >= count()) return;
    std::vector<Change> changes;
    for (auto& [leaf, v] : leaves) {
        const std::string path = base(row) + "/" + leaf;
        overrides_[path] = v;
        outgoing_.erase(path); // this Set carries the newest value
        changes.push_back({path, std::move(v)});
    }
    emit dataChanged(index(row), index(row));
    auto paths = std::make_shared<std::vector<std::string>>();
    for (const auto& c : changes) paths->push_back(c.path);
    const std::uint64_t id = connection_.set(std::move(changes), [this, paths](const Result& r) {
        for (const auto& path : *paths) {
            auto it = inFlight_.find(path);
            if (it == inFlight_.end() || it->second != r.id) continue;
            inFlight_.erase(it);
            if (!outgoing_.count(path)) overrides_.erase(path);
            emitPathChanged(path);
        }
        if (r.code != ErrorCode::Ok) emit failed(Connection::describe(r));
    });
    for (const auto& path : *paths) inFlight_[path] = id;
}

void StripModel::setFader(int row, double fader) { setVolumeDb(row, faderToDb(fader)); }

void StripModel::setVolumeDb(int row, double db) {
    edit(row, "volume_db", std::clamp(db, kMinVolumeDb, kMaxVolumeDb));
}

void StripModel::setPan(int row, double pan) {
    // Snap to the centre so that it is easy to hit with a finger.
    pan = std::clamp(pan, -1.0, 1.0);
    if (std::abs(pan) < 0.02) pan = 0.0;
    edit(row, "pan", pan);
}

void StripModel::toggleMute(int row) { edit(row, "mute", !flag(row, "mute")); }

void StripModel::toggleSolo(int row) {
    if (kind_ == Kind::Master) return;
    edit(row, "solo", !flag(row, "solo"));
}

void StripModel::toggleCue(int row) {
    if (kind_ == Kind::Master) return;
    edit(row, "cue", !flag(row, "cue"));
}

void StripModel::toggleRecord(int row) { edit(row, "record/enabled", !flag(row, "record/enabled")); }

void StripModel::toggleLowcut(int row) { edit(row, "lowcut/enabled", !flag(row, "lowcut/enabled")); }

void StripModel::toggleHighcut(int row) { edit(row, "highcut/enabled", !flag(row, "highcut/enabled")); }

void StripModel::setLowcutHz(int row, double hz) { edit(row, "lowcut/freq_hz", std::round(std::clamp(hz, 20.0, 2000.0))); }

void StripModel::setHighcutHz(int row, double hz) {
    edit(row, "highcut/freq_hz", std::round(std::clamp(hz, 1000.0, 20000.0) / 10.0) * 10.0);
}

void StripModel::clearClip(int row) {
    if (row < 0 || row >= count()) return;
    meters_[static_cast<std::size_t>(row)].clip = false;
    emit dataChanged(index(row), index(row), {ClipRole});
}

QVariantList StripModel::inputChoices(int row) const {
    QVariantList out;
    if (kind_ != Kind::Tracks || row < 0 || row >= count()) return out;
    const auto& tree = connection_.tree();
    const QString curKind = text(row, "input/kind");
    const QString curSource = text(row, "input/source");
    const int curChannel = static_cast<int>(number(row, "input/channel", 1));
    const int trackChannels = static_cast<int>(number(row, "channels", 2));
    bool currentListed = curKind == QLatin1String("none") || curKind.isEmpty();
    auto entry = [&](const QString& label, const QString& detail, const QString& kind, const QString& source,
                     int channel) {
        bool selected = curKind == kind;
        if (selected && kind == QLatin1String("process")) selected = curSource.compare(source, Qt::CaseInsensitive) == 0;
        else if (selected && kind == QLatin1String("device")) selected = curSource == source && curChannel == channel;
        else if (selected && kind != QLatin1String("none")) selected = curSource == source;
        if (selected && kind != QLatin1String("none")) currentListed = true;
        out.push_back(QVariantMap{{"label", label},   {"detail", detail},   {"kind", kind},
                                  {"source", source}, {"channel", channel}, {"selected", selected}});
    };
    auto header = [&](const QString& t) { out.push_back(QVariantMap{{"header", t}}); };

    entry(QStringLiteral("入力なし"), QString(), QStringLiteral("none"), QString(), 1);

    // Devices: multi-channel ones per channel (mono track) or per pair (stereo track).
    bool first = true;
    for (int i = 1;; ++i) {
        const std::string devBase = "/devices/input/" + std::to_string(i);
        const auto id = tree.getAs<std::string>(devBase + "/id");
        if (!id) break;
        if (first) header(QStringLiteral("入力デバイス"));
        first = false;
        const QString source = QString::fromStdString(*id);
        const QString name = QString::fromStdString(tree.getAs<std::string>(devBase + "/name").value_or(*id));
        const int channels = static_cast<int>(tree.getAs<std::int64_t>(devBase + "/channels").value_or(2));
        QStringList notes;
        const std::string api = tree.getAs<std::string>(devBase + "/api").value_or("");
        if (api == "asio") notes << QStringLiteral("ASIO");
        if (api == "vmic") notes << QStringLiteral("仮想ケーブル");
        if (!tree.getAs<bool>(devBase + "/connected").value_or(true)) notes << QStringLiteral("切断中");
        if (channels <= trackChannels) {
            entry(name, notes.join(QStringLiteral(" · ")), QStringLiteral("device"), source, 1);
            continue;
        }
        for (int ch = 1; ch + trackChannels - 1 <= channels; ch += trackChannels) {
            QStringList d = notes;
            d.prepend(trackChannels == 1 ? QStringLiteral("ch %1").arg(ch) : QStringLiteral("ch %1-%2").arg(ch).arg(ch + 1));
            entry(name, d.join(QStringLiteral(" · ")), QStringLiteral("device"), source, ch);
        }
    }

    // Applications that play sound (one line per executable).
    first = true;
    for (int i = 1;; ++i) {
        const std::string base = "/devices/process/" + std::to_string(i);
        const auto exe = tree.getAs<std::string>(base + "/exe");
        if (!exe) break;
        if (first) header(QStringLiteral("アプリの音"));
        first = false;
        const QString title = QString::fromStdString(tree.getAs<std::string>(base + "/title").value_or(""));
        entry(title.isEmpty() ? QString::fromStdString(*exe) : title, QString::fromStdString(*exe),
              QStringLiteral("process"), QString::fromStdString(*exe), 1);
    }
    // A chosen application that is not running stays visible.
    if (curKind == QLatin1String("process") && !currentListed) {
        if (first) header(QStringLiteral("アプリの音"));
        first = false;
        entry(curSource, QStringLiteral("起動していません"), QStringLiteral("process"), curSource, 1);
    }

    // Streams of the other PCs on the LAN (known while connected to them).
    first = true;
    for (int i = 1;; ++i) {
        const std::string base = "/lan/peers/" + std::to_string(i);
        const auto peerId = tree.getAs<std::string>(base + "/id");
        if (!peerId) break;
        const QString peer = QString::fromStdString(tree.getAs<std::string>(base + "/name").value_or(*peerId));
        for (int n = 1; n <= kMaxLanStreams; ++n) {
            const std::string sb = base + "/stream/" + std::to_string(n);
            const auto name = tree.getAs<std::string>(sb + "/name");
            if (!name) continue;
            if (first) header(QStringLiteral("LAN（別の PC）"));
            first = false;
            const bool active = tree.getAs<bool>(sb + "/active").value_or(false);
            entry(QStringLiteral("%1 · %2").arg(peer, name->empty() ? QStringLiteral("LAN %1").arg(n) : QString::fromStdString(*name)),
                  active ? QStringLiteral("送信中") : QStringLiteral("音なし"), QStringLiteral("lan"),
                  QStringLiteral("%1/%2").arg(QString::fromStdString(*peerId)).arg(n), 1);
        }
    }
    if (curKind == QLatin1String("lan") && !currentListed) {
        if (first) header(QStringLiteral("LAN（別の PC）"));
        entry(QStringLiteral("LAN %1").arg(curSource), QStringLiteral("相手が見つかりません"), QStringLiteral("lan"), curSource, 1);
    }

    header(QStringLiteral("サンプラー"));
    entry(QStringLiteral("サンプラー（全バンク）"), QString(), QStringLiteral("sampler"), QString(), 1);
    for (int b = 1; b <= kSamplerBanks; ++b)
        entry(QStringLiteral("サンプラー バンク %1").arg(b), QString(), QStringLiteral("sampler"), QString::number(b), 1);
    return out;
}

QVariantList StripModel::outputChoices() const {
    QVariantList out;
    if (kind_ != Kind::Master || count() == 0) return out;
    const auto& tree = connection_.tree();
    const QString current = text(0, "out/1/target");
    auto entry = [&](const QString& label, const QString& detail, const QString& target) {
        out.push_back(QVariantMap{{"label", label}, {"detail", detail}, {"target", target}, {"selected", current == target}});
    };
    entry(QStringLiteral("出力なし"), QString(), QString());
    for (int i = 1;; ++i) {
        const std::string devBase = "/devices/output/" + std::to_string(i);
        const auto id = tree.getAs<std::string>(devBase + "/id");
        if (!id) break;
        const bool vmic = tree.getAs<std::string>(devBase + "/api").value_or("") == "vmic";
        const QString target = QString::fromStdString((vmic ? "vmic/" : "device/") + *id);
        QString detail = vmic ? QStringLiteral("仮想マイク") : QString();
        if (!tree.getAs<bool>(devBase + "/connected").value_or(true))
            detail = detail.isEmpty() ? QStringLiteral("切断中") : detail + QStringLiteral(" · 切断中");
        entry(QString::fromStdString(tree.getAs<std::string>(devBase + "/name").value_or(*id)), detail, target);
    }
    return out;
}

void StripModel::setInput(int row, const QString& kind, const QString& source, int channel) {
    if (kind_ != Kind::Tracks) return;
    editTogether(row, {{"input/kind", kind.toStdString()},
                       {"input/source", kind == QLatin1String("none") ? std::string() : source.toStdString()},
                       {"input/channel", std::int64_t{std::max(1, channel)}}});
}

void StripModel::setOutput(const QString& target) {
    if (kind_ != Kind::Master) return;
    edit(0, "out/1/target", target.toStdString());
}

void StripModel::sendCall(std::string method, Args args, std::function<void(const Result&)> then) {
    connection_.call(std::move(method), std::move(args), [this, then = std::move(then)](const Result& r) {
        if (r.code != ErrorCode::Ok) {
            emit failed(Connection::describe(r));
            return;
        }
        if (then) then(r);
    });
}

void StripModel::addTrack(int channels) {
    if (kind_ != Kind::Tracks) return;
    if (!canAdd()) {
        emit failed(QStringLiteral("トラックは最大 %1 本です").arg(kMaxTracks));
        return;
    }
    sendCall("/mixer/track/add", {{"channels", std::int64_t{channels == 1 ? 1 : 2}}}, [this](const Result& r) {
        if (auto* id = std::get_if<std::int64_t>(&r.value))
            if (const int row = rowOfTrack(static_cast<int>(*id)); row >= 0) emit trackAdded(row);
    });
}

void StripModel::removeTrack(int row) {
    if (kind_ != Kind::Tracks || row < 0 || row >= count()) return;
    sendCall("/mixer/track/remove", {{"track", std::int64_t{rows_[static_cast<std::size_t>(row)]}}});
}

void StripModel::moveTrack(int row, int offset) {
    if (kind_ != Kind::Tracks || row < 0 || row >= count()) return;
    const int to = std::clamp(row + offset, 0, count() - 1);
    if (to == row) return;
    sendCall("/mixer/track/move",
             {{"track", std::int64_t{rows_[static_cast<std::size_t>(row)]}}, {"position", std::int64_t{to + 1}}});
}

// --- following the tree --------------------------------------------------------------------------

std::vector<int> StripModel::orderFromTree() const {
    const auto& tree = connection_.tree();
    if (kind_ == Kind::Master) return tree.contains("/master/volume_db") ? std::vector<int>{0} : std::vector<int>{};
    std::vector<std::pair<std::int64_t, int>> v;
    for (int n = 1; n <= kMaxTracks; ++n)
        if (auto pos = tree.getAs<std::int64_t>("/track/" + std::to_string(n) + "/position")) v.push_back({*pos, n});
    std::sort(v.begin(), v.end());
    std::vector<int> out;
    out.reserve(v.size());
    for (auto& [pos, n] : v) out.push_back(n);
    return out;
}

void StripModel::rebuild(bool resetAll) {
    const std::vector<int> next = orderFromTree();
    const int before = count();
    if (resetAll) {
        beginResetModel();
        rows_ = next;
        meters_.assign(rows_.size(), {});
        endResetModel();
    } else if (next != rows_) {
        // Removals, then inserts and moves in the new order, so that the view keeps its delegates.
        for (int i = count() - 1; i >= 0; --i) {
            if (std::find(next.begin(), next.end(), rows_[i]) != next.end()) continue;
            beginRemoveRows({}, i, i);
            rows_.erase(rows_.begin() + i);
            meters_.erase(meters_.begin() + i);
            endRemoveRows();
        }
        for (int i = 0; i < static_cast<int>(next.size()); ++i) {
            if (i < count() && rows_[i] == next[i]) continue;
            const auto it = std::find(rows_.begin() + i, rows_.end(), next[i]);
            if (it == rows_.end()) {
                beginInsertRows({}, i, i);
                rows_.insert(rows_.begin() + i, next[i]);
                meters_.insert(meters_.begin() + i, MeterState{});
                endInsertRows();
            } else {
                const int from = static_cast<int>(it - rows_.begin());
                beginMoveRows({}, from, from, {}, i);
                const MeterState m = meters_[from];
                rows_.erase(rows_.begin() + from);
                meters_.erase(meters_.begin() + from);
                rows_.insert(rows_.begin() + i, next[i]);
                meters_.insert(meters_.begin() + i, m);
                endMoveRows();
            }
        }
    }
    rowOfMeter_.clear();
    for (int i = 0; i < count(); ++i) {
        const int meterId = kind_ == Kind::Master ? 0 : static_cast<int>(number(i, "meter_id", rows_[i]));
        rowOfMeter_[meterId] = i;
    }
    // Pending edits of tracks that are gone.
    for (auto* m : {&overrides_, &outgoing_}) {
        for (auto it = m->begin(); it != m->end();) {
            const int track = trackOfPath(it->first);
            it = track && rowOfTrack(track) < 0 ? m->erase(it) : std::next(it);
        }
    }
    if (count() != before) emit countChanged();
}

void StripModel::onDelta(const Delta& d) {
    bool structural = false;
    for (const auto& r : d.removed) structural |= startsWith(r, kind_ == Kind::Master ? "/master" : "/track/");
    for (const auto& c : d.changes) {
        if (kind_ == Kind::Tracks)
            structural |= (trackOfPath(c.path) && c.path.size() > 9 &&
                           c.path.compare(c.path.size() - 9, 9, "/position") == 0);
        else
            structural |= rows_.empty() && startsWith(c.path, "/master/");
    }
    if (structural) rebuild(false);

    std::set<int> rows;
    bool all = false;
    for (const auto& c : d.changes) {
        if (startsWith(c.path, "/devices/")) {
            all = true;
            break;
        }
        if (kind_ == Kind::Master) {
            if (startsWith(c.path, "/master/") && count() > 0) rows.insert(0);
        } else if (const int track = trackOfPath(c.path)) {
            if (const int row = rowOfTrack(track); row >= 0) rows.insert(row);
        }
    }
    if (all) {
        if (count() > 0) emit dataChanged(index(0), index(count() - 1));
        return;
    }
    for (int row : rows) emit dataChanged(index(row), index(row));
}

void StripModel::emitPathChanged(const std::string& path) {
    int row = -1;
    if (kind_ == Kind::Master) row = count() > 0 && startsWith(path, "/master/") ? 0 : -1;
    else row = rowOfTrack(trackOfPath(path));
    if (row >= 0) emit dataChanged(index(row), index(row));
}

void StripModel::onMeters(const MeterPacket& p) {
    if (rows_.empty()) return;
    ++meterPackets_;
    bool any = false;
    for (const auto& e : p.entries) {
        const auto it = rowOfMeter_.find(e.meterId);
        if (it == rowOfMeter_.end()) continue;
        MeterState& m = meters_[static_cast<std::size_t>(it->second)];
        const int channels = std::clamp<int>(e.channels, 1, 2);
        for (int ch = 0; ch < 2; ++ch) m.level[ch] = meterScale(e.peakCdb[std::min(ch, channels - 1)]);
        if ((e.flags & kMeterFlagClip) || std::max(e.peakCdb[0], e.peakCdb[1]) >= 0) m.clip = true;
        any = true;
    }
    if (any) emit dataChanged(index(0), index(count() - 1), {LevelLRole, LevelRRole, ClipRole});
}

} // namespace dc::ui
