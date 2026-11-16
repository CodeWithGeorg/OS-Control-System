#ifndef IOMANAGER_H
#define IOMANAGER_H

#include <QWidget>
#include <QMap>
#include <QString>

#include <cstddef>

class QTableWidget;
class QPushButton;
class QSpinBox;
class QComboBox;
class QLineEdit;
class QCheckBox;
class QPlainTextEdit;
class QLabel;
class QTimer;

// Input/Output Management module.
// Every descriptor in the table is a REAL Linux file descriptor owned by this
// application's process. Nothing here is simulated.
class IOManager : public QWidget
{
    Q_OBJECT

public:
    explicit IOManager(QWidget *parent = nullptr);
    ~IOManager() override;          // closes every descriptor we opened

private:
    struct Descriptor
    {
        int fd = -1;
        QString resource;           // path, or "pipe read end", ...
        QString note;
    };

    // Widgets
    QLabel *statsLabel;
    QLineEdit *pathEdit;
    QComboBox *modeCombo;
    QTableWidget *table;
    QSpinBox *lengthSpin;
    QSpinBox *seekSpin;
    QComboBox *whenceCombo;
    QLineEdit *dataEdit;
    QCheckBox *newlineCheck;
    QPlainTextEdit *output;
    QLabel *statusLabel;
    QTimer *statsTimer;

    // State: only descriptors we opened ourselves are ever closed
    QMap<int, Descriptor> descriptors;

    // UI
    void setupUI();
    void refreshTable();
    void refreshStats();
    void report(const QString &message, bool error = false);
    void appendOutput(const QString &text);

    // Operations
    void openResource();
    void createPipe();
    void duplicateDescriptor();
    void closeDescriptor();
    void closeAllDescriptors();
    void readFromDescriptor();
    void writeToDescriptor();
    void seekDescriptor();
    void fillPipe();
    void showInfo();
    void showProcessDescriptors();
    void runErrorDemo();

    // Helpers
    Descriptor *selectedDescriptor();
    static QString errorText(int err);
    static QString accessText(int fd);
    static QString positionText(int fd);
    static QString hexDump(const unsigned char *data, size_t length);
    static size_t writeBytes(int fd, const char *data, size_t length, int &err);
};

#endif