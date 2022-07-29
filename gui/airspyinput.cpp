#include <QDir>
#include <QDebug>
//#include <QDeadlineTimer>
#include "airspyinput.h"

int airspyCb(airspy_transfer *transfer);

AirspyInput::AirspyInput(QObject *parent) : InputDevice(parent)
{
    id = InputDeviceId::AIRSPY;

    device = nullptr;
    deviceUnplugged = true;
    deviceRunning = false;
    dumpFile = nullptr;
    filterOutBuffer = new float[65536];
    filter = new AirspyDSFilter();

#if (AIRSPY_WDOG_ENABLE)
    connect(&watchDogTimer, &QTimer::timeout, this, &AirspyInput::watchDogTimeout);
#endif
}

AirspyInput::~AirspyInput()
{
    //qDebug() << Q_FUNC_INFO;
    if (nullptr != device)
    {
        stop();
        if (!deviceUnplugged)
        {
            airspy_close(device);
        }
        airspy_exit();
    }

    delete [] filterOutBuffer;
    delete filter;
}

void AirspyInput::tune(uint32_t freq)
{
    qDebug() << Q_FUNC_INFO << freq;
    frequency = freq;
    if ((deviceRunning) || (0 == freq))
    {   // worker is running
        //      sequence in this case is:
        //      1) stop
        //      2) wait for thread to finish
        //      3) start new worker on new frequency
        // ==> this way we can be sure that all buffers are reset and nothing left from previous channel
        stop();
    }
    else
    {   // worker is not running
        run();
    }
}

bool AirspyInput::openDevice()
{
    int ret = 0;

    // open first device
    ret = airspy_open(&device);
    if (AIRSPY_SUCCESS != ret)
    {
        qDebug() << "AIRSPY:  Failed opening device";
        device = nullptr;
        return false;
    }

    // set sample type
    ret = airspy_set_sample_type(device, AIRSPY_SAMPLE_FLOAT32_IQ);
    if (AIRSPY_SUCCESS != ret)
    {
        qDebug() << "AIRSPY:  Cannot set sample format";
        return false;
    }

    airspy_set_packing(device, 1);

    // Set sample rate
    ret = airspy_set_samplerate(device, 4096000);
    if (AIRSPY_SUCCESS != ret)
    {
        qDebug() << "AIRSPY: Setting sample rate failed";
        return false;
    }
#if 0
    // Get tuner gains
    uint32_t gainsCount = rtlsdr_get_tuner_gains(device, NULL);
    qDebug() << "RTL_SDR: Supported gain values" << gainsCount;
    int * gains = new int[gainsCount];
    gainsCount = rtlsdr_get_tuner_gains(device, gains);

    gainList = new QList<int>();
    for (int i = 0; i < gainsCount; i++) {
        //qDebug() << "RTL_SDR: gain " << (gains[i] / 10.0);
        gainList->append(gains[i]);
    }
    delete [] gains;
    emit gainListAvailable(gainList);

#endif
    connect(this, &AirspyInput::agcLevel, this, &AirspyInput::updateAgc, Qt::QueuedConnection);

    // set automatic gain
    setGainMode(GainMode::Software);
    //setGainMode(GainMode::Hardware);

    deviceUnplugged = false;

    emit deviceReady();

    return true;
}

void AirspyInput::run()
{
    qDebug() << Q_FUNC_INFO;

    // Reset buffer here - worker thread it not running, DAB waits for new data
    inputBuffer.reset();

    filter->reset();

    if (frequency != 0)
    {   // Tune to new frequency

        int ret = airspy_set_freq(device, frequency*1000);
        if (AIRSPY_SUCCESS != ret)
        {
            qDebug("AIRSPY: Tune to %d kHz failed", frequency);
            emit error(InputDeviceErrorCode::DeviceDisconnected);
            return;
        }

        // does nothing if not SW AGC
        resetAgc();
#if AIRSPY_WORKER
        worker = new AirspyWorker(device, this);
        //connect(worker, &AirspyWorker::agcLevel, this, &AirspyInput::updateAgc, Qt::QueuedConnection);
        connect(worker, &AirspyWorker::dumpedBytes, this, &InputDevice::dumpedBytes, Qt::QueuedConnection);
        connect(worker, &AirspyWorker::finished, this, &AirspyInput::readThreadStopped, Qt::QueuedConnection);
        connect(worker, &AirspyWorker::finished, worker, &QObject::deleteLater);
#endif
#if (AIRSPY_WDOG_ENABLE)
        watchDogTimer.start(1000 * INPUTDEVICE_WDOG_TIMEOUT_SEC);
#endif
#if AIRSPY_WORKER
        worker->start();
#else
        ret = airspy_start_rx(device, AirspyInput::callback, (void*)this);
        if (AIRSPY_SUCCESS != ret)
        {
            qDebug("AIRSPY: Failed to start RX");
            emit error(InputDeviceErrorCode::DeviceDisconnected);
            return;
        }
#endif
        deviceRunning = true;
    }
    else
    { /* tune to 0 => going to idle */  }

    emit tuned(frequency);
}

