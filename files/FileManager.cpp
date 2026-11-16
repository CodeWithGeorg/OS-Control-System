#include "FileManager.h"

#include <QCheckBox>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHash>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTabWidget>
#include <QTableWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <cstring>

#include <dirent.h>
#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static const qint64 kMaxViewBytes = 64 * 1024;     // largest file loaded into the editor
static const size_t kMaxListedEntries = 5000;      // keeps huge folders (like /proc) usable

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------
FileManager::FileManager(QWidget *parent)
    : QWidget(parent)
{
    setupUI();
    navigateTo(QDir::homePath());
}

// ---------------------------------------------------------------------------
// User interface
// ---------------------------------------------------------------------------
void FileManager::setupUI()
{
    QVBoxLayout *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(35, 30, 35, 30);
    mainLayout->setSpacing(10);

    QLabel *heading = new QLabel("File Subsystem");
    heading->setStyleSheet("font-size: 25px; font-weight: bold;");

    QLabel *description = new QLabel(
        "Create, read, write, rename, delete and inspect real files with POSIX system calls.");
    description->setStyleSheet("color: #6b7280;");

    // ----- Navigation row -----
    QPushButton *upButton      = new QPushButton("Up");
    QPushButton *homeButton    = new QPushButton("Home");
    QPushButton *goButton      = new QPushButton("Go");
    QPushButton *refreshButton = new QPushButton("Refresh");

    pathEdit = new QLineEdit;
    pathEdit->setPlaceholderText("Directory path");

    hiddenCheck = new QCheckBox("Show hidden");

    QHBoxLayout *navRow = new QHBoxLayout;
    navRow->addWidget(upButton);
    navRow->addWidget(homeButton);
    navRow->addWidget(pathEdit, 1);
    navRow->addWidget(goButton);
    navRow->addWidget(refreshButton);
    navRow->addWidget(hiddenCheck);

    // ----- Table -----
    table = new QTableWidget(0, 6);
    table->setHorizontalHeaderLabels({"Name", "Type", "Size", "Permissions", "Owner", "Modified"});
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Interactive);
    table->setColumnWidth(0, 220);
    table->horizontalHeader()->setStretchLastSection(true);
    table->verticalHeader()->setVisible(false);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setMinimumHeight(150);

    // ----- Action row -----
    QPushButton *newFileButton   = new QPushButton("New File");
    QPushButton *newFolderButton = new QPushButton("New Folder");
    QPushButton *viewButton      = new QPushButton("Open / Read");
    QPushButton *renameButton    = new QPushButton("Rename");
    QPushButton *permButton      = new QPushButton("Permissions");
    QPushButton *propsButton     = new QPushButton("Properties");
    QPushButton *deleteButton    = new QPushButton("Delete");

    deleteButton->setStyleSheet(
        "QPushButton { background-color: #dc2626; }"
        "QPushButton:hover { background-color: #b91c1c; }");

    QHBoxLayout *actionRow = new QHBoxLayout;
    actionRow->addWidget(newFileButton);
    actionRow->addWidget(newFolderButton);
    actionRow->addWidget(viewButton);
    actionRow->addWidget(renameButton);
    actionRow->addWidget(permButton);
    actionRow->addWidget(propsButton);
    actionRow->addStretch();
    actionRow->addWidget(deleteButton);

    // ----- Tab 1: editor -----
    QWidget *editorPage = new QWidget;
    QVBoxLayout *editorLayout = new QVBoxLayout(editorPage);
    editorLayout->setContentsMargins(8, 8, 8, 8);

    editorLabel = new QLabel("No file open. Use New File, or select a file and click Open / Read.");
    editorLabel->setStyleSheet("color: #374151; font-size: 12px;");

    editor = new QPlainTextEdit;
    editor->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    editor->setPlaceholderText("File content appears here.");

    QPushButton *saveButton = new QPushButton("Save (overwrite)");

    appendEdit = new QLineEdit;
    appendEdit->setPlaceholderText("Text to append to the end of the file");

    appendNewlineCheck = new QCheckBox("Add newline");
    appendNewlineCheck->setChecked(true);

    QPushButton *appendButton = new QPushButton("Append");

    QHBoxLayout *editorButtons = new QHBoxLayout;
    editorButtons->addWidget(saveButton);
    editorButtons->addSpacing(16);
    editorButtons->addWidget(appendEdit, 1);
    editorButtons->addWidget(appendNewlineCheck);
    editorButtons->addWidget(appendButton);

    editorLayout->addWidget(editorLabel);
    editorLayout->addWidget(editor, 1);
    editorLayout->addLayout(editorButtons);

    // ----- Tab 2: details / log -----
    output = new QPlainTextEdit;
    output->setReadOnly(true);
    output->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));

    tabs = new QTabWidget;
    tabs->addTab(editorPage, "File Content");
    tabs->addTab(output, "Details and Log");

    statusLabel = new QLabel("Ready.");
    statusLabel->setWordWrap(true);
    statusLabel->setStyleSheet("color: #374151; font-size: 12px;");

    mainLayout->addWidget(heading);
    mainLayout->addWidget(description);
    mainLayout->addLayout(navRow);
    mainLayout->addWidget(table, 2);
    mainLayout->addLayout(actionRow);
    mainLayout->addWidget(tabs, 2);
    mainLayout->addWidget(statusLabel);

    connect(upButton,      &QPushButton::clicked, this, &FileManager::goUp);
    connect(homeButton,    &QPushButton::clicked, this, &FileManager::goHome);
    connect(goButton,      &QPushButton::clicked, this, &FileManager::goToTyped);
    connect(pathEdit,      &QLineEdit::returnPressed, this, &FileManager::goToTyped);
    connect(refreshButton, &QPushButton::clicked, this, &FileManager::refresh);
    connect(hiddenCheck,   &QCheckBox::toggled, this, &FileManager::refresh);

    connect(newFileButton,   &QPushButton::clicked, this, &FileManager::createFile);
    connect(newFolderButton, &QPushButton::clicked, this, &FileManager::createFolder);
    connect(viewButton,      &QPushButton::clicked, this, &FileManager::viewFile);
    connect(renameButton,    &QPushButton::clicked, this, &FileManager::renameEntry);
    connect(permButton,      &QPushButton::clicked, this, &FileManager::changePermissions);
    connect(propsButton,     &QPushButton::clicked, this, &FileManager::showProperties);
    connect(deleteButton,    &QPushButton::clicked, this, &FileManager::deleteEntry);

    connect(saveButton,   &QPushButton::clicked, this, &FileManager::saveFile);
    connect(appendButton, &QPushButton::clicked, this, &FileManager::appendToFile);

    connect(table, &QTableWidget::cellDoubleClicked, this,
            [this](int row, int) { onCellActivated(row); });

    setStyleSheet(
        "QPushButton {"
        "  background-color: #2563eb; color: white;"
        "  padding: 8px 12px; border: none; border-radius: 5px;"
        "}"
        "QPushButton:hover { background-color: #1d4ed8; }"
    );
}

