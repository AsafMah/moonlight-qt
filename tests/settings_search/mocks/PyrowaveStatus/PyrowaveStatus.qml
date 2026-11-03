pragma Singleton
import QtQuick
QtObject {
    enum GpuState { GpuCheckNotRun, GpuCheckRunning, GpuCheckPassed, GpuCheckFailed, GpuCheckUnavailable }
    enum HostOffer { HostOfferUnknown, HostOfferOffered, HostOfferNotOffered }
    property int gpuCheckState: PyrowaveStatus.GpuCheckPassed
    property bool decoderImplemented: true
    property bool hasLastHost: false
    property int lastHostOffer: PyrowaveStatus.HostOfferUnknown
    property string lastHostName: ""
    property string lastHostSeen: ""
    property string gpuCheckSummary: "Fake GPU check passed"
    property string capabilityReport: "Fake capability report"
    property bool hasLastPreflight: false
    property string lastPreflightText: ""
    property bool lastPreflightOk: true
    property int lowBitrateThresholdKbps: 50000
    property int recommendedBitrateKbps: 100000
    function refreshHostStatus() {}
    function runCapabilityCheck() {}
}
