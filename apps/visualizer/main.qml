import QtQuick

Window {
    id: root

    title: "Autonomous Vehicle Visualizer"

    visible: true
    minimumWidth: 250
    minimumHeight: 250

    Viewport {
        id: viewport
        anchors.fill: parent
    }

    Timer {
        id: heartbeat_timer

        running: true
        repeat: true
        interval: 16

        onTriggered: VisualizerDriver.tick()
    }
}