void FileManager::report(const QString &message, bool error)
{
    statusLabel->setStyleSheet(
        error ? "color: #b91c1c; font-size: 12px; font-weight: bold;"
              : "color: #166534; font-size: 12px;");
    statusLabel->setText(message);
    appendOutput((error ? "[error] " : "[ok]    ") + message);
}

void FileManager::appendOutput(const QString &text)
{
    output->appendPlainText(text);
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
QString FileManager::errorText(int err)
{
    const char *name = nullptr;

    switch (err)
    {
    case ENOENT:    name = "ENOENT";    break;
    case EACCES:    name = "EACCES";    break;
    case EPERM:     name = "EPERM";     break;
    case EEXIST:    name = "EEXIST";    break;
    case ENOTDIR:   name = "ENOTDIR";   break;
    case EISDIR:    name = "EISDIR";    break;
    case ENOTEMPTY: name = "ENOTEMPTY"; break;
    case EXDEV:     name = "EXDEV";     break;
    case EROFS:     name = "EROFS";     break;
    case ELOOP:     name = "ELOOP";     break;
    case ENOSPC:    name = "ENOSPC";    break;
    case EBUSY:     name = "EBUSY";     break;
    case EINVAL:    name = "EINVAL";    break;
    case ENAMETOOLONG: name = "ENAMETOOLONG"; break;
    default: break;
    }

    const QString text = QString::fromLocal8Bit(strerror(err));

    if (name)
        return QString("%1 - %2").arg(QString::fromLatin1(name), text);

    return QString("errno %1 - %2").arg(err).arg(text);
}

// A file NAME (not a path): no slash, not "." or "..", at most 255 bytes.
bool FileManager::validName(const QString &name, QString &reason)
{
    if (name.isEmpty())
        reason = "The name is empty.";
    else if (name == "." || name == "..")
        reason = "\".\" and \"..\" are not valid names.";
    else if (name.contains('/'))
        reason = "A name cannot contain \"/\". Use the path bar to change folder.";
    else if (name.contains(QChar(0)))
        reason = "A name cannot contain a null character.";
    else if (name.toUtf8().size() > 255)
        reason = "The name is longer than 255 bytes (NAME_MAX).";
    else
        return true;

    return false;
}

// Build the familiar "ls -l" string, for example -rwxr-xr-x
QString FileManager::permissionString(mode_t mode)
{
    QString s;

    if (S_ISDIR(mode))       s += QChar('d');
    else if (S_ISLNK(mode))  s += QChar('l');
    else if (S_ISCHR(mode))  s += QChar('c');
    else if (S_ISBLK(mode))  s += QChar('b');
    else if (S_ISFIFO(mode)) s += QChar('p');
    else if (S_ISSOCK(mode)) s += QChar('s');
    else                     s += QChar('-');

    s += (mode & S_IRUSR) ? QChar('r') : QChar('-');
    s += (mode & S_IWUSR) ? QChar('w') : QChar('-');
    if (mode & S_ISUID)
        s += (mode & S_IXUSR) ? QChar('s') : QChar('S');
    else
        s += (mode & S_IXUSR) ? QChar('x') : QChar('-');

    s += (mode & S_IRGRP) ? QChar('r') : QChar('-');
    s += (mode & S_IWGRP) ? QChar('w') : QChar('-');
    if (mode & S_ISGID)
        s += (mode & S_IXGRP) ? QChar('s') : QChar('S');
    else
        s += (mode & S_IXGRP) ? QChar('x') : QChar('-');

    s += (mode & S_IROTH) ? QChar('r') : QChar('-');
    s += (mode & S_IWOTH) ? QChar('w') : QChar('-');
    if (mode & S_ISVTX)
        s += (mode & S_IXOTH) ? QChar('t') : QChar('T');
    else
        s += (mode & S_IXOTH) ? QChar('x') : QChar('-');

    return s;
}

QString FileManager::humanSize(qint64 bytes)
{
    if (bytes < 1024)
        return QString("%1 B").arg(bytes);
    if (bytes < 1024 * 1024)
        return QString("%1 KiB").arg(bytes / 1024.0, 0, 'f', 1);
    if (bytes < 1024LL * 1024 * 1024)
        return QString("%1 MiB").arg(bytes / (1024.0 * 1024.0), 0, 'f', 1);
    return QString("%1 GiB").arg(bytes / (1024.0 * 1024.0 * 1024.0), 0, 'f', 2);
}

QString FileManager::userName(uid_t uid)
{
    static QHash<uid_t, QString> cache;

    auto it = cache.constFind(uid);
    if (it != cache.constEnd())
        return it.value();

    const passwd *entry = getpwuid(uid);
    const QString name = entry ? QString::fromLocal8Bit(entry->pw_name)
                               : QString::number(uid);
    cache.insert(uid, name);
    return name;
}

QString FileManager::groupName(gid_t gid)
{
    static QHash<gid_t, QString> cache;

    auto it = cache.constFind(gid);
    if (it != cache.constEnd())
        return it.value();

    const group *entry = getgrgid(gid);
    const QString name = entry ? QString::fromLocal8Bit(entry->gr_name)
                               : QString::number(gid);
    cache.insert(gid, name);
    return name;
}

QString FileManager::timeText(time_t t)
{
    return QDateTime::fromSecsSinceEpoch(static_cast<qint64>(t))
        .toString("yyyy-MM-dd HH:mm:ss");
}

QString FileManager::hexDump(const unsigned char *data, size_t length)
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

// Open a path and make sure it is a REGULAR file (not a pipe, device or folder).
// O_NONBLOCK keeps a stray FIFO from freezing the window; O_NOCTTY avoids
// accidentally taking over a terminal.
int FileManager::openRegular(const QString &path, int flags, int &err)
{
    err = 0;

    const QByteArray native = QFile::encodeName(path);
    const int fd = ::open(native.constData(), flags | O_CLOEXEC | O_NONBLOCK | O_NOCTTY);

    if (fd < 0)
    {
        err = errno;
        return -1;
    }

    struct stat st;
    if (::fstat(fd, &st) != 0)
    {
        err = errno;
        ::close(fd);
        return -1;
    }

    if (!S_ISREG(st.st_mode))
    {
        err = S_ISDIR(st.st_mode) ? EISDIR : EINVAL;
        ::close(fd);
        return -1;
    }

    return fd;
}

bool FileManager::writeAll(int fd, const char *data, size_t length, int &err)
{
    err = 0;
    size_t written = 0;

    while (written < length)
    {
        const ssize_t result = ::write(fd, data + written, length - written);

        if (result < 0)
        {
            if (errno == EINTR)
                continue;

            err = errno;
            return false;
        }

        written += static_cast<size_t>(result);
    }

    return true;
}

QString FileManager::selectedName() const
{
    if (!table->selectionModel())
        return QString();

    const QModelIndexList rows = table->selectionModel()->selectedRows(0);
    if (rows.isEmpty())
        return QString();

    const QTableWidgetItem *item = table->item(rows.first().row(), 0);
    return item ? item->text() : QString();
}

QString FileManager::fullPath(const QString &name) const
{
    if (currentDir == "/")
        return "/" + name;

    return currentDir + "/" + name;
}

// ---------------------------------------------------------------------------
// Directory listing: opendir() / readdir() / lstat()
// ---------------------------------------------------------------------------
bool FileManager::listDirectory(const QString &path, std::vector<Entry> &entries, int &err) const
{
    err = 0;

    const QByteArray native = QFile::encodeName(path);
    DIR *directory = ::opendir(native.constData());

    if (!directory)
    {
        err = errno;
        return false;
    }

    const bool showHidden = hiddenCheck->isChecked();

    while (const dirent *item = ::readdir(directory))
    {
        const QString name = QFile::decodeName(item->d_name);

        if (name == "." || name == "..")
            continue;

        if (!showHidden && name.startsWith('.'))
            continue;

        if (entries.size() >= kMaxListedEntries)
            break;

        Entry entry;
        entry.name = name;

        const QString full = (path == "/") ? "/" + name : path + "/" + name;
        const QByteArray nativeFull = QFile::encodeName(full);

        // lstat() describes the entry itself, even when it is a symbolic link
        if (::lstat(nativeFull.constData(), &entry.st) != 0)
            continue;   // it vanished while we were reading the folder

        if (S_ISLNK(entry.st.st_mode))
        {
            char target[PATH_MAX];
            const ssize_t length = ::readlink(nativeFull.constData(), target, sizeof(target) - 1);
            if (length > 0)
            {
                target[length] = '\0';
                entry.linkTarget = QFile::decodeName(target);
            }
        }

        entries.push_back(entry);
    }

    ::closedir(directory);

    // Folders first, then names without regard to upper/lower case
    std::sort(entries.begin(), entries.end(), [](const Entry &a, const Entry &b) {
        const bool aDir = S_ISDIR(a.st.st_mode);
        const bool bDir = S_ISDIR(b.st.st_mode);

        if (aDir != bDir)
            return aDir;

        return a.name.compare(b.name, Qt::CaseInsensitive) < 0;
    });

    return true;
}

void FileManager::fillTable(const std::vector<Entry> &entries, const QString &selectName)
{
    table->setRowCount(0);

    for (const Entry &entry : entries)
    {
        QString type;
        const mode_t mode = entry.st.st_mode;

        if (S_ISDIR(mode))       type = "folder";
        else if (S_ISREG(mode))  type = "file";
        else if (S_ISLNK(mode))  type = "link -> " + entry.linkTarget;
        else if (S_ISFIFO(mode)) type = "pipe (FIFO)";
        else if (S_ISSOCK(mode)) type = "socket";
        else if (S_ISCHR(mode))  type = "character device";
        else if (S_ISBLK(mode))  type = "block device";
        else                     type = "other";

        const QString size = S_ISREG(mode) ? humanSize(static_cast<qint64>(entry.st.st_size))
                                           : QString("-");

        const QString permissions = QString("%1  (%2)")
            .arg(permissionString(mode),
                 QString::number(mode & 07777, 8).rightJustified(4, '0'));

        const QString owner = userName(entry.st.st_uid) + ":" + groupName(entry.st.st_gid);

        const int row = table->rowCount();
        table->insertRow(row);

        table->setItem(row, 0, new QTableWidgetItem(entry.name));
        table->setItem(row, 1, new QTableWidgetItem(type));
        table->setItem(row, 2, new QTableWidgetItem(size));
        table->setItem(row, 3, new QTableWidgetItem(permissions));
        table->setItem(row, 4, new QTableWidgetItem(owner));
        table->setItem(row, 5, new QTableWidgetItem(timeText(entry.st.st_mtime)));
    }

    if (!selectName.isEmpty())
        selectByName(selectName);
}

void FileManager::selectByName(const QString &name)
{
    for (int row = 0; row < table->rowCount(); ++row)
    {
        if (table->item(row, 0)->text() == name)
        {
            table->selectRow(row);
            table->scrollToItem(table->item(row, 0));
            return;
        }
    }
}

void FileManager::navigateTo(const QString &path)
{
    // realpath(): absolute path with ".." and symbolic links resolved
    const QByteArray native = QFile::encodeName(path);
    char resolved[PATH_MAX];

    if (!::realpath(native.constData(), resolved))
    {
        report(QString("Cannot open \"%1\": %2").arg(path, errorText(errno)), true);
        pathEdit->setText(currentDir);
        return;
    }

    const QString canonical = QFile::decodeName(resolved);

    struct stat st;
    if (::stat(resolved, &st) != 0)
    {
        report(QString("Cannot open \"%1\": %2").arg(canonical, errorText(errno)), true);
        pathEdit->setText(currentDir);
        return;
    }

    if (!S_ISDIR(st.st_mode))
    {
        report(QString("\"%1\" is not a folder.").arg(canonical), true);
        pathEdit->setText(currentDir);
        return;
    }

    std::vector<Entry> entries;
    int err = 0;

    if (!listDirectory(canonical, entries, err))
    {
        report(QString("Cannot read folder \"%1\": %2").arg(canonical, errorText(err)), true);
        pathEdit->setText(currentDir);
        return;
    }

    currentDir = canonical;
    pathEdit->setText(currentDir);
    fillTable(entries, QString());

    report(QString("Listed %1 item(s) in %2 using opendir() / readdir() / lstat().")
               .arg(entries.size()).arg(currentDir));
}

void FileManager::refresh()
{
    if (currentDir.isEmpty())
        return;

    const QString keep = selectedName();

    std::vector<Entry> entries;
    int err = 0;

    if (!listDirectory(currentDir, entries, err))
    {
        report(QString("Cannot read folder \"%1\": %2").arg(currentDir, errorText(err)), true);
        return;
    }

    fillTable(entries, keep);
    report(QString("Refreshed: %1 item(s) in %2.").arg(entries.size()).arg(currentDir));
}

void FileManager::goUp()
{
    navigateTo(currentDir + "/..");
}

void FileManager::goHome()
{
    navigateTo(QDir::homePath());
}

void FileManager::goToTyped()
{
    navigateTo(pathEdit->text().trimmed());
}

void FileManager::onCellActivated(int row)
{
    const QTableWidgetItem *item = table->item(row, 0);
    if (!item)
        return;

    const QString path = fullPath(item->text());
    const QByteArray native = QFile::encodeName(path);

    struct stat st;
    if (::stat(native.constData(), &st) != 0)   // stat() follows symbolic links
    {
        report(QString("Cannot access \"%1\": %2").arg(path, errorText(errno)), true);
        return;
    }

    if (S_ISDIR(st.st_mode))
        navigateTo(path);
    else if (S_ISREG(st.st_mode))
        loadIntoEditor(path);
    else
        report("Only folders and regular files can be opened here.", true);
}

// ---------------------------------------------------------------------------
// File content: open() / read() / write() / close()
// ---------------------------------------------------------------------------
void FileManager::clearEditor()
{
    editor->clear();
    editorPath.clear();
    editorLocked = false;
    editorLabel->setText("No file open. Use New File, or select a file and click Open / Read.");
}

void FileManager::loadIntoEditor(const QString &path)
{
    int err = 0;
    const int fd = openRegular(path, O_RDONLY, err);

    if (fd < 0)
    {
        report(QString("Cannot open \"%1\": %2").arg(path, errorText(err)), true);
        return;
    }

    QByteArray data;
    char buffer[8192];
    bool readFailed = false;

    while (data.size() <= kMaxViewBytes)
    {
        const ssize_t count = ::read(fd, buffer, sizeof(buffer));

        if (count < 0)
        {
            if (errno == EINTR)
                continue;

            err = errno;
            readFailed = true;
            break;
        }

        if (count == 0)
            break;

        data.append(buffer, static_cast<qsizetype>(count));
    }

    if (::close(fd) != 0 && !readFailed)
    {
        err = errno;
        readFailed = true;
    }

    if (readFailed)
    {
        report(QString("Reading \"%1\" failed: %2").arg(path, errorText(err)), true);
        return;
    }

    const bool tooLarge = data.size() > kMaxViewBytes;
    if (tooLarge)
        data.truncate(static_cast<qsizetype>(kMaxViewBytes));

    // Text only if it has no NUL bytes and is valid UTF-8 (so saving cannot corrupt it)
    const QString text = QString::fromUtf8(data);
    const bool isText = !data.contains('\0') && text.toUtf8() == data;

    editorPath = path;

    if (!isText)
    {
        editor->clear();
        editorLocked = true;
        editorLabel->setText(QString("%1 - binary data, shown as hex in the Details tab (editing disabled)")
                                 .arg(path));

        appendOutput(QString("--- First bytes of %1 ---").arg(path));
        appendOutput(hexDump(reinterpret_cast<const unsigned char *>(data.constData()),
                             static_cast<size_t>(std::min<qsizetype>(data.size(), 256))));
        tabs->setCurrentIndex(1);
        report("This file is binary, so it is shown as a hex dump instead of text.");
        return;
    }

    editor->setPlainText(text);
    editorLocked = tooLarge;

    if (tooLarge)
        editorLabel->setText(QString("%1 - only the first 64 KiB are shown. Saving is disabled so the "
                                     "rest of the file is not lost; Append still works.").arg(path));
    else
        editorLabel->setText(QString("Editing: %1  (%2 bytes)").arg(path).arg(data.size()));

    tabs->setCurrentIndex(0);
    report(QString("open() + read() loaded %1 byte(s) from %2.").arg(data.size()).arg(path));
}

void FileManager::viewFile()
{
    const QString name = selectedName();
    if (name.isEmpty())
    {
        report("Select a file first.", true);
        return;
    }

    loadIntoEditor(fullPath(name));
}

void FileManager::createFile()
{
    bool accepted = false;
    const QString name = QInputDialog::getText(
        this, "New File", "File name:", QLineEdit::Normal, "", &accepted).trimmed();

    if (!accepted)
        return;

    QString reason;
    if (!validName(name, reason))
    {
        report("Invalid name: " + reason, true);
        return;
    }

    const QString path = fullPath(name);
    const QByteArray native = QFile::encodeName(path);

    // O_EXCL: fail if the file already exists, instead of replacing it.
    // 0644 is only a request: the kernel removes the bits that are set in the umask.
    const int fd = ::open(native.constData(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    const int err = errno;

    if (fd < 0)
    {
        report(QString("Cannot create \"%1\": %2").arg(path, errorText(err)), true);
        return;
    }

    ::close(fd);

    refresh();
    selectByName(name);
    loadIntoEditor(path);
    report(QString("open(O_CREAT|O_EXCL, 0644) created %1. Type in the editor, then Save.").arg(path));
}

void FileManager::createFolder()
{
    bool accepted = false;
    const QString name = QInputDialog::getText(
        this, "New Folder", "Folder name:", QLineEdit::Normal, "", &accepted).trimmed();

    if (!accepted)
        return;

    QString reason;
    if (!validName(name, reason))
    {
        report("Invalid name: " + reason, true);
        return;
    }

    const QString path = fullPath(name);
    const QByteArray native = QFile::encodeName(path);

    if (::mkdir(native.constData(), 0755) != 0)
    {
        report(QString("Cannot create folder \"%1\": %2").arg(path, errorText(errno)), true);
        return;
    }

    refresh();
    selectByName(name);
    report(QString("mkdir(\"%1\", 0755) succeeded.").arg(path));
}

void FileManager::saveFile()
{
    if (editorPath.isEmpty())
    {
        report("No file is open. Create a file or open one first.", true);
        return;
    }

    if (editorLocked)
    {
        report("Saving is disabled for this file (it is binary or larger than 64 KiB).", true);
        return;
    }

    const QByteArray bytes = editor->toPlainText().toUtf8();

    int err = 0;

    // Opening with O_TRUNC erases the old content, so ask when there is some.
    int probe = openRegular(editorPath, O_RDONLY, err);
    if (probe >= 0)
    {
        struct stat st;
        const bool hasData = (::fstat(probe, &st) == 0 && st.st_size > 0);
        ::close(probe);

        if (hasData)
        {
            const auto answer = QMessageBox::question(
                this, "Overwrite file?",
                QString("Save will replace the current content of\n%1\nContinue?").arg(editorPath),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);

            if (answer != QMessageBox::Yes)
                return;
        }
    }

    // No O_CREAT here: if somebody deleted the file meanwhile, we want to know.
    const int fd = openRegular(editorPath, O_WRONLY | O_TRUNC, err);

    if (fd < 0)
    {
        report(QString("Cannot save \"%1\": %2").arg(editorPath, errorText(err)), true);
        return;
    }

    const bool wrote = writeAll(fd, bytes.constData(), static_cast<size_t>(bytes.size()), err);

    // close() can report a delayed write error, so its result matters.
    if (::close(fd) != 0 && wrote)
    {
        err = errno;
        report(QString("close() reported an error while saving: %1").arg(errorText(err)), true);
        return;
    }

    if (!wrote)
    {
        report(QString("write() failed: %1").arg(errorText(err)), true);
        return;
    }

    refresh();
    editorLabel->setText(QString("Editing: %1  (%2 bytes)").arg(editorPath).arg(bytes.size()));
    report(QString("write() saved %1 byte(s) to %2.").arg(bytes.size()).arg(editorPath));
}

void FileManager::appendToFile()
{
    if (editorPath.isEmpty())
    {
        report("No file is open. Create a file or open one first.", true);
        return;
    }

    QByteArray bytes = appendEdit->text().toUtf8();
    if (appendNewlineCheck->isChecked())
        bytes.append('\n');

    if (bytes.isEmpty())
    {
        report("Type the text to append.", true);
        return;
    }

    int err = 0;
    const int fd = openRegular(editorPath, O_WRONLY | O_APPEND, err);

    if (fd < 0)
    {
        report(QString("Cannot append to \"%1\": %2").arg(editorPath, errorText(err)), true);
        return;
    }

    // O_APPEND: the kernel moves to the end before EVERY write, atomically.
    const bool wrote = writeAll(fd, bytes.constData(), static_cast<size_t>(bytes.size()), err);
    const bool closed = (::close(fd) == 0);
    const int closeErr = errno;

    if (!wrote)
    {
        report(QString("write() failed: %1").arg(errorText(err)), true);
        return;
    }

    if (!closed)
    {
        report(QString("close() reported an error: %1").arg(errorText(closeErr)), true);
        return;
    }

    appendEdit->clear();
    const QString path = editorPath;
    loadIntoEditor(path);
    refresh();
    report(QString("O_APPEND write added %1 byte(s) to the end of %2.").arg(bytes.size()).arg(path));
}

// ---------------------------------------------------------------------------
// rename() / unlink() / rmdir() / chmod()
// ---------------------------------------------------------------------------
void FileManager::renameEntry()
{
    const QString oldName = selectedName();
    if (oldName.isEmpty())
    {
        report("Select a file or folder first.", true);
        return;
    }

    bool accepted = false;
    const QString newName = QInputDialog::getText(
        this, "Rename", "New name:", QLineEdit::Normal, oldName, &accepted).trimmed();

    if (!accepted || newName == oldName)
        return;

    QString reason;
    if (!validName(newName, reason))
    {
        report("Invalid name: " + reason, true);
        return;
    }

    const QString oldPath = fullPath(oldName);
    const QString newPath = fullPath(newName);
    const QByteArray nativeOld = QFile::encodeName(oldPath);
    const QByteArray nativeNew = QFile::encodeName(newPath);

    // rename() silently REPLACES an existing target, so check first.
    struct stat existing;
    if (::lstat(nativeNew.constData(), &existing) == 0)
    {
        const auto answer = QMessageBox::question(
            this, "Replace existing item?",
            QString("\"%1\" already exists. Renaming will replace it.\nContinue?").arg(newName),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);

        if (answer != QMessageBox::Yes)
            return;
    }

    if (::rename(nativeOld.constData(), nativeNew.constData()) != 0)
    {
        report(QString("rename() failed: %1").arg(errorText(errno)), true);
        return;
    }

    if (editorPath == oldPath)
    {
        editorPath = newPath;
        editorLabel->setText(QString("Editing: %1").arg(newPath));
    }

    refresh();
    selectByName(newName);
    report(QString("rename(\"%1\", \"%2\") succeeded.").arg(oldName, newName));
}

void FileManager::deleteEntry()
{
    const QString name = selectedName();
    if (name.isEmpty())
    {
        report("Select a file or folder first.", true);
        return;
    }

    const QString path = fullPath(name);
    const QByteArray native = QFile::encodeName(path);

    struct stat st;
    if (::lstat(native.constData(), &st) != 0)
    {
        report(QString("Cannot find \"%1\": %2").arg(path, errorText(errno)), true);
        refresh();
        return;
    }

    const bool isFolder = S_ISDIR(st.st_mode);

    const auto answer = QMessageBox::warning(
        this, isFolder ? "Delete folder?" : "Delete file?",
        isFolder
            ? QString("Delete the EMPTY folder\n%1\n(rmdir refuses folders that contain files.)").arg(path)
            : QString("Permanently delete\n%1\nThere is no recycle bin.").arg(path),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);

    if (answer != QMessageBox::Yes)
        return;

    // unlink() removes a name (a file or a symbolic link); rmdir() removes an empty folder.
    const int result = isFolder ? ::rmdir(native.constData()) : ::unlink(native.constData());
    const int err = errno;

    if (result != 0)
    {
        report(QString("%1 failed for \"%2\": %3")
                   .arg(QString(isFolder ? "rmdir()" : "unlink()"), path, errorText(err)), true);
        return;
    }

    if (editorPath == path)
        clearEditor();

    refresh();
    report(QString("%1(\"%2\") succeeded.").arg(QString(isFolder ? "rmdir" : "unlink"), path));
}

void FileManager::changePermissions()
{
    const QString name = selectedName();
    if (name.isEmpty())
    {
        report("Select a file or folder first.", true);
        return;
    }

    const QString path = fullPath(name);
    const QByteArray native = QFile::encodeName(path);

    struct stat st;
    if (::stat(native.constData(), &st) != 0)
    {
        report(QString("Cannot read \"%1\": %2").arg(path, errorText(errno)), true);
        return;
    }

    static const mode_t masks[3][3] = {
        {S_IRUSR, S_IWUSR, S_IXUSR},
        {S_IRGRP, S_IWGRP, S_IXGRP},
        {S_IROTH, S_IWOTH, S_IXOTH}
    };

    QDialog dialog(this);
    dialog.setWindowTitle("Permissions: " + name);

    QVBoxLayout *dialogLayout = new QVBoxLayout(&dialog);
    QGridLayout *grid = new QGridLayout;

    const char *columns[3] = {"Read", "Write", "Execute"};
    const char *rows[3] = {"Owner", "Group", "Others"};
    QCheckBox *boxes[3][3];

    for (int c = 0; c < 3; ++c)
        grid->addWidget(new QLabel(columns[c]), 0, c + 1);

    for (int r = 0; r < 3; ++r)
    {
        grid->addWidget(new QLabel(rows[r]), r + 1, 0);

        for (int c = 0; c < 3; ++c)
        {
            boxes[r][c] = new QCheckBox;
            boxes[r][c]->setChecked(st.st_mode & masks[r][c]);
            grid->addWidget(boxes[r][c], r + 1, c + 1);
        }
    }

    QLabel *preview = new QLabel;
    preview->setStyleSheet("font-weight: bold;");

    auto computeBits = [&boxes]() {
        mode_t bits = 0;
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c)
                if (boxes[r][c]->isChecked())
                    bits |= masks[r][c];
        return bits;
    };

    auto updatePreview = [&]() {
        const mode_t bits = computeBits();
        preview->setText(QString("New mode: %1   %2")
                             .arg(QString::number(bits, 8).rightJustified(3, '0'),
                                  permissionString((st.st_mode & S_IFMT) | bits).mid(1)));
    };

    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            connect(boxes[r][c], &QCheckBox::toggled, &dialog, updatePreview);

    updatePreview();

    QDialogButtonBox *buttons =
        new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    dialogLayout->addLayout(grid);
    dialogLayout->addWidget(preview);
    dialogLayout->addWidget(buttons);

    if (dialog.exec() != QDialog::Accepted)
        return;

    // Keep setuid / setgid / sticky bits as they were; change only the nine rwx bits.
    const mode_t newMode = (st.st_mode & 07000) | computeBits();

    if (::chmod(native.constData(), newMode) != 0)
    {
        report(QString("chmod() failed: %1 (only the owner or root may change permissions)")
                   .arg(errorText(errno)), true);
        return;
    }

    refresh();
    selectByName(name);
    report(QString("chmod(\"%1\", %2) succeeded.")
               .arg(path, QString::number(newMode & 07777, 8).rightJustified(4, '0')));
}

// ---------------------------------------------------------------------------
// Properties: lstat(), stat(), fstat(), access(), readlink()
// ---------------------------------------------------------------------------
void FileManager::showProperties()
{
    const QString name = selectedName();
    if (name.isEmpty())
    {
        report("Select a file or folder first.", true);
        return;
    }

    const QString path = fullPath(name);
    const QByteArray native = QFile::encodeName(path);

    struct stat linkStat;
    if (::lstat(native.constData(), &linkStat) != 0)
    {
        report(QString("lstat(\"%1\") failed: %2").arg(path, errorText(errno)), true);
        return;
    }

    appendOutput(QString("=== Properties of %1 ===").arg(path));
    appendOutput(QString("  Type:         %1").arg(S_ISLNK(linkStat.st_mode) ? "symbolic link" : "item"));

    // stat() follows a symbolic link to the real item
    struct stat st = linkStat;
    if (S_ISLNK(linkStat.st_mode))
    {
        char target[PATH_MAX];
        const ssize_t length = ::readlink(native.constData(), target, sizeof(target) - 1);
        if (length > 0)
        {
            target[length] = '\0';
            appendOutput(QString("  Points to:    %1   (readlink)").arg(QFile::decodeName(target)));
        }

        if (::stat(native.constData(), &st) != 0)
        {
            appendOutput(QString("  Target:       cannot be read (%1) - a broken link").arg(errorText(errno)));
            tabs->setCurrentIndex(1);
            report("This symbolic link points to something that does not exist.", true);
            return;
        }
    }

    char resolved[PATH_MAX];
    if (::realpath(native.constData(), resolved))
        appendOutput(QString("  Real path:    %1   (realpath)").arg(QFile::decodeName(resolved)));

    QString kind = "other";
    if (S_ISREG(st.st_mode))       kind = "regular file";
    else if (S_ISDIR(st.st_mode))  kind = "directory";
    else if (S_ISCHR(st.st_mode))  kind = "character device";
    else if (S_ISBLK(st.st_mode))  kind = "block device";
    else if (S_ISFIFO(st.st_mode)) kind = "pipe (FIFO)";
    else if (S_ISSOCK(st.st_mode)) kind = "socket";

    appendOutput(QString("  File type:    %1").arg(kind));
    appendOutput(QString("  Size:         %1 bytes (%2)")
                     .arg(static_cast<qlonglong>(st.st_size)).arg(humanSize(static_cast<qint64>(st.st_size))));
    appendOutput(QString("  Disk usage:   %1 blocks of 512 bytes, preferred I/O block %2 bytes")
                     .arg(static_cast<qlonglong>(st.st_blocks)).arg(static_cast<qlonglong>(st.st_blksize)));
    appendOutput(QString("  Inode:        %1   Hard links: %2")
                     .arg(static_cast<qulonglong>(st.st_ino)).arg(static_cast<qulonglong>(st.st_nlink)));
    appendOutput(QString("  Permissions:  %1  (octal %2)")
                     .arg(permissionString(st.st_mode),
                          QString::number(st.st_mode & 07777, 8).rightJustified(4, '0')));
    appendOutput(QString("  Owner:        %1 (uid %2)   Group: %3 (gid %4)")
                     .arg(userName(st.st_uid)).arg(static_cast<qulonglong>(st.st_uid))
                     .arg(groupName(st.st_gid)).arg(static_cast<qulonglong>(st.st_gid)));

    // access(): what may the CURRENT user do with this path?
    const bool canRead = ::access(native.constData(), R_OK) == 0;
    const bool canWrite = ::access(native.constData(), W_OK) == 0;
    const bool canExecute = ::access(native.constData(), X_OK) == 0;

    appendOutput(QString("  access():     you (uid %1) may read: %2, write: %3, execute/enter: %4")
                     .arg(getuid())
                     .arg(QString(canRead ? "yes" : "no"),
                          QString(canWrite ? "yes" : "no"),
                          QString(canExecute ? "yes" : "no")));

    appendOutput(QString("  Accessed:     %1  (atime)").arg(timeText(st.st_atime)));
    appendOutput(QString("  Modified:     %1  (mtime: content changed)").arg(timeText(st.st_mtime)));
    appendOutput(QString("  Changed:      %1  (ctime: metadata changed)").arg(timeText(st.st_ctime)));

    // fstat() asks about an OPEN descriptor instead of a path. It must agree with stat().
    if (S_ISREG(st.st_mode))
    {
        int err = 0;
        const int fd = openRegular(path, O_RDONLY, err);

        if (fd >= 0)
        {
            struct stat viaFd;
            if (::fstat(fd, &viaFd) == 0)
            {
                const bool same = viaFd.st_ino == st.st_ino && viaFd.st_size == st.st_size &&
                                  viaFd.st_mode == st.st_mode;
                appendOutput(QString("  fstat() on an open descriptor %1 stat(): %2")
                                 .arg(same ? "agrees with" : "differs from")
                                 .arg(same ? "same inode, size and mode" : "the file changed in between"));
            }
            ::close(fd);
        }
        else
        {
            appendOutput(QString("  fstat():      cannot open the file for reading (%1)").arg(errorText(err)));
        }
    }

    tabs->setCurrentIndex(1);
    report(QString("Showed the properties of %1. Compare with:  stat \"%2\"").arg(name, path));
}