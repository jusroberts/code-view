#include "connection.h"
#include "codeview/src/main.rs.h"

#include <QHash>
#include <QJsonDocument>
#include <QTimer>

namespace cv {

namespace {
QHash<QString, Connection *> &registry()
{
    static QHash<QString, Connection *> r;
    return r;
}
rust::Str str(const QByteArray &b) { return rust::Str(b.constData(), static_cast<size_t>(b.size())); }
} // namespace

Connection::Connection(const QString &id, const QString &label, const QString &sshTarget)
    : m_id(id)
    , m_label(label)
    , m_sshTarget(sshTarget)
{
    registry().insert(m_id, this);
}

Connection::~Connection()
{
    registry().remove(m_id);
    client_close(str(m_id.toUtf8()));
}

Connection *Connection::find(const QString &id) { return registry().value(id); }

void Connection::open()
{
    client_open(str(m_id.toUtf8()), str(m_sshTarget.toUtf8()));
}

void Connection::request(const QString &op, QJsonObject args, Callback cb)
{
    if (!m_connected) {
        if (cb) QTimer::singleShot(0, [cb, label = m_label] { cb(false, {}, QStringLiteral("Not connected to %1").arg(label)); });
        return;
    }
    const quint64 id = m_nextId++;
    args.insert(QStringLiteral("op"), op);
    const QJsonObject msg{{QStringLiteral("id"), static_cast<qint64>(id)}, {QStringLiteral("request"), args}};
    if (cb) m_pending.emplace(id, std::move(cb));
    client_send(str(m_id.toUtf8()), str(QJsonDocument(msg).toJson(QJsonDocument::Compact)));
}

void Connection::handleLine(const QByteArray &line)
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

void Connection::handleConnection(bool connected, const QString &detail)
{
    if (m_connected && !connected) {
        auto pending = std::move(m_pending);
        m_pending.clear();
        for (auto &[id, cb] : pending) cb(false, {}, QStringLiteral("Lost connection to %1").arg(m_label));
    }
    m_connected = connected;
    m_detail = detail;
    if (!connected) {
        if (onStateChanged) onStateChanged();
        return;
    }
    // Learn the host's name and home dir before announcing the connection.
    request(QStringLiteral("hello"), {}, [this](bool ok, const QJsonValue &data, const QString &) {
        if (ok) {
            m_hostname = data[QStringLiteral("hostname")].toString();
            m_home = data[QStringLiteral("home")].toString();
        }
        if (onStateChanged) onStateChanged();
    });
}

} // namespace cv
