#include "IPCManager.h"
#include "IPCProtocol.h"

#include <QComboBox>
#include <QCoreApplication>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>

#include <cerrno>
#include <initializer_list>
#include <csignal>
#include <cstring>
#include <ctime>
#include <new>

#include <fcntl.h>
#include <mqueue.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

void pauseMilliseconds(long ms)
{
    timespec pause;
    pause.tv_sec = ms / 1000;
    pause.tv_nsec = (ms % 1000) * 1000000L;
    nanosleep(&pause, nullptr);
}

// Move a descriptor to a high number (10 or more), so it can never be mistaken
// for 0, 1, 2, or the fixed numbers (3, 4) the child will receive.
int moveHigh(int fd)
{
    const int moved = ::fcntl(fd, F_DUPFD_CLOEXEC, 10);
    const int saved = errno;
    ::close(fd);
    errno = saved;
    return moved;
}

// One write() to a non-blocking descriptor. SIGPIPE is blocked just for this call:
// writing to a pipe whose reader has died then gives EPIPE instead of killing the GUI.
bool writeLine(int fd, const QByteArray &data, int &err)
{
    err = 0;

    sigset_t blocked;
    sigset_t previous;
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGPIPE);
    pthread_sigmask(SIG_BLOCK, &blocked, &previous);

    ssize_t result;
    do {
        result = ::write(fd, data.constData(), static_cast<size_t>(data.size()));
    } while (result < 0 && errno == EINTR);

    if (result < 0)
        err = errno;

    if (err == EPIPE)
    {
        const timespec zero = {0, 0};
        sigtimedwait(&blocked, nullptr, &zero);
    }

    pthread_sigmask(SIG_SETMASK, &previous, nullptr);

    return result == static_cast<ssize_t>(data.size());
}

// First letter after the process name in /proc/<pid>/stat: R, S, T (stopped), Z ...
QChar processStateLetter(pid_t pid)
{
    QFile file(QString("/proc/%1/stat").arg(pid));
    if (!file.open(QIODevice::ReadOnly))
        return QChar('?');

    const QByteArray text = file.readAll();
    const int close = text.lastIndexOf(')');
    if (close < 0 || close + 2 >= text.size())
        return QChar('?');

    return QChar(text.at(close + 2));
}

QString stateWord(QChar letter)
{
    switch (letter.toLatin1())
    {
    case 'R': return "running";
    case 'S': return "sleeping";
    case 'D': return "disk wait";
    case 'T': return "SUSPENDED";
    case 'Z': return "zombie";
    default:  return "unknown";
    }
}

// fork() + execv(): the child process is replaced by the helper program.
// 'dups' lists (source fd, target fd) pairs the child should have after the exec.
bool spawnChild(const QString &program, const QStringList &args,
                const QVector<QPair<int, int>> &dups, pid_t &pid, int &err)
{
    // Prepare everything BEFORE fork(): the child may only use simple system calls.
    QVector<QByteArray> storage;
    storage.append(QFile::encodeName(program));
    for (const QString &argument : args)
        storage.append(argument.toLocal8Bit());

    QVector<char *> argv;
    for (QByteArray &item : storage)
        argv.append(item.data());
    argv.append(nullptr);

    char **argvPointer = argv.data();
    const QPair<int, int> *dupPointer = dups.constData();
    const int dupCount = static_cast<int>(dups.size());

    // The child writes errno here if exec fails; the pipe closes itself when exec succeeds.
    int errorPipe[2];
    if (::pipe2(errorPipe, O_CLOEXEC) != 0)
    {
        err = errno;
        return false;
    }

    pid = ::fork();

    if (pid < 0)
    {
        err = errno;
        ::close(errorPipe[0]);
        ::close(errorPipe[1]);
        return false;
    }

    if (pid == 0)
    {
        // ----- CHILD: only async-signal-safe calls until execv() -----
        for (int i = 0; i < dupCount; ++i)
        {
            if (::dup2(dupPointer[i].first, dupPointer[i].second) < 0)
            {
                const int failure = errno;
                const ssize_t ignored = ::write(errorPipe[1], &failure, sizeof(failure));
                (void)ignored;
                _exit(126);
            }
        }

        ::execv(argvPointer[0], argvPointer);

        const int failure = errno;
        const ssize_t ignored = ::write(errorPipe[1], &failure, sizeof(failure));
        (void)ignored;
        _exit(127);
    }

    // ----- PARENT -----
    ::close(errorPipe[1]);

    int execError = 0;
    ssize_t count;
    do {
        count = ::read(errorPipe[0], &execError, sizeof(execError));
    } while (count < 0 && errno == EINTR);
    ::close(errorPipe[0]);

    if (count == static_cast<ssize_t>(sizeof(execError)))
    {
        ::waitpid(pid, nullptr, 0);
        err = execError;
        return false;
    }

    return true;
}

} // namespace

