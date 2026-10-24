#include "pyrowavestatus.h"
#include "pyrowavecapabilities.h"
#include "pyrowavehoststore.h"
#include "pyrowavepreflightstore.h"
#include "pyrowaveruntime.h"
#include "pyrowavevideodecoder.h"

#include <QLocale>
#include <QMetaObject>
#include <QRunnable>
#include <QThreadPool>

namespace {

// Runs the capability check on a pool thread and hands plain values back to
// the GUI thread through a queued call.
class CapabilityCheckTask : public QRunnable
{
public:
    explicit CapabilityCheckTask(PyrowaveStatus* status) : m_Status(status) {}

    void run() override
    {
        const PyrowaveCapabilityReport report = probePyrowaveCapabilities();

        // Translate and format on the worker; only plain values cross back.
        // A failure to read the GPUs is reported apart from a missing feature.
        int state = PyrowaveStatus::GpuCheckUnavailable;
        if (report.failure == PyrowaveCapabilityReport::Failure::None) {
            state = report.anyDeviceMeetsPrerequisites() ? PyrowaveStatus::GpuCheckPassed
                                                         : PyrowaveStatus::GpuCheckFailed;
        }

        QMetaObject::invokeMethod(m_Status, "publishGpuCheck", Qt::QueuedConnection,
                                  Q_ARG(int, state), Q_ARG(QString, report.summary()),
                                  Q_ARG(QString, report.toText()));
    }

private:
    PyrowaveStatus* m_Status;
};

}

PyrowaveStatus* PyrowaveStatus::get()
{
    // Never deleted, so a capability check that finishes during shutdown
    // still has a live object to report to.
    static PyrowaveStatus* s_Instance = new PyrowaveStatus();
    return s_Instance;
}

PyrowaveStatus::PyrowaveStatus(QObject* parent)
    : QObject(parent), m_HasLastHost(false), m_LastHostOffer(HostOfferUnknown),
      m_HasLastPreflight(false), m_LastPreflightOk(false), m_GpuCheckState(GpuCheckNotRun)
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

    const PyrowavePreflightStore::Entry preflight = PyrowavePreflightStore::last();
    m_HasLastPreflight = preflight.valid;
    m_LastPreflightOk = preflight.ok;
    m_LastPreflightText.clear();
    if (preflight.valid) {
        const QString when =
            QLocale().toString(preflight.time.toLocalTime(), QLocale::ShortFormat);
        const QString signal = preflight.hdrMode == 2   ? QStringLiteral("HLG")
                               : preflight.hdrMode == 1 ? QStringLiteral("HDR10")
                                                        : QStringLiteral("SDR");
        const QString stream = tr("%1x%2 at %3 FPS, %4")
                                   .arg(preflight.width)
                                   .arg(preflight.height)
                                   .arg(preflight.frameRate)
                                   .arg(signal);
        if (preflight.ok) {
            m_LastPreflightText = tr("Last stream check (%1): passed on %2 with %3 decoding (%4).")
                                      .arg(when, preflight.gpu, preflight.decodePath, stream);
        } else if (!preflight.gpu.isEmpty()) {
            m_LastPreflightText = tr("Last stream check (%1): failed on %2 (%3). %4")
                                      .arg(when, preflight.gpu, stream, preflight.reason);
        } else {
            m_LastPreflightText = tr("Last stream check (%1): failed (%2). %3")
                                      .arg(when, stream, preflight.reason);
        }
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

    // A QRunnable subclass rather than a lambda: QThreadPool::start() only
    // takes a functor from Qt 5.15, and the Steam Link build uses older Qt.
    QThreadPool::globalInstance()->start(new CapabilityCheckTask(this));
}

void PyrowaveStatus::publishGpuCheck(int state, const QString& summary, const QString& report)
{
    m_GpuCheckState = state;
    m_GpuCheckSummary = summary;
    m_CapabilityReport = report;
    emit gpuCheckChanged();
}
