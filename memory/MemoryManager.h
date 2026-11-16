#ifndef MEMORYMANAGER_H
#define MEMORYMANAGER_H

#include <QWidget>
#include <QMap>
#include <QString>

#include <cstddef>

class QTableWidget;
class QPushButton;
class QSpinBox;
class QComboBox;
class QLineEdit;
class QPlainTextEdit;
class QLabel;
class QTimer;

// Memory Management module.
// Every block shown here is REAL memory owned by this application's process,
// obtained with malloc(), calloc() or mmap() and tracked until it is released.
class MemoryManager : public QWidget
{
    Q_OBJECT

public:
    explicit MemoryManager(QWidget *parent = nullptr);
    ~MemoryManager() override;     // releases every block still allocated

private:
    enum class Kind { Malloc, Calloc, Mmap };

    struct Allocation
    {
        int id = 0;
        Kind kind = Kind::Malloc;
        void *ptr = nullptr;
        size_t size = 0;           // size the user asked for
    };

    // Widgets
    QLabel *statsLabel;
    QSpinBox *sizeSpin;
    QComboBox *unitCombo;
    QComboBox *methodCombo;
    QTableWidget *table;
    QSpinBox *offsetSpin;
    QLineEdit *dataEdit;
    QSpinBox *lengthSpin;
    QSpinBox *fillSpin;
    QPlainTextEdit *output;
    QLabel *statusLabel;
    QTimer *statsTimer;

    // State
    QMap<int, Allocation> allocations;
    int nextId = 1;

    // UI
    void setupUI();
    void refreshTable();
    void refreshStats();
    void report(const QString &message, bool error = false);
    void appendOutput(const QString &text);

    // Operations (buttons)
    void allocateMemory();
    void testAllocationFailure();
    void resizeMemory();
    void writeMemory();
    void readMemory();
    void fillMemory();
    void clearMemory();
    void freeMemory();
    void freeAllMemory();
    void showMapping();

    // Helpers
    Allocation *selectedAllocation();
    size_t requestedSize() const;
    size_t totalTracked() const;
    static size_t usableSize(const Allocation &allocation);
    static QString kindName(Kind kind);
    static QString addressText(const void *ptr);
    static QString hexDump(const unsigned char *data, size_t length, size_t baseOffset);
    static bool releaseBlock(const Allocation &allocation, int &errorCode);
};

#endif