// ---------------------------------------------------------------------------
// Constructor / destructor
// ---------------------------------------------------------------------------
IPCManager::IPCManager(QWidget *parent)
    : QWidget(parent)
{
    setupUI();

    pollTimer = new QTimer(this);
    pollTimer->setInterval(50);
    connect(pollTimer, &QTimer::timeout, this, &IPCManager::onTick);

    updateExplanation();
    updateInfo();
    updateButtons();
}

IPCManager::~IPCManager()
{
    stopChannel();      // never leave a child process or IPC object behind
}

// ---------------------------------------------------------------------------
// User interface
// ---------------------------------------------------------------------------
void IPCManager::setupUI()
{
    QVBoxLayout *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(35, 30, 35, 30);
    mainLayout->setSpacing(10);

    QLabel *heading = new QLabel("IPC Management");
    heading->setStyleSheet("font-size: 25px; font-weight: bold;");

    QLabel *description = new QLabel(
        "Real communication between two separate processes: this application and a child it creates.");
    description->setStyleSheet("color: #6b7280;");

    // ----- Channel row -----
    mechanismCombo = new QComboBox;
    mechanismCombo->addItem("Anonymous pipe  (pipe)");
    mechanismCombo->addItem("Named pipe / FIFO  (mkfifo)");
    mechanismCombo->addItem("Shared memory  (shm_open + mmap)");
    mechanismCombo->addItem("Message queue  (mq_open)");

    startButton = new QPushButton("Start Channel + Child");
    stopButton = new QPushButton("Stop Channel");
    stopButton->setStyleSheet(
        "QPushButton { background-color: #dc2626; }"
        "QPushButton:hover { background-color: #b91c1c; }"
        "QPushButton:disabled { background-color: #9ca3af; }");

    QHBoxLayout *channelRow = new QHBoxLayout;
    channelRow->addWidget(new QLabel("Mechanism:"));
    channelRow->addWidget(mechanismCombo, 1);
    channelRow->addWidget(startButton);
    channelRow->addWidget(stopButton);

    explainLabel = new QLabel;
    explainLabel->setWordWrap(true);
    explainLabel->setStyleSheet("color: #374151; font-size: 12px;");

    infoLabel = new QLabel;
    infoLabel->setWordWrap(true);
    infoLabel->setStyleSheet(
        "background-color: white; border: 1px solid #e5e7eb;"
        "border-radius: 6px; padding: 8px; font-size: 12px;");

    // ----- Message row -----
    messageEdit = new QLineEdit;
    messageEdit->setMaxLength(200);
    messageEdit->setPlaceholderText("Type a message for the child process");

    prioritySpin = new QSpinBox;
    prioritySpin->setRange(0, 31);
    prioritySpin->setValue(0);

    sendButton = new QPushButton("Send");
    priorityDemoButton = new QPushButton("Priority Demo");

    QHBoxLayout *messageRow = new QHBoxLayout;
    messageRow->addWidget(messageEdit, 1);
    messageRow->addWidget(new QLabel("Priority:"));
    messageRow->addWidget(prioritySpin);
    messageRow->addWidget(sendButton);
    messageRow->addWidget(priorityDemoButton);

    // ----- Log -----
    log = new QPlainTextEdit;
    log->setReadOnly(true);
    log->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    log->setMinimumHeight(180);

    statusLabel = new QLabel("Ready.");
    statusLabel->setWordWrap(true);
    statusLabel->setStyleSheet("color: #374151; font-size: 12px;");

    mainLayout->addWidget(heading);
    mainLayout->addWidget(description);
    mainLayout->addLayout(channelRow);
    mainLayout->addWidget(explainLabel);
    mainLayout->addWidget(infoLabel);
    mainLayout->addLayout(messageRow);
    mainLayout->addWidget(log, 1);
    mainLayout->addWidget(statusLabel);

    connect(startButton, &QPushButton::clicked, this, &IPCManager::startChannel);
    connect(stopButton, &QPushButton::clicked, this, &IPCManager::stopChannel);
    connect(sendButton, &QPushButton::clicked, this, &IPCManager::sendMessage);
    connect(messageEdit, &QLineEdit::returnPressed, this, &IPCManager::sendMessage);
    connect(priorityDemoButton, &QPushButton::clicked, this, &IPCManager::priorityDemo);
    connect(mechanismCombo, &QComboBox::currentIndexChanged, this, [this](int) {
        updateExplanation();
        updateButtons();
    });

    setStyleSheet(
        "QPushButton {"
        "  background-color: #2563eb; color: white;"
        "  padding: 8px 12px; border: none; border-radius: 5px;"
        "}"
        "QPushButton:hover { background-color: #1d4ed8; }"
        "QPushButton:disabled { background-color: #9ca3af; }"
    );
}

