#include "pyrowavestatus.h"
#include "pyrowavecapabilities.h"
#include "pyrowavehoststore.h"
#include "pyrowaveruntime.h"
#include "pyrowavevideodecoder.h"

#include <QLocale>
#include <QMetaObject>
#include <QThreadPool>

PyrowaveStatus* PyrowaveStatus::get()
{
    // Never deleted, so a capability check that finishes during shutdown
    // still has a live object to report to.
    static PyrowaveStatus* s_Instance = new PyrowaveStatus();
    return s_Instance;
}

PyrowaveStatus::PyrowaveStatus(QObject* parent)
    : QObject(parent), m_HasLastHost(false), m_LastHostOffer(HostOfferUnknown),
      m_GpuCheckState(GpuCheckNotRun)
{
    refreshHostStatus();
}

bool PyrowaveStatus::runtimeAvailable() const
{
    return PyrowaveRuntime::get().isAvailable();
}

QString PyrowaveStatus::runtimeMessage() const
{
    return PyrowaveRuntime::get().userMessage();
}

bool PyrowaveStatus::decoderImplemented() const
{
    return PyrowaveVideoDecoder::k_DecoderImplemented;
}

void PyrowaveStatus::refreshHostStatus()
{
    const PyrowaveHostStore::Entry entry = PyrowaveHostStore::lastConnectedHost();

    m_HasLastHost = entry.valid;
    m_LastHostName = entry.name;
    m_LastHostOffer = static_cast<int>(entry.offer);
    m_LastHostSeen.clear();
    if (entry.lastConnected.isValid()) {
        const QDateTime local = entry.lastConnected.toLocalTime();
        m_LastHostSeen = QLocale().toString(local, QLocale::ShortFormat);
    }

    emit hostStatusChanged();
}

void PyrowaveStatus::runCapabilityCheck()
{
    if (m_GpuCheckState == GpuCheckRunning) {
        return;
    }

    m_GpuCheckState = GpuCheckRunning;
    m_GpuCheckSummary = tr("Checking Vulkan capabilities...");
    emit gpuCheckChanged();

    QThreadPool::globalInstance()->start([this]() {
        const PyrowaveCapabilityReport report = probePyrowaveCapabilities();

        // Translate and format on the worker; only plain values cross back.
        // A failure to read the GPUs is reported apart from a missing feature.
        int state = GpuCheckUnavailable;
        if (report.failure == PyrowaveCapabilityReport::Failure::None) {
            state = report.anyDeviceMeetsPrerequisites() ? GpuCheckPassed : GpuCheckFailed;
        }
        const QString summary = report.summary();
        const QString text = report.toText();

        auto publish = [this, state, summary, text]() {
            m_GpuCheckState = state;
            m_GpuCheckSummary = summary;
            m_CapabilityReport = text;
            emit gpuCheckChanged();
        };
        QMetaObject::invokeMethod(this, publish, Qt::QueuedConnection);
    });
}
