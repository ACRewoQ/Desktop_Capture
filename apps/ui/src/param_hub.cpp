#include "param_hub.hpp"

#include "connection.hpp"

#include "dc/protocol/fx.hpp"
#include "dc/protocol/schema.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>

namespace dc::ui {

using namespace dc::proto;

namespace {

constexpr int kSendIntervalMs = 16; // ~60 Hz per parameter (protocol.md 5.3)

ParamHub* g_hub = nullptr;

bool startsWith(const std::string& s, const std::string& prefix) { return s.compare(0, prefix.size(), prefix) == 0; }

// "/a/b" covers "/a/b" and "/a/b/c", not "/a/bc".
bool covers(const std::string& prefix, const std::string& path) {
    return startsWith(path, prefix) && (path.size() == prefix.size() || path[prefix.size()] == '/');
}

// Number at the start of s ("12/x" -> 12), 0 when s does not start with one.
int leadingNumber(const std::string& s, std::size_t from = 0) {
    int n = 0;
    std::size_t i = from;
    for (; i < s.size() && s[i] >= '0' && s[i] <= '9'; ++i) n = n * 10 + (s[i] - '0');
    return i == from || n > 1000000 ? 0 : n;
}

const ParamSpec* specOf(const std::string& path) { return path.empty() ? nullptr : schemaV1().find(path); }

const char* typeText(ValueType t) {
    switch (t) {
    case ValueType::Bool: return "bool";
    case ValueType::Int: return "int";
    case ValueType::Float: return "float";
    case ValueType::String: return "string";
    default: return "nil";
    }
}

// A JS number with nothing after the point travels as an int (method arguments have no schema).
Value guessValue(const QVariant& v) {
    switch (v.typeId()) {
    case QMetaType::Bool: return v.toBool();
    case QMetaType::Int:
    case QMetaType::LongLong:
    case QMetaType::UInt:
    case QMetaType::ULongLong: return std::int64_t{v.toLongLong()};
    case QMetaType::Double:
    case QMetaType::Float: {
        const double d = v.toDouble();
        if (std::isfinite(d) && d == std::floor(d) && std::abs(d) < 9e15) return static_cast<std::int64_t>(d);
        return d;
    }
    case QMetaType::QString: return v.toString().toStdString();
    default:
        if (!v.isValid() || v.isNull()) return std::monostate{};
        return v.toString().toStdString();
    }
}

} // namespace

ParamHub::ParamHub(Connection& connection, QObject* parent) : QObject(parent), connection_(connection) {
    g_hub = this;
    flushTimer_.setSingleShot(true);
    flushTimer_.setTimerType(Qt::PreciseTimer);
    connect(&flushTimer_, &QTimer::timeout, this, &ParamHub::flush);
    connect(&connection_, &Connection::reset, this, [this] {
        overrides_.clear();
        outgoing_.clear();
        inFlight_.clear();
        notifyAll();
    });
    connect(&connection_, &Connection::deltaApplied, this, &ParamHub::onDelta);
}

ParamHub::~ParamHub() {
    if (g_hub == this) g_hub = nullptr;
}

ParamHub* ParamHub::instance() { return g_hub; }

// --- values --------------------------------------------------------------------------------------

QVariant ParamHub::toVariant(const Value& v) {
    if (auto* b = std::get_if<bool>(&v)) return *b;
    if (auto* i = std::get_if<std::int64_t>(&v)) {
        // QML numbers: plain int where it fits, so that === comparisons with literals work.
        if (*i >= INT32_MIN && *i <= INT32_MAX) return static_cast<int>(*i);
        return static_cast<double>(*i);
    }
    if (auto* d = std::get_if<double>(&v)) return *d;
    if (auto* s = std::get_if<std::string>(&v)) return QString::fromStdString(*s);
    return {};
}

Value ParamHub::fromVariant(const std::string& path, const QVariant& v) {
    const ParamSpec* spec = specOf(path);
    if (!spec) return guessValue(v);
    switch (spec->type) {
    case ValueType::Bool: return v.toBool();
    case ValueType::Int: return static_cast<std::int64_t>(std::llround(v.toDouble()));
    case ValueType::Float: return v.toDouble();
    case ValueType::String: return v.toString().toStdString();
    default: return guessValue(v);
    }
}

const Value* ParamHub::raw(const std::string& path) const {
    if (auto it = overrides_.find(path); it != overrides_.end()) return &it->second;
    return connection_.tree().get(path);
}

QVariant ParamHub::value(const QString& path) const {
    const Value* v = raw(path.toStdString());
    return v ? toVariant(*v) : QVariant();
}

bool ParamHub::exists(const QString& path) const { return raw(path.toStdString()) != nullptr; }

QList<int> ParamHub::indices(const QString& prefix) const {
    const std::string key = prefix.toStdString() + "/";
    QList<int> out;
    const auto& values = connection_.tree().values();
    for (auto it = values.lower_bound(key); it != values.end() && startsWith(it->first, key);) {
        const int n = leadingNumber(it->first, key.size());
        if (n <= 0) {
            ++it;
            continue;
        }
        out.push_back(n);
        it = values.lower_bound(key + std::to_string(n) + "0"); // past "<key>n/..." ('/' sorts before '0')
    }
    std::sort(out.begin(), out.end());
    return out;
}

QVariantList ParamHub::builtinEffects() const {
    QVariantList out;
    for (const auto& fx : builtinFx())
        out.push_back(QVariantMap{{"type", QString::fromStdString(fx.type)}, {"name", QString::fromStdString(fx.name)}});
    return out;
}

// --- editing -------------------------------------------------------------------------------------

void ParamHub::set(const QString& qpath, const QVariant& value) {
    const std::string path = qpath.toStdString();
    Value v = fromVariant(path, value);
    if (const Value* now = raw(path); now && *now == v) return;
    overrides_[path] = v;
    outgoing_[path] = std::move(v);
    notify(path);
    if (flushTimer_.isActive()) return;
    const qint64 since = sinceFlush_.isValid() ? sinceFlush_.elapsed() : kSendIntervalMs;
    flushTimer_.start(static_cast<int>(std::max<qint64>(0, kSendIntervalMs - since)));
}

void ParamHub::flush() {
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
            notify(path);
        });
        inFlight_[path] = id;
    }
}

