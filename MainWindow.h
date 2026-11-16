#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QList>
#include <QMainWindow>

class QPushButton;
class QStackedWidget;

class MainWindow : public QMainWindow
{
public:
    explicit MainWindow(QWidget *parent = nullptr);

private:
    QStackedWidget *pages;
    QList<QPushButton *> navButtons;     // one per page, same order as the pages

    void setupUI();
    void showPage(int index);
};

#endif