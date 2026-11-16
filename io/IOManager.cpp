#include "IOManager.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cerrno>
#include <climits>
#include <csignal>
#include <cstring>
#include <ctime>
#include <utility>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

// ---------------------------------------------------------------------------
// Constructor / destructor
// ---------------------------------------------------------------------------
IOManager::IOManager(QWidget *parent)
    : QWidget(parent)
{
    setupUI();
    refreshTable();

    statsTimer = new QTimer(this);
    connect(statsTimer, &QTimer::timeout, this, &IOManager::refreshStats);
    statsTimer->start(2000);
}

IOManager::~IOManager()
{
    // Never leak descriptors: close everything we opened.
    for (const Descriptor &descriptor : std::as_const(descriptors))
        ::close(descriptor.fd);

    descriptors.clear();
}

// ---------------------------------------------------------------------------
// User interface
// ---------------------------------------------------------------------------
void IOManager::setupUI()
{
    QVBoxLayout *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(35, 30, 35, 30);
    mainLayout->setSpacing(10);

    QLabel *heading = new QLabel("Input / Output Management");
    heading->setStyleSheet("font-size: 25px; font-weight: bold;");

    QLabel *description = new QLabel(
        "Work with real file descriptors: open(), read(), write(), lseek(), dup(), pipe(), ioctl(), close().");
    description->setStyleSheet("color: #6b7280;");

    statsLabel = new QLabel;
    statsLabel->setStyleSheet(
        "background-color: white; border: 1px solid #e5e7eb;"
        "border-radius: 6px; padding: 8px; font-size: 12px;");

    // ----- Row 1: open -----
    pathEdit = new QLineEdit("/tmp/occ_demo.txt");
    pathEdit->setPlaceholderText("File path, e.g. /tmp/occ_demo.txt or /dev/urandom");

    modeCombo = new QComboBox;
    modeCombo->addItem("Read only  (O_RDONLY)");
    modeCombo->addItem("Write only, create, truncate  (O_WRONLY|O_CREAT|O_TRUNC)");
    modeCombo->addItem("Read + write, create  (O_RDWR|O_CREAT)");
    modeCombo->addItem("Append, create  (O_WRONLY|O_APPEND|O_CREAT)");
    modeCombo->setCurrentIndex(2);

    QPushButton *openButton = new QPushButton("Open");

    QHBoxLayout *row1 = new QHBoxLayout;
    row1->addWidget(new QLabel("Path:"));
    row1->addWidget(pathEdit, 1);
    row1->addWidget(modeCombo);
    row1->addWidget(openButton);

    // ----- Row 2: pipe / dup / close -----
    QPushButton *pipeButton    = new QPushButton("Create Pipe");
    QPushButton *dupButton     = new QPushButton("dup() Selected");
    QPushButton *closeButton   = new QPushButton("Close Selected");
    QPushButton *closeAllButton = new QPushButton("Close All");

    const QString redStyle =
        "QPushButton { background-color: #dc2626; }"
        "QPushButton:hover { background-color: #b91c1c; }";
    closeButton->setStyleSheet(redStyle);
    closeAllButton->setStyleSheet(redStyle);

    QHBoxLayout *row2 = new QHBoxLayout;
    row2->addWidget(pipeButton);
    row2->addWidget(dupButton);
    row2->addStretch();
    row2->addWidget(closeButton);
    row2->addWidget(closeAllButton);

    // ----- Table -----
    table = new QTableWidget(0, 5);
    table->setHorizontalHeaderLabels({"FD", "Resource", "Access", "Position", "Note"});
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    table->verticalHeader()->setVisible(false);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setMinimumHeight(120);

    // ----- Row 3: read / seek -----
    lengthSpin = new QSpinBox;
    lengthSpin->setRange(1, 4096);
    lengthSpin->setValue(64);

    seekSpin = new QSpinBox;
    seekSpin->setRange(-1000000000, 1000000000);

    whenceCombo = new QComboBox;
    whenceCombo->addItems({"SEEK_SET (from start)", "SEEK_CUR (from here)", "SEEK_END (from end)"});

    QPushButton *readButton = new QPushButton("Read");
    QPushButton *seekButton = new QPushButton("Seek");

    QHBoxLayout *row3 = new QHBoxLayout;
    row3->addWidget(new QLabel("Bytes to read:"));
    row3->addWidget(lengthSpin);
    row3->addWidget(readButton);
    row3->addSpacing(12);
    row3->addWidget(new QLabel("lseek offset:"));
    row3->addWidget(seekSpin);
    row3->addWidget(whenceCombo);
    row3->addWidget(seekButton);

    // ----- Row 4: write -----
    dataEdit = new QLineEdit;
    dataEdit->setPlaceholderText("Text to write to the selected descriptor");

    newlineCheck = new QCheckBox("Add newline");
    newlineCheck->setChecked(true);

    QPushButton *writeButton = new QPushButton("Write");

    QHBoxLayout *row4 = new QHBoxLayout;
    row4->addWidget(dataEdit, 1);
    row4->addWidget(newlineCheck);
    row4->addWidget(writeButton);

    // ----- Row 5: inspection -----
    QPushButton *infoButton   = new QPushButton("Descriptor Info");
    QPushButton *procButton   = new QPushButton("Show /proc/self/fd");
    QPushButton *fillButton   = new QPushButton("Fill Pipe Until Full");
    QPushButton *errorButton  = new QPushButton("Run Error Demo");

    QHBoxLayout *row5 = new QHBoxLayout;
    row5->addWidget(infoButton);
    row5->addWidget(procButton);
    row5->addWidget(fillButton);
    row5->addStretch();
    row5->addWidget(errorButton);

    // ----- Output -----
    output = new QPlainTextEdit;
    output->setReadOnly(true);
    output->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    output->setMinimumHeight(140);

    statusLabel = new QLabel("Ready.");
    statusLabel->setWordWrap(true);
    statusLabel->setStyleSheet("color: #374151; font-size: 12px;");

    mainLayout->addWidget(heading);
    mainLayout->addWidget(description);
    mainLayout->addWidget(statsLabel);
    mainLayout->addLayout(row1);
    mainLayout->addLayout(row2);
    mainLayout->addWidget(table, 1);
    mainLayout->addLayout(row3);
    mainLayout->addLayout(row4);
    mainLayout->addLayout(row5);
    mainLayout->addWidget(output, 1);
    mainLayout->addWidget(statusLabel);

    connect(openButton,     &QPushButton::clicked, this, &IOManager::openResource);
    connect(pipeButton,     &QPushButton::clicked, this, &IOManager::createPipe);
    connect(dupButton,      &QPushButton::clicked, this, &IOManager::duplicateDescriptor);
    connect(closeButton,    &QPushButton::clicked, this, &IOManager::closeDescriptor);
    connect(closeAllButton, &QPushButton::clicked, this, &IOManager::closeAllDescriptors);
    connect(readButton,     &QPushButton::clicked, this, &IOManager::readFromDescriptor);
    connect(writeButton,    &QPushButton::clicked, this, &IOManager::writeToDescriptor);
    connect(seekButton,     &QPushButton::clicked, this, &IOManager::seekDescriptor);
    connect(infoButton,     &QPushButton::clicked, this, &IOManager::showInfo);
    connect(procButton,     &QPushButton::clicked, this, &IOManager::showProcessDescriptors);
    connect(fillButton,     &QPushButton::clicked, this, &IOManager::fillPipe);
    connect(errorButton,    &QPushButton::clicked, this, &IOManager::runErrorDemo);

    setStyleSheet(
        "QPushButton {"
        "  background-color: #2563eb; color: white;"
        "  padding: 8px 12px; border: none; border-radius: 5px;"
        "}"
        "QPushButton:hover { background-color: #1d4ed8; }"
    );
}

