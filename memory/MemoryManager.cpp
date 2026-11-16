#include "MemoryManager.h"

#include <QComboBox>
#include <QFile>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <utility>

#include <malloc.h>
#include <sys/mman.h>
#include <unistd.h>

// Safety limits so a mistake cannot freeze your laptop
static const size_t kMaxSingleAllocation = 512ULL * 1024 * 1024;    // 512 MiB
static const size_t kMaxTotalAllocation  = 1024ULL * 1024 * 1024;   // 1 GiB

// ---------------------------------------------------------------------------
// Constructor / destructor
// ---------------------------------------------------------------------------
MemoryManager::MemoryManager(QWidget *parent)
    : QWidget(parent)
{
    setupUI();
    refreshTable();
    refreshStats();

    statsTimer = new QTimer(this);
    connect(statsTimer, &QTimer::timeout, this, &MemoryManager::refreshStats);
    statsTimer->start(2000);
}

MemoryManager::~MemoryManager()
{
    // Never leak: give every block back to the system.
    for (const Allocation &allocation : std::as_const(allocations))
    {
        int ignored = 0;
        releaseBlock(allocation, ignored);
    }
    allocations.clear();
}

// ---------------------------------------------------------------------------
// User interface
// ---------------------------------------------------------------------------
void MemoryManager::setupUI()
{
    QVBoxLayout *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(35, 30, 35, 30);
    mainLayout->setSpacing(10);

    QLabel *heading = new QLabel("Memory Management");
    heading->setStyleSheet("font-size: 25px; font-weight: bold;");

    QLabel *description = new QLabel(
        "Allocate, use and release real memory with malloc(), calloc() and mmap().");
    description->setStyleSheet("color: #6b7280;");

    statsLabel = new QLabel;
    statsLabel->setStyleSheet(
        "background-color: white; border: 1px solid #e5e7eb;"
        "border-radius: 6px; padding: 8px; font-size: 12px;");

    // ----- Row 1: allocate / resize -----
    sizeSpin = new QSpinBox;
    sizeSpin->setRange(1, 1000000);
    sizeSpin->setValue(64);
    sizeSpin->setMinimumWidth(100);

    unitCombo = new QComboBox;
    unitCombo->addItems({"Bytes", "KiB", "MiB"});
    unitCombo->setCurrentIndex(1);

    methodCombo = new QComboBox;
    methodCombo->addItem("malloc()", static_cast<int>(Kind::Malloc));
    methodCombo->addItem("calloc()", static_cast<int>(Kind::Calloc));
    methodCombo->addItem("mmap()",   static_cast<int>(Kind::Mmap));

    QPushButton *allocateButton = new QPushButton("Allocate");
    QPushButton *resizeButton   = new QPushButton("Resize Selected");
    QPushButton *failureButton  = new QPushButton("Test Allocation Failure");

    QHBoxLayout *row1 = new QHBoxLayout;
    row1->addWidget(new QLabel("Size:"));
    row1->addWidget(sizeSpin);
    row1->addWidget(unitCombo);
    row1->addWidget(new QLabel("Method:"));
    row1->addWidget(methodCombo);
    row1->addWidget(allocateButton);
    row1->addWidget(resizeButton);
    row1->addStretch();
    row1->addWidget(failureButton);

    // ----- Table -----
    table = new QTableWidget(0, 5);
    table->setHorizontalHeaderLabels({"ID", "Method", "Address", "Requested (bytes)", "Usable (bytes)"});
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    table->verticalHeader()->setVisible(false);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setMinimumHeight(130);

    // ----- Row 2: write -----
    offsetSpin = new QSpinBox;
    offsetSpin->setRange(0, INT_MAX);

    dataEdit = new QLineEdit;
    dataEdit->setPlaceholderText("Text to write into the selected block");

    QPushButton *writeButton = new QPushButton("Write");

    QHBoxLayout *row2 = new QHBoxLayout;
    row2->addWidget(new QLabel("Offset:"));
    row2->addWidget(offsetSpin);
    row2->addWidget(dataEdit, 1);
    row2->addWidget(writeButton);

    // ----- Row 3: read / fill / clear / free -----
    lengthSpin = new QSpinBox;
    lengthSpin->setRange(1, 1024);
    lengthSpin->setValue(64);

    fillSpin = new QSpinBox;
    fillSpin->setRange(0, 255);
    fillSpin->setDisplayIntegerBase(16);
    fillSpin->setPrefix("0x");
    fillSpin->setValue(0xAA);

    QPushButton *readButton    = new QPushButton("Read");
    QPushButton *fillButton    = new QPushButton("Fill Block");
    QPushButton *clearButton   = new QPushButton("Clear (zero)");
    QPushButton *mappingButton = new QPushButton("Show in /proc/self/maps");
    QPushButton *freeButton    = new QPushButton("Free Selected");
    QPushButton *freeAllButton = new QPushButton("Free All");

    const QString redStyle =
        "QPushButton { background-color: #dc2626; }"
        "QPushButton:hover { background-color: #b91c1c; }";
    freeButton->setStyleSheet(redStyle);
    freeAllButton->setStyleSheet(redStyle);

    QHBoxLayout *row3 = new QHBoxLayout;
    row3->addWidget(new QLabel("Bytes to read:"));
    row3->addWidget(lengthSpin);
    row3->addWidget(readButton);
    row3->addSpacing(12);
    row3->addWidget(new QLabel("Fill byte:"));
    row3->addWidget(fillSpin);
    row3->addWidget(fillButton);
    row3->addWidget(clearButton);

    QHBoxLayout *row4 = new QHBoxLayout;
    row4->addWidget(mappingButton);
    row4->addStretch();
    row4->addWidget(freeButton);
    row4->addWidget(freeAllButton);

    // ----- Output -----
    output = new QPlainTextEdit;
    output->setReadOnly(true);
    output->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    output->setMinimumHeight(150);

    statusLabel = new QLabel("Ready.");
    statusLabel->setWordWrap(true);
    statusLabel->setStyleSheet("color: #374151; font-size: 12px;");

    mainLayout->addWidget(heading);
    mainLayout->addWidget(description);
    mainLayout->addWidget(statsLabel);
    mainLayout->addLayout(row1);
    mainLayout->addWidget(table, 1);
    mainLayout->addLayout(row2);
    mainLayout->addLayout(row3);
    mainLayout->addLayout(row4);
    mainLayout->addWidget(output, 1);
    mainLayout->addWidget(statusLabel);

    connect(allocateButton, &QPushButton::clicked, this, &MemoryManager::allocateMemory);
    connect(resizeButton,   &QPushButton::clicked, this, &MemoryManager::resizeMemory);
    connect(failureButton,  &QPushButton::clicked, this, &MemoryManager::testAllocationFailure);
    connect(writeButton,    &QPushButton::clicked, this, &MemoryManager::writeMemory);
    connect(readButton,     &QPushButton::clicked, this, &MemoryManager::readMemory);
    connect(fillButton,     &QPushButton::clicked, this, &MemoryManager::fillMemory);
    connect(clearButton,    &QPushButton::clicked, this, &MemoryManager::clearMemory);
    connect(mappingButton,  &QPushButton::clicked, this, &MemoryManager::showMapping);
    connect(freeButton,     &QPushButton::clicked, this, &MemoryManager::freeMemory);
    connect(freeAllButton,  &QPushButton::clicked, this, &MemoryManager::freeAllMemory);

    setStyleSheet(
        "QPushButton {"
        "  background-color: #2563eb; color: white;"
        "  padding: 8px 12px; border: none; border-radius: 5px;"
        "}"
        "QPushButton:hover { background-color: #1d4ed8; }"
    );
}

