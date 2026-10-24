#include "route_choices.hpp"

#include "connection.hpp"

#include "dc/protocol/schema.hpp"

#include <algorithm>
#include <cstdlib>
#include <utility>
#include <vector>

namespace dc::ui {

using namespace dc::proto;

namespace {

QVariantMap entry(const QString& label, const QString& detail, const QString& value) {
    return {{"label", label}, {"detail", detail}, {"value", value}};
}

QVariantMap header(const QString& text) { return {{"header", text}}; }

} // namespace

RouteChoices::RouteChoices(Connection& connection, QObject* parent) : QObject(parent), connection_(connection) {}

namespace {

QString trackNameIn(const ParamTree& tree, int track) {
    const auto name = tree.getAs<std::string>("/track/" + std::to_string(track) + "/name");
    if (name && !name->empty()) return QString::fromStdString(*name);
    return QStringLiteral("Track %1").arg(track);
}

QString lanStreamNameIn(const ParamTree& tree, int n) {
    const auto name = tree.getAs<std::string>("/lan/stream/" + std::to_string(n) + "/name");
    if (name && !name->empty()) return QString::fromStdString(*name);
    return QStringLiteral("LAN %1").arg(n);
}

} // namespace

QString RouteChoices::trackName(int track) const { return trackNameIn(connection_.tree(), track); }

QString RouteChoices::lanStreamName(int n) const { return lanStreamNameIn(connection_.tree(), n); }

QString RouteChoices::targetText(const QString& target) const { return describe(connection_.tree(), target); }

QString RouteChoices::describe(const ParamTree& tree, const QString& qtarget) {
    const std::string target = qtarget.toStdString();
    if (target.empty()) return QStringLiteral("なし");
    if (target == "master") return QStringLiteral("マスター");
    if (target.rfind("track/", 0) == 0) return trackNameIn(tree, std::atoi(target.c_str() + 6));
    if (target.rfind("lan/", 0) == 0) return lanStreamNameIn(tree, std::atoi(target.c_str() + 4));
    if (target.rfind("device/", 0) == 0 || target.rfind("vmic/", 0) == 0) {
        const std::string id = target.substr(target.find('/') + 1);
        for (int i = 1;; ++i) {
            const std::string base = "/devices/output/" + std::to_string(i);
            const auto devId = tree.getAs<std::string>(base + "/id");
            if (!devId) break;
            if (*devId != id) continue;
            const QString name = QString::fromStdString(tree.getAs<std::string>(base + "/name").value_or(id));
            return tree.getAs<bool>(base + "/connected").value_or(true) ? name : name + QStringLiteral("（切断中）");
        }
        return QString::fromStdString(id) + QStringLiteral("（見つかりません）");
    }
    return qtarget;
}

QVariantList RouteChoices::targetChoices(const QString& use, int track) const {
    const bool isOut = use == QLatin1String("out");
    const bool isSend = use == QLatin1String("send");
    const bool isCue = use == QLatin1String("cue");
    const auto& tree = connection_.tree();
    QVariantList out;
    out.push_back(entry(QStringLiteral("なし"), QString(), QString()));
    if (isOut) out.push_back(entry(QStringLiteral("マスター"), QString(), QStringLiteral("master")));

    if (isOut || isSend) {
        // Other tracks in screen order. Loops are refused by the engine (conflict).
        std::vector<std::pair<std::int64_t, int>> tracks;
        for (int n = 1; n <= kMaxTracks; ++n)
            if (n != track)
                if (auto pos = tree.getAs<std::int64_t>("/track/" + std::to_string(n) + "/position"))
                    tracks.push_back({*pos, n});
        std::sort(tracks.begin(), tracks.end());
        if (!tracks.empty()) out.push_back(header(QStringLiteral("トラック")));
        for (auto& [pos, n] : tracks)
            out.push_back(entry(trackName(n), QStringLiteral("トラック %1 の入力に足す").arg(n), QStringLiteral("track/%1").arg(n)));
    }

    bool first = true;
    for (int i = 1;; ++i) {
        const std::string base = "/devices/output/" + std::to_string(i);
        const auto id = tree.getAs<std::string>(base + "/id");
        if (!id) break;
        const bool vmic = tree.getAs<std::string>(base + "/api").value_or("") == "vmic";
        if (isCue && vmic) continue;
        if (first) out.push_back(header(QStringLiteral("出力デバイス")));
        first = false;
        QStringList detail;
        if (vmic) detail << QStringLiteral("仮想マイク");
        if (tree.getAs<std::string>(base + "/api").value_or("") == "asio") detail << QStringLiteral("ASIO");
        if (!tree.getAs<bool>(base + "/connected").value_or(true)) detail << QStringLiteral("切断中");
        out.push_back(entry(QString::fromStdString(tree.getAs<std::string>(base + "/name").value_or(*id)),
                            detail.join(QStringLiteral(" · ")),
                            QString::fromStdString((vmic ? "vmic/" : "device/") + *id)));
    }

    if (!isCue) {
        out.push_back(header(QStringLiteral("LAN 送信")));
        for (int n = 1; n <= kMaxLanStreams; ++n) {
            const std::string base = "/lan/stream/" + std::to_string(n);
            const auto subs = tree.getAs<std::int64_t>(base + "/subscribers").value_or(0);
            const QString codec = QString::fromStdString(tree.getAs<std::string>(base + "/codec").value_or("pcm24"));
            out.push_back(entry(lanStreamName(n),
                                subs > 0 ? QStringLiteral("%1 · 受信 %2 台").arg(codec).arg(subs) : codec,
                                QStringLiteral("lan/%1").arg(n)));
        }
    }
    return out;
}

} // namespace dc::ui
