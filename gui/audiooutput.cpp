/*
 * This file is part of the AbracaDABra project
 *
 * MIT License
 *
  * Copyright (c) 2019-2023 Petr Kopecký <xkejpi (at) gmail (dot) com>
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

#include <QDebug>
#include <QLoggingCategory>
#include <QThread>
#include <QAudioSink>
#include <QAudioDevice>

#include "audiooutput.h"

Q_LOGGING_CATEGORY(audioOutput, "AudioOutput", QtInfoMsg)

#if HAVE_PORTAUDIO

AudioOutput::AudioOutput( QObject *parent) : QObject(parent)
{
    m_inFifoPtr = nullptr;
    m_outStream = nullptr;
    m_numChannels = m_sampleRate_kHz = 0;
    m_linearVolume = 1.0;

    PaError err = Pa_Initialize();
    if (paNoError != err)
    {
        throw std::runtime_error(std::string(Q_FUNC_INFO) + "PortAudio error:" + Pa_GetErrorText( err ) );
    }

#ifdef AUDIOOUTPUT_RAW_FILE_OUT
    m_rawOut = fopen("audio.raw", "wb");
    if (!m_rawOut)
    {
        qCWarning(audioOutput, "Unable to open file: audio.raw");
    }
#endif

    connect(this, &AudioOutput::streamFinished, this, &AudioOutput::onStreamFinished, Qt::QueuedConnection);
}

AudioOutput::~AudioOutput()
{
    if (nullptr != m_outStream)
    {
        if (Pa_IsStreamActive(m_outStream))
        {
            //Pa_AbortStream(m_outStream);
            Pa_StopStream(m_outStream);
        }
        Pa_CloseStream(m_outStream);
    }

    PaError err = Pa_Terminate();
    if (paNoError != err)
    {
        qCCritical(audioOutput, "PortAudio Pa_Terminate() error: %s", Pa_GetErrorText(err));
    }

#ifdef AUDIOOUTPUT_RAW_FILE_OUT
    if (m_rawOut)
    {
        fclose(m_rawOut);
    }
#endif
}

void AudioOutput::start(audioFifo_t *buffer)
{
    uint32_t sRate = buffer->sampleRate;
    uint8_t numCh = buffer->numChannels;

    bool isNewStreamParams = (m_sampleRate_kHz != sRate/1000) || (m_numChannels != numCh);
    if (isNewStreamParams)
    {
        if (nullptr != m_outStream)
        {
            if (!Pa_IsStreamStopped(m_outStream))
            {
                Pa_StopStream(m_outStream);
            }

            Pa_CloseStream(m_outStream);
            m_outStream = nullptr;
        }

        m_sampleRate_kHz = sRate/1000;
        m_numChannels = numCh;

        m_bytesPerFrame = numCh * sizeof(int16_t);
        m_bufferFrames = AUDIOOUTPUT_FADE_TIME_MS * m_sampleRate_kHz;  // 120 ms (FIFO size should be integer multiple of this)

        // mute ramp is exponential
        // value are precalculated to save MIPS in runtime
        // unmute ramp is then calculated as 2.0 - m_muteFactor in runtime
        // m_muteFactor is calculated for change from 0dB to AUDIOOUTPUT_FADE_MIN_DB in AUDIOOUTPUT_FADE_TIME_MS
        m_muteFactor = powf(10, AUDIOOUTPUT_FADE_MIN_DB/(20.0*AUDIOOUTPUT_FADE_TIME_MS*m_sampleRate_kHz));

        /* Open an audio I/O stream. */
        PaError err = Pa_OpenDefaultStream( &m_outStream,
                                           0,              /* no input channels */
                                           numCh,          /* stereo output */
                                           paInt16,        /* 16 bit floating point output */
                                           sRate,
                                           m_bufferFrames, /* frames per buffer, i.e. the number
                                                           of sample frames that PortAudio will
                                                           request from the callback. Many apps
                                                           may want to use
                                                           paFramesPerBufferUnspecified, which
                                                           tells PortAudio to pick the best,
                                                           possibly changing, buffer size.*/
                                           portAudioCb,    /* callback */
                                           (void *) this);
        if (paNoError != err)
        {
            throw std::runtime_error(std::string(Q_FUNC_INFO) + "PortAudio error:" + Pa_GetErrorText( err ) );
        }

        err = Pa_SetStreamFinishedCallback(m_outStream, portAudioStreamFinishedCb);
        if (paNoError != err)
        {
            throw std::runtime_error(std::string(Q_FUNC_INFO) + "PortAudio error:" + Pa_GetErrorText( err ) );
        }
    }
    else
    {   // stream parameters are the same - just starting
        // Pa_StopStream() is required event that streaming was stopped by paComplete return value from callback
        if (!Pa_IsStreamStopped(m_outStream))
        {
            Pa_StopStream(m_outStream);
        }
    }

    m_inFifoPtr = buffer;
    m_playbackState = AudioOutputPlaybackState::Muted;
    m_cbRequest &= ~(Request::Stop | Request::Restart);  // reset stop and restart bits

    PaError err = Pa_StartStream(m_outStream);
    if (paNoError != err)
    {
        throw std::runtime_error(std::string(Q_FUNC_INFO) + "PortAudio error:" + Pa_GetErrorText( err ) );
    }

}

void AudioOutput::restart(audioFifo_t *buffer)
{
    if (nullptr != m_outStream)
    {
        m_restartFifoPtr = buffer;
        m_cbRequest |= Request::Restart;  // set restart bit
    }
}

void AudioOutput::stop()
{
    if (nullptr != m_outStream)
    {
        m_cbRequest |= Request::Stop;     // set stop bit
    }
}