void MemoryManager::report(const QString &message, bool error)
{
    statusLabel->setStyleSheet(
        error ? "color: #b91c1c; font-size: 12px; font-weight: bold;"
              : "color: #166534; font-size: 12px;");
    statusLabel->setText(message);
    appendOutput((error ? "[error] " : "[ok]    ") + message);
}

void MemoryManager::appendOutput(const QString &text)
{
    output->appendPlainText(text);
}

// Rebuild the table, keeping the same block selected.
void MemoryManager::refreshTable()
{
    int selectedId = -1;
    if (const Allocation *current = selectedAllocation())
        selectedId = current->id;

    table->setRowCount(0);

    for (const Allocation &allocation : std::as_const(allocations))
    {
        const int row = table->rowCount();
        table->insertRow(row);

        QTableWidgetItem *idItem = new QTableWidgetItem;
        idItem->setData(Qt::DisplayRole, allocation.id);

        table->setItem(row, 0, idItem);
        table->setItem(row, 1, new QTableWidgetItem(kindName(allocation.kind)));
        table->setItem(row, 2, new QTableWidgetItem(addressText(allocation.ptr)));
        table->setItem(row, 3, new QTableWidgetItem(QString::number(allocation.size)));
        table->setItem(row, 4, new QTableWidgetItem(QString::number(usableSize(allocation))));

        if (allocation.id == selectedId)
            table->selectRow(row);
    }

    refreshStats();
}

