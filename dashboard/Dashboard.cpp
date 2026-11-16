#include "Dashboard.h"

#include <QCoreApplication>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QFileInfo>
#include <QFrame>
#include <QGridLayout>
#include <QLabel>
#include <QProgressBar>
#include <QScrollArea>
#include <QShowEvent>
#include <QStandardPaths>
#include <QSysInfo>
#include <QThread>
#include <QTimer>
#include <QVBoxLayout>

#include <pwd.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/utsname.h>
#include <unistd.h>

// Where the Super Admin helper and its PolicyKit rule are installed
static const char *const kHelperPath = "/usr/local/libexec/os-control-center/os-control-helper";
static const char *const kPolicyPath = "/usr/share/polkit-1/actions/org.oscontrol.helper.policy";

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------
Dashboard::Dashboard(QWidget *parent)
    : QWidget(parent)
{
    setupUI();
    refresh();

    timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, [this]() {
        if (isVisible())            // no work while another page is shown
            refresh();
    });
    timer->start(2000);
}

void Dashboard::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    refresh();                      // show fresh numbers the moment the page opens
}

// ---------------------------------------------------------------------------
// User interface
// ---------------------------------------------------------------------------
void Dashboard::setupUI()
{
    QVBoxLayout *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);

    QScrollArea *scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    outer->addWidget(scroll);

    QWidget *content = new QWidget;
    scroll->setWidget(content);

    content->setStyleSheet(
        "QFrame#tile, QFrame#card, QFrame#panel {"
        "  background-color: white; border: 1px solid #e5e7eb; border-radius: 10px;"
        "}"
        "QFrame#card:hover { border: 1px solid #2563eb; }"
    );

    QVBoxLayout *layout = new QVBoxLayout(content);
    layout->setContentsMargins(35, 30, 35, 30);
    layout->setSpacing(16);

    QLabel *heading = new QLabel("System Dashboard");
    heading->setStyleSheet("font-size: 27px; font-weight: bold;");

    QLabel *description = new QLabel(
        "Live view of this computer and of the five operating-system modules. Click a card to open a module.");
    description->setStyleSheet("color: #6b7280; font-size: 13px;");

    layout->addWidget(heading);
    layout->addWidget(description);

    // ----- Information tiles -----
    QGridLayout *tiles = new QGridLayout;
    tiles->setSpacing(12);

    auto makeTile = [&](const QString &title, int row, int column) -> QLabel * {
        QFrame *tile = new QFrame;
        tile->setObjectName("tile");

        QVBoxLayout *tileLayout = new QVBoxLayout(tile);
        tileLayout->setContentsMargins(12, 8, 12, 8);
        tileLayout->setSpacing(2);

        QLabel *caption = new QLabel(title.toUpper());
        caption->setStyleSheet("color: #6b7280; font-size: 10px; font-weight: bold; border: none;");

        QLabel *value = new QLabel("-");
        value->setStyleSheet("font-size: 14px; font-weight: bold; border: none;");
        value->setWordWrap(true);

        tileLayout->addWidget(caption);
        tileLayout->addWidget(value);
        tiles->addWidget(tile, row, column);
        return value;
    };

    hostValue   = makeTile("Computer", 0, 0);
    osValue     = makeTile("Operating system", 0, 1);
    kernelValue = makeTile("Kernel", 0, 2);
    userValue   = makeTile("Signed in as", 1, 0);
    uptimeValue = makeTile("Uptime", 1, 1);
    loadValue   = makeTile("Load average (1, 5, 15 min)", 1, 2);

    layout->addLayout(tiles);

    // ----- Memory gauge -----
    QFrame *memoryPanel = new QFrame;
    memoryPanel->setObjectName("panel");

    QVBoxLayout *memoryLayout = new QVBoxLayout(memoryPanel);
    memoryLayout->setContentsMargins(12, 8, 12, 10);
    memoryLayout->setSpacing(4);

    QLabel *memoryCaption = new QLabel("SYSTEM MEMORY");
    memoryCaption->setStyleSheet("color: #6b7280; font-size: 10px; font-weight: bold; border: none;");

    memoryBar = new QProgressBar;
    memoryBar->setRange(0, 100);
    memoryBar->setTextVisible(false);
    memoryBar->setFixedHeight(10);
    memoryBar->setStyleSheet(
        "QProgressBar { background: #e5e7eb; border: none; border-radius: 5px; }"
        "QProgressBar::chunk { background: #2563eb; border-radius: 5px; }");

    memoryText = new QLabel("-");
    memoryText->setStyleSheet("font-size: 12px; border: none;");

    memoryLayout->addWidget(memoryCaption);
    memoryLayout->addWidget(memoryBar);
    memoryLayout->addWidget(memoryText);
    layout->addWidget(memoryPanel);

    // ----- Module cards -----
    QLabel *modulesHeading = new QLabel("Modules");
    modulesHeading->setStyleSheet("font-size: 16px; font-weight: bold;");
    layout->addWidget(modulesHeading);

    const QStringList titles = {
        "Process Management", "Memory Management", "I/O Management",
        "File Subsystem", "IPC Management"
    };

    const QStringList descriptions = {
        "List, start, stop, suspend and resume processes",
        "Allocate and release memory with malloc and mmap",
        "Work with file descriptors, pipes and ioctl",
        "Create, read, rename, delete and inspect files",
        "Talk to a child process with pipes, shared memory and queues"
    };

    QGridLayout *cards = new QGridLayout;
    cards->setSpacing(12);

    for (int i = 0; i < kModuleCount; ++i)
    {
        QFrame *card = new QFrame;
        card->setObjectName("card");
        card->setCursor(Qt::PointingHandCursor);
        card->installEventFilter(this);
        cardIndex.insert(card, i + 1);          // page 0 is the dashboard itself

        QVBoxLayout *cardLayout = new QVBoxLayout(card);
        cardLayout->setContentsMargins(14, 10, 14, 10);
        cardLayout->setSpacing(3);

        QLabel *title = new QLabel(titles[i]);
        title->setStyleSheet("font-size: 15px; font-weight: bold; border: none;");

        QLabel *info = new QLabel(descriptions[i]);
        info->setStyleSheet("color: #6b7280; font-size: 12px; border: none;");
        info->setWordWrap(true);

        moduleLive[i] = new QLabel("-");
        moduleLive[i]->setStyleSheet("color: #2563eb; font-size: 12px; font-weight: bold; border: none;");
        moduleLive[i]->setWordWrap(true);

        // Let clicks on the labels reach the card
        title->setAttribute(Qt::WA_TransparentForMouseEvents);
        info->setAttribute(Qt::WA_TransparentForMouseEvents);
        moduleLive[i]->setAttribute(Qt::WA_TransparentForMouseEvents);

        cardLayout->addWidget(title);
        cardLayout->addWidget(info);
        cardLayout->addWidget(moduleLive[i]);

        cards->addWidget(card, i / 2, i % 2);
    }

    layout->addLayout(cards);

    // ----- Readiness checklist -----
    QLabel *checksHeading = new QLabel("System readiness");
    checksHeading->setStyleSheet("font-size: 16px; font-weight: bold;");
    layout->addWidget(checksHeading);

    QFrame *checksPanel = new QFrame;
    checksPanel->setObjectName("panel");

    QVBoxLayout *checksLayout = new QVBoxLayout(checksPanel);
    checksLayout->setContentsMargins(14, 10, 14, 10);
    checksLayout->setSpacing(4);

    for (int i = 0; i < kCheckCount; ++i)
    {
        checkLabel[i] = new QLabel("-");
        checkLabel[i]->setStyleSheet("font-size: 12px; border: none;");
        checksLayout->addWidget(checkLabel[i]);
    }

    layout->addWidget(checksPanel);

    banner = new QLabel;
    banner->setWordWrap(true);
    layout->addWidget(banner);

    layout->addStretch();
}

