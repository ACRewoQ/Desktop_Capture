#pragma once

#include "dc/protocol/messages.hpp"

#include <QAbstractListModel>
#include <QElapsedTimer>
#include <QJSValue>
#include <QObject>
#include <QPointer>
#include <QTimer>
#include <QVariant>

#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace dc::ui {

class Connection;
class ParamValue;

// Any parameter of the tree for the screens that are not strips (settings, effects, video, ...):
// reads with the local edits on top, edits shown at once and sent at most about 60 times per second
// per parameter (like StripModel), Calls, and the schema's range and choices. QML reaches it through
// the Param and ParamList types below, or as the context property "params".
class ParamHub : public QObject {
    Q_OBJECT

public:
    explicit ParamHub(Connection& connection, QObject* parent = nullptr);
    ~ParamHub() override;

    // The hub of the running app (the QML types find it here).
    static ParamHub* instance();
    Connection& connection() { return connection_; }

    const proto::Value* raw(const std::string& path) const;
    Q_INVOKABLE QVariant value(const QString& path) const;
    Q_INVOKABLE bool exists(const QString& path) const;
    // Shown at once, sent within ~16 ms; the value is converted to the parameter's type.
    Q_INVOKABLE void set(const QString& path, const QVariant& value);
    // Several parameters in one Set (the engine applies all or none), e.g. {"/a": 1, "/b": "x"}.
    Q_INVOKABLE void setMany(const QVariantMap& changes);
    // callback(ok, valueOrMessage) runs once with the result; errors are also reported by failed().
    Q_INVOKABLE void call(const QString& method, const QVariantMap& args = {}, const QJSValue& callback = {});
    // Numbers that exist directly below a list prefix: indices("/video/source") -> [1, 2, 5].
    Q_INVOKABLE QList<int> indices(const QString& prefix) const;
    bool pending(const std::string& path) const { return overrides_.count(path) > 0; }

    static QVariant toVariant(const proto::Value& v);
    // Uses the schema's type for path when it has one (a JS number 2 becomes an int for an int).
    static proto::Value fromVariant(const std::string& path, const QVariant& v);

    void watch(ParamValue* p);
    void unwatch(ParamValue* p);

signals:
    void failed(const QString& message);

private:
    void flush();
    void onDelta(const proto::Delta& d);
    void notify(const std::string& path);
    void notifyAll();

    Connection& connection_;
    std::map<std::string, proto::Value> overrides_;
    std::map<std::string, proto::Value> outgoing_;
    std::map<std::string, std::uint64_t> inFlight_;
    QTimer flushTimer_;
    QElapsedTimer sinceFlush_;
    std::unordered_map<std::string, std::vector<ParamValue*>> watchers_;
};

// QML "Param": one parameter, e.g. Param { id: mode; path: "/system/audio_mode" }.
class ParamValue : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString path READ path WRITE setPath NOTIFY pathChanged)
    Q_PROPERTY(QVariant value READ value NOTIFY valueChanged)
    Q_PROPERTY(bool exists READ exists NOTIFY valueChanged)
    Q_PROPERTY(bool pending READ pending NOTIFY valueChanged)
    // From the schema (fixed for a path).
    Q_PROPERTY(QString type READ type NOTIFY pathChanged)
    Q_PROPERTY(bool readOnly READ readOnly NOTIFY pathChanged)
    Q_PROPERTY(QVariant min READ min NOTIFY pathChanged)
    Q_PROPERTY(QVariant max READ max NOTIFY pathChanged)
    Q_PROPERTY(QVariantList choices READ choices NOTIFY pathChanged)
    Q_PROPERTY(QVariant defaultValue READ defaultValue NOTIFY pathChanged)
    Q_PROPERTY(QString description READ description NOTIFY pathChanged)

public:
    explicit ParamValue(QObject* parent = nullptr);
    ~ParamValue() override;

    QString path() const { return path_; }
    void setPath(const QString& path);
    QVariant value() const;
    bool exists() const;
    bool pending() const;
    QString type() const;
    bool readOnly() const;
    QVariant min() const;
    QVariant max() const;
    QVariantList choices() const;
    QVariant defaultValue() const;
    QString description() const;

    Q_INVOKABLE void set(const QVariant& value);

    const std::string& key() const { return key_; }
    void changed() { emit valueChanged(); }

signals:
    void pathChanged();
    void valueChanged();

private:
    QString path_;
    std::string key_;
    QPointer<ParamHub> hub_;
};

// QML "ParamList": the numbered children of a list prefix, e.g. ParamList { prefix: "/video/source" }
// lists /video/source/1, /video/source/3, ... Sorted by number, or by the int leaf orderBy
// ("position") when set. Roles: num (the number) and base ("/video/source/3").
class ParamList : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(QString prefix READ prefix WRITE setPrefix NOTIFY prefixChanged)
    Q_PROPERTY(QString orderBy READ orderBy WRITE setOrderBy NOTIFY prefixChanged)
    Q_PROPERTY(int count READ count NOTIFY countChanged)

public:
    explicit ParamList(QObject* parent = nullptr);

    QString prefix() const { return prefix_; }
    void setPrefix(const QString& prefix);
    QString orderBy() const { return orderBy_; }
    void setOrderBy(const QString& leaf);
    int count() const { return static_cast<int>(nums_.size()); }

    Q_INVOKABLE int numAt(int row) const;
    Q_INVOKABLE int rowOf(int num) const;

    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

signals:
    void prefixChanged();
    void countChanged();

private:
    void attach();
    void rebuild();
    void onDelta(const proto::Delta& d);
    std::vector<int> read() const;

    QString prefix_;
    QString orderBy_;
    std::string key_; // prefix + "/"
    std::vector<int> nums_;
    QPointer<ParamHub> hub_;
    QMetaObject::Connection deltaConn_, resetConn_;
};

} // namespace dc::ui