void AudioOutput::mute(bool on)
{
    if (on)
    {   // set mute bit
        m_cbRequest |= Request::Mute;
    }
    else
    {   // reset mute bit
        m_cbRequest &= ~Request::Mute;
    }
}

void AudioOutput::setVolume(int value)
{
    m_linearVolume = QAudio::convertVolume(value / qreal(100),
                                           QAudio::LogarithmicVolumeScale,
                                           QAudio::LinearVolumeScale);
}

int AudioOutput::portAudioCb( const void *inputBuffer, void *outputBuffer, unsigned long nBufferFrames,
                             const PaStreamCallbackTimeInfo* timeInfo, PaStreamCallbackFlags statusFlags, void *ctx)
{
    Q_UNUSED(inputBuffer);
    Q_UNUSED(timeInfo);

#ifdef AUDIOOUTPUT_RAW_FILE_OUT
    int ret = static_cast<AudioOutput*>(ctx)->portAudioCbPrivate(outputBuffer, nBufferFrames);
    if (static_cast<AudioOutput*>(ctx)->m_rawOut)
    {
        fwrite(outputBuffer, sizeof(int16_t), nBufferFrames * static_cast<AudioOutput*>(ctx)->m_numChannels, static_cast<AudioOutput*>(ctx)->m_rawOut);
    }
    return ret;
#else
    if (statusFlags)
    {
        qCWarning(audioOutput) << "Port Audio statusFlags =" << statusFlags;
    }

    return static_cast<AudioOutput*>(ctx)->portAudioCbPrivate(outputBuffer, nBufferFrames);
#endif
}