void AirspyInput::stop()
{
    qDebug() << Q_FUNC_INFO;

    if (AIRSPY_TRUE == airspy_is_streaming(device))
    {   // if devise is running, stop worker
        airspy_stop_rx(device);
#if AIRSPY_WORKER
        worker->wait(QDeadlineTimer(2000));
        while (!worker->isFinished())
        {
            qDebug() << "Worker thread not finished after timeout - this should not happen :-(";

            // reset buffer - and tell the thread it is empty - buffer will be reset in any case
            pthread_mutex_lock(&inputBuffer.countMutex);
            inputBuffer.count = 0;
            pthread_cond_signal(&inputBuffer.countCondition);
            pthread_mutex_unlock(&inputBuffer.countMutex);
            worker->wait(2000);
        }
#else
        QThread::msleep(50);
        while (AIRSPY_TRUE == airspy_is_streaming(device))
        {
            qDebug() << "Airspy not finished after timeout - this should not happen :-(";

            // reset buffer - and tell the thread it is empty - buffer will be reset in any case
            pthread_mutex_lock(&inputBuffer.countMutex);
            inputBuffer.count = 0;
            pthread_cond_signal(&inputBuffer.countCondition);
            pthread_mutex_unlock(&inputBuffer.countMutex);
            QThread::msleep(2000);
        }

        deviceRunning = false;       // this flag say that we want to stop worker intentionally
        readThreadStopped();
#endif
    }
    else if (0 == frequency)
    {   // going to idle
        emit tuned(0);
    }
}

void AirspyInput::setGainMode(GainMode mode, int lnaIdx, int mixerIdx, int ifIdx)
{
    switch (mode)
    {
    case GainMode::Hardware:
        if (gainMode == mode)
        {   // do nothing -> mode does not change
            break;
        }
        // mode changes
        gainMode = mode;
        airspy_set_lna_agc(device, 1);
        airspy_set_mixer_agc(device, 1);
        resetAgc();
        break;
    case GainMode::Manual:
        gainMode = mode;
        airspy_set_vga_gain(device, ifIdx);
        if (lnaIdx < 0)
        {
            airspy_set_lna_agc(device, 1);
        }
        else
        {
            airspy_set_lna_agc(device, 0);
            airspy_set_lna_gain(device, lnaIdx);
        }
        if (mixerIdx < 0)
        {
            airspy_set_mixer_agc(device, 1);
        }
        else
        {
            airspy_set_mixer_agc(device, 0);
            airspy_set_mixer_gain(device, mixerIdx);
        }
        break;
    case GainMode::Software:
        if (gainMode == mode)
        {   // do nothing -> mode does not change
            break;
        }
        gainMode = mode;
        resetAgc();
        break;
    }
}