void ParamHub::setMany(const QVariantMap& changes) {
    std::vector<Change> list;
    auto paths = std::make_shared<std::vector<std::string>>();
    for (auto it = changes.begin(); it != changes.end(); ++it) {
        const std::string path = it.key().toStdString();
        Value v = fromVariant(path, it.value());
        overrides_[path] = v;
        outgoing_.erase(path); // this Set carries the newest value
        list.push_back({path, std::move(v)});
        paths->push_back(path);
    }
    if (list.empty()) return;
    for (const auto& p : *paths) notify(p);
    const std::uint64_t id = connection_.set(std::move(list), [this, paths](const Result& r) {
        for (const auto& path : *paths) {
            auto it = inFlight_.find(path);
            if (it == inFlight_.end() || it->second != r.id) continue;
            inFlight_.erase(it);
            if (!outgoing_.count(path)) overrides_.erase(path);
            notify(path);
        }
        if (r.code != ErrorCode::Ok) emit failed(Connection::describe(r));
    });
    for (const auto& path : *paths) inFlight_[path] = id;
}

void ParamHub::call(const QString& method, const QVariantMap& args, const QJSValue& callback) {
    Args a;
    for (auto it = args.begin(); it != args.end(); ++it) a[it.key().toStdString()] = guessValue(it.value());
    connection_.call(method.toStdString(), std::move(a), [this, callback](const Result& r) {
        const bool ok = r.code == ErrorCode::Ok;
        if (!ok) emit failed(Connection::describe(r));
        if (callback.isCallable()) {
            QJSValue cb = callback;
            QJSValue second;
            if (ok) {
                const QVariant v = toVariant(r.value);
                if (v.typeId() == QMetaType::Bool) second = QJSValue(v.toBool());
                else if (v.typeId() == QMetaType::Int) second = QJSValue(v.toInt());
                else if (v.typeId() == QMetaType::Double) second = QJSValue(v.toDouble());
                else if (v.typeId() == QMetaType::QString) second = QJSValue(v.toString());
                else second = QJSValue(QJSValue::NullValue);
            } else {
                second = QJSValue(Connection::describe(r));
            }
            cb.call({QJSValue(ok), second});
        }
    });
}

