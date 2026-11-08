#include "daemonclient.h"
#include "codeview/src/main.rs.h"

#include <QJsonDocument>
#include <QTimer>

namespace cv {

DaemonClient &DaemonClient::instance()
{
    static DaemonClient client;
    return client;
}

void DaemonClient::request(const QString &op, QJsonObject args, Callback cb)
{
    if (!m_connected) {
        if (cb) QTimer::singleShot(0, [cb] { cb(false, {}, QStringLiteral("Not connected to codeviewd")); });
        return;
    }
    const quint64 id = m_nextId++;
    args.insert(QStringLiteral("op"), op);
    const QJsonObject msg{{QStringLiteral("id"), static_cast<qint64>(id)}, {QStringLiteral("request"), args}};
    const QByteArray json = QJsonDocument(msg).toJson(QJsonDocument::Compact);
    if (cb) m_pending.emplace(id, std::move(cb));
    client_send(rust::Str(json.constData(), static_cast<size_t>(json.size())));
}

void DaemonClient::handleLine(const QByteArray &line)
{
    const QJsonObject msg = QJsonDocument::fromJson(line).object();
    const QString kind = msg.value(QStringLiteral("kind")).toString();
    if (kind == QLatin1String("response")) {
        const auto it = m_pending.find(static_cast<quint64>(msg.value(QStringLiteral("id")).toInteger()));
        if (it == m_pending.end()) return;
        Callback cb = std::move(it->second);
        m_pending.erase(it);
        cb(msg.value(QStringLiteral("ok")).toBool(), msg.value(QStringLiteral("data")),
           msg.value(QStringLiteral("error")).toString());
    } else if (kind == QLatin1String("event") && onEvent) {
        onEvent(msg.value(QStringLiteral("event")).toObject());
    }
}

void DaemonClient::handleConnection(bool connected, const QString &detail)
{
    if (m_connected && !connected) {
        auto pending = std::move(m_pending);
        m_pending.clear();
        for (auto &[id, cb] : pending) cb(false, {}, QStringLiteral("Lost connection to codeviewd"));
    }
    const bool changed = m_connected != connected;
    m_connected = connected;
    if (onConnectionChanged && (changed || !connected)) onConnectionChanged(connected, detail);
}

} // namespace cv