void IPCManager::updateButtons()
{
    const bool isQueue = (mechanismCombo->currentIndex() == static_cast<int>(Mechanism::MessageQueue));

    mechanismCombo->setEnabled(!running);
    startButton->setEnabled(!running);
    stopButton->setEnabled(running);
    sendButton->setEnabled(running);
    messageEdit->setEnabled(running);
    prioritySpin->setEnabled(running && isQueue);
    priorityDemoButton->setEnabled(running && isQueue);
}

void IPCManager::updateExplanation()
{
    switch (static_cast<Mechanism>(mechanismCombo->currentIndex()))
    {
    case Mechanism::Pipe:
        explainLabel->setText(
            "Anonymous pipe: a one-way byte stream in the kernel, created with pipe() BEFORE fork() so the "
            "child inherits it. Only related processes can use it. We use two pipes (one per direction). "
            "A pipe has no message boundaries, so each message ends with a newline.");
        break;
    case Mechanism::Fifo:
        explainLabel->setText(
            "Named pipe (FIFO): works like a pipe but has a NAME in the filesystem, created with mkfifo(). "
            "Any process that can open the path may use it, related or not. Opening blocks until both "
            "ends are present.");
        break;
    case Mechanism::SharedMemory:
        explainLabel->setText(
            "POSIX shared memory: shm_open() makes a memory object and mmap() maps the SAME physical memory "
            "into both processes. No data is copied through the kernel, so it is the fastest mechanism. "
            "We synchronize with atomic counters (stop-and-wait).");
        break;
    case Mechanism::MessageQueue:
        explainLabel->setText(
            "POSIX message queue: the kernel stores whole messages (boundaries are kept) and hands out the "
            "HIGHEST PRIORITY first. The Priority Demo suspends the child with SIGSTOP, queues three "
            "messages, then resumes it so you can see the order change.");
        break;
    }
}

void IPCManager::updateInfo()
{
    if (!running)
    {
        infoLabel->setText("No channel is open. Choose a mechanism and click \"Start Channel + Child\".");
        return;
    }

    QString text = QString("This application (parent) PID %1   |   Child PID %2 (%3)")
                       .arg(getpid()).arg(childPid)
                       .arg(stateWord(processStateLetter(childPid)));

    if (mechanism == Mechanism::MessageQueue && mqTo != static_cast<mqd_t>(-1))
    {
        mq_attr attributes;
        if (mq_getattr(mqTo, &attributes) == 0)
            text += QString("   |   Waiting in the queue to the child: %1 of %2")
                        .arg(attributes.mq_curmsgs).arg(attributes.mq_maxmsg);
    }

    infoLabel->setText(text);
}

void IPCManager::report(const QString &message, bool error)
{
    statusLabel->setStyleSheet(
        error ? "color: #b91c1c; font-size: 12px; font-weight: bold;"
              : "color: #166534; font-size: 12px;");
    statusLabel->setText(message);

    if (error)
        logEvent("ERROR: " + message);
}

void IPCManager::logEvent(const QString &text)
{
    log->appendPlainText(QDateTime::currentDateTime().toString("HH:mm:ss.zzz") + "   " + text);
}

void IPCManager::logSent(const QString &text)
{
    logEvent(QString("PARENT %1  ->  child:   %2").arg(getpid()).arg(text));
}

void IPCManager::logReceived(const QString &text)
{
    logEvent(QString("PARENT %1  <-  child %2:   %3").arg(getpid()).arg(childPid).arg(text));
}