void IOManager::report(const QString &message, bool error)
{
    statusLabel->setStyleSheet(
        error ? "color: #b91c1c; font-size: 12px; font-weight: bold;"
              : "color: #166534; font-size: 12px;");
    statusLabel->setText(message);
    appendOutput((error ? "[error] " : "[ok]    ") + message);
}

void IOManager::appendOutput(const QString &text)
{
    output->appendPlainText(text);
}

// Rebuild the table, keeping the same descriptor selected.
void IOManager::refreshTable()
{
    int selectedFd = -1;
    if (const Descriptor *current = selectedDescriptor())
        selectedFd = current->fd;

    table->setRowCount(0);

    for (const Descriptor &descriptor : std::as_const(descriptors))
    {
        const int row = table->rowCount();
        table->insertRow(row);

        QTableWidgetItem *fdItem = new QTableWidgetItem;
        fdItem->setData(Qt::DisplayRole, descriptor.fd);

        table->setItem(row, 0, fdItem);
        table->setItem(row, 1, new QTableWidgetItem(descriptor.resource));
        table->setItem(row, 2, new QTableWidgetItem(accessText(descriptor.fd)));
        table->setItem(row, 3, new QTableWidgetItem(positionText(descriptor.fd)));
        table->setItem(row, 4, new QTableWidgetItem(descriptor.note));

        if (descriptor.fd == selectedFd)
            table->selectRow(row);
    }

    refreshStats();
}