void AirspyInput::setGain(int gIdx)
{
    if (GainMode::Hardware == gainMode)
    {
        if (gIdx < AIRSPY_HW_AGC_MIN)
        {
            gIdx = AIRSPY_HW_AGC_MIN;
        }
        if (gIdx > AIRSPY_HW_AGC_MAX)
        {
            gIdx = AIRSPY_HW_AGC_MAX;
        }

        if (gIdx == gainIdx)
        {
            return;
        }
        // else
        gainIdx = gIdx;

        int ret = airspy_set_vga_gain(device, gainIdx);
        if (AIRSPY_SUCCESS != ret)
        {
            qDebug() << "AIRSPY: Failed to set tuner gain";
        }
        else
        {
            qDebug() << "AIRSPY: Tuner VGA gain set to" << gainIdx;
            //emit agcGain(gainList->at(gainIdx));
        }
        return;
    }
    if (GainMode::Software == gainMode)
    {
        if (gIdx < AIRSPY_SW_AGC_MIN)
        {
            gIdx = AIRSPY_SW_AGC_MIN;
        }
        if (gIdx > AIRSPY_SW_AGC_MAX)
        {
            gIdx = AIRSPY_SW_AGC_MAX;
        }

        if (gIdx == gainIdx)
        {
            return;
        }
        // else
        gainIdx = gIdx;

        int ret = airspy_set_sensitivity_gain(device, gainIdx);
        if (AIRSPY_SUCCESS != ret)
        {
            qDebug() << "AIRSPY: Failed to set tuner gain";
        }
        else
        {
            qDebug() << "AIRSPY: Tuner Sensitivity gain set to" << gainIdx;
            //emit agcGain(gainList->at(gainIdx));
        }
        return;
    }
}

void AirspyInput::resetAgc()
{
    signalLevel = 0.05;
    if (GainMode::Software == gainMode)
    {
        gainIdx = -1;
        setGain((AIRSPY_SW_AGC_MAX+1)/2); // set it to the middle
        return;
    }
    if (GainMode::Hardware == gainMode)
    {
        gainIdx = -1;
        setGain(6);
    }
}

void AirspyInput::updateAgc(float level)
{
    if (level > 0.1)
    {
        //qDebug()  << Q_FUNC_INFO << level << "==> sensitivity down";
        setGain(gainIdx-1);
        return;
    }
    if (level < 0.005)
    {
        //qDebug()  << Q_FUNC_INFO << level << "==> sensitivity up";
        setGain(gainIdx+1);
    }
}

void AirspyInput::readThreadStopped()
{
    //qDebug() << Q_FUNC_INFO << deviceRunning;
#if (AIRSPY_WDOG_ENABLE)
    watchDogTimer.stop();
#endif

    if (deviceRunning)
    {   // if device should be running then it measn reading error thus device is disconnected
        qDebug() << "Airspy is unplugged.";
        deviceUnplugged = true;
        deviceRunning = false;

        // fill buffer (artificially to avoid blocking of the DAB processing thread)
        inputBuffer.fillDummy();

        emit error(InputDeviceErrorCode::DeviceDisconnected);
    }
    else
    {
        //qDebug() << "Airspy Reading thread stopped";

        // in this thread was stopped by to tune to new frequency, there is no other reason to stop the thread
        run();
    }
}

#if (AIRSPY_WDOG_ENABLE)
void AirspyInput::watchDogTimeout()
{
    if (AIRSPY_TRUE != airspy_is_streaming(device))
    {
        qDebug() << Q_FUNC_INFO << "watchdog timeout";
        inputBuffer.fillDummy();
        emit error(InputDeviceErrorCode::NoDataAvailable);
    }
}
#endif

void AirspyInput::startDumpToFile(const QString & filename)
{
    dumpFile = fopen(QDir::toNativeSeparators(filename).toUtf8().data(), "wb");
    if (nullptr != dumpFile)
    {
#if AIRSPY_WORKER
        worker->dumpToFileStart(dumpFile);
#else
        fileMutex.lock();
        enaDumpToFile = true;
        fileMutex.unlock();
#endif
        emit dumpingToFile(true, 2*sizeof(float));
    }
}

void AirspyInput::stopDumpToFile()
{
#if AIRSPY_WORKER
    worker->dumpToFileStop();
#else
    enaDumpToFile = false;
    fileMutex.lock();
    dumpFile = nullptr;
    fileMutex.unlock();
#endif
    fflush(dumpFile);
    fclose(dumpFile);

    emit dumpingToFile(false);
}

void AirspyInput::setBW(int bw)
{
#if 0
    if (bw > 0)
    {
        int ret = rtlsdr_set_tuner_bandwidth(device, bw);
        if (ret != 0)
        {
            qDebug() << "AIRSPY: Failed to set tuner BW";
        }
        else
        {
            qDebug() << "AIRSPY: bandwidth set to" << bw/1000.0 << "kHz";
        }
    }
#endif
}