// --- following the tree --------------------------------------------------------------------------

void ParamHub::watch(ParamValue* p) {
    if (!p->key().empty()) watchers_[p->key()].push_back(p);
}

void ParamHub::unwatch(ParamValue* p) {
    auto it = watchers_.find(p->key());
    if (it == watchers_.end()) return;
    auto& v = it->second;
    v.erase(std::remove(v.begin(), v.end(), p), v.end());
    if (v.empty()) watchers_.erase(it);
}

void ParamHub::notify(const std::string& path) {
    auto it = watchers_.find(path);
    if (it == watchers_.end()) return;
    const auto list = it->second; // a handler may create or destroy watchers
    for (ParamValue* p : list) p->changed();
}

void ParamHub::notifyAll() {
    std::vector<ParamValue*> all;
    for (auto& [path, list] : watchers_) all.insert(all.end(), list.begin(), list.end());
    for (ParamValue* p : all) p->changed();
}

void ParamHub::onDelta(const Delta& d) {
    for (const auto& c : d.changes) notify(c.path);
    if (d.removed.empty()) return;
    std::vector<std::string> hit;
    for (const auto& [path, list] : watchers_)
        for (const auto& prefix : d.removed)
            if (covers(prefix, path)) {
                hit.push_back(path);
                break;
            }
    for (const auto& r : d.removed)
        for (auto* m : {&overrides_, &outgoing_})
            for (auto it = m->begin(); it != m->end();) it = covers(r, it->first) ? m->erase(it) : std::next(it);
    for (const auto& path : hit) notify(path);
}

// --- Param ---------------------------------------------------------------------------------------

ParamValue::ParamValue(QObject* parent) : QObject(parent), hub_(ParamHub::instance()) {}

ParamValue::~ParamValue() {
    if (hub_) hub_->unwatch(this);
}

void ParamValue::setPath(const QString& path) {
    if (path == path_) return;
    if (hub_) hub_->unwatch(this);
    path_ = path;
    key_ = path.toStdString();
    if (hub_) hub_->watch(this);
    emit pathChanged();
    emit valueChanged();
}

QVariant ParamValue::value() const {
    if (!hub_) return {};
    if (const Value* v = hub_->raw(key_)) return ParamHub::toVariant(*v);
    // Not in the tree (yet): the schema's default, so that controls still draw sensibly.
    return defaultValue();
}

bool ParamValue::exists() const { return hub_ && hub_->raw(key_) != nullptr; }
bool ParamValue::pending() const { return hub_ && hub_->pending(key_); }

QString ParamValue::type() const {
    const ParamSpec* s = specOf(key_);
    return QString::fromLatin1(s ? typeText(s->type) : "nil");
}

bool ParamValue::readOnly() const {
    const ParamSpec* s = specOf(key_);
    if (!s || s->access == Access::ReadOnly) return true;
    // the engine refuses a remote controller's Set of these (protocol.md 5.4.3)
    return s->access == Access::LocalOnly && hub_ && hub_->connection().remote();
}

QVariant ParamValue::min() const {
    const ParamSpec* s = specOf(key_);
    return s && s->min ? QVariant(*s->min) : QVariant();
}

QVariant ParamValue::max() const {
    const ParamSpec* s = specOf(key_);
    return s && s->max ? QVariant(*s->max) : QVariant();
}

QVariantList ParamValue::choices() const {
    QVariantList out;
    const ParamSpec* s = specOf(key_);
    if (!s) return out;
    for (const auto& c : s->choices) out.push_back(QString::fromStdString(c));
    for (auto c : s->intChoices) out.push_back(static_cast<int>(c));
    return out;
}

