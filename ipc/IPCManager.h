#ifndef IPCMANAGER_H
#define IPCMANAGER_H

#include <QWidget>
#include <QByteArray>
#include <QPair>
#include <QString>
#include <QStringList>
#include <QVector>

#include <mqueue.h>
#include <sys/types.h>

struct SharedBlock;

class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class QTimer;

// Inter-Process Communication module.
// The GUI starts a REAL child process (fork + execv of os-control-ipc-child)
// and talks to it through an anonymous pipe, a named pipe (FIFO), POSIX shared
// memory or a POSIX message queue.
class IPCManager : public QWidget
{
    Q_OBJECT

public:
    explicit IPCManager(QWidget *parent = nullptr);
    ~IPCManager() override;

private:
    enum class Mechanism { Pipe = 0, Fifo = 1, SharedMemory = 2, MessageQueue = 3 };

    // Widgets
    QComboBox *mechanismCombo;
    QLabel *explainLabel;
    QLabel *infoLabel;
    QPushButton *startButton;
    QPushButton *stopButton;
    QLineEdit *messageEdit;
    QSpinBox *prioritySpin;
    QPushButton *sendButton;
    QPushButton *priorityDemoButton;
    QPlainTextEdit *log;
    QLabel *statusLabel;
    QTimer *pollTimer;

    // State
    Mechanism mechanism = Mechanism::Pipe;
    bool running = false;
    pid_t childPid = -1;
    int tickCount = 0;

    // Pipe and FIFO
    int toChildFd = -1;            // parent's write end
    int fromChildFd = -1;          // parent's read end
    int childReadFd = -1;          // child's ends (closed in the parent after fork)
    int childWriteFd = -1;
    QString fifoToChild;
    QString fifoFromChild;
    bool fifoWriterPending = false;
    qint64 fifoDeadline = 0;
    QByteArray incoming;

    // Shared memory
    QString shmName;
    SharedBlock *shm = nullptr;
    unsigned int shmSendSeq = 0;
    unsigned int shmLastReceived = 0;

    // Message queue
    QString mqToName;
    QString mqFromName;
    mqd_t mqTo = static_cast<mqd_t>(-1);
    mqd_t mqFrom = static_cast<mqd_t>(-1);

    // UI
    void setupUI();
    void updateButtons();
    void updateExplanation();
    void updateInfo();
    void report(const QString &message, bool error = false);
    void logEvent(const QString &text);
    void logSent(const QString &text);
    void logReceived(const QString &text);

    // Channel lifecycle
    void startChannel();
    void stopChannel();
    void releaseResources();
    bool setupPipe(QStringList &args, QVector<QPair<int, int>> &dups, QString &why);
    bool setupFifo(QStringList &args, QString &why);
    bool setupSharedMemory(QStringList &args, QString &why);
    bool setupMessageQueue(QStringList &args, QString &why);

    // Messaging
    void sendMessage();
    void sendQuitMessage();
    void priorityDemo();
    void onTick();
    void pollIncoming();
    void pollStream();
    void pollSharedMemory();
    void pollMessageQueue();
    bool reapChild(int timeoutMs, int &status);
    void handleChildExit(int status);

    // Helpers
    QString sanitizedMessage() const;
    static QString errorText(int err);
};

#endif