void AirspyInput::setBiasT(bool ena)
{
    if (ena)
    {
        int ret = airspy_set_rf_bias(device, ena);
        if (ret != 0)
        {
            qDebug() << "AIRSPY: Failed to enable bias-T";
        }
    }
}

void AirspyInput::dumpBuffer(unsigned char *buf, uint32_t len)
{
    fileMutex.lock();
    if (nullptr != dumpFile)
    {
        ssize_t bytes = fwrite(buf, 1, len, dumpFile);
        emit dumpedBytes(bytes);
    }
    fileMutex.unlock();
}

int AirspyInput::callback(airspy_transfer* transfer)
{
    AirspyInput * thisPtr = static_cast<AirspyInput *>(transfer->ctx);
    thisPtr->processInputData(transfer);

    return 0;
}

void AirspyInput::processInputData(airspy_transfer *transfer)
{
    //qDebug() << Q_FUNC_INFO << transfer->sample_count;

    if (transfer->dropped_samples > 0)
    {
        qDebug() << "AIRSPY: dropping" << transfer->dropped_samples << "samples";
    }

    // len is number of I and Q samples
    // get FIFO space
    pthread_mutex_lock(&inputBuffer.countMutex);
    uint64_t count = inputBuffer.count;
    Q_ASSERT(count <= INPUT_FIFO_SIZE);

    pthread_mutex_unlock(&inputBuffer.countMutex);

    uint64_t bytesToWrite = transfer->sample_count*sizeof(float); //*2/2 (considering downsampling by 2)

    if ((INPUT_FIFO_SIZE - count) < bytesToWrite) //*2/2 (considering downsampling by 2)
    {
        qDebug() << Q_FUNC_INFO << "dropping" << transfer->sample_count << "IQ samples...";
        return;
    }

    // input samples are IQ = [float float] @ 4096kHz
    // going to transform them to [float float] @ 2048kHz
    float maxAbs2;
    filter->process((float*) transfer->samples, filterOutBuffer, transfer->sample_count, maxAbs2);

#if (AIRSPY_AGC_ENABLE > 0)
#define LEV_C 0.01
    // calculate signal level (rectifier, fast attack slow release)
    signalLevel = LEV_C * maxAbs2 + signalLevel - LEV_C * signalLevel;

    static uint_fast8_t cntr = 0;
    if (0 == (++cntr & 0x07))
    {
        //qDebug() << signalLevel;
        emit agcLevel(signalLevel);
    }
#endif

    if (isDumpingIQ())
    {
        dumpBuffer((unsigned char *) filterOutBuffer, bytesToWrite);
    }

    // there is enough room in buffer
    uint64_t bytesTillEnd = INPUT_FIFO_SIZE - inputBuffer.head;

    if (bytesTillEnd >= bytesToWrite)
    {
        std::memcpy((inputBuffer.buffer + inputBuffer.head), (uint8_t *) filterOutBuffer, bytesToWrite);
        inputBuffer.head = (inputBuffer.head + bytesToWrite);
    }
    else
    {
        std::memcpy((inputBuffer.buffer + inputBuffer.head), (uint8_t *) filterOutBuffer, bytesTillEnd);
        std::memcpy((inputBuffer.buffer), ((uint8_t *) filterOutBuffer)+bytesTillEnd, bytesToWrite-bytesTillEnd);
        inputBuffer.head = bytesToWrite-bytesTillEnd;
    }

    pthread_mutex_lock(&inputBuffer.countMutex);
    inputBuffer.count = inputBuffer.count + bytesToWrite;
    pthread_cond_signal(&inputBuffer.countCondition);
    pthread_mutex_unlock(&inputBuffer.countMutex);
}

#if 0
AirspyWorker::AirspyWorker(struct airspy_device *d, QObject *parent) : QThread(parent)
{
    enaDumpToFile = false;
    dumpFile = nullptr;
    airspyPtr = parent;
    device = d;    
}

