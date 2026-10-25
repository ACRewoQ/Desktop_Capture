#pragma once

#include "dc/protocol/messages.hpp"
#include "dc/protocol/meter.hpp"

#include <QAbstractListModel>
#include <QElapsedTimer>
#include <QTimer>

#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace dc::ui {

class Connection;

// Mixer strips mirrored from the parameter tree: the normal tracks in their screen order, or the
// master strip alone. Edits are shown at once (optimistic) and sent as Set, at most about 60 per
// second per parameter; the local value is kept until the server answered and then the tree's value
// shows again, so an error reverts the control by itself.
class StripModel : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)
    Q_PROPERTY(bool canAdd READ canAdd NOTIFY countChanged)
    Q_PROPERTY(int maxTracks READ maxTracks CONSTANT)

public:
    enum class Kind { Tracks, Master };

    enum Role {
        TrackIdRole = Qt::UserRole + 1,
        NameRole,
        ChannelsRole,
        InputTextRole,
        InputOkRole,
        VolumeDbRole,
        FaderRole,
        PanRole,
        MuteRole,
        SoloRole,
        LowcutOnRole,
        LowcutHzRole,
        HighcutOnRole,
        HighcutHzRole,
        LevelLRole,
        LevelRRole,
        ClipRole,
        CueRole,
        RecordRole,
        FxCountRole,
    };

    StripModel(Connection& connection, Kind kind, QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    int count() const { return static_cast<int>(rows_.size()); }
    bool canAdd() const;
    int maxTracks() const;
    Q_INVOKABLE int trackIdAt(int row) const;
    Q_INVOKABLE QString inputTextAt(int row) const { return row >= 0 && row < count() ? inputText(row) : QString(); }
    int rowOfTrack(int trackId) const;
    // Edits not yet confirmed by the server (sent or waiting for the 60Hz slot).
    bool hasPendingEdits() const { return !overrides_.empty(); }
    int meterPackets() const { return meterPackets_; }

    // Fader travel 0..1 <-> dB: linear in dB from -60 to +12, the bottom is silence.
    Q_INVOKABLE static double faderToDb(double fader);
    Q_INVOKABLE static double dbToFader(double db);
    Q_INVOKABLE static QString dbText(double db);
    Q_INVOKABLE static QString hzText(double hz);

    Q_INVOKABLE void setFader(int row, double fader);
    Q_INVOKABLE void setVolumeDb(int row, double db);
    Q_INVOKABLE void setPan(int row, double pan);
    Q_INVOKABLE void toggleMute(int row);
    Q_INVOKABLE void toggleSolo(int row);
    Q_INVOKABLE void toggleCue(int row);
    Q_INVOKABLE void toggleRecord(int row);
    Q_INVOKABLE void toggleLowcut(int row);
    Q_INVOKABLE void toggleHighcut(int row);
    Q_INVOKABLE void setLowcutHz(int row, double hz);
    Q_INVOKABLE void setHighcutHz(int row, double hz);
    Q_INVOKABLE void clearClip(int row);

    // What the input (tracks) or output (master) can be set to, for the picker: a list of
    // {label, detail, kind, source, channel, selected} (tracks) or {label, detail, target, selected}
    // (master), from /devices/. Multi-channel devices are offered per channel (mono track) or per
    // pair (stereo track).
    Q_INVOKABLE QVariantList inputChoices(int row) const;
    Q_INVOKABLE QVariantList outputChoices() const;
    // Kind, source and channel go in one Set, so that the engine never opens a half-changed input.
    Q_INVOKABLE void setInput(int row, const QString& kind, const QString& source, int channel);
    Q_INVOKABLE void setOutput(const QString& target); // master output 1

    // Tracks only.
    Q_INVOKABLE void addTrack(int channels);
    Q_INVOKABLE void removeTrack(int row);
    Q_INVOKABLE void moveTrack(int row, int offset);

signals:
    void countChanged();
    void failed(const QString& message);
    // The track that this client just added (for scrolling to it).
    void trackAdded(int row);

private:
    struct MeterState {
        double level[2] = {0, 0}; // 0..1 on the meter scale
        bool clip = false;
    };

    std::string base(int row) const;
    const proto::Value* value(int row, const char* leaf) const;
    double number(int row, const char* leaf, double fallback = 0) const;
    bool flag(int row, const char* leaf) const;
    QString text(int row, const char* leaf) const;
    QString inputText(int row) const;

    void edit(int row, const char* leaf, proto::Value v);
    void editTogether(int row, std::vector<std::pair<const char*, proto::Value>> leaves);
    void flush();
    void sendCall(std::string method, proto::Args args, std::function<void(const proto::Result&)> then = {});

    void rebuild(bool resetAll);
    void onDelta(const proto::Delta& d);
    void onMeters(const proto::MeterPacket& p);
    std::vector<int> orderFromTree() const;
    void emitPathChanged(const std::string& path);

    Connection& connection_;
    Kind kind_;
    std::vector<int> rows_; // track ids in screen order (0 = master)
    std::vector<MeterState> meters_;
    std::unordered_map<int, int> rowOfMeter_;
    int meterPackets_ = 0;

    std::map<std::string, proto::Value> overrides_; // path -> value shown until the server answered
    std::map<std::string, proto::Value> outgoing_;  // path -> value waiting for the next send slot
    std::map<std::string, std::uint64_t> inFlight_; // path -> id of the last Set that carries it
    QTimer flushTimer_;
    QElapsedTimer sinceFlush_;
};

} // namespace dc::ui
