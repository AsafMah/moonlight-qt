pragma Singleton
import QtQuick
QtObject {
    property var downloads: []
    function installUpdate(url) { downloads = downloads.concat([url]) }
}