void IOManager::refreshStats()
{
    const int total = QDir("/proc/self/fd").entryList(QDir::NoDotAndDotDot | QDir::AllEntries).size();

    statsLabel->setText(
        QString("This process (PID %1)  |  Open descriptors in total: %2  |  "
                "Opened by this module: %3")
            .arg(getpid()).arg(total).arg(descriptors.size()));
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
IOManager::Descriptor *IOManager::selectedDescriptor()
{
    if (!table || !table->selectionModel())
        return nullptr;

    const QModelIndexList rows = table->selectionModel()->selectedRows(0);
    if (rows.isEmpty())
        return nullptr;

    const QTableWidgetItem *item = table->item(rows.first().row(), 0);
    if (!item)
        return nullptr;

    auto it = descriptors.find(item->data(Qt::DisplayRole).toInt());
    return it == descriptors.end() ? nullptr : &it.value();
}

QString IOManager::errorText(int err)
{
    const char *name = nullptr;

    switch (err)
    {
    case ENOENT:  name = "ENOENT";  break;
    case EACCES:  name = "EACCES";  break;
    case EPERM:   name = "EPERM";   break;
    case EBADF:   name = "EBADF";   break;
    case EISDIR:  name = "EISDIR";  break;
    case EAGAIN:  name = "EAGAIN";  break;   // same value as EWOULDBLOCK
    case ESPIPE:  name = "ESPIPE";  break;
    case EPIPE:   name = "EPIPE";   break;
    case EEXIST:  name = "EEXIST";  break;
    case ENXIO:   name = "ENXIO";   break;
    case EMFILE:  name = "EMFILE";  break;
    case ENOSPC:  name = "ENOSPC";  break;
    case ENOTTY:  name = "ENOTTY";  break;
    case EINVAL:  name = "EINVAL";  break;
    case ENAMETOOLONG: name = "ENAMETOOLONG"; break;
    default: break;
    }

    const QString text = QString::fromLocal8Bit(strerror(err));

    if (name)
        return QString("%1 - %2").arg(QString::fromLatin1(name), text);

    return QString("errno %1 - %2").arg(err).arg(text);
}

QString IOManager::accessText(int fd)
{
    const int flags = ::fcntl(fd, F_GETFL);
    if (flags < 0)
        return "?";

    QString text;
    switch (flags & O_ACCMODE)
    {
    case O_RDONLY: text = "read"; break;
    case O_WRONLY: text = "write"; break;
    case O_RDWR:   text = "read/write"; break;
    default:       text = "?"; break;
    }

    if (flags & O_APPEND)
        text += ", append";
    if (flags & O_NONBLOCK)
        text += ", non-blocking";

    return text;
}

QString IOManager::positionText(int fd)
{
    const off_t position = ::lseek(fd, 0, SEEK_CUR);

    if (position < 0)
        return errno == ESPIPE ? "n/a (not seekable)" : "?";

    return QString::number(static_cast<qlonglong>(position));
}

QString IOManager::hexDump(const unsigned char *data, size_t length)
{
    QString out;

    for (size_t i = 0; i < length; i += 16)
    {
        QString hex;
        QString ascii;

        for (size_t j = 0; j < 16; ++j)
        {
            if (i + j < length)
            {
                const unsigned char c = data[i + j];
                hex += QString("%1 ").arg(static_cast<qulonglong>(c), 2, 16, QChar('0'));
                ascii += (c >= 32 && c < 127) ? QChar(c) : QChar('.');
            }
            else
            {
                hex += "   ";
            }
        }

        out += QString("  +%1  %2 |%3|\n")
                   .arg(static_cast<qulonglong>(i), 4, 16, QChar('0'))
                   .arg(hex, ascii);
    }

    return out;
}

// Write all bytes, handling interrupted calls (EINTR) and partial writes.
// SIGPIPE is blocked just for this call, so writing to a pipe with no reader
// gives us the error EPIPE instead of killing the application.
// Returns how many bytes were written; err is 0 if everything was written.
size_t IOManager::writeBytes(int fd, const char *data, size_t length, int &err)
{
    err = 0;

    sigset_t blocked;
    sigset_t previous;
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGPIPE);
    pthread_sigmask(SIG_BLOCK, &blocked, &previous);

    size_t written = 0;

    while (written < length)
    {
        const ssize_t result = ::write(fd, data + written, length - written);

        if (result < 0)
        {
            if (errno == EINTR)
                continue;

            err = errno;
            break;
        }

        written += static_cast<size_t>(result);
    }

    if (err == EPIPE)
    {
        // Remove the SIGPIPE that is now pending, so it is not delivered later.
        const timespec zero = {0, 0};
        sigtimedwait(&blocked, nullptr, &zero);
    }

    pthread_sigmask(SIG_SETMASK, &previous, nullptr);
    return written;
}

