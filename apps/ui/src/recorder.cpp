#include "recorder.hpp"

#include "connection.hpp"

namespace dc::ui {

using namespace dc::proto;

namespace {

QString str(const Connection& c, const char* path) {
    return QString::fromStdString(c.tree().getAs<std::string>(path).value_or(""));
}

qint64 num(const Connection& c, const char* path) {
    return static_cast<qint64>(c.tree().getAs<std::int64_t>(path).value_or(0));
}

} // namespace

Recorder::Recorder(Connection& connection, QObject* parent) : QObject(parent), connection_(connection) {
    connect(&connection_, &Connection::reset, this, &Recorder::changed);
    connect(&connection_, &Connection::stateChanged, this, &Recorder::changed);
    connect(&connection_, &Connection::deltaApplied, this, [this](const Delta& d) {
        for (const auto& c : d.changes)
            if (c.path.rfind("/capture/", 0) == 0) {
                emit changed();
                return;
            }
    });
}

QString Recorder::state() const {
    const QString s = str(connection_, "/capture/state");
    return s.isEmpty() ? QStringLiteral("idle") : s;
}

bool Recorder::busy() const {
    const QString s = state();
    return requesting_ || s == QLatin1String("starting") || s == QLatin1String("stopping");
}

QString Recorder::elapsedText() const {
    const qint64 sec = num(connection_, "/capture/elapsed_ms") / 1000;
    return QStringLiteral("%1:%2:%3")
        .arg(sec / 3600, 2, 10, QLatin1Char('0'))
        .arg(sec / 60 % 60, 2, 10, QLatin1Char('0'))
        .arg(sec % 60, 2, 10, QLatin1Char('0'));
}

QString Recorder::file() const { return str(connection_, "/capture/file"); }

QString Recorder::error() const { return str(connection_, "/capture/error"); }

QString Recorder::summary() const {
    const QString codec = str(connection_, "/capture/video/codec");
    const QString codecText = codec == QLatin1String("h264")   ? QStringLiteral("H.264")
                              : codec == QLatin1String("hevc") ? QStringLiteral("HEVC")
                                                               : codec.toUpper();
    QString encoder = str(connection_, "/capture/video/encoder");
    encoder = encoder == QLatin1String("auto") ? QStringLiteral("自動") : encoder.toUpper();
    return QStringLiteral("%1 · %2 %3×%4 %5fps · %6")
        .arg(str(connection_, "/capture/container").toUpper(), codecText)
        .arg(num(connection_, "/capture/video/width"))
        .arg(num(connection_, "/capture/video/height"))
        .arg(num(connection_, "/capture/video/fps"))
        .arg(encoder);
}

void Recorder::toggle() {
    if (busy()) return;
    if (recording()) stop();
    else start();
}

void Recorder::start() { request("/capture/record/start"); }

void Recorder::stop() { request("/capture/record/stop"); }

void Recorder::request(const char* method) {
    requesting_ = true;
    emit changed();
    connection_.call(method, {}, [this](const Result& r) {
        requesting_ = false;
        if (r.code != ErrorCode::Ok) emit failed(Connection::describe(r));
        emit changed();
    });
}

} // namespace dc::ui