void AirspyWorker::run()
{
    qDebug() << "AirspyWorker thread start" << QThread::currentThreadId();
#if (AIRSPY_DOC_ENABLE > 0)
    dcI = 0.0;
    dcQ = 0.0;
#endif
    agcLev = 0.0;
    wdogIsRunningFlag = false;  // first callback sets it to true

    airspy_start_rx(device, airspyCb, (void*)this);

    qDebug() << "AirspyWorker thread end" << QThread::currentThreadId();
}

void AirspyWorker::dumpToFileStart(FILE * f)
{
    fileMutex.lock();
    dumpFile = f;
    enaDumpToFile = true;
    fileMutex.unlock();
}

void AirspyWorker::dumpToFileStop()
{
    enaDumpToFile = false;
    fileMutex.lock();
    dumpFile = nullptr;
    fileMutex.unlock();
}

void AirspyWorker::dumpBuffer(unsigned char *buf, uint32_t len)
{
    fileMutex.lock();
    if (nullptr != dumpFile)
    {
        ssize_t bytes = fwrite(buf, 1, len, dumpFile);
        emit dumpedBytes(bytes);
    }
    fileMutex.unlock();
}

bool AirspyWorker::isRunning()
{
    bool flag = wdogIsRunningFlag;
    wdogIsRunningFlag = false;
    return flag;
}