// ---------------------------------------------------------------------------
// Operations
// ---------------------------------------------------------------------------
void IOManager::openResource()
{
    const QString path = pathEdit->text().trimmed();

    if (path.isEmpty())
    {
        report("Type a file path first.", true);
        return;
    }

    int flags = O_RDONLY;
    switch (modeCombo->currentIndex())
    {
    case 0: flags = O_RDONLY; break;
    case 1: flags = O_WRONLY | O_CREAT | O_TRUNC; break;
    case 2: flags = O_RDWR | O_CREAT; break;
    case 3: flags = O_WRONLY | O_APPEND | O_CREAT; break;
    default: break;
    }

    // Protect the user: truncating destroys the old content.
    if (flags & O_TRUNC)
    {
        const QFileInfo info(path);
        if (info.exists() && info.size() > 0)
        {
            const auto answer = QMessageBox::question(
                this, "Truncate file?",
                QString("\"%1\" already contains data.\n"
                        "Opening with O_TRUNC will erase it. Continue?").arg(path),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No);

            if (answer != QMessageBox::Yes)
                return;
        }
    }

    // O_CLOEXEC: children started by the Process module do not inherit it.
    // O_NONBLOCK: a read that has no data returns EAGAIN instead of freezing the window.
    const QByteArray nativePath = QFile::encodeName(path);
    const int fd = ::open(nativePath.constData(), flags | O_CLOEXEC | O_NONBLOCK, 0644);
    const int err = errno;

    if (fd < 0)
    {
        report(QString("open(\"%1\") failed: %2").arg(path, errorText(err)), true);
        return;
    }

    Descriptor descriptor;
    descriptor.fd = fd;
    descriptor.resource = path;
    descriptors.insert(fd, descriptor);

    refreshTable();
    report(QString("open() returned file descriptor %1 for \"%2\".").arg(fd).arg(path));
}