QString IPCManager::errorText(int err)
{
    const char *name = nullptr;

    switch (err)
    {
    case ENOENT: name = "ENOENT"; break;
    case EACCES: name = "EACCES"; break;
    case EEXIST: name = "EEXIST"; break;
    case EAGAIN: name = "EAGAIN"; break;
    case EMFILE: name = "EMFILE"; break;
    case ENFILE: name = "ENFILE"; break;
    case ENOSPC: name = "ENOSPC"; break;
    case ENOSYS: name = "ENOSYS"; break;
    case EPIPE:  name = "EPIPE";  break;
    case EINVAL: name = "EINVAL"; break;
    case ENOMEM: name = "ENOMEM"; break;
    case EMSGSIZE: name = "EMSGSIZE"; break;
    default: break;
    }

    const QString text = QString::fromLocal8Bit(strerror(err));

    if (name)
        return QString("%1 - %2").arg(QString::fromLatin1(name), text);

    return QString("errno %1 - %2").arg(err).arg(text);
}

// Newlines would break the line-based pipe protocol, so they become spaces.
QString IPCManager::sanitizedMessage() const
{
    QString text = messageEdit->text();
    text.replace('\n', ' ');
    text.replace('\r', ' ');
    return text;
}

// ---------------------------------------------------------------------------
// Creating the IPC objects
// ---------------------------------------------------------------------------
bool IPCManager::setupPipe(QStringList &args, QVector<QPair<int, int>> &dups, QString &why)
{
    int toChild[2];
    int fromChild[2];

    if (::pipe2(toChild, O_CLOEXEC) != 0)
    {
        why = "pipe2() failed: " + errorText(errno);
        return false;
    }

    if (::pipe2(fromChild, O_CLOEXEC) != 0)
    {
        why = "pipe2() failed: " + errorText(errno);
        ::close(toChild[0]);
        ::close(toChild[1]);
        return false;
    }

    childReadFd = moveHigh(toChild[0]);
    toChildFd = moveHigh(toChild[1]);
    fromChildFd = moveHigh(fromChild[0]);
    childWriteFd = moveHigh(fromChild[1]);

    if (childReadFd < 0 || toChildFd < 0 || fromChildFd < 0 || childWriteFd < 0)
    {
        why = "Could not prepare the pipe descriptors: " + errorText(errno);
        return false;
    }

    // Our ends are non-blocking, so a full or empty pipe can never freeze the window.
    ::fcntl(toChildFd, F_SETFL, ::fcntl(toChildFd, F_GETFL) | O_NONBLOCK);
    ::fcntl(fromChildFd, F_SETFL, ::fcntl(fromChildFd, F_GETFL) | O_NONBLOCK);

    // After exec the child owns descriptors 3 (read) and 4 (write).
    args << "pipe" << "3" << "4";
    dups.append(qMakePair(childReadFd, 3));
    dups.append(qMakePair(childWriteFd, 4));
    return true;
}

