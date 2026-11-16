#ifndef PROCESSMANAGER_H
#define PROCESSMANAGER_H

#include <QWidget>
#include <QSet>
#include <sys/types.h>

class QTableWidget;
class QPushButton;
class QCheckBox;
class QLabel;
class QTimer;

// A process is identified by PID *and* start time.
// The PID alone is not safe: Linux can reuse a PID after a process exits.
struct ProcessIdentity
{
    pid_t pid = -1;
    qulonglong startTime = 0;   // field 22 of /proc/<pid>/stat (clock ticks since boot)

    bool isValid() const { return pid > 0; }
    void clear() { pid = -1; startTime = 0; }
};

class ProcessManager : public QWidget
{
    Q_OBJECT

public:
    explicit ProcessManager(QWidget *parent = nullptr);

private:
    // Widgets
    QTableWidget *processTable;
    QPushButton *refreshButton;
    QPushButton *startButton;
    QPushButton *stopButton;
    QPushButton *forceStopButton;
    QPushButton *suspendButton;
    QPushButton *resumeButton;
    QCheckBox *autoRefreshCheck;
    QLabel *statusLabel;
    QTimer *refreshTimer;

    // State
    ProcessIdentity selected;          // remembered by identity, not by table row
    QSet<pid_t> childProcesses;        // children we created with fork()

    // UI
    void setupUI();
    void setStatus(const QString &message, bool error = false);

    // Process list
    void loadProcesses();
    void onSelectionChanged();
    void onTimerTick();

    // Actions
    void startProcess();
    void stopProcess();
    void forceStopProcess();
    void suspendProcess();
    void resumeProcess();
    void sendSignalToSelected(int signalNumber, const QString &actionName);

    // Super Admin: ask pkexec to run the helper for ONE signal on ONE process
    void runPrivilegedSignal(const ProcessIdentity &target,
                             int signalNumber,
                             const QString &actionName);
    bool privilegedBusy = false;

    // Helpers
    void reapChildren();
    static bool readStartTime(pid_t pid, qulonglong &startTime);
    static bool readParentPid(pid_t pid, pid_t &parentPid);
    static QString describeState(QChar code);
    static QString userName(uid_t uid);
};

#endif