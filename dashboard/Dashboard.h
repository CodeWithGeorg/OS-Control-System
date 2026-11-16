#ifndef DASHBOARD_H
#define DASHBOARD_H

#include <QWidget>
#include <QHash>
#include <QList>
#include <QString>

class QEvent;
class QLabel;
class QProgressBar;
class QShowEvent;
class QTimer;

// Live system dashboard: system information, a memory gauge, one clickable
// card per module (with live numbers) and a readiness checklist.
class Dashboard : public QWidget
{
    Q_OBJECT

public:
    explicit Dashboard(QWidget *parent = nullptr);

signals:
    // pageIndex is the page number in the main window's stacked widget
    void moduleRequested(int pageIndex);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void showEvent(QShowEvent *event) override;

private:
    static const int kModuleCount = 5;
    static const int kCheckCount = 7;

    QLabel *hostValue;
    QLabel *osValue;
    QLabel *kernelValue;
    QLabel *uptimeValue;
    QLabel *loadValue;
    QLabel *userValue;
    QProgressBar *memoryBar;
    QLabel *memoryText;
    QLabel *moduleLive[kModuleCount];
    QLabel *checkLabel[kCheckCount];
    QLabel *banner;

    QHash<QObject *, int> cardIndex;     // card widget -> page index
    QTimer *timer;

    void setupUI();
    void refresh();

    static QString formatDuration(qint64 seconds);
    static QString formatKiB(qint64 kib);
};

#endif