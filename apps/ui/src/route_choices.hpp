#pragma once

#include "dc/protocol/param_tree.hpp"

#include <QObject>
#include <QVariantList>

#include <string>

namespace dc::ui {

class Connection;

// Where audio can go (protocol.md 4.3), for the output, send and cue pickers: the choices with
// display names from /devices/output, the tracks and /lan/stream, and the display name of a target.
// Context property "routes".
class RouteChoices : public QObject {
    Q_OBJECT

public:
    explicit RouteChoices(Connection& connection, QObject* parent = nullptr);

    // use: "out" (track output), "send", "master" (master output) or "cue". track = the track
    // being edited (left out of the list). Entries: {label, detail, value}, and {header} lines.
    Q_INVOKABLE QVariantList targetChoices(const QString& use, int track = 0) const;
    // "master" -> "マスター", "device/<id>" -> device name, "track/3" -> its name, ...
    Q_INVOKABLE QString targetText(const QString& target) const;
    static QString describe(const proto::ParamTree& tree, const QString& target);
    Q_INVOKABLE QString trackName(int track) const;
    Q_INVOKABLE QString lanStreamName(int n) const;

private:

    Connection& connection_;
};

} // namespace dc::ui
