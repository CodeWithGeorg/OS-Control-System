#include "ProcessManager.h"

#include <QAbstractItemView>
#include <QCheckBox>
#include <QDir>
#include <QFileInfo>
#include <QFile>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QScrollBar>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTimer>
#include <QVBoxLayout>
#include <QVector>
#include <QByteArray>

#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <pwd.h>
#include <sys/wait.h>
#include <unistd.h>

// Location of the privileged helper. It must be owned by root (see privileged/install.sh);
// PolicyKit only authorizes this exact path.
static const char *const kHelperPath =
    "/usr/local/libexec/os-control-center/os-control-helper";

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------
ProcessManager::ProcessManager(QWidget *parent)
    : QWidget(parent)
{
    setupUI();
    loadProcesses();

    refreshTimer = new QTimer(this);
    connect(refreshTimer, &QTimer::timeout, this, &ProcessManager::onTimerTick);
    refreshTimer->start(3000);   // controlled by the "Auto refresh" checkbox
}

// ---------------------------------------------------------------------------
// User interface
// ---------------------------------------------------------------------------
void ProcessManager::setupUI()
{
    QVBoxLayout *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(35, 30, 35, 30);
    mainLayout->setSpacing(12);

    QLabel *heading = new QLabel("Process Management");
    heading->setStyleSheet("font-size: 25px; font-weight: bold;");

    QLabel *description = new QLabel("Monitor and control running Linux processes.");
    description->setStyleSheet("color: #6b7280;");

    processTable = new QTableWidget(0, 4);
    processTable->setHorizontalHeaderLabels({"PID", "Process Name", "State", "User"});
    processTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    processTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    processTable->verticalHeader()->setVisible(false);
    processTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    processTable->setSelectionMode(QAbstractItemView::SingleSelection);
    processTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    processTable->setSortingEnabled(true);
    processTable->sortByColumn(0, Qt::AscendingOrder);

    startButton     = new QPushButton("Start Process");
    stopButton      = new QPushButton("Stop (SIGTERM)");
    forceStopButton = new QPushButton("Force Stop (SIGKILL)");
    suspendButton   = new QPushButton("Suspend (SIGSTOP)");
    resumeButton    = new QPushButton("Resume (SIGCONT)");
    refreshButton   = new QPushButton("Refresh");
    autoRefreshCheck = new QCheckBox("Auto refresh (3 s)");
    autoRefreshCheck->setChecked(true);

    QHBoxLayout *buttonLayout = new QHBoxLayout;
    buttonLayout->addWidget(startButton);
    buttonLayout->addWidget(stopButton);
    buttonLayout->addWidget(forceStopButton);
    buttonLayout->addWidget(suspendButton);
    buttonLayout->addWidget(resumeButton);
    buttonLayout->addStretch();
    buttonLayout->addWidget(autoRefreshCheck);
    buttonLayout->addWidget(refreshButton);

    statusLabel = new QLabel("Ready.");
    statusLabel->setWordWrap(true);
    statusLabel->setStyleSheet("color: #374151; font-size: 12px;");

    mainLayout->addWidget(heading);
    mainLayout->addWidget(description);
    mainLayout->addWidget(processTable, 1);
    mainLayout->addLayout(buttonLayout);
    mainLayout->addWidget(statusLabel);

    connect(processTable, &QTableWidget::itemSelectionChanged,
            this, &ProcessManager::onSelectionChanged);

    connect(refreshButton,   &QPushButton::clicked, this, &ProcessManager::loadProcesses);
    connect(startButton,     &QPushButton::clicked, this, &ProcessManager::startProcess);
    connect(stopButton,      &QPushButton::clicked, this, &ProcessManager::stopProcess);
    connect(forceStopButton, &QPushButton::clicked, this, &ProcessManager::forceStopProcess);
    connect(suspendButton,   &QPushButton::clicked, this, &ProcessManager::suspendProcess);
    connect(resumeButton,    &QPushButton::clicked, this, &ProcessManager::resumeProcess);

    setStyleSheet(
        "QPushButton {"
        "  background-color: #2563eb; color: white;"
        "  padding: 9px 12px; border: none; border-radius: 5px;"
        "}"
        "QPushButton:hover { background-color: #1d4ed8; }"
    );
}