int AudioOutput::portAudioCbPrivate(void *outputBuffer, unsigned long nBufferFrames)
{
    // read samples from input buffer
    m_inFifoPtr->mutex.lock();
    uint64_t count = m_inFifoPtr->count;
    m_inFifoPtr->mutex.unlock();

    uint64_t bytesToRead = m_bytesPerFrame * nBufferFrames;
    uint32_t availableSamples = nBufferFrames;

    // is any bit is set then mute is requested (mute | stop | restart)
    unsigned int request = m_cbRequest;

    if (AudioOutputPlaybackState::Muted == m_playbackState)
    {   // muted
        // condition to unmute is enough samples && !muteFlag
        if (count > 6*bytesToRead)
        {   // enough samples => reading data from input fifo
            if (Request::None != request)
            {   // staying muted -> setting output buffer to 0
                memset(outputBuffer, 0, bytesToRead);

                // shifting buffer pointers
                m_inFifoPtr->tail = (m_inFifoPtr->tail + bytesToRead) % AUDIO_FIFO_SIZE;
                m_inFifoPtr->mutex.lock();
                m_inFifoPtr->count -= bytesToRead;
                m_inFifoPtr->countChanged.wakeAll();
                m_inFifoPtr->mutex.unlock();

                if (request & (Request::Stop | Request::Restart))
                {   // stop or restart requested ==> finish playback
                    return paComplete;
                }

                // done
                return paContinue;
            }
            else { /* no request */ }

            // at this point we have enough sample to unmute and there is no request => preparing data
            uint64_t bytesToEnd = AUDIO_FIFO_SIZE - m_inFifoPtr->tail;
            if (bytesToEnd < bytesToRead)
            {
                float volume = m_linearVolume;
                if (volume > 0.9)
                {   // only copy here
                    memcpy((uint8_t*)outputBuffer, m_inFifoPtr->buffer+m_inFifoPtr->tail, bytesToEnd);
                    memcpy((uint8_t*)outputBuffer + bytesToEnd, m_inFifoPtr->buffer, (bytesToRead - bytesToEnd));
                    m_inFifoPtr->tail = bytesToRead - bytesToEnd;
                }
                else
                {
                    //memcpy((uint8_t*)outputBuffer, m_inFifoPtr->buffer+m_inFifoPtr->tail, bytesToEnd);
                    int16_t * inDataPtr = (int16_t *) (m_inFifoPtr->buffer+m_inFifoPtr->tail);
                    int16_t * outDataPtr = (int16_t *) outputBuffer;
                    // each sample is int16 => 2 bytes per samples
                    uint_fast32_t numSamples = (bytesToEnd >> 1);

                    Q_ASSERT(numSamples*sizeof(int16_t) == bytesToEnd);

                    for (uint_fast32_t n = 0; n < numSamples; ++n)
                    {
#if AUDIOOUTPUT_PORTAUDIO_VOLUME_ROUND
                        *outDataPtr++ = int16_t(std::lroundf(volume * *inDataPtr++));
#else
                        *outDataPtr++ = int16_t(volume * *inDataPtr++);
#endif
                    }

                    //memcpy((uint8_t*)outputBuffer + bytesToEnd, m_inFifoPtr->buffer, (bytesToRead - bytesToEnd));
                    inDataPtr = (int16_t *) m_inFifoPtr->buffer;
                    outDataPtr = (int16_t *) ((uint8_t*)outputBuffer + bytesToEnd);
                    // each sample is int16 => 2 bytes per samples
                    numSamples = ((bytesToRead - bytesToEnd) >> 1);

                    Q_ASSERT(numSamples*sizeof(int16_t) == (bytesToRead - bytesToEnd));

                    for (uint_fast32_t n = 0; n < numSamples; ++n)
                    {
#if AUDIOOUTPUT_PORTAUDIO_VOLUME_ROUND
                        *outDataPtr++ = int16_t(std::lroundf(volume * *inDataPtr++));
#else
                        *outDataPtr++ = int16_t(volume * *inDataPtr++);
#endif
                    }
                    m_inFifoPtr->tail = bytesToRead - bytesToEnd;
                }
            }
            else
            {
                float volume = m_linearVolume;
                if (volume > 0.9)
                {  // only copy here
                    memcpy((uint8_t*) outputBuffer, m_inFifoPtr->buffer+m_inFifoPtr->tail, bytesToRead);
                    m_inFifoPtr->tail += bytesToRead;
                }
                else
                {
                    //memcpy((uint8_t*) outputBuffer, m_inFifoPtr->buffer+m_inFifoPtr->tail, bytesToRead);
                    int16_t * inDataPtr = (int16_t *) (m_inFifoPtr->buffer+m_inFifoPtr->tail);
                    int16_t * outDataPtr = (int16_t *) outputBuffer;
                    // each sample is int16 => 2 bytes per samples
                    uint_fast32_t numSamples = (bytesToRead >> 1);

                    Q_ASSERT(numSamples*sizeof(int16_t) == bytesToRead);

                    for (uint_fast32_t n = 0; n < numSamples; ++n)
                    {
#if AUDIOOUTPUT_PORTAUDIO_VOLUME_ROUND
                        *outDataPtr++ = int16_t(std::lroundf(volume * *inDataPtr++));
#else
                        *outDataPtr++ = int16_t(volume * *inDataPtr++);
#endif
                    }

                    m_inFifoPtr->tail += bytesToRead;
                }
            }
            m_inFifoPtr->mutex.lock();
            m_inFifoPtr->count -= bytesToRead;
            m_inFifoPtr->countChanged.wakeAll();
            m_inFifoPtr->mutex.unlock();

            // unmute request
            request = Request::None;
        }
        else
        {   // not enough samples ==> inserting silence
            qCDebug(audioOutput, "Muted: Inserting silence [%lu ms]", nBufferFrames/m_sampleRate_kHz);
            memset(outputBuffer, 0, bytesToRead);

            if (request & (Request::Stop | Request::Restart))
            {   // stop or restart requested ==> finish playback
                return paComplete;
            }

            // done
            return paContinue;
        }
    }
    else
    {   // (AudioOutputPlaybackState::Muted != m_playbackState)
        // cannot be anything else than Muted or Playing ==> playing

        // condition to mute is not enough samples || muteFlag
        if (count < bytesToRead)
        {   // not enough samples -> reading what we have and filling rest with zeros
            // minimum mute time is 1ms (m_sampleRate_kHz samples) , if less then hard mute
            if (m_sampleRate_kHz*m_bytesPerFrame > count)
            {   // nothing to play (cannot apply mute ramp)
                qCInfo(audioOutput, "Hard mute [no samples available]");
                memset(outputBuffer, 0, bytesToRead);
                m_playbackState = AudioOutputPlaybackState::Muted;
                return paContinue;
            }

            // there are some samples available
            availableSamples = count/m_bytesPerFrame;

            Q_ASSERT(count == availableSamples*m_bytesPerFrame);

            // copy all available samples to output buffer
            uint64_t bytesToEnd = AUDIO_FIFO_SIZE - m_inFifoPtr->tail;
            if (bytesToEnd < count)
            {
                float volume = m_linearVolume;
                if (volume > 0.9)
                {   // only copy here
                    memcpy((uint8_t*)outputBuffer, m_inFifoPtr->buffer+m_inFifoPtr->tail, bytesToEnd);
                    memcpy((uint8_t*)outputBuffer + bytesToEnd, m_inFifoPtr->buffer, (count - bytesToEnd));
                    m_inFifoPtr->tail = count - bytesToEnd;
                }
                else
                {
                    //memcpy((uint8_t*)outputBuffer, m_inFifoPtr->buffer+m_inFifoPtr->tail, bytesToEnd);
                    int16_t * inDataPtr = (int16_t *) (m_inFifoPtr->buffer+m_inFifoPtr->tail);
                    int16_t * outDataPtr = (int16_t *) outputBuffer;
                    // each sample is int16 => 2 bytes per samples
                    uint_fast32_t numSamples = (bytesToEnd >> 1);

                    Q_ASSERT(numSamples*sizeof(int16_t) == bytesToEnd);

                    for (uint_fast32_t n = 0; n < numSamples; ++n)
                    {
#if AUDIOOUTPUT_PORTAUDIO_VOLUME_ROUND
                        *outDataPtr++ = int16_t(std::lroundf(volume * *inDataPtr++));
#else
                        *outDataPtr++ = int16_t(volume * *inDataPtr++);
#endif
                    }
                    //memcpy((uint8_t*)outputBuffer + bytesToEnd, m_inFifoPtr->buffer, (count - bytesToEnd));
                    inDataPtr = (int16_t *) m_inFifoPtr->buffer;
                    // each sample is int16 => 2 bytes per samples
                    numSamples = ((count - bytesToEnd) >> 1);

                    Q_ASSERT(numSamples*sizeof(int16_t) == (count - bytesToEnd));

                    for (uint_fast32_t n = 0; n < numSamples; ++n)
                    {
#if AUDIOOUTPUT_PORTAUDIO_VOLUME_ROUND
                        *outDataPtr++ = int16_t(std::lroundf(volume * *inDataPtr++));
#else
                        *outDataPtr++ = int16_t(volume * *inDataPtr++);
#endif
                    }
                    m_inFifoPtr->tail = count - bytesToEnd;
                }
            }
            else
            {
                float volume = m_linearVolume;
                if (volume > 0.9)
                {   // only copy here
                    memcpy((uint8_t*) outputBuffer, m_inFifoPtr->buffer+m_inFifoPtr->tail, count);
                    m_inFifoPtr->tail += count;
                }
                else
                {
                    //memcpy((uint8_t*) outputBuffer, m_inFifoPtr->buffer+m_inFifoPtr->tail, count);
                    int16_t * inDataPtr = (int16_t *) (m_inFifoPtr->buffer+m_inFifoPtr->tail);
                    int16_t * outDataPtr = (int16_t *) outputBuffer;
                    // each sample is int16 => 2 bytes per samples
                    uint_fast32_t numSamples = (count >> 1);

                    Q_ASSERT(numSamples*sizeof(int16_t) == count);

                    for (uint_fast32_t n = 0; n < numSamples; ++n)
                    {
#if AUDIOOUTPUT_PORTAUDIO_VOLUME_ROUND
                        *outDataPtr++ = int16_t(std::lroundf(volume * *inDataPtr++));
#else
                        *outDataPtr++ = int16_t(volume * *inDataPtr++);
#endif
                    }
                    m_inFifoPtr->tail += count;
                }
            }
            // set rest of the samples to be 0
            memset((uint8_t*)outputBuffer+count, 0, bytesToRead-count);

            m_inFifoPtr->mutex.lock();
            m_inFifoPtr->count -= count;
            m_inFifoPtr->countChanged.wakeAll();
            m_inFifoPtr->mutex.unlock();

            // request to apply mute ramp
            request = Request::Mute;
        }
        else
        {   // enough sample available -> reading samples
            uint64_t bytesToEnd = AUDIO_FIFO_SIZE - m_inFifoPtr->tail;
            if (bytesToEnd < bytesToRead)
            {
                float volume = m_linearVolume;
                if (volume > 0.9)
                {   // only copy here
                    memcpy((uint8_t*)outputBuffer, m_inFifoPtr->buffer+m_inFifoPtr->tail, bytesToEnd);
                    memcpy((uint8_t*)outputBuffer + bytesToEnd, m_inFifoPtr->buffer, (bytesToRead - bytesToEnd));
                    m_inFifoPtr->tail = bytesToRead - bytesToEnd;
                }
                else
                {
                    //memcpy((uint8_t*)outputBuffer, m_inFifoPtr->buffer+m_inFifoPtr->tail, bytesToEnd);
                    int16_t * inDataPtr = (int16_t *) (m_inFifoPtr->buffer+m_inFifoPtr->tail);
                    int16_t * outDataPtr = (int16_t *) outputBuffer;
                    // each sample is int16 => 2 bytes per samples
                    uint_fast32_t numSamples = (bytesToEnd >> 1);

                    Q_ASSERT(numSamples*sizeof(int16_t) == bytesToEnd);

                    for (uint_fast32_t n = 0; n < numSamples; ++n)
                    {
#if AUDIOOUTPUT_PORTAUDIO_VOLUME_ROUND
                        *outDataPtr++ = int16_t(std::lroundf(volume * *inDataPtr++));
#else
                        *outDataPtr++ = int16_t(volume * *inDataPtr++);
#endif
                    }

                    //memcpy((uint8_t*)outputBuffer + bytesToEnd, m_inFifoPtr->buffer, (bytesToRead - bytesToEnd));
                    inDataPtr = (int16_t *) m_inFifoPtr->buffer;
                    outDataPtr = (int16_t *) ((uint8_t*)outputBuffer + bytesToEnd);
                    // each sample is int16 => 2 bytes per samples
                    numSamples = ((bytesToRead - bytesToEnd) >> 1);

                    Q_ASSERT(numSamples*sizeof(int16_t) == (bytesToRead - bytesToEnd));

                    for (uint_fast32_t n = 0; n < numSamples; ++n)
                    {
#if AUDIOOUTPUT_PORTAUDIO_VOLUME_ROUND
                        *outDataPtr++ = int16_t(std::lroundf(volume * *inDataPtr++));
#else
                        *outDataPtr++ = int16_t(volume * *inDataPtr++);
#endif
                    }
                    m_inFifoPtr->tail = bytesToRead - bytesToEnd;
                }
            }
            else
            {
                float volume = m_linearVolume;
                if (volume > 0.9)
                {
                    memcpy((uint8_t*) outputBuffer, m_inFifoPtr->buffer+m_inFifoPtr->tail, bytesToRead);
                    m_inFifoPtr->tail += bytesToRead;
                }
                else
                {
                    //memcpy((uint8_t*) outputBuffer, m_inFifoPtr->buffer+m_inFifoPtr->tail, bytesToRead);
                    int16_t * inDataPtr = (int16_t *) (m_inFifoPtr->buffer+m_inFifoPtr->tail);
                    int16_t * outDataPtr = (int16_t *) outputBuffer;
                    // each sample is int16 => 2 bytes per samples
                    uint_fast32_t numSamples = (bytesToRead >> 1);

                    Q_ASSERT(numSamples*sizeof(int16_t) == bytesToRead);

                    for (uint_fast32_t n = 0; n < numSamples; ++n)
                    {
#if AUDIOOUTPUT_PORTAUDIO_VOLUME_ROUND
                        *outDataPtr++ = int16_t(std::lroundf(volume * *inDataPtr++));
#else
                        *outDataPtr++ = int16_t(volume * *inDataPtr++);
#endif
                    }
                    m_inFifoPtr->tail += bytesToRead;
                }
            }
            m_inFifoPtr->mutex.lock();
            m_inFifoPtr->count -= bytesToRead;
            m_inFifoPtr->countChanged.wakeAll();
            m_inFifoPtr->mutex.unlock();

//            if ((Request::Restart & request) && (count >= 4*bytesToRead))
//            {   // removing restart flag ==> play all samples we have
//                request &= ~Request::Restart;
//            }

            if (Request::None == request)
            {   // done
                return paContinue;
            }
        }
    }

    // at this point we have buffer that needs to be muted or unmuted
    // it is indicated by request variable

    // unmute
    if (Request::None == request)
    {   // unmute can be requested only when there is enough samples
        qCInfo(audioOutput) << "Unmuting audio";

        float coe = 2.0 - m_muteFactor;
        float gain = AUDIOOUTPUT_FADE_MIN_LIN;
        int16_t * dataPtr = (int16_t *) outputBuffer;
        for (uint_fast32_t n = 0; n < availableSamples; ++n)
        {
            for (uint_fast8_t c = 0; c < m_numChannels; ++c)
            {
                *dataPtr = int16_t(std::lroundf(gain * *dataPtr));
                dataPtr++;
            }
            gain = gain * coe;  // after by purpose
        }

        m_playbackState = AudioOutputPlaybackState::Playing; // playing

        // done
        return paContinue;
    }
    else
    {   // there is so request => muting
        // mute can be requested when there is not enough samples or from HMI
        qCInfo(audioOutput, "Muting... [available %u samples]", availableSamples);
        float coe = m_muteFactor;
        if (availableSamples < AUDIOOUTPUT_FADE_TIME_MS * m_sampleRate_kHz)
        {   // less samples than expected available => need to calculate new coef
            coe = powf(10, AUDIOOUTPUT_FADE_MIN_DB/(20.0*availableSamples));
        }

        float gain = 1.0;

        int16_t * dataPtr = (int16_t *) outputBuffer;
        for (uint_fast32_t n = 0; n < availableSamples; ++n)
        {
            gain = gain * coe;  // before by purpose
            for (uint_fast8_t c = 0; c < m_numChannels; ++c)
            {
                *dataPtr = int16_t(std::lroundf(gain * *dataPtr));
                dataPtr++;
            }
        }

        m_playbackState = AudioOutputPlaybackState::Muted; // muted

        if (request & (Request::Stop | Request::Restart))
        {   // stop or restart requested ==> finish playback
            return paComplete;
        }
    }

    return paContinue;
}