QVariant ParamValue::defaultValue() const {
    const ParamSpec* s = specOf(key_);
    return s ? ParamHub::toVariant(s->defaultValue) : QVariant();
}

QString ParamValue::description() const {
    const ParamSpec* s = specOf(key_);
    return s ? QString::fromStdString(s->description) : QString();
}

void ParamValue::set(const QVariant& value) {
    if (hub_ && !key_.empty()) hub_->set(path_, value);
}

// --- ParamList -----------------------------------------------------------------------------------

ParamList::ParamList(QObject* parent) : QAbstractListModel(parent), hub_(ParamHub::instance()) { attach(); }

void ParamList::attach() {
    if (!hub_) return;
    deltaConn_ = connect(&hub_->connection(), &Connection::deltaApplied, this, &ParamList::onDelta);
    resetConn_ = connect(&hub_->connection(), &Connection::reset, this, &ParamList::rebuild);
}

void ParamList::setPrefix(const QString& prefix) {
    if (prefix == prefix_) return;
    prefix_ = prefix;
    key_ = prefix.isEmpty() ? std::string() : prefix.toStdString() + "/";
    emit prefixChanged();
    rebuild();
}

void ParamList::setOrderBy(const QString& leaf) {
    if (leaf == orderBy_) return;
    orderBy_ = leaf;
    emit prefixChanged();
    rebuild();
}

std::vector<int> ParamList::read() const {
    std::vector<int> out;
    if (!hub_ || key_.empty()) return out;
    for (int n : hub_->indices(prefix_)) out.push_back(n);
    if (!orderBy_.isEmpty()) {
        const auto& tree = hub_->connection().tree();
        const std::string leaf = "/" + orderBy_.toStdString();
        auto pos = [&](int n) {
            const Value* v = tree.get(key_ + std::to_string(n) + leaf);
            return v ? asNumber(*v).value_or(n) : static_cast<double>(n);
        };
        std::stable_sort(out.begin(), out.end(), [&](int a, int b) { return pos(a) < pos(b); });
    }
    return out;
}

void ParamList::rebuild() {
    std::vector<int> next = read();
    if (next == nums_) return;
    const int before = count();
    beginResetModel();
    nums_ = std::move(next);
    endResetModel();
    if (count() != before) emit countChanged();
}

void ParamList::onDelta(const Delta& d) {
    if (key_.empty()) return;
    bool structural = false;
    for (const auto& r : d.removed) structural |= startsWith(r + "/", key_) || startsWith(r, key_);
    const std::string leaf = orderBy_.isEmpty() ? std::string() : "/" + orderBy_.toStdString();
    for (const auto& c : d.changes) {
        if (structural) break;
        if (!startsWith(c.path, key_)) continue;
        const int n = leadingNumber(c.path, key_.size());
        if (n <= 0) continue;
        if (std::find(nums_.begin(), nums_.end(), n) == nums_.end()) structural = true;
        else if (!leaf.empty() && c.path == key_ + std::to_string(n) + leaf) structural = true;
    }
    if (structural) rebuild();
}

int ParamList::numAt(int row) const { return row >= 0 && row < count() ? nums_[static_cast<std::size_t>(row)] : 0; }

int ParamList::rowOf(int num) const {
    const auto it = std::find(nums_.begin(), nums_.end(), num);
    return it == nums_.end() ? -1 : static_cast<int>(it - nums_.begin());
}

int ParamList::rowCount(const QModelIndex& parent) const { return parent.isValid() ? 0 : count(); }

QVariant ParamList::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() >= count()) return {};
    const int n = nums_[static_cast<std::size_t>(index.row())];
    if (role == Qt::UserRole + 1) return n;
    if (role == Qt::UserRole + 2) return prefix_ + QLatin1Char('/') + QString::number(n);
    return {};
}

QHash<int, QByteArray> ParamList::roleNames() const { return {{Qt::UserRole + 1, "num"}, {Qt::UserRole + 2, "base"}}; }

} // namespace dc::ui
