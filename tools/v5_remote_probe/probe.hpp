#pragma once

#include <QElapsedTimer>
#include <QJsonObject>
#include <QObject>
#include <QSet>
#include <QTimer>

class QQuickWindow;

// Process statistics shown in the status bar and written to the JSON report: memory, CPU, frame rate,
// and how many faders are being dragged at the same time.
class Probe : public QObject {
    Q_OBJECT
    Q_PROPERTY(double workingSetMb READ workingSetMb NOTIFY statsChanged)
    Q_PROPERTY(double privateMb READ privateMb NOTIFY statsChanged)
    Q_PROPERTY(double peakWorkingSetMb READ peakWorkingSetMb NOTIFY statsChanged)
    Q_PROPERTY(double cpuPercent READ cpuPercent NOTIFY statsChanged)
    Q_PROPERTY(int fps READ fps NOTIFY statsChanged)
    Q_PROPERTY(int activeFaders READ activeFaders NOTIFY fadersChanged)
    Q_PROPERTY(int maxSimultaneousFaders READ maxSimultaneousFaders NOTIFY fadersChanged)
    Q_PROPERTY(QString info READ info CONSTANT)

public:
    struct Memory {
        double workingSetMb = 0;
        double privateMb = 0;
        double peakWorkingSetMb = 0;
    };
    static Memory sampleMemory();

    explicit Probe(QString info, QObject* parent = nullptr);

    void attach(QQuickWindow* window);

    double workingSetMb() const { return mem_.workingSetMb; }
    double privateMb() const { return mem_.privateMb; }
    double peakWorkingSetMb() const { return mem_.peakWorkingSetMb; }
    double cpuPercent() const { return cpuPercent_; }
    int fps() const { return fps_; }
    int activeFaders() const { return static_cast<int>(grabbed_.size()); }
    int maxSimultaneousFaders() const { return maxSimultaneous_; }
    QString info() const { return info_; }

    // Called by the QML faders when a pointer grabs or releases them.
    Q_INVOKABLE void faderGrab(int row, bool grabbed);
    void resetMaxSimultaneous();

    int qmlWarnings() const { return qmlWarnings_; }
    void countQmlWarning() { ++qmlWarnings_; }

    // Snapshot of everything above for the report file.
    QJsonObject toJson() const;

signals:
    void statsChanged();
    void fadersChanged();

private:
    void tick();

    QString info_;
    QTimer timer_;
    QElapsedTimer wall_;
    double lastCpuSec_ = 0;
    double lastWallSec_ = 0;
    int frames_ = 0;
    int fps_ = 0;
    double cpuPercent_ = 0;
    Memory mem_;
    QSet<int> grabbed_;
    int maxSimultaneous_ = 0;
    int qmlWarnings_ = 0;
};
