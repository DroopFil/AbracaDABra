#ifndef AUDIOOUTPUT_H
#define AUDIOOUTPUT_H

#include <QIODevice>
#include <QObject>
#include <QWaitCondition>
#include <QMutex>
#include <QTimer>

#include "audiofifo.h"

#define AUDIOOUTPUT_USE_PORTAUDIO

#ifdef AUDIOOUTPUT_USE_PORTAUDIO
#include "portaudio.h"
#else
#if QT_VERSION < 0x060000
#include <QAudioDeviceInfo>
#else
#include <QAudioSink>
#include <QMediaDevices>
#endif
#endif

#ifdef QT_DEBUG
//#define AUDIOOUTPUT_DBG_TIMER 1
#define AUDIOOUTPUT_DBG_AVRG_SIZE 32
#endif
//#define AUDIOOUTPUT_RAW_FILE_OUT

#define AUDIOOUTPUT_FADE_TIME_MS 60

#ifdef AUDIOOUTPUT_USE_PORTAUDIO // port audio allows to set number of sampels in callback
#if (AUDIOOUTPUT_FADE_TIME_MS > AUDIO_FIFO_CHUNK_MS)
#error "(AUDIOOUTPUT_FADE_TIME_MS > AUDIO_FIFO_CHUNK_MS)"
#endif
#endif

enum class AudioOutputPlaybackState
{
    StatePlaying = 0,
    StateMuted = 1,
    StateDoMute = 2,
    StateDoUnmute = 3,
};


class AudioIODevice;

class AudioOutput : public QObject
{
    Q_OBJECT

public:
    AudioOutput(audioFifo_t *buffer);
    ~AudioOutput();
    void stop();

public slots:
    void start(uint32_t sRate, uint8_t numChannels);
    void mute(bool on);

signals:
    void stateChanged(AudioOutputPlaybackState state);

private:
#ifdef AUDIOOUTPUT_RAW_FILE_OUT
    FILE * rawOut;
#endif
    std::atomic<bool> m_muteFlag  = false;
    std::atomic<bool> m_stopFlag  = false;
    std::vector<float> m_muteRamp;

#ifdef AUDIOOUTPUT_USE_PORTAUDIO
    PaStream * m_outStream = nullptr;
    audioFifo_t * m_inFifoPtr = nullptr;
    uint8_t m_numChannels;
    uint32_t m_sampleRate_kHz;
    unsigned int m_bufferFrames;
    uint8_t m_bytesPerFrame;

    AudioOutputPlaybackState m_playbackState;

    int portAudioCbPrivate(void *outputBuffer, unsigned long nBufferFrames);

    friend int portAudioCb(const void *inputBuffer, void *outputBuffer, unsigned long nBufferFrames,
                     const PaStreamCallbackTimeInfo* timeInfo, PaStreamCallbackFlags statusFlags, void *ctx);

#else

    // Qt audio
    AudioIODevice * ioDevice;
#if QT_VERSION < 0x060000 // Qt5
    uint8_t m_numChannels;
    uint32_t m_sampleRate_kHz;
    QTimer * audioStartTimer;
    QAudioOutput * audioOutput;

    void checkInputBuffer();
    void initTimer();
    void destroyTimer();
#else  // Qt6
    QMediaDevices * devices;
    QAudioSink * audioOutput;
    QTimer * muteTimer;
#endif
    void handleStateChanged(QAudio::State newState);
    void muteStep();
#endif

#if AUDIOOUTPUT_DBG_TIMER
    int64_t m_minCount = INT64_MAX;
    int64_t m_maxCount = INT64_MIN;
    int64_t m_dbgBuf[AUDIOOUTPUT_DBG_AVRG_SIZE];
    int8_t m_cntr = 0;
    int64_t m_sum = 0;
    QTimer * m_dbgTimer;
    void bufferMonitor();
#endif

    int64_t bytesAvailable() const;
    void doStop();
    void onStateChange(AudioOutputPlaybackState state);
};

#if (!defined AUDIOOUTPUT_USE_PORTAUDIO)
class AudioIODevice : public QIODevice
{
public:
    AudioIODevice(audioFifo_t *buffer, QObject *parent = nullptr);

    void start();
    void stop();

    int64_t readData(char *data, int64_t maxlen) override;
    int64_t writeData(const char *data, int64_t len) override;
    int64_t bytesAvailable() const override;

#if QT_VERSION >= 0x060000 // Qt6
    void setFormat(const QAudioFormat & format);
#endif
private:
    audioFifo_t * m_inFifoPtr = nullptr;
#if QT_VERSION >= 0x060000 // Qt6
    enum
    {
        StatePlaying = 0,
        StateMuted = 1,
        StateDoMute = 2,
        StateDoUnmute = 3,
    } m_playbackState;
    uint8_t m_bytesPerFrame;
    uint32_t m_sampleRate_kHz;
    uint8_t m_numChannels;
#endif
};
#endif

#endif // AUDIOOUTPUT_H