void IOManager::createPipe()
{
    int fds[2] = {-1, -1};

    if (::pipe2(fds, O_CLOEXEC | O_NONBLOCK) != 0)
    {
        report("pipe2() failed: " + errorText(errno), true);
        return;
    }

    Descriptor readEnd;
    readEnd.fd = fds[0];
    readEnd.resource = "pipe (read end)";
    readEnd.note = QString("Data written to fd %1 comes out here").arg(fds[1]);

    Descriptor writeEnd;
    writeEnd.fd = fds[1];
    writeEnd.resource = "pipe (write end)";
    writeEnd.note = QString("Data written here is read from fd %1").arg(fds[0]);

    descriptors.insert(readEnd.fd, readEnd);
    descriptors.insert(writeEnd.fd, writeEnd);

    refreshTable();
    report(QString("pipe2() created a pipe: read end = fd %1, write end = fd %2.")
               .arg(fds[0]).arg(fds[1]));
}

void IOManager::duplicateDescriptor()
{
    Descriptor *source = selectedDescriptor();
    if (!source)
    {
        report("Select a descriptor first.", true);
        return;
    }

    const int sourceFd = source->fd;
    const QString sourceResource = source->resource;

    const int copy = ::dup(sourceFd);
    const int err = errno;

    if (copy < 0)
    {
        report("dup() failed: " + errorText(err), true);
        return;
    }

    ::fcntl(copy, F_SETFD, FD_CLOEXEC);

    Descriptor descriptor;
    descriptor.fd = copy;
    descriptor.resource = sourceResource;
    descriptor.note = QString("dup() of fd %1 - shares its file position").arg(sourceFd);
    descriptors.insert(copy, descriptor);

    refreshTable();
    report(QString("dup(%1) returned the new descriptor %2.").arg(sourceFd).arg(copy));
    appendOutput(QString("        Both descriptors point to the SAME open file, so reading through "
                         "fd %1 also moves the position of fd %2.").arg(sourceFd).arg(copy));
}

void IOManager::closeDescriptor()
{
    Descriptor *descriptor = selectedDescriptor();
    if (!descriptor)
    {
        report("Select a descriptor first.", true);
        return;
    }

    const int fd = descriptor->fd;

    if (::close(fd) != 0)
    {
        report(QString("close(%1) failed: %2").arg(fd).arg(errorText(errno)), true);
        return;
    }

    descriptors.remove(fd);
    refreshTable();
    report(QString("close(%1) succeeded. The number %1 can now be reused by the next open().").arg(fd));
}

void IOManager::closeAllDescriptors()
{
    int closed = 0;

    for (const Descriptor &descriptor : std::as_const(descriptors))
    {
        if (::close(descriptor.fd) == 0)
            ++closed;
    }

    descriptors.clear();
    refreshTable();
    report(QString("Closed %1 descriptor(s).").arg(closed));
}

void IOManager::readFromDescriptor()
{
    Descriptor *descriptor = selectedDescriptor();
    if (!descriptor)
    {
        report("Select a descriptor first.", true);
        return;
    }

    const int fd = descriptor->fd;
    const size_t wanted = static_cast<size_t>(lengthSpin->value());

    QByteArray buffer(static_cast<qsizetype>(wanted), '\0');
    ssize_t count;

    do {
        count = ::read(fd, buffer.data(), wanted);
    } while (count < 0 && errno == EINTR);

    const int err = errno;

    if (count < 0)
    {
        if (err == EAGAIN)
            report(QString("read(%1): no data available right now (EAGAIN). "
                           "A blocking read would have frozen this window.").arg(fd), true);
        else
            report(QString("read(%1) failed: %2").arg(fd).arg(errorText(err)), true);

        return;
    }

    if (count == 0)
    {
        report(QString("read(%1) returned 0: end of file (or no writer left on a pipe).").arg(fd));
        refreshTable();
        return;
    }

    appendOutput(QString("--- read(%1, %2 bytes requested) returned %3 bytes ---")
                     .arg(fd).arg(wanted).arg(count));
    appendOutput(hexDump(reinterpret_cast<const unsigned char *>(buffer.constData()),
                         static_cast<size_t>(count)));

    refreshTable();
    report(QString("read(%1) returned %2 byte(s).").arg(fd).arg(count));
}

