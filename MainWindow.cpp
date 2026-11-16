#include "MainWindow.h"

#include "dashboard/Dashboard.h"
#include "files/FileManager.h"
#include "io/IOManager.h"
#include "ipc/IPCManager.h"
#include "memory/MemoryManager.h"
#include "process/ProcessManager.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QStackedWidget>
#include <QStringList>
#include <QVBoxLayout>
#include <QWidget>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setWindowTitle("OS Control Center");
    resize(1100, 740);
    setMinimumSize(850, 550);

    setupUI();
}

void MainWindow::setupUI()
{
    QWidget *central = new QWidget(this);
    setCentralWidget(central);

    QHBoxLayout *mainLayout = new QHBoxLayout(central);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);

    // ---------------- SIDEBAR ----------------
    QWidget *sidebar = new QWidget;
    sidebar->setFixedWidth(230);
    sidebar->setStyleSheet("background-color: #111827;");

    QVBoxLayout *sideLayout = new QVBoxLayout(sidebar);
    sideLayout->setContentsMargins(15, 25, 15, 20);
    sideLayout->setSpacing(12);

    QLabel *logo = new QLabel("OS CONTROL");
    logo->setStyleSheet("color: white; font-size: 22px; font-weight: bold;");

    QLabel *subtitle = new QLabel("SYSTEM MANAGEMENT");
    subtitle->setStyleSheet("color: #9ca3af; font-size: 10px;");

    sideLayout->addWidget(logo);
    sideLayout->addWidget(subtitle);
    sideLayout->addSpacing(30);

    pages = new QStackedWidget;

    // The order of this list is the order of the pages added below.
    const QStringList navigation = {
        "Dashboard",
        "Process Management",
        "Memory Management",
        "I/O Management",
        "File Subsystem",
        "IPC Management"
    };

    for (int i = 0; i < navigation.size(); i++)
    {
        QPushButton *button = new QPushButton(navigation[i]);

        button->setCursor(Qt::PointingHandCursor);
        button->setMinimumHeight(43);
        button->setCheckable(true);
        button->setAutoExclusive(true);      // exactly one button is highlighted

        button->setStyleSheet(
            "QPushButton {"
            "color: #d1d5db;"
            "background: transparent;"
            "border: none;"
            "border-radius: 7px;"
            "text-align: left;"
            "padding-left: 12px;"
            "font-size: 13px;"
            "}"
            "QPushButton:hover {"
            "background-color: #374151;"
            "color: white;"
            "}"
            "QPushButton:checked {"
            "background-color: #2563eb;"
            "color: white;"
            "font-weight: bold;"
            "}"
        );

        sideLayout->addWidget(button);
        navButtons.append(button);

        connect(button, &QPushButton::clicked, this, [this, i]() {
            showPage(i);
        });
    }

    sideLayout->addStretch();

    QLabel *version = new QLabel("Version 1.0 | C++ / Qt 6");
    version->setStyleSheet("color: #6b7280; font-size: 10px;");
    sideLayout->addWidget(version);

    // ---------------- PAGES ----------------
    Dashboard *dashboard = new Dashboard;
    connect(dashboard, &Dashboard::moduleRequested, this, [this](int index) {
        showPage(index);                      // a click on a dashboard card
    });

    pages->addWidget(dashboard);               // 0 Dashboard
    pages->addWidget(new ProcessManager);      // 1 Process Management
    pages->addWidget(new MemoryManager);       // 2 Memory Management
    pages->addWidget(new IOManager);           // 3 I/O Management
    pages->addWidget(new FileManager);         // 4 File Subsystem
    pages->addWidget(new IPCManager);          // 5 IPC Management

    mainLayout->addWidget(sidebar);
    mainLayout->addWidget(pages, 1);

    // ---------------- APPLICATION STYLE ----------------
    setStyleSheet(
        "QMainWindow { background-color: #f3f4f6; }"
        "QLabel { color: #111827; }"
    );

    showPage(0);
}

void MainWindow::showPage(int index)
{
    if (index < 0 || index >= pages->count())
        return;

    pages->setCurrentIndex(index);

    if (index < navButtons.size())
        navButtons[index]->setChecked(true);  // keep the sidebar in step with the page
}