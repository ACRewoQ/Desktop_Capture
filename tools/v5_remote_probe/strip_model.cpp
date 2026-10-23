#include "strip_model.hpp"

#include "dc/control/client.hpp"
#include "dc/protocol/meter.hpp"
#include "dc/protocol/schema.hpp"

#include <QNetworkDatagram>
#include <QUdpSocket>

#include <algorithm>
#include <cmath>
#include <map>

namespace {

constexpr double kFaderMinDb = -60.0;
constexpr double kFaderMaxDb = dc::proto::kMaxVolumeDb;
const char* const kClientName = "dc-v5-probe";

QVariant toVariant(const dc::proto::Value& v) {
    if (auto* b = std::get_if<bool>(&v)) return *b;
    if (auto* i = std::get_if<std::int64_t>(&v)) return static_cast<qlonglong>(*i);
    if (auto* d = std::get_if<double>(&v)) return *d;
    if (auto* s = std::get_if<std::string>(&v)) return QString::fromStdString(*s);
    return {};
}

// "/track/12/volume_db" -> 12 and "volume_db"; 0 for anything else.
int splitTrackPath(const QString& path, QString* leaf) {
    const auto parts = path.split('/', Qt::SkipEmptyParts);
    if (parts.size() != 3 || parts[0] != QLatin1String("track")) return 0;
    *leaf = parts[2];
    return parts[1].toInt();
}

} // namespace

StripModel::StripModel(QObject* parent) : QAbstractListModel(parent) {}

StripModel::~StripModel() {
    if (client_) client_->disconnect();
}

void StripModel::simulate(int n) {
    beginResetModel();
    strips_.clear();
    for (int i = 0; i < n; ++i) {
        Strip s;
        s.trackId = i + 1;
        s.meterId = i + 1;
        s.name = QStringLiteral("Track %1").arg(i + 1);
        strips_.push_back(s);
    }
    endResetModel();
    emit countChanged();
    connect(&meterTimer_, &QTimer::timeout, this, &StripModel::simulateMeters);
    // Precise: a coarse 33 ms timer lands on the 15.6 ms Windows tick and gives ~21 Hz.
    meterTimer_.setTimerType(Qt::PreciseTimer);
    meterTimer_.start(33); // same rate as the engine's UDP meters
}

bool StripModel::connectTo(const QString& host, quint16 port, int n, QString* error) {
    meterSocket_ = new QUdpSocket(this);
    if (!meterSocket_->bind(QHostAddress::AnyIPv4, 0)) {
        *error = QStringLiteral("cannot open the UDP meter socket");
        return false;
    }
    connect(meterSocket_, &QUdpSocket::readyRead, this, &StripModel::readMeters);

    dc::control::ControlClient::Options opt;
    opt.host = host.toStdString();
    opt.port = port;
    opt.clientName = kClientName;
    opt.role = dc::proto::ClientRole::Remote;
    opt.meterPort = meterSocket_->localPort();
    client_ = std::make_unique<dc::control::ControlClient>(opt);
    // Network thread -> GUI thread.
    client_->onDelta = [this](const dc::proto::Delta& d) {
        if (d.originClient == kClientName) return; // our own fader moves come back as echoes
        for (const auto& c : d.changes) {
            const QString path = QString::fromStdString(c.path);
            const QVariant value = toVariant(c.value);
            QMetaObject::invokeMethod(this, [this, path, value] { applyRemote(path, value); }, Qt::QueuedConnection);
        }
    };
    std::string err;
    if (!client_->connect(std::chrono::seconds(5), &err)) {
        *error = QString::fromStdString(err);
        client_.reset();
        return false;
    }

    // Grow to n tracks like a user pressing "+" (the mock server starts with 4).
    auto trackCount = [this] {
        int count = 0;
        const auto tree = client_->tree();
        for (const auto& [path, v] : tree.values()) {
            QString leaf;
            if (splitTrackPath(QString::fromStdString(path), &leaf) && leaf == QLatin1String("name")) ++count;
        }
        return count;
    };
    for (int have = trackCount(); have < n; ++have) {
        const auto r = client_->call("/mixer/track/add").get();
        if (r.code != dc::proto::ErrorCode::Ok) break;
    }
    // The Call result can arrive before its Delta; wait for the mirror to settle.
    client_->waitForRev(client_->tree().rev() + 1, std::chrono::milliseconds(300));
    loadFromTree();
    emit connectedChanged();
    return true;
}

void StripModel::loadFromTree() {
    const auto tree = client_->tree();
    std::map<std::pair<std::int64_t, int>, Strip> ordered; // (position, id) -> strip
    for (const auto& [path, v] : tree.values()) {
        QString leaf;
        const int id = splitTrackPath(QString::fromStdString(path), &leaf);
        if (!id || leaf != QLatin1String("name")) continue;
        const std::string base = "/track/" + std::to_string(id);
        Strip s;
        s.trackId = id;
        s.name = QString::fromStdString(tree.getAs<std::string>(base + "/name").value_or(""));
        if (s.name.isEmpty()) s.name = QStringLiteral("Track %1").arg(id);
        s.volumeDb = tree.getAs<double>(base + "/volume_db").value_or(0.0);
        s.mute = tree.getAs<bool>(base + "/mute").value_or(false);
        s.meterId = static_cast<int>(tree.getAs<std::int64_t>(base + "/meter_id").value_or(0));
        ordered[{tree.getAs<std::int64_t>(base + "/position").value_or(id), id}] = s;
    }
    beginResetModel();
    strips_.clear();
    for (auto& [key, s] : ordered) strips_.push_back(s);
    endResetModel();
    emit countChanged();
}

