#ifndef FILEMANAGER_H
#define FILEMANAGER_H

#include <QWidget>
#include <QString>

#include <sys/stat.h>
#include <vector>

class QTableWidget;
class QLineEdit;
class QCheckBox;
class QPlainTextEdit;
class QLabel;
class QTabWidget;

// File Subsystem module.
// A small file manager built directly on POSIX calls:
// opendir/readdir, lstat/stat/fstat, open/read/write/close,
// mkdir, rename, unlink, rmdir, chmod and access.
class FileManager : public QWidget
{
    Q_OBJECT

public:
    explicit FileManager(QWidget *parent = nullptr);

private:
    struct Entry
    {
        QString name;
        struct stat st;             // from lstat(): describes the entry itself
        QString linkTarget;         // only for symbolic links
    };

    // Widgets
    QLineEdit *pathEdit;
    QCheckBox *hiddenCheck;
    QTableWidget *table;
    QTabWidget *tabs;
    QLabel *editorLabel;
    QPlainTextEdit *editor;
    QLineEdit *appendEdit;
    QCheckBox *appendNewlineCheck;
    QPlainTextEdit *output;
    QLabel *statusLabel;

    // State
    QString currentDir;
    QString editorPath;             // file the editor belongs to ("" = none)
    bool editorLocked = false;      // true if saving could damage the file

    // UI
    void setupUI();
    void report(const QString &message, bool error = false);
    void appendOutput(const QString &text);

    // Directory
    bool listDirectory(const QString &path, std::vector<Entry> &entries, int &err) const;
    void navigateTo(const QString &path);
    void refresh();
    void goUp();
    void goHome();
    void goToTyped();
    void onCellActivated(int row);
    void fillTable(const std::vector<Entry> &entries, const QString &selectName);
    void selectByName(const QString &name);

    // File operations
    void createFile();
    void createFolder();
    void viewFile();
    void saveFile();
    void appendToFile();
    void renameEntry();
    void deleteEntry();
    void changePermissions();
    void showProperties();
    void loadIntoEditor(const QString &path);
    void clearEditor();

    // Helpers
    QString selectedName() const;
    QString fullPath(const QString &name) const;
    static bool validName(const QString &name, QString &reason);
    static QString errorText(int err);
    static QString permissionString(mode_t mode);
    static QString humanSize(qint64 bytes);
    static QString userName(uid_t uid);
    static QString groupName(gid_t gid);
    static QString timeText(time_t t);
    static QString hexDump(const unsigned char *data, size_t length);
    static int openRegular(const QString &path, int flags, int &err);
    static bool writeAll(int fd, const char *data, size_t length, int &err);
};

#endif