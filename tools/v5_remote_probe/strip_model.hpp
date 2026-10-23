#pragma once

#include <QAbstractListModel>
#include <QTimer>

#include <memory>
#include <vector>

class QUdpSocket;

namespace dc::control {
class ControlClient;
}

// Mixer strips shown by the probe. Either simulated locally or mirrored from a control server
// (mixd or dc-mock-server), in which case fader moves are sent as Set and meters come over UDP.
class StripModel : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)
    Q_PROPERTY(bool connected READ connected NOTIFY connectedChanged)

public:
    enum Role { TrackIdRole = Qt::UserRole + 1, NameRole, VolumeDbRole, PositionRole, LevelRole, MuteRole };

    explicit StripModel(QObject* parent = nullptr);
    ~StripModel() override;

    // Local simulation with n strips and moving meters.
    void simulate(int n);
    // Connects to a control server, adds tracks until there are at least n, and mirrors them.
    bool connectTo(const QString& host, quint16 port, int n, QString* error);

    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    int count() const { return static_cast<int>(strips_.size()); }
    bool connected() const { return client_ != nullptr; }
    double volumeDb(int row) const;

    // Fader position 0..1 <-> dB. Linear in dB from -60 to +12, bottom = silence.
    Q_INVOKABLE static double positionToDb(double position);
    Q_INVOKABLE static double dbToPosition(double db);

    Q_INVOKABLE void setPosition(int row, double position);
    Q_INVOKABLE void setVolumeDb(int row, double db);
    Q_INVOKABLE void toggleMute(int row);

signals:
    void countChanged();
    void connectedChanged();

private:
    struct Strip {
        int trackId = 0;
        int meterId = 0;
        QString name;
        double volumeDb = 0;
        double level = 0; // meter 0..1
        bool mute = false;
    };

    void loadFromTree();
    void applyRemote(const QString& path, const QVariant& value);
    void readMeters();
    void simulateMeters();
    int rowOfTrack(int trackId) const;

    std::vector<Strip> strips_;
    QTimer meterTimer_;
    double phase_ = 0;
    std::unique_ptr<dc::control::ControlClient> client_;
    QUdpSocket* meterSocket_ = nullptr;
};