void ProcessManager::setStatus(const QString &message, bool error)
{
    statusLabel->setStyleSheet(
        error ? "color: #b91c1c; font-size: 12px; font-weight: bold;"
              : "color: #166534; font-size: 12px;");
    statusLabel->setText(message);
}

// ---------------------------------------------------------------------------
// /proc helpers
// ---------------------------------------------------------------------------

// /proc/<pid>/stat looks like:  1234 (some name) S 1 ... 
// The name may contain spaces and ')' so we must split after the LAST ')'.
// After that, the fields are numbered from field 3 (state).
// Field 4 = ppid (index 1), field 22 = starttime (index 19).
static bool readStatFields(pid_t pid, QString &name, QStringList &fields)
{
    QFile file("/proc/" + QString::number(pid) + "/stat");
    if (!file.open(QIODevice::ReadOnly))
        return false;

    const QString text = QString::fromLocal8Bit(file.readAll());
    const int open = text.indexOf('(');
    const int close = text.lastIndexOf(')');
    if (open < 0 || close < open)
        return false;

    name = text.mid(open + 1, close - open - 1);
    fields = text.mid(close + 1).trimmed().split(' ', Qt::SkipEmptyParts);
    return fields.size() > 19;
}

bool ProcessManager::readStartTime(pid_t pid, qulonglong &startTime)
{
    QString name;
    QStringList fields;
    if (!readStatFields(pid, name, fields))
        return false;

    bool ok = false;
    startTime = fields[19].toULongLong(&ok);
    return ok;
}

bool ProcessManager::readParentPid(pid_t pid, pid_t &parentPid)
{
    QString name;
    QStringList fields;
    if (!readStatFields(pid, name, fields))
        return false;

    bool ok = false;
    parentPid = static_cast<pid_t>(fields[1].toLongLong(&ok));
    return ok;
}

QString ProcessManager::describeState(QChar code)
{
    switch (code.toLatin1())
    {
    case 'R': return "Running";
    case 'S': return "Sleeping";
    case 'D': return "Disk sleep";
    case 'T': return "Stopped";
    case 't': return "Tracing stop";
    case 'Z': return "Zombie";
    case 'X': return "Dead";
    case 'I': return "Idle";
    default:  return QString(code);
    }
}

QString ProcessManager::userName(uid_t uid)
{
    const passwd *entry = getpwuid(uid);
    if (entry)
        return QString::fromLocal8Bit(entry->pw_name);
    return QString::number(uid);
}

// ---------------------------------------------------------------------------
// Process list
// ---------------------------------------------------------------------------
void ProcessManager::loadProcesses()
{
    // Block signals so rebuilding the table does not overwrite 'selected'.
    // The selection is remembered by identity (PID + start time), not by row.
    processTable->blockSignals(true);
    processTable->setUpdatesEnabled(false);
    processTable->setSortingEnabled(false);   // sorting while inserting reorders rows

    const int scrollPosition = processTable->verticalScrollBar()->value();
    const int sortColumn = processTable->horizontalHeader()->sortIndicatorSection();
    const Qt::SortOrder sortOrder = processTable->horizontalHeader()->sortIndicatorOrder();

    processTable->clearContents();
    processTable->setRowCount(0);

    bool selectedStillExists = false;
    int selectedRow = -1;

    const QStringList entries = QDir("/proc").entryList(QDir::Dirs | QDir::NoDotAndDotDot);

    for (const QString &entry : entries)
    {
        bool ok = false;
        const pid_t pid = static_cast<pid_t>(entry.toLongLong(&ok));
        if (!ok || pid <= 0)
            continue;

        QString name;
        QStringList fields;
        if (!readStatFields(pid, name, fields))
            continue;   // process exited while we were reading

        const QString state = describeState(fields[0].at(0));
        const qulonglong startTime = fields[19].toULongLong();

        // Owner: first number on the "Uid:" line is the real UID.
        QString user = "?";
        QFile statusFile("/proc/" + entry + "/status");
        if (statusFile.open(QIODevice::ReadOnly))
        {
            const QList<QByteArray> lines = statusFile.readAll().split('\n');
            for (const QByteArray &line : lines)
            {
                if (line.startsWith("Uid:"))
                {
                    const QList<QByteArray> parts = line.simplified().split(' ');
                    if (parts.size() >= 2)
                    {
                        const uid_t uid = parts[1].toUInt();
                        user = userName(uid) + " (" + QString::number(uid) + ")";
                    }
                    break;
                }
            }
        }

        const int row = processTable->rowCount();
        processTable->insertRow(row);

        // Numeric PID so sorting by PID is numeric, not alphabetical
        QTableWidgetItem *pidItem = new QTableWidgetItem;
        pidItem->setData(Qt::DisplayRole, static_cast<qlonglong>(pid));
        pidItem->setData(Qt::UserRole, static_cast<qulonglong>(startTime));

        processTable->setItem(row, 0, pidItem);
        processTable->setItem(row, 1, new QTableWidgetItem(name));
        processTable->setItem(row, 2, new QTableWidgetItem(state));
        processTable->setItem(row, 3, new QTableWidgetItem(user));

        if (selected.isValid() && pid == selected.pid && startTime == selected.startTime)
            selectedStillExists = true;
    }

    processTable->setSortingEnabled(true);
    processTable->sortByColumn(sortColumn, sortOrder);

    // Find the selected process again AFTER sorting (row numbers changed)
    if (selectedStillExists)
    {
        for (int row = 0; row < processTable->rowCount(); ++row)
        {
            const QTableWidgetItem *item = processTable->item(row, 0);
            if (item->data(Qt::DisplayRole).toLongLong() == selected.pid &&
                item->data(Qt::UserRole).toULongLong() == selected.startTime)
            {
                selectedRow = row;
                break;
            }
        }
    }

    if (selectedRow >= 0)
        processTable->selectRow(selectedRow);
    else
        selected.clear();   // the process is gone (or its PID was reused)

    processTable->verticalScrollBar()->setValue(scrollPosition);
    processTable->setUpdatesEnabled(true);
    processTable->blockSignals(false);
}