void AudioOutput::portAudioStreamFinishedCb(void *ctx)
{
    static_cast<AudioOutput*>(ctx)->portAudioStreamFinishedPrivateCb();
}

void AudioOutput::onStreamFinished()
{
    if (m_cbRequest & Request::Restart)
    {   // restart was requested (flag is cleared in start routine)
        emit audioOutputRestart();
        start(m_restartFifoPtr);        
    }
#ifdef Q_OS_WIN
    else if (!(m_cbRequest & Request::Stop))
    {   // finished and not stop request ==> problem with output device
        // start again
        qCWarning(audioOutput, "Current audio device probably removed, trying new default device");

        // hotplug handling
        Pa_Terminate();
        Pa_Initialize();

        /* Open an audio I/O stream. */
        PaError err = Pa_OpenDefaultStream( &m_outStream,
                                           0,              /* no input channels */
                                           m_numChannels,  /* stereo output */
                                           paInt16,        /* 16 bit floating point output */
                                           m_sampleRate_kHz*1000,
                                           m_bufferFrames, /* frames per buffer, i.e. the number
                                                           of sample frames that PortAudio will
                                                           request from the callback. Many apps
                                                           may want to use
                                                           paFramesPerBufferUnspecified, which
                                                           tells PortAudio to pick the best,
                                                           possibly changing, buffer size.*/
                                           portAudioCb,    /* callback */
                                           (void *) this);
        if (paNoError != err)
        {
            throw std::runtime_error(std::string(Q_FUNC_INFO) + "PortAudio error:" + Pa_GetErrorText( err ) );
        }

        err = Pa_SetStreamFinishedCallback(m_outStream, portAudioStreamFinishedCb);
        if (paNoError != err)
        {
            throw std::runtime_error(std::string(Q_FUNC_INFO) + "PortAudio error:" + Pa_GetErrorText( err ) );
        }

        m_playbackState = AudioOutputPlaybackState::Muted;
        m_cbRequest &= ~(Request::Stop | Request::Restart);  // reset stop and restart bits

        err = Pa_StartStream(m_outStream);
        if (paNoError != err)
        {
            throw std::runtime_error(std::string(Q_FUNC_INFO) + "PortAudio error:" + Pa_GetErrorText( err ) );
        }
    }
#endif
    else { /* do nothing */ }
}