bool IPCManager::setupFifo(QStringList &args, QString &why)
{
    fifoToChild = QString("/tmp/occ_fifo_%1_to_child").arg(getpid());
    fifoFromChild = QString("/tmp/occ_fifo_%1_from_child").arg(getpid());

    const QByteArray toPath = QFile::encodeName(fifoToChild);
    const QByteArray fromPath = QFile::encodeName(fifoFromChild);

    ::unlink(toPath.constData());       // remove leftovers of an earlier crash
    ::unlink(fromPath.constData());

    if (::mkfifo(toPath.constData(), 0600) != 0)
    {
        why = QString("mkfifo(%1) failed: %2").arg(fifoToChild, errorText(errno));
        fifoToChild.clear();
        fifoFromChild.clear();
        return false;
    }

    if (::mkfifo(fromPath.constData(), 0600) != 0)
    {
        why = QString("mkfifo(%1) failed: %2").arg(fifoFromChild, errorText(errno));
        return false;
    }

    // Open our reading end first, so the child's open() for writing succeeds at once.
    fromChildFd = ::open(fromPath.constData(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fromChildFd < 0)
    {
        why = "open() of the FIFO failed: " + errorText(errno);
        return false;
    }

    // Our writing end can only be opened after the child opens the other side: see onTick().
    fifoWriterPending = true;
    fifoDeadline = QDateTime::currentMSecsSinceEpoch() + 5000;

    args << "fifo" << fifoToChild << fifoFromChild;
    return true;
}

bool IPCManager::setupSharedMemory(QStringList &args, QString &why)
{
    shmName = QString("/occ_shm_%1").arg(getpid());
    const QByteArray name = shmName.toLocal8Bit();

    ::shm_unlink(name.constData());     // remove leftovers of an earlier crash

    const int fd = ::shm_open(name.constData(), O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd < 0)
    {
        why = "shm_open() failed: " + errorText(errno);
        shmName.clear();
        return false;
    }

    if (::ftruncate(fd, sizeof(SharedBlock)) != 0)
    {
        why = "ftruncate() failed: " + errorText(errno);
        ::close(fd);
        return false;
    }

    void *memory = ::mmap(nullptr, sizeof(SharedBlock), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    const int err = errno;
    ::close(fd);        // the mapping stays valid after the descriptor is closed

    if (memory == MAP_FAILED)
    {
        why = "mmap() failed: " + errorText(err);
        return false;
    }

    std::memset(memory, 0, sizeof(SharedBlock));
    shm = new (memory) SharedBlock();
    shmSendSeq = 0;
    shmLastReceived = 0;

    args << "shm" << shmName;
    return true;
}

bool IPCManager::setupMessageQueue(QStringList &args, QString &why)
{
    mqToName = QString("/occ_mq_to_%1").arg(getpid());
    mqFromName = QString("/occ_mq_from_%1").arg(getpid());

    const QByteArray toName = mqToName.toLocal8Bit();
    const QByteArray fromName = mqFromName.toLocal8Bit();

    ::mq_unlink(toName.constData());    // remove leftovers of an earlier crash
    ::mq_unlink(fromName.constData());

    mq_attr attributes;
    std::memset(&attributes, 0, sizeof(attributes));
    attributes.mq_maxmsg = 8;
    attributes.mq_msgsize = static_cast<long>(kIpcMessageSize);

    // O_NONBLOCK: a full queue gives EAGAIN instead of freezing the window.
    mqTo = ::mq_open(toName.constData(), O_WRONLY | O_CREAT | O_EXCL | O_NONBLOCK, 0600, &attributes);
    if (mqTo == static_cast<mqd_t>(-1))
    {
        why = "mq_open() failed: " + errorText(errno);
        mqToName.clear();
        mqFromName.clear();
        return false;
    }

    mqFrom = ::mq_open(fromName.constData(), O_RDONLY | O_CREAT | O_EXCL | O_NONBLOCK, 0600, &attributes);
    if (mqFrom == static_cast<mqd_t>(-1))
    {
        why = "mq_open() failed: " + errorText(errno);
        mqFromName.clear();
        return false;
    }

    args << "mq" << mqToName << mqFromName;
    return true;
}

// ---------------------------------------------------------------------------
// Start / stop
// ---------------------------------------------------------------------------
void IPCManager::startChannel()
{
    if (running)
        return;

    const QString helper = QCoreApplication::applicationDirPath() + "/os-control-ipc-child";

    if (!QFileInfo(helper).isExecutable())
    {
        report("The child program was not found: " + helper +
               "\nBuild the whole project again with: cmake --build build", true);
        return;
    }

    mechanism = static_cast<Mechanism>(mechanismCombo->currentIndex());
    incoming.clear();
    tickCount = 0;

    QStringList args;
    QVector<QPair<int, int>> dups;
    QString why;
    bool ok = false;

    switch (mechanism)
    {
    case Mechanism::Pipe:         ok = setupPipe(args, dups, why); break;
    case Mechanism::Fifo:         ok = setupFifo(args, why); break;
    case Mechanism::SharedMemory: ok = setupSharedMemory(args, why); break;
    case Mechanism::MessageQueue: ok = setupMessageQueue(args, why); break;
    }

    if (!ok)
    {
        releaseResources();
        report(why, true);
        return;
    }

    pid_t pid = -1;
    int err = 0;

    if (!spawnChild(helper, args, dups, pid, err))
    {
        releaseResources();
        report("Could not start the child process: " + errorText(err), true);
        return;
    }

    // The parent must close the child's pipe ends, or end-of-file could never be detected.
    if (childReadFd >= 0)  { ::close(childReadFd);  childReadFd = -1; }
    if (childWriteFd >= 0) { ::close(childWriteFd); childWriteFd = -1; }

    childPid = pid;
    running = true;
    pollTimer->start();

    log->clear();
    logEvent(QString("fork() + execv() created the child process, PID %1 (parent: PID %2).")
                 .arg(childPid).arg(getpid()));

    const QString parentPid = QString::number(getpid());
    const QString child = QString::number(childPid);

    logEvent("Check it yourself in a terminal:");
    logEvent(QString("    ps -o pid,ppid,stat,cmd -p %1,%2").arg(parentPid, child));

    switch (mechanism)
    {
    case Mechanism::Pipe:
        logEvent(QString("    ls -l /proc/%1/fd        (descriptors 3 and 4 are the pipes)").arg(child));
        break;
    case Mechanism::Fifo:
        logEvent("    ls -l /tmp/occ_fifo_*        (the first letter p means: named pipe)");
        break;
    case Mechanism::SharedMemory:
        logEvent("    ls -l /dev/shm/occ_shm_*");
        logEvent(QString("    grep occ_shm /proc/%1/maps /proc/%2/maps     (both map the same object)")
                     .arg(parentPid, child));
        break;
    case Mechanism::MessageQueue:
        logEvent("    ls -l /dev/mqueue/           (if empty: sudo mount -t mqueue none /dev/mqueue)");
        logEvent(QString("    cat /dev/mqueue/occ_mq_to_%1").arg(parentPid));
        break;
    }

    logEvent("--------------------------------------------------------------");

    updateButtons();
    updateInfo();
    report(QString("Channel open. Child process PID %1 is running.").arg(childPid));
    messageEdit->setFocus();
}

// Wait up to timeoutMs for the child to end. Returns true once it has been reaped.
bool IPCManager::reapChild(int timeoutMs, int &status)
{
    status = 0;

    if (childPid <= 0)
        return true;

    int waited = 0;

    for (;;)
    {
        const pid_t result = ::waitpid(childPid, &status, WNOHANG);

        if (result == childPid)
            return true;

        if (result < 0 && errno != EINTR)
        {
            status = 0;     // not our child any more: nothing left to wait for
            return true;
        }

        if (waited >= timeoutMs)
            return false;

        pauseMilliseconds(10);
        waited += 10;
    }
}

void IPCManager::sendQuitMessage()
{
    int err = 0;

    switch (mechanism)
    {
    case Mechanism::Pipe:
    case Mechanism::Fifo:
        if (toChildFd >= 0)
            writeLine(toChildFd, QByteArray("quit\n"), err);
        break;

    case Mechanism::SharedMemory:
        if (shm)
        {
            std::strncpy(shm->toChild, "quit", kIpcMessageSize - 1);
            shm->toChild[kIpcMessageSize - 1] = '\0';
            shm->toChildSeq.store(++shmSendSeq, std::memory_order_release);
        }
        break;

    case Mechanism::MessageQueue:
        if (mqTo != static_cast<mqd_t>(-1))
            ::mq_send(mqTo, "quit", 4, 31);     // highest priority: jumps the queue
        break;
    }
}

void IPCManager::stopChannel()
{
    if (!running)
        return;

    logEvent("Stopping: asking the child to quit.");

    ::kill(childPid, SIGCONT);      // a suspended child could never read "quit"
    sendQuitMessage();

    int status = 0;
    bool exited = reapChild(1500, status);

    if (!exited)
    {
        logEvent("The child did not answer. Sending SIGTERM.");
        ::kill(childPid, SIGTERM);
        exited = reapChild(1000, status);
    }

    if (!exited)
    {
        logEvent("The child ignored SIGTERM. Sending SIGKILL.");
        ::kill(childPid, SIGKILL);
        exited = reapChild(2000, status);
    }

    pollIncoming();                 // show the child's last words
    handleChildExit(exited ? status : 0);
}

// The child has ended (we already reaped it with waitpid): report it and clean up.
void IPCManager::handleChildExit(int status)
{
    QString how;

    if (WIFEXITED(status))
        how = QString("exited normally with code %1").arg(WEXITSTATUS(status));
    else if (WIFSIGNALED(status))
        how = QString("was killed by signal %1 (%2)")
                  .arg(WTERMSIG(status)).arg(QString::fromLocal8Bit(strsignal(WTERMSIG(status))));
    else
        how = "has ended";

    logEvent(QString("waitpid(): child PID %1 %2.").arg(childPid).arg(how));

    pollTimer->stop();
    releaseResources();

    logEvent("All IPC objects were removed (close / unlink / munmap / shm_unlink / mq_unlink).");

    running = false;
    childPid = -1;

    updateButtons();
    updateInfo();
    report("Channel closed. The child process has ended.");
}

// Close descriptors and DELETE the kernel objects, so nothing is left behind.
void IPCManager::releaseResources()
{
    for (int *fd : {&toChildFd, &fromChildFd, &childReadFd, &childWriteFd})
    {
        if (*fd >= 0)
        {
            ::close(*fd);
            *fd = -1;
        }
    }

    if (!fifoToChild.isEmpty())
        ::unlink(QFile::encodeName(fifoToChild).constData());
    if (!fifoFromChild.isEmpty())
        ::unlink(QFile::encodeName(fifoFromChild).constData());
    fifoToChild.clear();
    fifoFromChild.clear();
    fifoWriterPending = false;

    if (shm)
    {
        ::munmap(shm, sizeof(SharedBlock));
        shm = nullptr;
    }
    if (!shmName.isEmpty())
    {
        ::shm_unlink(shmName.toLocal8Bit().constData());
        shmName.clear();
    }

    if (mqTo != static_cast<mqd_t>(-1))
    {
        ::mq_close(mqTo);
        mqTo = static_cast<mqd_t>(-1);
    }
    if (mqFrom != static_cast<mqd_t>(-1))
    {
        ::mq_close(mqFrom);
        mqFrom = static_cast<mqd_t>(-1);
    }
    if (!mqToName.isEmpty())
        ::mq_unlink(mqToName.toLocal8Bit().constData());
    if (!mqFromName.isEmpty())
        ::mq_unlink(mqFromName.toLocal8Bit().constData());
    mqToName.clear();
    mqFromName.clear();

    incoming.clear();
}

// ---------------------------------------------------------------------------
// Sending
// ---------------------------------------------------------------------------
void IPCManager::sendMessage()
{
    if (!running)
    {
        report("Start a channel first.", true);
        return;
    }

    const QString text = sanitizedMessage();
    if (text.isEmpty())
    {
        report("Type a message first.", true);
        return;
    }

    const QByteArray bytes = text.toUtf8();
    int err = 0;

    switch (mechanism)
    {
    case Mechanism::Pipe:
    case Mechanism::Fifo:
    {
        if (toChildFd < 0)
        {
            report("The channel is not ready yet: waiting for the child to open the FIFO.", true);
            return;
        }

        QByteArray line = bytes;
        line.append('\n');

        if (!writeLine(toChildFd, line, err))
        {
            report(err == EAGAIN ? "The pipe is full: the child is not reading."
                                 : "write() failed: " + errorText(err), true);
            return;
        }

        logSent(text);
        break;
    }

    case Mechanism::SharedMemory:
    {
        // Stop-and-wait: the child must have read the previous message first.
        if (shm->toChildAck.load(std::memory_order_acquire) != shmSendSeq)
        {
            report("The child has not read the previous message yet. Try again in a moment.", true);
            return;
        }

        std::strncpy(shm->toChild, bytes.constData(), kIpcMessageSize - 1);
        shm->toChild[kIpcMessageSize - 1] = '\0';
        shm->toChildSeq.store(++shmSendSeq, std::memory_order_release);

        logSent(text + QString("   [written into shared memory, sequence %1]").arg(shmSendSeq));
        break;
    }

    case Mechanism::MessageQueue:
    {
        const unsigned int priority = static_cast<unsigned int>(prioritySpin->value());

        if (::mq_send(mqTo, bytes.constData(), static_cast<size_t>(bytes.size()), priority) != 0)
        {
            err = errno;
            report(err == EAGAIN ? "The queue is full (8 messages): the child is not reading."
                                 : "mq_send() failed: " + errorText(err), true);
            return;
        }

        logSent(text + QString("   [priority %1]").arg(priority));
        break;
    }
    }

    messageEdit->clear();
    report("Message sent. Waiting for the child's answer...");
}

// Suspend the child, queue three messages with different priorities, resume it.
void IPCManager::priorityDemo()
{
    if (!running || mechanism != Mechanism::MessageQueue)
    {
        report("Start a Message queue channel first.", true);
        return;
    }

    logEvent("--- Priority demo ---");

    if (::kill(childPid, SIGSTOP) != 0)
    {
        report("kill(SIGSTOP) failed: " + errorText(errno), true);
        return;
    }

    // Wait until the kernel has really stopped it, so it cannot read anything early.
    bool stopped = false;
    for (int i = 0; i < 200 && !stopped; ++i)
    {
        if (processStateLetter(childPid) == QChar('T'))
            stopped = true;
        else
            pauseMilliseconds(5);
    }

    if (!stopped)
    {
        ::kill(childPid, SIGCONT);
        report("The child could not be suspended.", true);
        return;
    }

    logEvent(QString("Child PID %1 is suspended (SIGSTOP, state T). Sending three messages:").arg(childPid));

    struct Demo { const char *text; unsigned int priority; };
    const Demo demo[] = {
        {"first sent - LOW priority", 1},
        {"second sent - HIGH priority", 9},
        {"third sent - MEDIUM priority", 5},
    };

    for (const Demo &item : demo)
    {
        if (::mq_send(mqTo, item.text, std::strlen(item.text), item.priority) == 0)
            logSent(QString("%1   [priority %2]").arg(QString::fromLatin1(item.text)).arg(item.priority));
        else
            logEvent("mq_send() failed: " + errorText(errno));
    }

    mq_attr attributes;
    if (::mq_getattr(mqTo, &attributes) == 0)
        logEvent(QString("%1 message(s) are waiting inside the kernel queue.").arg(attributes.mq_curmsgs));

    ::kill(childPid, SIGCONT);
    logEvent("SIGCONT sent. The child wakes up. Watch the ORDER of its answers: highest priority first.");
    report("Priority demo started.");
}

// ---------------------------------------------------------------------------
// Receiving
// ---------------------------------------------------------------------------
void IPCManager::onTick()
{
    if (!running)
        return;

    ++tickCount;

    // FIFO: open our writing end as soon as the child is waiting on the other side.
    if (fifoWriterPending)
    {
        const int fd = ::open(QFile::encodeName(fifoToChild).constData(),
                              O_WRONLY | O_NONBLOCK | O_CLOEXEC);

        if (fd >= 0)
        {
            toChildFd = fd;
            fifoWriterPending = false;
            logEvent("The child opened the FIFO. The channel is ready in both directions.");
        }
        else if (errno != ENXIO || QDateTime::currentMSecsSinceEpoch() > fifoDeadline)
        {
            const int err = errno;
            report("Could not open the FIFO for writing: " + errorText(err), true);
            stopChannel();
            return;
        }
    }

    pollIncoming();

    int status = 0;
    if (reapChild(0, status))
    {
        pollIncoming();
        handleChildExit(status);
        return;
    }

    if (tickCount % 10 == 0)
        updateInfo();
}

void IPCManager::pollIncoming()
{
    switch (mechanism)
    {
    case Mechanism::Pipe:
    case Mechanism::Fifo:         pollStream(); break;
    case Mechanism::SharedMemory: pollSharedMemory(); break;
    case Mechanism::MessageQueue: pollMessageQueue(); break;
    }
}

void IPCManager::pollStream()
{
    if (fromChildFd < 0)
        return;

    char buffer[2048];

    for (;;)
    {
        const ssize_t count = ::read(fromChildFd, buffer, sizeof(buffer));

        if (count > 0)
        {
            incoming.append(buffer, static_cast<qsizetype>(count));
            continue;
        }

        if (count < 0 && errno == EINTR)
            continue;

        break;      // nothing more right now (EAGAIN), or no writer yet
    }

    // Split the byte stream into messages at each newline.
    int newline;
    while ((newline = static_cast<int>(incoming.indexOf('\n'))) >= 0)
    {
        const QByteArray line = incoming.left(newline);
        incoming.remove(0, newline + 1);
        logReceived(QString::fromUtf8(line));
    }
}

void IPCManager::pollSharedMemory()
{
    if (!shm)
        return;

    const unsigned int seq = shm->toParentSeq.load(std::memory_order_acquire);

    if (seq == shmLastReceived)
        return;

    char message[kIpcMessageSize];
    std::memcpy(message, shm->toParent, sizeof(message));
    message[sizeof(message) - 1] = '\0';

    shmLastReceived = seq;
    shm->toParentAck.store(seq, std::memory_order_release);

    logReceived(QString::fromUtf8(message) + QString("   [read from shared memory, sequence %1]").arg(seq));
}

void IPCManager::pollMessageQueue()
{
    if (mqFrom == static_cast<mqd_t>(-1))
        return;

    char buffer[kIpcMessageSize + 1];

    for (;;)
    {
        unsigned int priority = 0;
        const ssize_t count = ::mq_receive(mqFrom, buffer, kIpcMessageSize, &priority);

        if (count < 0)
            break;      // EAGAIN: the queue is empty

        buffer[count] = '\0';
        logReceived(QString::fromUtf8(buffer));
    }
}