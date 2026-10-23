#pragma once

#include <QObject>
#include <QString>

// PyroWave state for the settings page (QML singleton "PyrowaveStatus").
// Everything here is read-only: it never changes preferences or the codec
// negotiation.
class PyrowaveStatus : public QObject
{
    Q_OBJECT

public:
    enum HostOffer
    {
        HostOfferUnknown = 0,
        HostOfferOffered = 1,
        HostOfferNotOffered = 2,
    };
    Q_ENUM(HostOffer)

    enum GpuCheckState
    {
        GpuCheckNotRun,
        GpuCheckRunning,
        GpuCheckPassed,
        GpuCheckFailed,
        // The check couldn't read the GPUs, which is not the same as a GPU
        // that lacks a feature.
        GpuCheckUnavailable,
    };
    Q_ENUM(GpuCheckState)

    // The codec choice is shown only when the runtime loaded.
    Q_PROPERTY(bool runtimeAvailable READ runtimeAvailable CONSTANT)
    Q_PROPERTY(QString runtimeMessage READ runtimeMessage CONSTANT)
    Q_PROPERTY(bool decoderImplemented READ decoderImplemented CONSTANT)

    Q_PROPERTY(bool hasLastHost READ hasLastHost NOTIFY hostStatusChanged)
    Q_PROPERTY(QString lastHostName READ lastHostName NOTIFY hostStatusChanged)
    Q_PROPERTY(int lastHostOffer READ lastHostOffer NOTIFY hostStatusChanged)
    Q_PROPERTY(QString lastHostSeen READ lastHostSeen NOTIFY hostStatusChanged)

    Q_PROPERTY(int gpuCheckState READ gpuCheckState NOTIFY gpuCheckChanged)
    Q_PROPERTY(QString gpuCheckSummary READ gpuCheckSummary NOTIFY gpuCheckChanged)
    Q_PROPERTY(QString capabilityReport READ capabilityReport NOTIFY gpuCheckChanged)

    // Bitrate guidance from the roadmap: below the threshold, offer the
    // recommended value but never apply it automatically.
    Q_PROPERTY(int lowBitrateThresholdKbps READ lowBitrateThresholdKbps CONSTANT)
    Q_PROPERTY(int recommendedBitrateKbps READ recommendedBitrateKbps CONSTANT)

    static PyrowaveStatus* get();

    bool runtimeAvailable() const;
    QString runtimeMessage() const;
    bool decoderImplemented() const;

    bool hasLastHost() const { return m_HasLastHost; }
    QString lastHostName() const { return m_LastHostName; }
    int lastHostOffer() const { return m_LastHostOffer; }
    QString lastHostSeen() const { return m_LastHostSeen; }

    int gpuCheckState() const { return m_GpuCheckState; }
    QString gpuCheckSummary() const { return m_GpuCheckSummary; }
    QString capabilityReport() const { return m_CapabilityReport; }

    int lowBitrateThresholdKbps() const { return 150000; }
    int recommendedBitrateKbps() const { return 400000; }

    // Re-reads the stored host status (a stream may have updated it).
    Q_INVOKABLE void refreshHostStatus();

    // Runs the read-only Vulkan capability check on a worker thread. Results
    // arrive through gpuCheckChanged. Repeated calls while running are ignored.
    Q_INVOKABLE void runCapabilityCheck();

signals:
    void hostStatusChanged();
    void gpuCheckChanged();

private:
    explicit PyrowaveStatus(QObject* parent = nullptr);

    bool m_HasLastHost;
    QString m_LastHostName;
    int m_LastHostOffer;
    QString m_LastHostSeen;

    int m_GpuCheckState;
    QString m_GpuCheckSummary;
    QString m_CapabilityReport;
};