#else // HAVE_PORTAUDIO

AudioOutput::AudioOutput(QObject *parent) : QObject(parent)
{
    m_devices = new  QMediaDevices(this);
    m_audioSink = nullptr;
    m_ioDevice = nullptr;
    m_linearVolume = 1.0;
    m_ioDevice = new AudioIODevice();

    //m_currentAudioDevice = m_devices->defaultAudioOutput();
    connect(m_devices, &QMediaDevices::audioOutputsChanged, this, &AudioOutput::updateAudioDevices);
}

AudioOutput::~AudioOutput()
{
    if (nullptr != m_audioSink)
    {
        m_audioSink->stop();
        delete m_audioSink;
    }
    m_ioDevice->close();
    delete m_ioDevice;
}

void AudioOutput::start(audioFifo_t *buffer)
{
    uint32_t sRate = buffer->sampleRate;
    uint8_t numCh = buffer->numChannels;

    QAudioFormat format;    
    format.setSampleRate(sRate);
    format.setSampleFormat(QAudioFormat::Int16);
    format.setChannelCount(numCh);
    if (numCh > 1)
    {
        format.setChannelConfig(QAudioFormat::ChannelConfigStereo);
    }
    else
    {
        format.setChannelConfig(QAudioFormat::ChannelConfigMono);
    }

    if (!m_currentAudioDevice.isFormatSupported(format))
    {
        qWarning() << "Default format not supported - trying to use preferred";
        format = m_currentAudioDevice.preferredFormat();
    }

    if (nullptr != m_audioSink)
    {
        // audioSink exists --> delete
        delete m_audioSink;
        m_audioSink = nullptr;
    }

    // create audio sink
    m_audioSink = new QAudioSink(m_currentAudioDevice, format, this);

    // set buffer size to 2* AUDIO_FIFO_CHUNK_MS ms
    m_audioSink->setBufferSize(2 * AUDIO_FIFO_CHUNK_MS * sRate/1000 * numCh * sizeof(int16_t));

    connect(m_audioSink, &QAudioSink::stateChanged, this, &AudioOutput::handleStateChanged);

    m_audioSink->setVolume(m_linearVolume);
    m_currentFifoPtr = buffer;

    // start IO device
    m_ioDevice->close();
    m_ioDevice->setBuffer(m_currentFifoPtr);
    m_ioDevice->start();
    m_audioSink->start(m_ioDevice);
}