void IOManager::writeToDescriptor()
{
    Descriptor *descriptor = selectedDescriptor();
    if (!descriptor)
    {
        report("Select a descriptor first.", true);
        return;
    }

    QByteArray bytes = dataEdit->text().toUtf8();
    if (newlineCheck->isChecked())
        bytes.append('\n');

    if (bytes.isEmpty())
    {
        report("Type some text to write.", true);
        return;
    }

    const int fd = descriptor->fd;
    int err = 0;
    const size_t written = writeBytes(fd, bytes.constData(), static_cast<size_t>(bytes.size()), err);

    refreshTable();

    if (err != 0)
    {
        report(QString("write(%1) wrote %2 of %3 bytes, then failed: %4")
                   .arg(fd).arg(written).arg(bytes.size()).arg(errorText(err)), true);
        return;
    }

    report(QString("write(%1) wrote %2 byte(s).").arg(fd).arg(written));
}

void IOManager::seekDescriptor()
{
    Descriptor *descriptor = selectedDescriptor();
    if (!descriptor)
    {
        report("Select a descriptor first.", true);
        return;
    }

    static const int whenceValues[] = {SEEK_SET, SEEK_CUR, SEEK_END};
    static const char *const whenceNames[] = {"SEEK_SET", "SEEK_CUR", "SEEK_END"};

    const int index = std::clamp(whenceCombo->currentIndex(), 0, 2);
    const int fd = descriptor->fd;
    const off_t offset = static_cast<off_t>(seekSpin->value());

    const off_t result = ::lseek(fd, offset, whenceValues[index]);
    const int err = errno;

    if (result < 0)
    {
        report(QString("lseek(%1, %2, %3) failed: %4")
                   .arg(fd).arg(static_cast<qlonglong>(offset))
                   .arg(QString::fromLatin1(whenceNames[index])).arg(errorText(err)), true);
        return;
    }

    refreshTable();
    report(QString("lseek(%1, %2, %3): the position is now %4.")
               .arg(fd).arg(static_cast<qlonglong>(offset))
               .arg(QString::fromLatin1(whenceNames[index]))
               .arg(static_cast<qlonglong>(result)));
}

// Write 4096-byte chunks into a pipe until the kernel buffer is full.
void IOManager::fillPipe()
{
    Descriptor *descriptor = selectedDescriptor();
    if (!descriptor)
    {
        report("Select the WRITE end of a pipe first.", true);
        return;
    }

    const int fd = descriptor->fd;

    struct stat st;
    if (::fstat(fd, &st) != 0 || !S_ISFIFO(st.st_mode))
    {
        report(QString("fd %1 is not a pipe.").arg(fd), true);
        return;
    }

    if ((::fcntl(fd, F_GETFL) & O_ACCMODE) != O_WRONLY)
    {
        report("Select the write end of the pipe (the one that says \"write\").", true);
        return;
    }

    const QByteArray chunk(4096, 'x');
    size_t total = 0;
    int err = 0;

    while (err == 0 && total < 16 * 1024 * 1024)
        total += writeBytes(fd, chunk.constData(), static_cast<size_t>(chunk.size()), err);

    refreshTable();

    const int capacity = ::fcntl(fd, F_GETPIPE_SZ);

    if (err == EAGAIN)
    {
        report(QString("The pipe is full after %1 bytes (kernel pipe capacity: %2 bytes). "
                       "Another write would block, so it returned EAGAIN.")
                   .arg(total).arg(capacity));
        appendOutput("        Read from the other end to make room, then fill it again.");
    }
    else
    {
        report(QString("Stopped after %1 bytes: %2").arg(total).arg(errorText(err)), true);
    }
}