// The user clicked a row: remember WHICH process it is.
void ProcessManager::onSelectionChanged()
{
    const QModelIndexList rows = processTable->selectionModel()->selectedRows(0);
    if (rows.isEmpty())
    {
        selected.clear();
        return;
    }

    const QTableWidgetItem *item = processTable->item(rows.first().row(), 0);
    if (!item)
    {
        selected.clear();
        return;
    }

    selected.pid = static_cast<pid_t>(item->data(Qt::DisplayRole).toLongLong());
    selected.startTime = item->data(Qt::UserRole).toULongLong();
}

void ProcessManager::onTimerTick()
{
    reapChildren();                 // always clean up our children (no zombies)

    if (autoRefreshCheck->isChecked())
        loadProcesses();
}

// ---------------------------------------------------------------------------
// Child processes: waitpid() so finished children do not stay as zombies
// ---------------------------------------------------------------------------
void ProcessManager::reapChildren()
{
    for (auto it = childProcesses.begin(); it != childProcesses.end(); )
    {
        int status = 0;
        const pid_t result = waitpid(*it, &status, WNOHANG);

        if (result == 0)
        {
            ++it;           // still running
            continue;
        }

        if (result == *it)
        {
            if (WIFEXITED(status))
                setStatus(QString("Child PID %1 exited with code %2.")
                              .arg(*it).arg(WEXITSTATUS(status)));
            else if (WIFSIGNALED(status))
                setStatus(QString("Child PID %1 was killed by signal %2 (%3).")
                              .arg(*it).arg(WTERMSIG(status))
                              .arg(QString::fromLocal8Bit(strsignal(WTERMSIG(status)))));
        }

        // result == pid (reaped) or result == -1 (no such child): forget it
        it = childProcesses.erase(it);
    }
}