void AudioOutput::restart(audioFifo_t *buffer)
{
    if (nullptr != m_audioSink)
    {
        if (!m_ioDevice->isMuted())
        {   // delay stop until audio is muted
            m_restartFifoPtr = buffer;
            m_ioDevice->stop();
            return;
        }
        else
        { /* were are already muted - doRestart now */ }

        doRestart(buffer);
    }
    else { /* do nothing */ }
}

void AudioOutput::mute(bool on)
{
    m_ioDevice->mute(on);
}

void AudioOutput::setVolume(int value)
{
    m_linearVolume = QAudio::convertVolume(value / qreal(100),
                                           QAudio::LogarithmicVolumeScale,
                                           QAudio::LinearVolumeScale);
    if (nullptr != m_audioSink)
    {
        m_audioSink->setVolume(m_linearVolume);
    }
}

QList<QAudioDevice> AudioOutput::getAudioDevices()
{
    QList<QAudioDevice> list;
    const QAudioDevice &defaultDeviceInfo = m_devices->defaultAudioOutput();
    list.append(defaultDeviceInfo);

    for (auto &deviceInfo : m_devices->audioOutputs())
    {
        //qDebug() << deviceInfo.description() << deviceInfo.id() << "?=" << m_currentAudioDevice.description() << m_currentAudioDevice.id();
        if (deviceInfo != defaultDeviceInfo)
        {
            list.append(deviceInfo);
        }
    }
    return list;
}

void AudioOutput::setAudioDevice(const QByteArray & deviceId)
{
    if ((!deviceId.isEmpty()) && (deviceId == m_currentAudioDevice.id()))
    {   // do nothing
        return;
    }
    QList<QAudioDevice> list = getAudioDevices();
    for (const auto & dev : list)
    {
        if (dev.id() == deviceId)
        {
            m_currentAudioDevice = dev;
            emit audioDeviceChanged(m_currentAudioDevice.id());
            // change output device to newly selected
            restart(m_currentFifoPtr);
            return;
        }
    }

    // device not found => choosing default
    m_currentAudioDevice = m_devices->defaultAudioOutput();
    emit audioDeviceChanged(m_currentAudioDevice.id());
    // change output device to newly selected
    restart(m_currentFifoPtr);
}

void AudioOutput::stop()
{
    if (nullptr != m_audioSink)
    {
        if (!m_ioDevice->isMuted())
        {   // delay stop until audio is muted
            m_ioDevice->stop();
            return;
        }
        else                       
        { /* were are already muted - doStop now */ }

        doStop();
    }
    else { /* do nothing */ }
}

void AudioOutput::doStop()
{
    m_audioSink->stop();
    m_ioDevice->close();
}

void AudioOutput::doRestart(audioFifo_t *buffer)
{
    m_restartFifoPtr = nullptr;
    m_audioSink->stop();
    emit audioOutputRestart();
    start(buffer);
}

void AudioOutput::handleStateChanged(QAudio::State newState)
{
    //qDebug() << Q_FUNC_INFO << newState;
    switch (newState)
    {
    case QAudio::ActiveState:
        // do nothing
        break;
    case QAudio::IdleState:
        // no more data
        if (m_ioDevice->isMuted())
        {   // this is correct state when stop is requested
            if (nullptr != m_restartFifoPtr)
            {   // restart was requested
                doRestart(m_restartFifoPtr);
            }
            else
            {   // stop was requested
                doStop();
            }
        }
        else
        {
            if (QAudio::Error::NoError == m_audioSink->error())
            {
                qCWarning(audioOutput) << "Audio going to Idle state unexpectly, trying to restart...";
                doRestart(m_currentFifoPtr);
            }
            else
            {   // some error -> doing stop
                qCWarning(audioOutput) << "Audio going to Idle state unexpectly, error code:" << m_audioSink->error();
                doStop();
                emit audioOutputError();
            }
        }
        break;
    case QAudio::StoppedState:
        // Stopped for other reasons
        break;

    default:
        // ... other cases as appropriate
        break;
    }
}

void AudioOutput::updateAudioDevices()
{
    QList<QAudioDevice> list = getAudioDevices();

    emit audioDevicesList(list);

    bool currentDeviceFound = false;
    for (auto & dev : list)
    {
        if (dev.id() == m_currentAudioDevice.id())
        {
            currentDeviceFound = true;
            break;
        }
    }

    if (!currentDeviceFound)
    {   // current device no longer exists => default is used
        m_currentAudioDevice = m_devices->defaultAudioOutput();
        //qDebug() << "New output device" << m_currentAudioDevice.description();
    }
    emit audioDeviceChanged(m_currentAudioDevice.id());
}

AudioIODevice::AudioIODevice(QObject *parent) : QIODevice(parent)
{
}