// fstat() + fcntl() + ioctl() + /proc: everything the kernel knows about one descriptor.
void IOManager::showInfo()
{
    Descriptor *descriptor = selectedDescriptor();
    if (!descriptor)
    {
        report("Select a descriptor first.", true);
        return;
    }

    const int fd = descriptor->fd;

    struct stat st;
    if (::fstat(fd, &st) != 0)
    {
        report(QString("fstat(%1) failed: %2").arg(fd).arg(errorText(errno)), true);
        return;
    }

    QString type = "other";
    if (S_ISREG(st.st_mode))       type = "regular file";
    else if (S_ISDIR(st.st_mode))  type = "directory";
    else if (S_ISCHR(st.st_mode))  type = "character device";
    else if (S_ISBLK(st.st_mode))  type = "block device";
    else if (S_ISFIFO(st.st_mode)) type = "pipe / FIFO";
    else if (S_ISSOCK(st.st_mode)) type = "socket";

    appendOutput(QString("--- Information about fd %1 ---").arg(fd));
    appendOutput(QString("  fstat():   type = %1, size = %2 bytes, inode = %3, permissions = %4, owner uid = %5")
                     .arg(type)
                     .arg(static_cast<qlonglong>(st.st_size))
                     .arg(static_cast<qulonglong>(st.st_ino))
                     .arg(QString::number(st.st_mode & 07777, 8).rightJustified(4, '0'))
                     .arg(static_cast<qulonglong>(st.st_uid)));

    const int statusFlags = ::fcntl(fd, F_GETFL);
    const int descriptorFlags = ::fcntl(fd, F_GETFD);
    appendOutput(QString("  fcntl():   access = %1, close-on-exec = %2")
                     .arg(accessText(fd))
                     .arg(descriptorFlags >= 0 && (descriptorFlags & FD_CLOEXEC) ? "yes" : "no"));
    Q_UNUSED(statusFlags);

    if (S_ISFIFO(st.st_mode))
    {
        const int capacity = ::fcntl(fd, F_GETPIPE_SZ);
        if (capacity > 0)
            appendOutput(QString("  fcntl(F_GETPIPE_SZ): pipe buffer capacity = %1 bytes").arg(capacity));
    }

    int available = 0;
    if (::ioctl(fd, FIONREAD, &available) == 0)
        appendOutput(QString("  ioctl(FIONREAD): %1 byte(s) waiting to be read").arg(available));
    else
        appendOutput(QString("  ioctl(FIONREAD): not supported here (%1)").arg(errorText(errno)));

    char target[PATH_MAX];
    const QByteArray linkPath = QString("/proc/self/fd/%1").arg(fd).toLocal8Bit();
    const ssize_t length = ::readlink(linkPath.constData(), target, sizeof(target) - 1);
    if (length > 0)
    {
        target[length] = '\0';
        appendOutput(QString("  /proc/self/fd/%1  ->  %2").arg(fd).arg(QString::fromLocal8Bit(target)));
    }

    QFile info(QString("/proc/self/fdinfo/%1").arg(fd));
    if (info.open(QIODevice::ReadOnly))
    {
        appendOutput("  /proc/self/fdinfo/" + QString::number(fd) + ":");
        const QStringList lines = QString::fromLocal8Bit(info.readAll()).split('\n', Qt::SkipEmptyParts);
        for (const QString &line : lines)
            appendOutput("      " + line);
    }

    report(QString("Showed information about fd %1.").arg(fd));
}