// ---------------------------------------------------------------------------
// Start a process: fork() + execvp()
// ---------------------------------------------------------------------------
void ProcessManager::startProcess()
{
    bool accepted = false;
    const QString command = QInputDialog::getText(
        this, "Start Process",
        "Enter command (example: sleep 300):",
        QLineEdit::Normal, "", &accepted);

    if (!accepted || command.trimmed().isEmpty())
        return;

    const QStringList arguments = QProcess::splitCommand(command);
    if (arguments.isEmpty())
        return;

    // Build argv BEFORE fork(): the child must not allocate memory.
    QVector<QByteArray> argumentBytes;
    for (const QString &argument : arguments)
        argumentBytes.append(argument.toLocal8Bit());

    QVector<char *> argv;
    for (QByteArray &argument : argumentBytes)
        argv.append(argument.data());
    argv.append(nullptr);

    // Pipe used by the child to report an exec failure to the parent.
    // O_CLOEXEC closes it automatically when exec succeeds.
    int errorPipe[2];
    if (pipe2(errorPipe, O_CLOEXEC) < 0)
    {
        setStatus("pipe2() failed: " + QString::fromLocal8Bit(strerror(errno)), true);
        return;
    }

    const pid_t pid = fork();

    if (pid < 0)
    {
        const int err = errno;
        ::close(errorPipe[0]);
        ::close(errorPipe[1]);
        setStatus("fork() failed: " + QString::fromLocal8Bit(strerror(err)), true);
        return;
    }

    if (pid == 0)
    {
        // ----- CHILD: only async-signal-safe calls from here -----
        setsid();   // own session: not tied to the GUI's terminal

        const int devNull = open("/dev/null", O_RDONLY);
        if (devNull >= 0)
        {
            dup2(devNull, STDIN_FILENO);
            if (devNull != STDIN_FILENO)
                ::close(devNull);   // do not leak an extra descriptor into the new program
        }

        execvp(argv[0], argv.data());

        // Only reached if execvp failed: tell the parent why.
        const int err = errno;
        const ssize_t ignored = write(errorPipe[1], &err, sizeof(err));
        (void)ignored;
        _exit(127);
    }

    // ----- PARENT -----
    ::close(errorPipe[1]);

    int execError = 0;
    ssize_t bytesRead;
    do {
        bytesRead = read(errorPipe[0], &execError, sizeof(execError));
    } while (bytesRead < 0 && errno == EINTR);
    ::close(errorPipe[0]);

    if (bytesRead == static_cast<ssize_t>(sizeof(execError)))
    {
        waitpid(pid, nullptr, 0);   // reap the failed child immediately
        setStatus(QString("Cannot run '%1': %2")
                      .arg(arguments.first(),
                           QString::fromLocal8Bit(strerror(execError))), true);
        return;
    }

    childProcesses.insert(pid);
    setStatus(QString("Started '%1' with PID %2.").arg(arguments.first()).arg(pid));
    loadProcesses();
}

// ---------------------------------------------------------------------------
// Sending signals
// ---------------------------------------------------------------------------
void ProcessManager::stopProcess()
{
    sendSignalToSelected(SIGTERM, "Stop (SIGTERM)");
}

void ProcessManager::forceStopProcess()
{
    if (!selected.isValid())
    {
        setStatus("Select a process first.", true);
        return;
    }

    const auto answer = QMessageBox::warning(
        this, "Confirm Force Stop",
        QString("Force-kill PID %1 with SIGKILL?\nThe process cannot save its data.")
            .arg(selected.pid),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);

    if (answer == QMessageBox::Yes)
        sendSignalToSelected(SIGKILL, "Force Stop (SIGKILL)");
}

void ProcessManager::suspendProcess()
{
    sendSignalToSelected(SIGSTOP, "Suspend (SIGSTOP)");
}

void ProcessManager::resumeProcess()
{
    sendSignalToSelected(SIGCONT, "Resume (SIGCONT)");
}

