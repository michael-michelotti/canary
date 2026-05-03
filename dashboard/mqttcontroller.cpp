#include "mqttcontroller.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QDebug>

MqttController::MqttController(QObject *parent) : QObject(parent) {
    m_client = new QMqttClient(this);
    m_client->setHostname("192.168.1.180");
    m_client->setPort(1883);

    connect(m_client, &QMqttClient::connected,    this, &MqttController::onConnected);
    connect(m_client, &QMqttClient::connected,    this, &MqttController::connectedChanged);
    connect(m_client, &QMqttClient::disconnected, this, &MqttController::connectedChanged);
    connect(m_client, &QMqttClient::messageReceived,
            this, &MqttController::onMessageReceived);

    m_timer.start();
    m_client->connectToHost();
}

bool MqttController::isConnected() const {
    return m_client->state() == QMqttClient::Connected;
}

void MqttController::onConnected() {
    qDebug() << "connected, subscribing to canary/#";
    m_client->subscribe(QMqttTopicFilter("canary/#"));
}

void MqttController::onMessageReceived(const QByteArray &msg, const QMqttTopicName &topic) {
    qDebug().noquote() << "rx" << topic.name() << msg;

    const QJsonDocument doc = QJsonDocument::fromJson(msg);
    if (!doc.isObject()) {
        qWarning() << "not a JSON object:" << msg;
        return;
    }

    const double value = doc.object().value(QStringLiteral("value")).toDouble();
    const double t     = m_timer.elapsed() / 1000.0;
    const QString name = topic.name();

    if      (name == QStringLiteral("canary/temp"))                 emit tempPoint(t, value);
    else if (name == QStringLiteral("canary/temp_corrected"))       emit tempCorrectedPoint(t, value);
    else if (name == QStringLiteral("canary/temp_sht45"))           emit tempSht45Point(t, value);
    else if (name == QStringLiteral("canary/temp_sht45_corrected")) emit tempSht45CorrectedPoint(t, value);
    else if (name == QStringLiteral("canary/humidity"))             emit humidityPoint(t, value);
    else if (name == QStringLiteral("canary/humidity_corrected"))   emit humidityCorrectedPoint(t, value);
    else if (name == QStringLiteral("canary/pressure"))             emit pressurePoint(t, value);
    else if (name == QStringLiteral("canary/voc"))                  emit vocPoint(t, value);
}