// Read VmSize / VmRSS of THIS process from /proc/self/status
void MemoryManager::refreshStats()
{
    qlonglong vmSizeKb = -1;
    qlonglong vmRssKb = -1;

    QFile file("/proc/self/status");
    if (file.open(QIODevice::ReadOnly))
    {
        const QList<QByteArray> lines = file.readAll().split('\n');
        for (const QByteArray &line : lines)
        {
            if (line.startsWith("VmSize:"))
                vmSizeKb = line.simplified().split(' ').value(1).toLongLong();
            else if (line.startsWith("VmRSS:"))
                vmRssKb = line.simplified().split(' ').value(1).toLongLong();
        }
    }

    statsLabel->setText(
        QString("This process (PID %1)  |  Virtual (VmSize): %2 MiB  |  "
                "In RAM (VmRSS): %3 MiB  |  Page size: %4 bytes  |  "
                "Tracked: %5 blocks, %6 bytes")
            .arg(getpid())
            .arg(vmSizeKb / 1024.0, 0, 'f', 1)
            .arg(vmRssKb / 1024.0, 0, 'f', 1)
            .arg(sysconf(_SC_PAGESIZE))
            .arg(allocations.size())
            .arg(static_cast<qulonglong>(totalTracked())));
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
MemoryManager::Allocation *MemoryManager::selectedAllocation()
{
    if (!table || !table->selectionModel())
        return nullptr;

    const QModelIndexList rows = table->selectionModel()->selectedRows(0);
    if (rows.isEmpty())
        return nullptr;

    const QTableWidgetItem *item = table->item(rows.first().row(), 0);
    if (!item)
        return nullptr;

    auto it = allocations.find(item->data(Qt::DisplayRole).toInt());
    return it == allocations.end() ? nullptr : &it.value();
}

size_t MemoryManager::requestedSize() const
{
    size_t multiplier = 1;
    if (unitCombo->currentIndex() == 1)
        multiplier = 1024;
    else if (unitCombo->currentIndex() == 2)
        multiplier = 1024 * 1024;

    return static_cast<size_t>(sizeSpin->value()) * multiplier;
}

size_t MemoryManager::totalTracked() const
{
    size_t total = 0;
    for (const Allocation &allocation : allocations)
        total += allocation.size;
    return total;
}

// malloc() may give MORE than requested; malloc_usable_size() tells us how much.
// mmap() always works in whole pages.
size_t MemoryManager::usableSize(const Allocation &allocation)
{
    if (allocation.kind == Kind::Mmap)
    {
        const size_t page = static_cast<size_t>(sysconf(_SC_PAGESIZE));
        return (allocation.size + page - 1) / page * page;
    }
    return malloc_usable_size(allocation.ptr);
}

QString MemoryManager::kindName(Kind kind)
{
    switch (kind)
    {
    case Kind::Malloc: return "malloc()";
    case Kind::Calloc: return "calloc()";
    case Kind::Mmap:   return "mmap()";
    }
    return "?";
}

QString MemoryManager::addressText(const void *ptr)
{
    return "0x" + QString::number(reinterpret_cast<quintptr>(ptr), 16);
}

QString MemoryManager::hexDump(const unsigned char *data, size_t length, size_t baseOffset)
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
                   .arg(static_cast<qulonglong>(baseOffset + i), 4, 16, QChar('0'))
                   .arg(hex, ascii);
    }

    return out;
}