void AudioIODevice::setBuffer(audioFifo_t * buffer)
{
    m_inFifoPtr = buffer;

    m_sampleRate_kHz = buffer->sampleRate / 1000;
    m_numChannels = buffer->numChannels;
    m_bytesPerFrame = m_numChannels * sizeof(int16_t);

    // mute ramp is exponential
    // value are precalculated to save MIPS in runtime
    // unmute ramp is then calculated as 2.0 - m_muteFactor in runtime
    // m_muteFactor is calculated for change from 0dB to AUDIOOUTPUT_FADE_MIN_DB in AUDIOOUTPUT_FADE_TIME_MS
    m_muteFactor = powf(10, AUDIOOUTPUT_FADE_MIN_DB/(20.0*AUDIOOUTPUT_FADE_TIME_MS*m_sampleRate_kHz));       
}

void AudioIODevice::start()
{
    m_stopFlag = false;
    m_doStop = false;
    m_playbackState = AudioOutputPlaybackState::Muted;    
    open(QIODevice::ReadOnly);

    // for testing of error
    QTimer::singleShot(10000, this, [this](){ m_doStop = true; } );
}

void AudioIODevice::stop()
{
    m_stopFlag = true;     // set stop bit
}

qint64 AudioIODevice::readData(char *data, qint64 len)
{
    if (m_doStop || (0 == len))
    {
        return 0;
    }

    // read samples from input buffer
    m_inFifoPtr->mutex.lock();
    uint64_t count = m_inFifoPtr->count;
    m_inFifoPtr->mutex.unlock();

    bool muteRequest = m_muteFlag || m_stopFlag;
    m_doStop = m_stopFlag;

    uint64_t bytesToRead = len;

    uint64_t numSamples = len / m_bytesPerFrame;

    //qDebug() << Q_FUNC_INFO << len << count;

    if (AudioOutputPlaybackState::Muted == m_playbackState)
    {   // muted
        // condition to unmute is enough samples
        if (count > 500*m_sampleRate_kHz*m_bytesPerFrame)    // 800ms of signal
        {   // enough samples => reading data from input fifo
            if (muteRequest)
            {   // staying muted -> setting output buffer to 0
                memset(data, 0, bytesToRead);

                // shifting buffer pointers
                m_inFifoPtr->tail = (m_inFifoPtr->tail + bytesToRead) % AUDIO_FIFO_SIZE;
                m_inFifoPtr->mutex.lock();
                m_inFifoPtr->count -= bytesToRead;
                m_inFifoPtr->countChanged.wakeAll();
                m_inFifoPtr->mutex.unlock();

                // done
                return bytesToRead;
            }
            else { /* no request */ }

            // at this point we have enough sample to unmute and there is no request => preparing data
            uint64_t bytesToEnd = AUDIO_FIFO_SIZE - m_inFifoPtr->tail;
            if (bytesToEnd < bytesToRead)
            {
                memcpy((uint8_t*)data, m_inFifoPtr->buffer+m_inFifoPtr->tail, bytesToEnd);
                memcpy((uint8_t*)data + bytesToEnd, m_inFifoPtr->buffer, (bytesToRead - bytesToEnd));
                m_inFifoPtr->tail = bytesToRead - bytesToEnd;
            }
            else
            {
                memcpy((uint8_t*) data, m_inFifoPtr->buffer+m_inFifoPtr->tail, bytesToRead);
                m_inFifoPtr->tail += bytesToRead;
            }

            m_inFifoPtr->mutex.lock();
            m_inFifoPtr->count -= bytesToRead;
            m_inFifoPtr->countChanged.wakeAll();
            m_inFifoPtr->mutex.unlock();

            // unmute request
            muteRequest = false;
        }
        else
        {   // not enough samples ==> inserting silence
            qCDebug(audioOutput, "Muted: Inserting silence [%llu ms]", bytesToRead / (m_bytesPerFrame * m_sampleRate_kHz));

            memset(data, 0, bytesToRead);

            // done
            return bytesToRead;
        }
    }
    else
    {   // (AudioOutputPlaybackState::Muted != m_playbackState)
        // cannot be anything else than Muted or Playing ==> playing

        // condition to mute is not enough samples || muteFlag
        if (count < bytesToRead)
        {   // not enough samples -> reading what we have and filling rest with zeros
            // minimum mute time is 1ms (m_sampleRate_kHz samples) , if less then hard mute
            if (m_sampleRate_kHz*m_bytesPerFrame > count)
            {   // nothing to play
                qCInfo(audioOutput, "Hard mute [no samples available]");
                memset(data, 0, bytesToRead);
                m_playbackState = AudioOutputPlaybackState::Muted;
                return bytesToRead;
            }

            // there are some samples available

            //            // this condition is to avoid mute when there are still enough samples
            //            if (!muteRequest && (count > AUDIOOUTPUT_FADE_TIME_MS*m_sampleRate_kHz*m_bytesPerFrame))
            //            {  // at least 2x mute time is available
            //                bytesToRead = count - AUDIOOUTPUT_FADE_TIME_MS*m_sampleRate_kHz*m_bytesPerFrame;

            //                uint64_t bytesToEnd = AUDIO_FIFO_SIZE - m_inFifoPtr->tail;
            //                if (bytesToEnd < bytesToRead)
            //                {
            //                    memcpy((uint8_t*)data, m_inFifoPtr->buffer+m_inFifoPtr->tail, bytesToEnd);
            //                    memcpy((uint8_t*)data + bytesToEnd, m_inFifoPtr->buffer, (bytesToRead - bytesToEnd));
            //                    m_inFifoPtr->tail = bytesToRead - bytesToEnd;
            //                }
            //                else
            //                {
            //                    memcpy((uint8_t*) data, m_inFifoPtr->buffer+m_inFifoPtr->tail, bytesToRead);
            //                    m_inFifoPtr->tail += bytesToRead;
            //                }

            //                m_inFifoPtr->mutex.lock();
            //                m_inFifoPtr->count -= bytesToRead;
            //                m_inFifoPtr->countChanged.wakeAll();
            //                m_inFifoPtr->mutex.unlock();

            //                return bytesToRead;
            //            }

            // copy all available samples to output buffer
            uint64_t bytesToEnd = AUDIO_FIFO_SIZE - m_inFifoPtr->tail;
            if (bytesToEnd < count)
            {
                memcpy((uint8_t*)data, m_inFifoPtr->buffer+m_inFifoPtr->tail, bytesToEnd);
                memcpy((uint8_t*)data + bytesToEnd, m_inFifoPtr->buffer, (count - bytesToEnd));
                m_inFifoPtr->tail = count - bytesToEnd;
            }
            else
            {
                memcpy((uint8_t*) data, m_inFifoPtr->buffer+m_inFifoPtr->tail, count);
                m_inFifoPtr->tail += count;
            }
            // set rest of the samples to be 0
            memset((uint8_t*)data+count, 0, bytesToRead-count);

            m_inFifoPtr->mutex.lock();
            m_inFifoPtr->count -= count;
            m_inFifoPtr->countChanged.wakeAll();
            m_inFifoPtr->mutex.unlock();

            numSamples = count / m_bytesPerFrame;

            // request to apply mute ramp
            muteRequest = true;  // mute
        }
        else
        {   // enough sample available -> reading samples
            uint64_t bytesToEnd = AUDIO_FIFO_SIZE - m_inFifoPtr->tail;
            if (bytesToEnd < bytesToRead)
            {
                memcpy((uint8_t*)data, m_inFifoPtr->buffer+m_inFifoPtr->tail, bytesToEnd);
                memcpy((uint8_t*)data + bytesToEnd, m_inFifoPtr->buffer, (bytesToRead - bytesToEnd));
                m_inFifoPtr->tail = bytesToRead - bytesToEnd;
            }
            else
            {
                memcpy((uint8_t*) data, m_inFifoPtr->buffer+m_inFifoPtr->tail, bytesToRead);
                m_inFifoPtr->tail += bytesToRead;
            }

            m_inFifoPtr->mutex.lock();
            m_inFifoPtr->count -= bytesToRead;
            m_inFifoPtr->countChanged.wakeAll();
            m_inFifoPtr->mutex.unlock();

            if (!muteRequest)
            {   // done
                return bytesToRead;
            }
        }
    }

    // at this point we have buffer that needs to be muted or unmuted
    // it is indicated by playbackState variable

    // unmute
    if (false == muteRequest)
    {   // unmute can be requested only when there is enough samples
        qCInfo(audioOutput) << "Unmuting audio";
        float coe = 2.0 - m_muteFactor;
        if (numSamples < AUDIOOUTPUT_FADE_TIME_MS*m_sampleRate_kHz)
        {
            coe = 2.0 - powf(10, AUDIOOUTPUT_FADE_MIN_DB/(20.0*numSamples));
        }
        else
        {
            numSamples = AUDIOOUTPUT_FADE_TIME_MS*m_sampleRate_kHz;
        }

        float gain = AUDIOOUTPUT_FADE_MIN_LIN;
        int16_t * dataPtr = (int16_t *) data;
        for (uint_fast32_t n = 0; n < numSamples; ++n)
        {
            for (uint_fast8_t c = 0; c < m_numChannels; ++c)
            {
                *dataPtr = int16_t(qRound(gain * *dataPtr));
                dataPtr++;
            }
            gain = gain * coe;  // after by purpose
        }
        m_playbackState = AudioOutputPlaybackState::Playing; // playing

        return bytesToRead;
    }
    else
    {   // mute can be requested when there is not enough samples or from HMI
        qCInfo(audioOutput, "Muting... [available %llu samples]", numSamples);
        if (numSamples < AUDIOOUTPUT_FADE_TIME_MS*m_sampleRate_kHz)
        {
            float gain = 1.0;
            float coe = powf(10, AUDIOOUTPUT_FADE_MIN_DB/(20.0*numSamples));

            int16_t * dataPtr = (int16_t *) data;
            for (uint_fast32_t n = 0; n < numSamples; ++n)
            {
                gain = gain * coe;  // before by purpose
                for (uint_fast8_t c = 0; c < m_numChannels; ++c)
                {
                    *dataPtr = int16_t(qRound(gain * *dataPtr));
                    dataPtr++;
                }
            }
        }
        else
        {
            float gain = 1.0;
            float coe = m_muteFactor;

            int16_t * dataPtr = (int16_t *) data;
            for (uint_fast32_t n = 0; n < AUDIOOUTPUT_FADE_TIME_MS*m_sampleRate_kHz; ++n)
            {
                gain = gain * coe;  // before by purpose
                for (uint_fast8_t c = 0; c < m_numChannels; ++c)
                {
                    *dataPtr = int16_t(qRound(gain * *dataPtr));
                    dataPtr++;
                }
            }
            memset(dataPtr, 0, bytesToRead - AUDIOOUTPUT_FADE_TIME_MS*m_sampleRate_kHz*m_bytesPerFrame);
        }
        m_playbackState = AudioOutputPlaybackState::Muted; // muted
    }

    return bytesToRead;
}

qint64 AudioIODevice::writeData(const char *data, qint64 len)
{
    Q_UNUSED(data);
    Q_UNUSED(len);

    return 0;
}

qint64 AudioIODevice::bytesAvailable() const
{
    m_inFifoPtr->mutex.lock();
    int64_t count = m_inFifoPtr->count;
    m_inFifoPtr->mutex.unlock();

    return count;
}

void AudioIODevice::mute(bool on)
{
   m_muteFlag = on;
}

#endif // HAVE_PORTAUDIO
