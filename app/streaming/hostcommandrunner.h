#pragma once

#include <QObject>
#include <QString>

#include <functional>

class NvComputer;

// Runs Foundation Sunshine super commands (NvHTTP::sendSuperCmd) on a worker
// thread, one at a time, so the HTTPS request never blocks the stream loop.
// The worker only sees a snapshot of the connection details, never the
// session or the NvComputer (same pattern as the runtime bitrate worker).
class HostCommandRunner : public QObject
{
    Q_OBJECT

public:
    // accepted: the host answered <supercmd> != 0. error: why the request
    // failed, empty when it reached the host.
    using Callback = std::function<void(bool accepted, const QString& error)>;

    explicit HostCommandRunner(QObject* parent = nullptr);

    // Starts the request. Returns false while another one is running. The
    // callback runs on this object's thread, and not at all if this object
    // is destroyed first.
    bool run(NvComputer* computer, const QString& commandId, Callback callback);

    // The result arrives as a queued signal, so the session must keep
    // pumping Qt events while this is true.
    bool isBusy() const { return m_Busy; }

private:
    bool m_Busy = false;
};
