#ifndef SETUPDIALOG_H
#define SETUPDIALOG_H

#include <QDialog>
#include <QWidget>
#include <QList>
#include <QAbstractButton>
#include "config.h"
#include "inputdevice.h"
#include "airspyinput.h"
#include "rawfileinput.h"

QT_BEGIN_NAMESPACE
namespace Ui { class SetupDialog; }
QT_END_NAMESPACE

class SetupDialog : public QDialog
{
    Q_OBJECT
public:
    // this is to store active state
    struct Settings
    {
        InputDeviceId inputDevice;
        struct
        {
            QString file;
            RawFileInputFormat format;
            bool loopEna;
        } rawfile;
        struct
        {
            RtlGainMode gainMode;
            int gainIdx;            
            int bandwidth;
            bool biasT;
        } rtlsdr;
        struct
        {
            RtlGainMode gainMode;
            int gainIdx;
            QString tcpAddress;
            int tcpPort;
        } rtltcp;
#ifdef HAVE_RARTTCP
        struct
        {
            QString tcpAddress;
            int tcpPort;
        } rarttcp;
#endif
#ifdef HAVE_AIRSPY
        struct
        {
            AirspyGainStr gain;
            bool biasT;
            bool dataPacking;
            bool prefer4096kHz;
        } airspy;
#endif
#ifdef HAVE_SOAPYSDR
        struct
        {
            SoapyGainMode gainMode;
            int gainIdx;
            QString devArgs;
            int channel;
            //bool prefer4096kHz;
        } soapysdr;
#endif
    };

    SetupDialog(QWidget *parent = nullptr);
    Settings settings() const;
    void setGainValues(const QList<float> & gainList);
    void resetInputDevice();
    void onExpertMode(bool ena);
    void setSettings(const Settings &settings);

signals:
    void inputDeviceChanged(const InputDeviceId & inputDevice);
    void newSettings();

protected:
    void showEvent(QShowEvent *event);

private:
    Ui::SetupDialog *ui;
    Settings m_settings;
    QString rawfilename;
    QList<float> rtlsdrGainList;
    QList<float> rtltcpGainList;
    QList<float> soapysdrGainList;

    void onButtonClicked(QAbstractButton *button);
    void onInputChanged(int index);
    void onOpenFileButtonClicked();
    void setStatusLabel();

    void onConnectDeviceClicked();

    void onRtlGainModeToggled(bool checked);
    void onRtlSdrGainSliderChanged(int val);
    void activateRtlSdrControls(bool en);

    void onTcpGainModeToggled(bool checked);
    void onRtlTcpGainSliderChanged(int val);
    void onRtlTcpIpAddrEditFinished();
    void onRtlTcpPortValueChanged(int val);
    void activateRtlTcpControls(bool en);

    void onRawFileFormatChanged(int idx);

#ifdef HAVE_AIRSPY
    void onAirspyModeToggled(bool checked);
    void onAirspySensitivityGainSliderChanged(int val);
    void onAirspyIFGainSliderChanged(int val);
    void onAirspyLNAGainSliderChanged(int val);
    void onAirspyMixerGainSliderChanged(int val);
    void onAirspyLNAAGCstateChanged(int state);
    void onAirspyMixerAGCstateChanged(int state);
    void activateAirspyControls(bool en);
#endif
#ifdef HAVE_SOAPYSDR
    void onSoapySdrGainSliderChanged(int val);
    void activateSoapySdrControls(bool en);
    void onSoapySdrDevArgsEditFinished();
    void onSoapySdrChannelEditFinished();
    void onSoapySdrGainModeToggled(bool checked);
#endif
};

#endif // SETUPDIALOG_H