// One function for all four actions. We do NOT check ownership ourselves:
// the kernel does that inside kill() and tells us the reason through errno.
void ProcessManager::sendSignalToSelected(int signalNumber, const QString &actionName)
{
    if (!selected.isValid())
    {
        setStatus("Select a process first.", true);
        return;
    }

    const pid_t pid = selected.pid;

    // Safety: never signal ourselves, init, or kernel threads.
    if (pid == 1 || pid == getpid())
    {
        setStatus("Refused: PID " + QString::number(pid) +
                  " is protected (init or this application).", true);
        return;
    }

    pid_t parent = 0;
    if (pid == 2 || (readParentPid(pid, parent) && parent == 2))
    {
        setStatus("Refused: PID " + QString::number(pid) + " is a kernel thread.", true);
        return;
    }

    // PID-reuse guard: is it still the SAME process we selected?
    qulonglong currentStart = 0;
    if (!readStartTime(pid, currentStart))
    {
        setStatus("PID " + QString::number(pid) + " no longer exists.", true);
        loadProcesses();
        return;
    }
    if (currentStart != selected.startTime)
    {
        setStatus("PID " + QString::number(pid) +
                  " now belongs to a different process. Refresh and select again.", true);
        loadProcesses();
        return;
    }

    if (kill(pid, signalNumber) == 0)
    {
        setStatus(QString("%1 sent to PID %2.").arg(actionName).arg(pid));
    }
    else
    {
        const int err = errno;
        QString reason;

        if (err == EPERM)
            reason = QString("Permission denied. The kernel does not allow user '%1' "
                             "(UID %2) to signal this process. "
                             "Super Admin mode would be needed.")
                         .arg(userName(getuid())).arg(getuid());
        else if (err == ESRCH)
            reason = "The process no longer exists.";
        else
            reason = QString::fromLocal8Bit(strerror(err));

        setStatus(QString("%1 failed for PID %2: %3").arg(actionName).arg(pid).arg(reason), true);

        if (err == EPERM)
        {
            const ProcessIdentity target = selected;

            const auto answer = QMessageBox::question(
                this, "Administrator permission required",
                QString("Linux denied \"%1\" on PID %2.\n\n"
                        "Authenticate as administrator to perform ONLY this one action?")
                    .arg(actionName).arg(target.pid),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No);

            if (answer == QMessageBox::Yes)
                runPrivilegedSignal(target, signalNumber, actionName);
            return;
        }
    }

    // Show the new state (e.g. T = Stopped) shortly after the signal is handled
    QTimer::singleShot(300, this, &ProcessManager::loadProcesses);
}

// ---------------------------------------------------------------------------
// Super Admin: run the helper through pkexec (PolicyKit asks for the password).
// The GUI itself never becomes root. QProcess is asynchronous, so the window
// stays responsive while the password dialog is open.
// ---------------------------------------------------------------------------
void ProcessManager::runPrivilegedSignal(const ProcessIdentity &target,
                                         int signalNumber,
                                         const QString &actionName)
{
    if (privilegedBusy)
    {
        setStatus("An administrator request is already in progress.", true);
        return;
    }

    const QString helperPath = QString::fromLatin1(kHelperPath);

    if (!QFileInfo::exists(helperPath))
    {
        setStatus("Administrator helper is not installed. "
                  "Run:  bash privileged/install.sh", true);
        return;
    }

    // Only these four are ever sent; the helper checks the list again.
    QString signalWord;
    switch (signalNumber)
    {
    case SIGTERM: signalWord = "TERM"; break;
    case SIGKILL: signalWord = "KILL"; break;
    case SIGSTOP: signalWord = "STOP"; break;
    case SIGCONT: signalWord = "CONT"; break;
    default:
        setStatus("Signal not supported in administrator mode.", true);
        return;
    }

    QStringList arguments;
    arguments << helperPath << "signal"
              << QString::number(target.pid)
              << QString::number(target.startTime)
              << signalWord;

    QProcess *helper = new QProcess(this);
    privilegedBusy = true;
    setStatus("Waiting for administrator authentication...");

    connect(helper, &QProcess::errorOccurred, this,
            [this, helper](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
        {
            privilegedBusy = false;
            setStatus("Could not start pkexec. Is it installed?", true);
            helper->deleteLater();
        }
    });

    connect(helper, &QProcess::finished, this,
            [this, helper, target, actionName](int exitCode, QProcess::ExitStatus) {
        privilegedBusy = false;

        const QString message =
            QString::fromLocal8Bit(helper->readAllStandardError()).trimmed();

        if (exitCode == 0)
            setStatus(QString("%1 sent to PID %2 with administrator rights.")
                          .arg(actionName).arg(target.pid));
        else if (exitCode == 126)
            setStatus("Authentication was cancelled. Nothing was changed.", true);
        else if (exitCode == 127)
            setStatus("Not authorized, or no authentication agent is running. " + message, true);
        else
            setStatus(QString("%1 failed for PID %2: %3")
                          .arg(actionName).arg(target.pid)
                          .arg(message.isEmpty()
                                   ? QString("helper exit code %1").arg(exitCode)
                                   : message), true);

        helper->deleteLater();
        QTimer::singleShot(300, this, &ProcessManager::loadProcesses);
    });

    helper->start("pkexec", arguments);
}