void StripModel::applyRemote(const QString& path, const QVariant& value) {
    QString leaf;
    const int row = rowOfTrack(splitTrackPath(path, &leaf));
    if (row < 0) return;
    Strip& s = strips_[row];
    QList<int> roles;
    if (leaf == QLatin1String("volume_db")) {
        s.volumeDb = value.toDouble();
        roles = {VolumeDbRole, PositionRole};
    } else if (leaf == QLatin1String("mute")) {
        s.mute = value.toBool();
        roles = {MuteRole};
    } else if (leaf == QLatin1String("name")) {
        s.name = value.toString();
        roles = {NameRole};
    } else {
        return;
    }
    emit dataChanged(index(row), index(row), roles);
}

void StripModel::readMeters() {
    std::map<int, double> levels;
    while (meterSocket_->hasPendingDatagrams()) {
        const QNetworkDatagram dg = meterSocket_->receiveDatagram();
        const QByteArray bytes = dg.data();
        const auto p = dc::proto::decodeMeterPacket(
            {reinterpret_cast<const std::uint8_t*>(bytes.constData()), static_cast<std::size_t>(bytes.size())});
        if (!p) continue;
        for (const auto& e : p->entries) {
            const double db = std::max(e.peakCdb[0], e.peakCdb[1]) / 100.0;
            levels[e.meterId] = std::clamp((db - kFaderMinDb) / -kFaderMinDb, 0.0, 1.0);
        }
    }
    if (levels.empty() || strips_.empty()) return;
    for (auto& s : strips_)
        if (auto it = levels.find(s.meterId); it != levels.end()) s.level = it->second;
    emit dataChanged(index(0), index(count() - 1), {LevelRole});
}

void StripModel::simulateMeters() {
    if (strips_.empty()) return;
    phase_ += 0.033;
    for (std::size_t i = 0; i < strips_.size(); ++i) {
        const double x = phase_ * (1.0 + 0.13 * static_cast<double>(i));
        strips_[i].level = strips_[i].mute ? 0.0 : 0.45 + 0.35 * std::sin(x) * std::sin(x * 2.7 + 1.0);
    }
    emit dataChanged(index(0), index(count() - 1), {LevelRole});
}

int StripModel::rowOfTrack(int trackId) const {
    for (std::size_t i = 0; i < strips_.size(); ++i)
        if (strips_[i].trackId == trackId) return static_cast<int>(i);
    return -1;
}

int StripModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : count();
}

QVariant StripModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() >= count()) return {};
    const Strip& s = strips_[static_cast<std::size_t>(index.row())];
    switch (role) {
    case TrackIdRole: return s.trackId;
    case NameRole: return s.name;
    case VolumeDbRole: return s.volumeDb;
    case PositionRole: return dbToPosition(s.volumeDb);
    case LevelRole: return s.level;
    case MuteRole: return s.mute;
    default: return {};
    }
}

QHash<int, QByteArray> StripModel::roleNames() const {
    return {{TrackIdRole, "trackId"}, {NameRole, "name"},   {VolumeDbRole, "volumeDb"},
            {PositionRole, "position"}, {LevelRole, "level"}, {MuteRole, "mute"}};
}

double StripModel::volumeDb(int row) const {
    return row >= 0 && row < count() ? strips_[static_cast<std::size_t>(row)].volumeDb : 0.0;
}

double StripModel::positionToDb(double position) {
    if (position <= 0.0) return dc::proto::kMinVolumeDb;
    return kFaderMinDb + std::min(position, 1.0) * (kFaderMaxDb - kFaderMinDb);
}

double StripModel::dbToPosition(double db) {
    if (db <= kFaderMinDb) return 0.0;
    return std::min((db - kFaderMinDb) / (kFaderMaxDb - kFaderMinDb), 1.0);
}

void StripModel::setPosition(int row, double position) {
    setVolumeDb(row, positionToDb(position));
}

void StripModel::setVolumeDb(int row, double db) {
    if (row < 0 || row >= count()) return;
    Strip& s = strips_[static_cast<std::size_t>(row)];
    db = std::clamp(db, dc::proto::kMinVolumeDb, kFaderMaxDb);
    if (s.volumeDb == db) return;
    s.volumeDb = db;
    emit dataChanged(index(row), index(row), {VolumeDbRole, PositionRole});
    if (client_) (void)client_->set({{"/track/" + std::to_string(s.trackId) + "/volume_db", db}});
}

void StripModel::toggleMute(int row) {
    if (row < 0 || row >= count()) return;
    Strip& s = strips_[static_cast<std::size_t>(row)];
    s.mute = !s.mute;
    emit dataChanged(index(row), index(row), {MuteRole});
    if (client_) (void)client_->set({{"/track/" + std::to_string(s.trackId) + "/mute", s.mute}});
}
