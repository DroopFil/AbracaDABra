#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QThread>
#include <QStandardItemModel>
#include <QTimer>
#include <QCloseEvent>
#include <QLabel>
#include <QProgressBar>
#include <QGraphicsView>
#include <QGraphicsPixmapItem>

#include "setupdialog.h"
#include "inputdevice.h"
#include "rawfileinput.h"
#include "rtlsdrinput.h"
#include "radiocontrol.h"
#include "dldecoder.h"
#include "motdecoder.h"
#include "audiodecoder.h"
#include "audiooutput.h"
#include "servicelist.h"
#include "slmodel.h"

QT_BEGIN_NAMESPACE
namespace Ui { class MainWindow; }
QT_END_NAMESPACE

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

public slots:
    void inputDeviceReady();
    void updateEnsembleInfo(const RadioControlEnsemble &ens);
    void updateSyncStatus(uint8_t sync);
    void updateSnrLevel(float snr);
    void updateServiceList(const RadioControlServiceListEntry &slEntry);
    void updateDL(const QString &dl);
    void updateSLS(const QByteArray & b);
    void updateAudioInfo(const AudioParameters &params);
    void updateDabTime(const QDateTime & d);
    void changeInputDevice(const InputDeviceId &d);
    void tuneFinished(uint32_t freq);
    void enableFileLooping(bool ena) { fileLooping = ena; }

signals:
    void serviceRequest(uint32_t freq, uint32_t SId, uint8_t SCIdS);

private slots:
    void on_channelCombo_currentIndexChanged(int index);
    void clearEnsembleInformationLabels();
    void clearServiceInformationLabels();
    void initInputDevice(const InputDeviceId &d);
    void onEndOfFile();
    void onRawFileStop();
    void serviceListCurrentChanged(const QModelIndex &current, const QModelIndex &previous);
    void audioServiceChanged(const RadioControlAudioService &s);

protected:        
     void closeEvent(QCloseEvent *event);
     void showEvent(QShowEvent *event);
     void resizeEvent(QResizeEvent *event);
private:
    Ui::MainWindow *ui;
    SetupDialog * setupDialog;
    QProgressBar * snrProgress;
    QLabel  * timeLabel;
    QLabel * syncLabel;
    QLabel * snrLabel;
    QGraphicsPixmapItem * slsPixmapItem;
    QMenu * menu;
    QAction * setupAct;
    QAction * clearServiceListAct;
//    QAction * scanAct;

    QThread * radioControlThr;
    RadioControl * radioControl;

    DLDecoder * dlDecoder;
    MOTDecoder * motDecoder;

    InputDeviceId inputDeviceId = InputDeviceId::UNDEFINED;
    InputDevice * inputDevice = nullptr;

    QThread * audioDecoderThr;
    AudioDecoder * audioDecoder;

#if (!defined AUDIOOUTPUT_USE_RTAUDIO) &&  (!defined AUDIOOUTPUT_USE_PORTAUDIO)
    QThread * audioOutThr;
#endif
    AudioOutput * audioOutput;

    bool isPlaying = false;
    bool fileLooping = false;
    bool deviceChangeRequested = false;
    uint32_t frequency = 0;
    DabSId SId;
    uint32_t SCIdS = 0;

    ServiceList * serviceList;
    QStandardItemModel * serviceListModel;
    SLModel * slModel;

    void onServiceSelection();
    void onChannelSelection();
    void clearServiceList();
    void loadSettings();
    void saveSettings();

    void printServiceList();
};

// this implementation allow scaling od SLS with the window
class SLSView : public QGraphicsView
{
public:
    SLSView(QWidget *parent = nullptr) : QGraphicsView(parent) {}
    void fitInViewTight(const QRectF &rect, Qt::AspectRatioMode aspectRatioMode)
    {   // https://bugreports.qt.io/browse/QTBUG-11945
        if (nullptr == scene() || rect.isNull())
            return;
        // Reset the view scale to 1:1.
        //QRectF unity = matrix().mapRect(QRectF(0, 0, 1, 1));
        QRectF unity = transform().mapRect(QRectF(0, 0, 1, 1));
        if (unity.isEmpty())
            return;
        scale(1 / unity.width(), 1 / unity.height());
        // Find the ideal x / y scaling ratio to fit \a rect in the view.
        QRectF viewRect = viewport()->rect();
        if (viewRect.isEmpty())
            return;
        //QRectF sceneRect = matrix().mapRect(rect);
        QRectF sceneRect = transform().mapRect(rect);
        if (sceneRect.isEmpty())
            return;
        qreal xratio = viewRect.width() / sceneRect.width();
        qreal yratio = viewRect.height() / sceneRect.height();
        // Respect the aspect ratio mode.
        switch (aspectRatioMode) {
        case Qt::KeepAspectRatio:
            xratio = yratio = qMin(xratio, yratio);
            break;
        case Qt::KeepAspectRatioByExpanding:
            xratio = yratio = qMax(xratio, yratio);
            break;
        case Qt::IgnoreAspectRatio:
            break;
        }
        // Scale and center on the center of \a rect.
        scale(xratio, yratio);
        centerOn(rect.center());
    }

protected:
//    void resizeEvent(QResizeEvent *event)
//    {
//        if (nullptr != scene())
//        {
//            QList<QGraphicsItem *> gitems = scene()->items();
//            if (gitems.size() >= 1)
//            {   // exactly one item should exist - this is for safety
//                //qDebug() << rect() << scene()->itemsBoundingRect();
//                fitInViewTight(gitems[0], Qt::KeepAspectRatio);
//            }
//        }
//        QGraphicsView::resizeEvent(event);
//    }
};

#endif // MAINWINDOW_H