// List the REAL descriptor table of this process, straight from the kernel.
void IOManager::showProcessDescriptors()
{
    QStringList names = QDir("/proc/self/fd").entryList(QDir::NoDotAndDotDot | QDir::AllEntries);

    std::sort(names.begin(), names.end(), [](const QString &a, const QString &b) {
        return a.toInt() < b.toInt();
    });

    appendOutput(QString("--- /proc/%1/fd : every descriptor this process has open ---").arg(getpid()));

    for (const QString &name : std::as_const(names))
    {
        char target[PATH_MAX];
        const QByteArray linkPath = ("/proc/self/fd/" + name).toLocal8Bit();
        const ssize_t length = ::readlink(linkPath.constData(), target, sizeof(target) - 1);

        QString destination = "?";
        if (length > 0)
        {
            target[length] = '\0';
            destination = QString::fromLocal8Bit(target);
        }

        const bool ours = descriptors.contains(name.toInt());

        appendOutput(QString("  %1  ->  %2%3")
                         .arg(name.rightJustified(3), destination,
                              ours ? QString("     <-- opened here") : QString()));
    }

    appendOutput("  (One entry is the directory scan we just did. The others belong to Qt and the system.)");
    report(QString("Listed %1 descriptor(s). Compare with:  ls -l /proc/%2/fd")
               .arg(names.size()).arg(getpid()));
}

// Trigger real errors on purpose and show how each one is reported.
void IOManager::runErrorDemo()
{
    appendOutput("--- Error handling demo: each call below fails on purpose ---");

    auto tryOpen = [this](const char *path, int flags, const QString &label) {
        const int fd = ::open(path, flags | O_CLOEXEC | O_NONBLOCK);
        const int err = errno;

        if (fd < 0)
        {
            appendOutput(QString("  %1\n      -> %2").arg(label, errorText(err)));
        }
        else
        {
            appendOutput(QString("  %1\n      -> succeeded (fd %2); closing it again.").arg(label).arg(fd));
            ::close(fd);
        }
    };

    tryOpen("/nonexistent/file.txt", O_RDONLY, "open(\"/nonexistent/file.txt\", O_RDONLY)");
    tryOpen("/etc/shadow", O_RDONLY, "open(\"/etc/shadow\", O_RDONLY)   (only root may read it)");
    tryOpen("/tmp", O_WRONLY, "open(\"/tmp\", O_WRONLY)   (a directory cannot be written)");

    errno = 0;
    const int closeResult = ::close(9999);
    const int closeError = errno;
    appendOutput(QString("  close(9999)   (we never opened it)\n      -> %1")
                     .arg(closeResult == 0 ? QString("succeeded?!") : errorText(closeError)));

    int fds[2] = {-1, -1};
    if (::pipe2(fds, O_CLOEXEC | O_NONBLOCK) == 0)
    {
        char byte;

        errno = 0;
        const ssize_t readResult = ::read(fds[1], &byte, 1);
        const int readError = errno;
        appendOutput(QString("  read() on the WRITE end of a pipe\n      -> %1")
                         .arg(readResult < 0 ? errorText(readError) : QString("succeeded?!")));

        errno = 0;
        const ssize_t emptyRead = ::read(fds[0], &byte, 1);
        const int emptyError = errno;
        appendOutput(QString("  read() on an EMPTY pipe (non-blocking)\n      -> %1")
                         .arg(emptyRead < 0 ? errorText(emptyError) : QString("got data?!")));

        ::close(fds[0]);   // close the only reader

        int writeError = 0;
        const size_t wrote = writeBytes(fds[1], "x", 1, writeError);
        appendOutput(QString("  write() to a pipe whose read end is closed\n      -> %1")
                         .arg(wrote == 0 && writeError != 0 ? errorText(writeError)
                                                           : QString("succeeded?!")));

        ::close(fds[1]);
    }

    report("Error demo finished. The application is still running: every failure was handled.");
}