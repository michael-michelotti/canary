import QtQuick
import QtQuick.Layouts
import QtGraphs

Rectangle {
    id: root

    property string title: ""
    property string subtitle: ""
    property string unit: ""
    property color  lineColor: "#89b4fa"
    property real   yMin: 0
    property real   yMax: 100
    property int    windowSec: 60
    property int    maxPoints: 300
    property int    valuePrecision: 1
    property real   currentValue: NaN

    color: "#1e1e2e"
    radius: 12
    border.color: "#313244"
    border.width: 1

    /* Colored accent bar down the left edge — ties the card to its line color. */
    Rectangle {
        id: accent
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        width: 3
        color: root.lineColor
        /* Only round the left corners to match the card radius. */
        radius: 0
        Rectangle {
            anchors.left: parent.left
            anchors.top: parent.top
            width: parent.width
            height: root.radius
            color: parent.color
        }
    }

    function addPoint(t, value) {
        series.append(t, value)
        if (series.count > maxPoints)
            series.remove(0)
        if (t > xAxis.max) {
            xAxis.max = t
            xAxis.min = Math.max(0, t - windowSec)
        }
        currentValue = value
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.leftMargin: 20
        anchors.rightMargin: 16
        anchors.topMargin: 14
        anchors.bottomMargin: 14
        spacing: 10

        RowLayout {
            Layout.fillWidth: true
            spacing: 12

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 2
                Text {
                    text: root.title.toUpperCase()
                    color: "#cdd6f4"
                    font.pixelSize: 12
                    font.weight: Font.DemiBold
                    font.letterSpacing: 1.2
                }
                Text {
                    text: root.subtitle
                    color: "#6c7086"
                    font.pixelSize: 11
                    visible: root.subtitle !== ""
                }
            }

            Row {
                spacing: 4
                Text {
                    text: isNaN(root.currentValue)
                        ? "—"
                        : root.currentValue.toFixed(root.valuePrecision)
                    color: root.lineColor
                    font.pixelSize: 30
                    font.weight: Font.Bold
                    font.family: "Segoe UI"
                }
                Text {
                    text: root.unit
                    color: "#6c7086"
                    font.pixelSize: 13
                    font.weight: Font.Medium
                    anchors.bottom: parent.bottom
                    anchors.bottomMargin: 5
                }
            }
        }

        GraphsView {
            Layout.fillWidth: true
            Layout.fillHeight: true

            axisX: ValueAxis {
                id: xAxis
                min: 0
                max: root.windowSec
            }
            axisY: ValueAxis {
                min: root.yMin
                max: root.yMax
            }

            LineSeries {
                id: series
                color: root.lineColor
                width: 2.5
            }
        }
    }
}