bool Dashboard::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() == QEvent::MouseButtonRelease)
    {
        const auto it = cardIndex.constFind(watched);
        if (it != cardIndex.constEnd())
        {
            emit moduleRequested(it.value());
            return true;
        }
    }

    return QWidget::eventFilter(watched, event);
}

// ---------------------------------------------------------------------------
// Formatting helpers
// ---------------------------------------------------------------------------
QString Dashboard::formatDuration(qint64 seconds)
{
    const qint64 days = seconds / 86400;
    const qint64 hours = (seconds % 86400) / 3600;
    const qint64 minutes = (seconds % 3600) / 60;

    if (days > 0)
        return QString("%1 d %2 h %3 min").arg(days).arg(hours).arg(minutes);
    if (hours > 0)
        return QString("%1 h %2 min").arg(hours).arg(minutes);
    return QString("%1 min").arg(minutes);
}

QString Dashboard::formatKiB(qint64 kib)
{
    if (kib < 1024)
        return QString("%1 KiB").arg(kib);
    if (kib < 1024 * 1024)
        return QString("%1 MiB").arg(kib / 1024.0, 0, 'f', 1);
    return QString("%1 GiB").arg(kib / (1024.0 * 1024.0), 0, 'f', 2);
}

// ---------------------------------------------------------------------------
// Live data: everything below is read from the kernel (/proc, uname, statvfs, stat)
// ---------------------------------------------------------------------------
void Dashboard::refresh()
{
    // ----- uname(): computer and kernel -----
    utsname info;
    const bool haveUname = (uname(&info) == 0);

    hostValue->setText(haveUname ? QString::fromLocal8Bit(info.nodename) : "?");
    osValue->setText(QSysInfo::prettyProductName());
    kernelValue->setText(haveUname ? QString("%1 %2").arg(QString::fromLocal8Bit(info.sysname),
                                                           QString::fromLocal8Bit(info.release))
                                   : "?");

    const passwd *entry = getpwuid(getuid());
    userValue->setText(QString("%1 (UID %2)")
                           .arg(entry ? QString::fromLocal8Bit(entry->pw_name) : QString("?"))
                           .arg(getuid()));

    // ----- /proc/uptime and /proc/loadavg -----
    QFile uptimeFile("/proc/uptime");
    if (uptimeFile.open(QIODevice::ReadOnly))
        uptimeValue->setText(formatDuration(static_cast<qint64>(
            QString::fromLatin1(uptimeFile.readAll()).section(' ', 0, 0).toDouble())));

    QFile loadFile("/proc/loadavg");
    if (loadFile.open(QIODevice::ReadOnly))
    {
        const QStringList parts = QString::fromLatin1(loadFile.readAll()).split(' ');
        if (parts.size() >= 3)
            loadValue->setText(QString("%1   %2   %3   (%4 CPUs)")
                                   .arg(parts[0], parts[1], parts[2])
                                   .arg(QThread::idealThreadCount()));
    }

    // ----- /proc/meminfo -----
    qint64 totalKiB = 0;
    qint64 availableKiB = 0;

    QFile memFile("/proc/meminfo");
    if (memFile.open(QIODevice::ReadOnly))
    {
        const QList<QByteArray> lines = memFile.readAll().split('\n');
        for (const QByteArray &line : lines)
        {
            if (line.startsWith("MemTotal:"))
                totalKiB = line.simplified().split(' ').value(1).toLongLong();
            else if (line.startsWith("MemAvailable:"))
                availableKiB = line.simplified().split(' ').value(1).toLongLong();
        }
    }

    if (totalKiB > 0)
    {
        const qint64 usedKiB = totalKiB - availableKiB;
        memoryBar->setValue(static_cast<int>(usedKiB * 100 / totalKiB));
        memoryText->setText(QString("%1 in use of %2   (%3 available)")
                                .arg(formatKiB(usedKiB), formatKiB(totalKiB), formatKiB(availableKiB)));
    }

    // ----- /proc: process count, and is a PolicyKit agent running? -----
    int processCount = 0;
    int ownedCount = 0;
    bool agentRunning = false;

    const QStringList entries = QDir("/proc").entryList(QDir::Dirs | QDir::NoDotAndDotDot);

    for (const QString &name : entries)
    {
        bool numeric = false;
        name.toInt(&numeric);
        if (!numeric)
            continue;

        ++processCount;

        struct stat st;
        const QByteArray path = ("/proc/" + name).toLocal8Bit();
        if (::stat(path.constData(), &st) == 0 && st.st_uid == getuid())
            ++ownedCount;

        if (!agentRunning)
        {
            QFile comm("/proc/" + name + "/comm");
            if (comm.open(QIODevice::ReadOnly))
            {
                const QString process = QString::fromLocal8Bit(comm.readAll()).trimmed();
                if (process.contains("polkit") && !process.startsWith("polkitd"))
                    agentRunning = true;
            }
        }
    }

    // ----- Module cards -----
    moduleLive[0]->setText(QString("%1 processes running, %2 owned by you")
                               .arg(processCount).arg(ownedCount));

    qint64 vmSizeKiB = 0;
    qint64 vmRssKiB = 0;
    QFile selfStatus("/proc/self/status");
    if (selfStatus.open(QIODevice::ReadOnly))
    {
        const QList<QByteArray> lines = selfStatus.readAll().split('\n');
        for (const QByteArray &line : lines)
        {
            if (line.startsWith("VmSize:"))
                vmSizeKiB = line.simplified().split(' ').value(1).toLongLong();
            else if (line.startsWith("VmRSS:"))
                vmRssKiB = line.simplified().split(' ').value(1).toLongLong();
        }
    }
    moduleLive[1]->setText(QString("This application uses %1 of RAM (%2 virtual)")
                               .arg(formatKiB(vmRssKiB), formatKiB(vmSizeKiB)));

    const int descriptorCount =
        QDir("/proc/self/fd").entryList(QDir::NoDotAndDotDot | QDir::AllEntries).size();
    moduleLive[2]->setText(QString("This application has %1 file descriptors open").arg(descriptorCount));

    struct statvfs disk;
    if (statvfs(QDir::homePath().toLocal8Bit().constData(), &disk) == 0)
    {
        const qint64 freeKiB = static_cast<qint64>(disk.f_bavail) * static_cast<qint64>(disk.f_frsize) / 1024;
        const qint64 totalDiskKiB = static_cast<qint64>(disk.f_blocks) * static_cast<qint64>(disk.f_frsize) / 1024;
        moduleLive[3]->setText(QString("Home disk: %1 free of %2")
                                   .arg(formatKiB(freeKiB), formatKiB(totalDiskKiB)));
    }

    const bool childPresent =
        QFileInfo(QCoreApplication::applicationDirPath() + "/os-control-ipc-child").isExecutable();
    const bool queuesSupported = QFileInfo::exists("/proc/sys/fs/mqueue");

    moduleLive[4]->setText(QString("Child program: %1   |   POSIX message queues: %2")
                               .arg(QString(childPresent ? "ready" : "MISSING"),
                                    QString(queuesSupported ? "supported" : "NOT supported")));

    // ----- Readiness checks -----
    struct stat helperStat;
    const bool helperExists = (::stat(kHelperPath, &helperStat) == 0);
    // The helper runs as root, so it must belong to root and nobody else may modify it.
    const bool helperSafe = helperExists && helperStat.st_uid == 0 &&
                            (helperStat.st_mode & (S_IWGRP | S_IWOTH)) == 0;

    struct stat policyStat;
    const bool policyOk = (::stat(kPolicyPath, &policyStat) == 0 && policyStat.st_size > 0);

    const bool pkexecFound = !QStandardPaths::findExecutable("pkexec").isEmpty();
    const bool isLinux = haveUname && QString::fromLocal8Bit(info.sysname) == "Linux";

    struct Check { QString text; bool ok; };
    const Check checks[kCheckCount] = {
        {"Linux kernel detected", isLinux},
        {"pkexec (PolicyKit) is installed", pkexecFound},
        {"A PolicyKit authentication agent is running (password dialog)", agentRunning},
        {"Super Admin helper is installed and protected (owned by root, not writable by others)", helperSafe},
        {"PolicyKit rule for the helper is installed and not empty", policyOk},
        {"IPC child program is built next to the application", childPresent},
        {"POSIX message queues are supported by the kernel", queuesSupported},
    };

    int failed = 0;
    for (int i = 0; i < kCheckCount; ++i)
    {
        if (!checks[i].ok)
            ++failed;

        checkLabel[i]->setText(
            QString("<span style='color:%1; font-weight:bold;'>%2</span>&nbsp; %3")
                .arg(QString(checks[i].ok ? "#16a34a" : "#dc2626"),
                     QString(checks[i].ok ? "OK" : "FIX"),
                     checks[i].text.toHtmlEscaped()));
    }

    if (failed == 0)
    {
        banner->setText("ALL CHECKS PASSED  |  OS CONTROL CENTER IS READY");
        banner->setStyleSheet(
            "background-color: #dcfce7; color: #166534; padding: 14px;"
            "border-radius: 7px; font-weight: bold; font-size: 11px;");
    }
    else
    {
        banner->setText(QString("%1 CHECK(S) NEED ATTENTION  |  Normal features still work. "
                                "Super Admin mode needs the items marked FIX (run: bash privileged/install.sh).")
                            .arg(failed));
        banner->setStyleSheet(
            "background-color: #fef3c7; color: #92400e; padding: 14px;"
            "border-radius: 7px; font-weight: bold; font-size: 11px;");
    }
}