/*
 * This file is part of the AbracaDABra project
 *
 * MIT License
 *
 * Copyright (c) 2019-2024 Petr Kopecký <xkejpi (at) gmail (dot) com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#ifndef SETTINGS_H
#define SETTINGS_H

#include <QLocale>
#include <QGeoCoordinate>
#include <QAbstractItemModel>
#include <QDateTime>
#include <QColor>
#include "config.h"
#include "inputdevice.h"
#if HAVE_AIRSPY
#include "airspyinput.h"
#endif
#include "rawfileinput.h"

class Settings
{
public:
    enum class ApplicationStyle { Default = 0, Light, Dark};
    enum class GeolocationSource { System = 0, Manual, SerialPort };
    enum class ProxyConfig { NoProxy = 0, System, Manual };

    Settings() {};

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
        uint32_t bandwidth;
        bool biasT;
        int agcLevelMax;
        int ppm;
    } rtlsdr;
    struct
    {
        RtlGainMode gainMode;
        int gainIdx;
        QString tcpAddress;
        int tcpPort;
        int agcLevelMax;
        int ppm;
    } rtltcp;
#if HAVE_AIRSPY
    struct
    {
        AirspyGainStr gain;
        bool biasT;
        bool dataPacking;
        bool prefer4096kHz;
    } airspy;
#endif
#if HAVE_SOAPYSDR
    struct
    {
        SoapyGainMode gainMode;
        int gainIdx;
        QString devArgs;
        QString antenna;
        int channel;
        uint32_t bandwidth;
    } soapysdr;
#endif
#if HAVE_RARTTCP
    struct
    {
        QString tcpAddress;
        int tcpPort;
    } rarttcp;
#endif
    uint16_t announcementEna;
    bool bringWindowToForeground;
    ApplicationStyle applicationStyle;
    QLocale::Language lang;
    bool expertModeEna;
    bool dlPlusEna;
    int noiseConcealmentLevel;
    bool xmlHeaderEna;
    bool spiAppEna;
    bool useInternet;
    bool radioDnsEna;
    bool trayIconEna;
    QColor slsBackground = Qt::red;
    bool updateCheckEna;
    QDateTime updateCheckTime;

    // audio recording settings
    struct AudioRec{
        QString folder;
        bool captureOutput;
        bool autoStopEna;
        bool dl;
        bool dlAbsTime;
    } audioRec;

    // this is settings for UA data dumping (storage)
    struct UADumpSettings
    {
        QString folder;
        bool overwriteEna;
        bool slsEna;
        bool spiEna;
        QString slsPattern;
        QString spiPattern;
    } uaDump;

    struct TIISettings {
        GeolocationSource locationSource;
        QGeoCoordinate coordinates;
        QString serialPort;
        QString logFolder;
        bool showSpectumPlot;
        QByteArray geometry;
        QByteArray splitterState;
    } tii;
    struct SNRSettings {
        QByteArray geometry;
    } snr;
    struct EPGSettings {
        bool filterEmptyEpg;
        bool filterEnsemble;
        QPersistentModelIndex selectedItem;
        QByteArray geometry;
    } epg;
    struct Proxy {
        ProxyConfig config;
        QString server;
        uint32_t port;
        QString user;
        QByteArray pass;
    } proxy;
    struct EnsembleInfo {
        QByteArray geometry;
    } ensembleInfo;
    struct Log {
        QByteArray geometry;
    } log;
    struct CatSLS {
        QByteArray geometry;
    } catSls;
    struct ScannerSettings {
        QString exportPath;
        QByteArray geometry;
        QByteArray splitterState;
        QMap<uint32_t, bool> channelSelection;
        int mode;
        int numCycles;
        int waitForSync;
        int waitForEnsemble;        
    } scanner;

};

#endif // SETTINGS_H