int airspyCb(airspy_transfer* transfer)
{
    qDebug() << Q_FUNC_INFO << transfer->sample_count;

#if (AIRSPY_DOC_ENABLE > 0)
    int_fast32_t sumI = 0;
    int_fast32_t sumQ = 0;
#define DC_C 0.05
#endif

#if (AIRSPY_AGC_ENABLE > 0)
    int maxVal = 0;
#define LEV_CATT 0.1
#define LEV_CREL 0.0001
#endif
    if (transfer->dropped_samples > 0)
    {
        qDebug() << "AIRSPY: dropping" << transfer->dropped_samples << "samples";
    }

    uint64_t len = transfer->sample_count*sizeof(int16_t)*2;

    AirspyWorker * airspyWorker = static_cast<AirspyWorker *>(transfer->ctx);
    if (airspyWorker->isDumpingIQ())
    {
        airspyWorker->dumpBuffer((unsigned char *) transfer->samples, len);
    }

#if (AIRSPY_WDOG_ENABLE)
    // reset watchDog flag, timer sets it to true
    airspyWorker->wdogIsRunningFlag = true;
#endif

    // retrieving memories
#if (AIRSPY_DOC_ENABLE > 0)
    float dcI = airspyWorker->dcI;
    float dcQ = airspyWorker->dcQ;
#endif
#if (AIRSPY_AGC_ENABLE > 0)
    float agcLev = airspyWorker->agcLev;
#endif

    // len is number of I and Q samples
    // get FIFO space
    pthread_mutex_lock(&inputBuffer.countMutex);
    uint64_t count = inputBuffer.count;
    Q_ASSERT(count <= INPUT_FIFO_SIZE);

    pthread_mutex_unlock(&inputBuffer.countMutex);

    if ((INPUT_FIFO_SIZE - count) < transfer->sample_count*sizeof(float)) // considering downsampling by 2
    {
        qDebug() << Q_FUNC_INFO << "dropping" << transfer->sample_count << "IQ samples...";
        return 0;
    }

    // input samples are IQ = [int16_t int16_t]
    // going to transform them to [float float] = float _Complex

    // there is enough room in buffer
    uint64_t bytesTillEnd = INPUT_FIFO_SIZE - inputBuffer.head;
    int16_t * inPtr = (int16_t *) transfer->samples;
    if (bytesTillEnd >= transfer->sample_count*2*sizeof(float))
    {
        int16_t * inPtr = (int16_t *) transfer->samples;
        float * outPtr = (float *)(inputBuffer.buffer + inputBuffer.head);
        for (uint64_t k=0; k<transfer->sample_count/2; ++k)
        {   // convert to float
#if ((AIRSPY_DOC_ENABLE == 0) && ((AIRSPY_AGC_ENABLE == 0)))
            *outPtr = float(0.5 * inPtr[4*k]);
            *outPtr++ += float(0.5 * inPtr[4*k+2]);
            *outPtr = float(0.5 * inPtr[4*k+1]);
            *outPtr++ += float(0.5 * inPtr[4*k+3]);
#else // ((AIRSPY_DOC_ENABLE == 0) && ((AIRSPY_AGC_ENABLE == 0)))
            int_fast8_t tmp = *inPtr++ - 128; // I or Q

#if (AIRSPY_AGC_ENABLE > 0)
            int_fast8_t absTmp = abs(tmp);

            // catch maximum value (used to avoid overflow)
            if (absTmp > maxVal)
            {
                maxVal = absTmp;
            }

            // calculate signal level (rectifier, fast attack slow release)
            float c = LEV_CREL;
            if (absTmp > agcLev)
            {
                c = LEV_CATT;
            }
            agcLev = c * absTmp + agcLev - c * agcLev;
#endif  // (AIRSPY_AGC_ENABLE > 0)

#if (AIRSPY_DOC_ENABLE > 0)
            // subtract DC
            if (k & 0x1)
            {   // Q
                sumQ += tmp;
                *outPtr++ = float(tmp) - dcQ;
            }
            else
            {  // I
                sumI += tmp;
                *outPtr++ = float(tmp) - dcI;
            }
#else
            *outPtr++ = float(tmp);
#endif  // AIRSPY_DOC_ENABLE
#endif  // ((AIRSPY_DOC_ENABLE == 0) && ((AIRSPY_AGC_ENABLE == 0)))
        }
        inputBuffer.head = (inputBuffer.head + len*sizeof(float));
    }
    else
    {
        Q_ASSERT(sizeof(float) == 4);
        uint64_t samplesTillEnd = bytesTillEnd >> 2; // / sizeof(float);
        Q_ASSERT((samplesTillEnd & 0x01) == 0);      // even number of samples

        float * outPtr = (float *)(inputBuffer.buffer + inputBuffer.head);
        for (uint64_t k=0; k<samplesTillEnd/2; ++k)
        {   // convert to float
#if ((AIRSPY_DOC_ENABLE == 0) && ((AIRSPY_AGC_ENABLE == 0)))
            *outPtr = float(0.5 * inPtr[4*k]);
            *outPtr++ += float(0.5 * inPtr[4*k+2]);
            *outPtr = float(0.5 * inPtr[4*k+1]);
            *outPtr++ += float(0.5 * inPtr[4*k+3]);

#else // ((AIRSPY_DOC_ENABLE == 0) && ((AIRSPY_AGC_ENABLE == 0)))
            int_fast8_t tmp = *inPtr++ - 128; // I or Q

#if (AIRSPY_AGC_ENABLE > 0)
            int_fast8_t absTmp = abs(tmp);

            // catch maximum value (used to avoid overflow)
            if (absTmp > maxVal)
            {
                maxVal = absTmp;
            }

            // calculate signal level (rectifier, fast attack slow release)
            float c = LEV_CREL;
            if (absTmp > agcLev)
            {
                c = LEV_CATT;
            }
            agcLev = c * absTmp + agcLev - c * agcLev;
#endif  // (AIRSPY_AGC_ENABLE > 0)

#if (AIRSPY_DOC_ENABLE > 0)
            // subtract DC
            if (k & 0x1)
            {   // Q
                sumQ += tmp;
                *outPtr++ = float(tmp) - dcQ;
            }
            else
            {  // I
                sumI += tmp;
                *outPtr++ = float(tmp) - dcI;
            }
#else
            *outPtr++ = float(tmp);
#endif  // AIRSPY_DOC_ENABLE
#endif  // ((AIRSPY_DOC_ENABLE == 0) && ((AIRSPY_AGC_ENABLE == 0)))
        }

        outPtr = (float *)(inputBuffer.buffer);
        for (uint64_t k=0; k<(len-samplesTillEnd)/2; ++k)
        {   // convert to float
#if ((AIRSPY_DOC_ENABLE == 0) && ((AIRSPY_AGC_ENABLE == 0)))
            *outPtr = float(0.5 * inPtr[4*k]);
            *outPtr++ += float(0.5 * inPtr[4*k+2]);
            *outPtr = float(0.5 * inPtr[4*k+1]);
            *outPtr++ += float(0.5 * inPtr[4*k+3]);

#else // ((AIRSPY_DOC_ENABLE == 0) && ((AIRSPY_AGC_ENABLE == 0)))
            int_fast8_t tmp = *inPtr++ - 128; // I or Q

#if (AIRSPY_AGC_ENABLE > 0)
            int_fast8_t absTmp = abs(tmp);

            // catch maximum value (used to avoid overflow)
            if (absTmp > maxVal)
            {
                maxVal = absTmp;
            }

            // calculate signal level (rectifier, fast attack slow release)
            float c = LEV_CREL;
            if (absTmp > agcLev)
            {
                c = LEV_CATT;
            }
            agcLev = c * absTmp + agcLev - c * agcLev;
#endif  // (AIRSPY_AGC_ENABLE > 0)

#if (AIRSPY_DOC_ENABLE > 0)
            // subtract DC
            if (k & 0x1)
            {   // Q
                sumQ += tmp;
                *outPtr++ = float(tmp) - dcQ;
            }
            else
            {  // I
                sumI += tmp;
                *outPtr++ = float(tmp) - dcI;
            }
#else
            *outPtr++ = float(tmp);
#endif  // AIRSPY_DOC_ENABLE
#endif  // ((AIRSPY_DOC_ENABLE == 0) && ((AIRSPY_AGC_ENABLE == 0)))
        }
        inputBuffer.head = (len-samplesTillEnd)*sizeof(float);
    }

#if (AIRSPY_DOC_ENABLE > 0)
    // calculate correction values for next input buffer
    airspyWorker->dcI = sumI * DC_C / (len >> 1) + dcI - DC_C * dcI;
    airspyWorker->dcQ = sumQ * DC_C / (len >> 1) + dcQ - DC_C * dcQ;
#endif

#if (AIRSPY_AGC_ENABLE > 0)
    // store memory
    airspyWorker->agcLev = agcLev;

    airspyWorker->emitAgcLevel(agcLev, maxVal);
#endif

    pthread_mutex_lock(&inputBuffer.countMutex);
    inputBuffer.count = inputBuffer.count + len*sizeof(float);
    pthread_cond_signal(&inputBuffer.countCondition);
    pthread_mutex_unlock(&inputBuffer.countMutex);

    return 0;
}
#endif

