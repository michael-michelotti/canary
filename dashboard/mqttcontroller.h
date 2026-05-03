#pragma once

#include <QObject>
#include <QElapsedTimer>
#include <QQmlEngine>
#include <QMqttClient>

class MqttController : public QObject {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(bool connected READ isConnected NOTIFY connectedChanged)

public:
    explicit MqttController(QObject *parent = nullptr);
    bool isConnected() const;

signals:
    void tempPoint(qreal t, qreal value);
    void tempCorrectedPoint(qreal t, qreal value);
    void tempSht45Point(qreal t, qreal value);
    void tempSht45CorrectedPoint(qreal t, qreal value);
    void humidityPoint(qreal t, qreal value);
    void humidityCorrectedPoint(qreal t, qreal value);
    void pressurePoint(qreal t, qreal value);
    void vocPoint(qreal t, qreal value);
    void connectedChanged();

private slots:
    void onConnected();
    void onMessageReceived(const QByteArray &msg, const QMqttTopicName &topic);

private:
    QMqttClient   *m_client;
    QElapsedTimer  m_timer;
};
