import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import canarydashboard

ApplicationWindow {
    id: win
    width: 1480
    height: 960
    visible: true
    title: "Canary — Indoor Air Quality"
    color: "#11111b"

    MqttController {
        id: mqtt
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        /* ---------- Header ---------- */
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 64
            color: "#181825"
            border.color: "#313244"
            border.width: 0

            Rectangle {
                anchors.bottom: parent.bottom
                width: parent.width
                height: 1
                color: "#313244"
            }

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 24
                anchors.rightMargin: 24
                spacing: 12

                /* Small "logo" dot */
                Rectangle {
                    width: 10; height: 10
                    radius: 5
                    color: "#a6e3a1"
                }

                ColumnLayout {
                    spacing: 0
                    Text {
                        text: "CANARY"
                        color: "#cdd6f4"
                        font.pixelSize: 16
                        font.weight: Font.Bold
                        font.letterSpacing: 2.5
                    }
                    Text {
                        text: "Indoor Air Quality Monitor"
                        color: "#6c7086"
                        font.pixelSize: 11
                        font.letterSpacing: 0.3
                    }
                }

                Item { Layout.fillWidth: true }

                /* Connection pill */
                Rectangle {
                    Layout.preferredHeight: 28
                    Layout.preferredWidth: statusRow.implicitWidth + 24
                    radius: 14
                    color: mqtt.connected ? "#1e2e1e" : "#2e1e1e"
                    border.color: mqtt.connected ? "#a6e3a1" : "#f38ba8"
                    border.width: 1

                    RowLayout {
                        id: statusRow
                        anchors.centerIn: parent
                        spacing: 8
                        Rectangle {
                            width: 8; height: 8; radius: 4
                            color: mqtt.connected ? "#a6e3a1" : "#f38ba8"
                        }
                        Text {
                            text: mqtt.connected ? "Connected" : "Disconnected"
                            color: mqtt.connected ? "#a6e3a1" : "#f38ba8"
                            font.pixelSize: 11
                            font.weight: Font.DemiBold
                            font.letterSpacing: 0.8
                        }
                    }
                }
            }
        }

        /* ---------- Chart grid ----------
         * Four columns so each raw sensor sits next to its self-heating-corrected
         * counterpart. Pressure has no correction so it spans 2 to balance row 2.
         */
        GridLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.margins: 20
            columns: 4
            rowSpacing: 16
            columnSpacing: 16

            SensorChart {
                id: tempChart
                Layout.fillWidth: true
                Layout.fillHeight: true
                title: "Temperature"
                subtitle: "TMP119 • raw"
                unit: "°C"
                yMin: 15; yMax: 35
                valuePrecision: 2
                lineColor: "#f38ba8"
            }

            SensorChart {
                id: tempCorrectedChart
                Layout.fillWidth: true
                Layout.fillHeight: true
                title: "Temperature"
                subtitle: "TMP119 • ambient corrected"
                unit: "°C"
                yMin: 15; yMax: 35
                valuePrecision: 2
                lineColor: "#f38ba8"
            }

            SensorChart {
                id: tempSht45Chart
                Layout.fillWidth: true
                Layout.fillHeight: true
                title: "Temperature"
                subtitle: "SHT45 • raw"
                unit: "°C"
                yMin: 15; yMax: 35
                valuePrecision: 2
                lineColor: "#fab387"
            }

            SensorChart {
                id: tempSht45CorrectedChart
                Layout.fillWidth: true
                Layout.fillHeight: true
                title: "Temperature"
                subtitle: "SHT45 • ambient corrected"
                unit: "°C"
                yMin: 15; yMax: 35
                valuePrecision: 2
                lineColor: "#fab387"
            }

            SensorChart {
                id: humidityChart
                Layout.fillWidth: true
                Layout.fillHeight: true
                title: "Humidity"
                subtitle: "SHT45 • raw"
                unit: "%RH"
                yMin: 0; yMax: 100
                valuePrecision: 1
                lineColor: "#89dceb"
            }

            SensorChart {
                id: humidityCorrectedChart
                Layout.fillWidth: true
                Layout.fillHeight: true
                title: "Humidity"
                subtitle: "SHT45 • ambient corrected"
                unit: "%RH"
                yMin: 0; yMax: 100
                valuePrecision: 1
                lineColor: "#89dceb"
            }

            SensorChart {
                id: pressureChart
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.columnSpan: 2
                title: "Barometric Pressure"
                subtitle: "BMP388"
                unit: "kPa"
                yMin: 95; yMax: 105
                valuePrecision: 2
                lineColor: "#cba6f7"
            }

            SensorChart {
                id: vocChart
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.columnSpan: 4
                title: "Volatile Organic Compounds"
                subtitle: "SGP40 • Sensirion VOC index"
                unit: "index"
                yMin: 0; yMax: 500
                valuePrecision: 0
                lineColor: "#a6e3a1"
            }
        }
    }

    Connections {
        target: mqtt
        function onTempPoint(t, v)                 { tempChart.addPoint(t, v) }
        function onTempCorrectedPoint(t, v)        { tempCorrectedChart.addPoint(t, v) }
        function onTempSht45Point(t, v)            { tempSht45Chart.addPoint(t, v) }
        function onTempSht45CorrectedPoint(t, v)   { tempSht45CorrectedChart.addPoint(t, v) }
        function onHumidityPoint(t, v)             { humidityChart.addPoint(t, v) }
        function onHumidityCorrectedPoint(t, v)    { humidityCorrectedChart.addPoint(t, v) }
        function onPressurePoint(t, v)             { pressureChart.addPoint(t, v) }
        function onVocPoint(t, v)                  { vocChart.addPoint(t, v) }
    }
}