AirspyDSFilter::AirspyDSFilter()
{
#if AIRSPY_FILTER_IQ_INTERLEAVED
    buffer = new float[4*taps];  // I Q interleaved, double size for circular buffer
    bufferPtr = buffer;
#else
    bufferI = new float[2*taps];
    bufferQ = new float[2*taps];
    bufferPtrI = bufferI;
    bufferPtrQ = bufferQ;
#endif


}

AirspyDSFilter::~AirspyDSFilter()
{
#if AIRSPY_FILTER_IQ_INTERLEAVED
    delete [] buffer;
#else
    delete [] bufferI;
    delete [] bufferQ;
#endif
}

void AirspyDSFilter::reset()
{
#if AIRSPY_FILTER_IQ_INTERLEAVED
    bufferPtr = buffer;
    for (int n = 0; n < 4*taps; ++n)
    {
        buffer[n] = 0;
    }
#else
    bufferPtrI = bufferI;
    bufferPtrQ = bufferQ;
    for (int n = 0; n < 2*taps; ++n)
    {
        bufferI[n] = 0;
    }
    for (int n = 0; n < 2*taps; ++n)
    {
        bufferQ[n] = 0;
    }
#endif
}

void AirspyDSFilter::process(float *inDataIQ, float *outDataIQ, int numIQ, float & maxAbs2)
{

#if (AIRSPY_AGC_ENABLE > 0)
    maxAbs2 = 0;
#endif
#if AIRSPY_FILTER_IQ_INTERLEAVED
    //Q_ASSERT(numIQ > 4*len);

    //    if (0 != (uint64_t(inDataIQ) & 0x00F))
    //    {
    //        qDebug("Data not aligned %8.8X", inDataIQ);
    //    }

    // prolog
    std::memcpy(buffer+tapsX2-2, inDataIQ, (tapsX2+2)*sizeof(float));

    for (int n = 0; n<taps+1; n+=2)
    {
        float * fwd = buffer + 2*n;
        float * rev = buffer + 2*n + tapsX2 - 2 + 1;

        float accI = 0;
        float accQ = 0;

        for (int c = 0; c < (taps+1)/4; ++c)
        {
            accI += *fwd++ * coef[c];
            accQ += *rev-- * coef[c];
            accQ += *fwd++ * coef[c];
            accI += *rev-- * coef[c];

            fwd += 2;  // zero coe
            rev -= 2;  // zero coe
        }

        accI += *(fwd-2) * coef[(taps+1)/4];
        accQ += *(fwd-1) * coef[(taps+1)/4];

        *outDataIQ++ = accI;
        *outDataIQ++ = accQ;

#if (AIRSPY_AGC_ENABLE > 0)
        float abs2 = accI*accI + accQ*accQ;
        if (maxAbs2 < abs2)
        {
            maxAbs2 = abs2;
        }
#endif
    }

    // main loop
    for (int n = taps+1; n<numIQ; n+=2)
    {
        //float * fwd = inDataIQ + (lenX2+2) + 2*n;
        //float * rev = inDataIQ + (lenX2+2) + lenX2 - 2 + 2*n;
        float * fwd = inDataIQ + 2*n - (tapsX2 - 2);
        float * rev = inDataIQ + 2*n + 1;

        float accI = 0;
        float accQ = 0;

        for (int c = 0; c < (taps+1)/4; ++c)
        {
            accI += *fwd++ * coef[c];
            accQ += *rev-- * coef[c];
            accQ += *fwd++ * coef[c];
            accI += *rev-- * coef[c];

            fwd += 2;  // zero coe
            rev -= 2;  // zero coe
        }

        accI += *(fwd-2) * coef[(taps+1)/4];
        accQ += *(fwd-1) * coef[(taps+1)/4];

        *outDataIQ++ = accI;
        *outDataIQ++ = accQ;

#if (AIRSPY_AGC_ENABLE > 0)
        float abs2 = accI*accI + accQ*accQ;
        if (maxAbs2 < abs2)
        {
            maxAbs2 = abs2;
        }
#endif
    }

    // epilog
    std::memcpy(buffer, inDataIQ + (2*numIQ) - (tapsX2-2), (tapsX2-2)*sizeof(float));

#else
    for (int n = 0; n<numIQ/2; ++n)
    {
        float * fwdI = bufferPtrI;
        float * revI = bufferPtrI + taps;
        float * fwdQ = bufferPtrQ;
        float * revQ = bufferPtrQ + taps;


        *fwdI++ = *inDataIQ;
        *revI = *inDataIQ++;
        *fwdQ++ = *inDataIQ;
        *revQ = *inDataIQ++;

        float accI = 0;
        float accQ = 0;

        for (int c = 0; c < (taps+1)/4; ++c)
        {
            accI += (*fwdI + *revI)*coef[c];
            fwdI += 2;  // every other coeff is zero
            revI -= 2;  // every other coeff is zero
            accQ += (*fwdQ + *revQ)*coef[c];
            fwdQ += 2;  // every other coeff is zero
            revQ -= 2;  // every other coeff is zero
        }

        accI += *(fwdI-1) * coef[(taps+1)/4];
        accQ += *(fwdQ-1) * coef[(taps+1)/4];

        *outDataIQ++ = accI;
        *outDataIQ++ = accQ;

#if (AIRSPY_AGC_ENABLE > 0)
        float abs2 = accI*accI + accQ*accQ;
        if (maxAbs2 < abs2)
        {
            maxAbs2 = abs2;
        }
#endif

        bufferPtrQ += 1;
        if (++bufferPtrI == bufferI + taps)
        {
            bufferPtrI = bufferI;
            bufferPtrQ = bufferQ;
        }

        // insert new samples to delay line
        *(bufferPtrI + taps) = *inDataIQ;   // I
        *bufferPtrI++ = *inDataIQ++;       // I
        *(bufferPtrQ + taps) = *inDataIQ;   // I
        *bufferPtrQ++ = *inDataIQ++;       // I

        if (bufferPtrI == bufferI + taps)
        {
            bufferPtrI = bufferI;
            bufferPtrQ = bufferQ;
        }
    }

#endif
}