// Give one block back: free() for heap blocks, munmap() for mapped blocks.
bool MemoryManager::releaseBlock(const Allocation &allocation, int &errorCode)
{
    errorCode = 0;

    if (allocation.kind == Kind::Mmap)
    {
        if (::munmap(allocation.ptr, allocation.size) != 0)
        {
            errorCode = errno;
            return false;
        }
        return true;
    }

    ::free(allocation.ptr);
    return true;
}

// ---------------------------------------------------------------------------
// Operations
// ---------------------------------------------------------------------------
void MemoryManager::allocateMemory()
{
    const size_t size = requestedSize();
    const Kind kind = static_cast<Kind>(methodCombo->currentData().toInt());

    if (size > kMaxSingleAllocation)
    {
        report("Refused: one block may be at most 512 MiB in this demo.", true);
        return;
    }
    if (totalTracked() + size > kMaxTotalAllocation)
    {
        report("Refused: total tracked memory would exceed 1 GiB. Free something first.", true);
        return;
    }

    void *ptr = nullptr;
    int err = 0;

    switch (kind)
    {
    case Kind::Malloc:
        ptr = ::malloc(size);
        err = errno;
        break;
    case Kind::Calloc:
        ptr = ::calloc(1, size);
        err = errno;
        break;
    case Kind::Mmap:
    {
        void *mapped = ::mmap(nullptr, size, PROT_READ | PROT_WRITE,
                              MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        err = errno;
        ptr = (mapped == MAP_FAILED) ? nullptr : mapped;
        break;
    }
    }

    if (!ptr)
    {
        report(QString("%1 of %2 bytes FAILED: %3")
                   .arg(kindName(kind)).arg(size)
                   .arg(QString::fromLocal8Bit(strerror(err))), true);
        return;
    }

    Allocation allocation;
    allocation.id = nextId++;
    allocation.kind = kind;
    allocation.ptr = ptr;
    allocation.size = size;
    allocations.insert(allocation.id, allocation);

    refreshTable();
    report(QString("Block #%1: %2 gave %3 bytes at %4. Usable: %5 bytes.")
               .arg(allocation.id).arg(kindName(kind)).arg(size)
               .arg(addressText(ptr)).arg(usableSize(allocation)));

    if (kind == Kind::Calloc || kind == Kind::Mmap)
        appendOutput("        (This memory is guaranteed to start as all zeros.)");
    else
        appendOutput("        (malloc() memory is NOT initialized: it may hold old data.)");
}

// Ask for an impossible amount (1 PiB) and prove the error is handled.
void MemoryManager::testAllocationFailure()
{
    const size_t huge = static_cast<size_t>(1) << 50;   // 1 PiB > 47-bit address space

    // volatile stops the compiler from optimizing the call away
    void *volatile viaMalloc = ::malloc(huge);
    const int mallocError = errno;

    void *volatile viaMmap = ::mmap(nullptr, huge, PROT_READ | PROT_WRITE,
                                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    const int mmapError = errno;

    appendOutput("--- Allocation failure test: requesting 1 PiB ---");

    if (viaMalloc == nullptr)
        appendOutput(QString("  malloc(1 PiB) returned NULL: %1")
                         .arg(QString::fromLocal8Bit(strerror(mallocError))));
    else
    {
        appendOutput("  malloc(1 PiB) unexpectedly succeeded; releasing it.");
        ::free(viaMalloc);
    }

    if (viaMmap == MAP_FAILED)
        appendOutput(QString("  mmap(1 PiB) returned MAP_FAILED: %1")
                         .arg(QString::fromLocal8Bit(strerror(mmapError))));
    else
    {
        appendOutput("  mmap(1 PiB) unexpectedly succeeded; releasing it.");
        ::munmap(viaMmap, huge);
    }

    report("Allocation failure handled safely: the application kept running.");
}

void MemoryManager::resizeMemory()
{
    Allocation *allocation = selectedAllocation();
    if (!allocation)
    {
        report("Select a block first.", true);
        return;
    }

    const size_t newSize = requestedSize();

    if (newSize > kMaxSingleAllocation ||
        totalTracked() - allocation->size + newSize > kMaxTotalAllocation)
    {
        report("Refused: the new size is over the demo limits (512 MiB per block, 1 GiB total).", true);
        return;
    }

    void *oldPtr = allocation->ptr;
    const size_t oldSize = allocation->size;
    void *newPtr = nullptr;
    int err = 0;

    if (allocation->kind == Kind::Mmap)
    {
        // mremap(): resize a mapping; the kernel may move it.
        void *mapped = ::mremap(oldPtr, oldSize, newSize, MREMAP_MAYMOVE);
        err = errno;
        newPtr = (mapped == MAP_FAILED) ? nullptr : mapped;
    }
    else
    {
        // Use a temporary: if realloc() fails, the old block is still valid.
        newPtr = ::realloc(oldPtr, newSize);
        err = errno;
    }

    if (!newPtr)
    {
        report(QString("Resize of block #%1 FAILED: %2 (the old block is unchanged).")
                   .arg(allocation->id).arg(QString::fromLocal8Bit(strerror(err))), true);
        return;
    }

    const int id = allocation->id;
    allocation->ptr = newPtr;
    allocation->size = newSize;

    refreshTable();
    report(QString("Block #%1 resized %2 -> %3 bytes. Address %4 (%5).")
               .arg(id).arg(oldSize).arg(newSize).arg(addressText(newPtr),
                    newPtr == oldPtr ? "same address" : "moved from " + addressText(oldPtr)));
}

void MemoryManager::writeMemory()
{
    Allocation *allocation = selectedAllocation();
    if (!allocation)
    {
        report("Select a block first.", true);
        return;
    }

    const QByteArray bytes = dataEdit->text().toUtf8();
    const size_t offset = static_cast<size_t>(offsetSpin->value());

    if (bytes.isEmpty())
    {
        report("Type some text to write.", true);
        return;
    }

    // Bounds check: writing past the end is a buffer overflow.
    if (offset > allocation->size || static_cast<size_t>(bytes.size()) > allocation->size - offset)
    {
        report(QString("Refused: writing %1 bytes at offset %2 would go past the end of "
                       "block #%3 (%4 bytes). That would be a buffer overflow.")
                   .arg(bytes.size()).arg(offset).arg(allocation->id).arg(allocation->size), true);
        return;
    }

    std::memcpy(static_cast<unsigned char *>(allocation->ptr) + offset, bytes.constData(),
                static_cast<size_t>(bytes.size()));

    report(QString("memcpy(): wrote %1 bytes at offset %2 of block #%3.")
               .arg(bytes.size()).arg(offset).arg(allocation->id));

    const size_t dumpLength = std::min<size_t>(static_cast<size_t>(bytes.size()), 256);
    appendOutput(hexDump(static_cast<const unsigned char *>(allocation->ptr) + offset,
                         dumpLength, offset));
}

void MemoryManager::readMemory()
{
    Allocation *allocation = selectedAllocation();
    if (!allocation)
    {
        report("Select a block first.", true);
        return;
    }

    const size_t offset = static_cast<size_t>(offsetSpin->value());

    if (offset >= allocation->size)
    {
        report(QString("Refused: offset %1 is outside block #%2 (%3 bytes).")
                   .arg(offset).arg(allocation->id).arg(allocation->size), true);
        return;
    }

    const size_t length = std::min<size_t>(static_cast<size_t>(lengthSpin->value()),
                                           allocation->size - offset);

    appendOutput(QString("--- Block #%1 (%2) at %3, offset %4, %5 bytes ---")
                     .arg(allocation->id).arg(kindName(allocation->kind))
                     .arg(addressText(allocation->ptr)).arg(offset).arg(length));
    appendOutput(hexDump(static_cast<const unsigned char *>(allocation->ptr) + offset,
                         length, offset));

    report(QString("Read %1 bytes from block #%2.").arg(length).arg(allocation->id));
}

// Touching every page forces the kernel to give real RAM: watch VmRSS grow.
void MemoryManager::fillMemory()
{
    Allocation *allocation = selectedAllocation();
    if (!allocation)
    {
        report("Select a block first.", true);
        return;
    }

    const int value = fillSpin->value();
    std::memset(allocation->ptr, value, allocation->size);

    refreshStats();
    report(QString("memset(): filled all %1 bytes of block #%2 with 0x%3. "
                   "Look at VmRSS above: the pages are now really in RAM.")
               .arg(allocation->size).arg(allocation->id)
               .arg(value, 2, 16, QChar('0')));
}

void MemoryManager::clearMemory()
{
    Allocation *allocation = selectedAllocation();
    if (!allocation)
    {
        report("Select a block first.", true);
        return;
    }

    std::memset(allocation->ptr, 0, allocation->size);

    refreshStats();
    report(QString("memset(): cleared block #%1 (%2 bytes set to zero).")
               .arg(allocation->id).arg(allocation->size));
}

void MemoryManager::freeMemory()
{
    Allocation *allocation = selectedAllocation();
    if (!allocation)
    {
        report("Select a block first.", true);
        return;
    }

    const Allocation copy = *allocation;
    int err = 0;

    if (!releaseBlock(copy, err))
    {
        report(QString("munmap() failed for block #%1: %2")
                   .arg(copy.id).arg(QString::fromLocal8Bit(strerror(err))), true);
        return;
    }

    allocations.remove(copy.id);
    refreshTable();
    report(QString("Block #%1 released with %2.")
               .arg(copy.id).arg(copy.kind == Kind::Mmap ? "munmap()" : "free()"));
}

void MemoryManager::freeAllMemory()
{
    int released = 0;

    for (const Allocation &allocation : std::as_const(allocations))
    {
        int err = 0;
        if (releaseBlock(allocation, err))
            ++released;
    }

    allocations.clear();
    refreshTable();
    report(QString("Released %1 block(s).").arg(released));
}

// Prove where the block really lives: find its address in /proc/self/maps.
void MemoryManager::showMapping()
{
    Allocation *allocation = selectedAllocation();
    if (!allocation)
    {
        report("Select a block first.", true);
        return;
    }

    const quintptr address = reinterpret_cast<quintptr>(allocation->ptr);

    QFile file("/proc/self/maps");
    if (!file.open(QIODevice::ReadOnly))
    {
        report("Cannot read /proc/self/maps.", true);
        return;
    }

    const QList<QByteArray> lines = file.readAll().split('\n');

    for (const QByteArray &line : lines)
    {
        const QList<QByteArray> parts = line.split(' ');
        if (parts.isEmpty())
            continue;

        const QList<QByteArray> range = parts.first().split('-');
        if (range.size() != 2)
            continue;

        bool okStart = false;
        bool okEnd = false;
        const quintptr start = range[0].toULongLong(&okStart, 16);
        const quintptr end = range[1].toULongLong(&okEnd, 16);

        if (okStart && okEnd && address >= start && address < end)
        {
            appendOutput(QString("--- Block #%1 at %2 is inside this mapping ---")
                             .arg(allocation->id).arg(addressText(allocation->ptr)));
            appendOutput("  " + QString::fromLocal8Bit(line));
            appendOutput("  (rw-p = read/write, private. [heap] = classic heap. "
                         "An unnamed region = an mmap()ed area.)");
            report(QString("Found block #%1 in /proc/self/maps.").arg(allocation->id));
            return;
        }
    }

    report("Address not found in /proc/self/maps.", true);
}