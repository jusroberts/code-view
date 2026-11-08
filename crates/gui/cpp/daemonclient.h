#pragma once
#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <functional>
#include <unordered_map>

namespace cv {

using Callback = std::function<void(bool ok, const QJsonValue &data, const QString &error)>;

// Request/response bookkeeping on the GUI thread. The socket itself is owned by Rust.
class DaemonClient
{
public:
    static DaemonClient &instance();

    // Sends {"op": op, ...args}; `cb` runs on the GUI thread with the response.
    void request(const QString &op, QJsonObject args = {}, Callback cb = {});
    bool connected() const { return m_connected; }

    std::function<void(const QJsonObject &event)> onEvent;
    std::function<void(bool connected, const QString &detail)> onConnectionChanged;

    void handleLine(const QByteArray &line);
    void handleConnection(bool connected, const QString &detail);

private:
    quint64 m_nextId = 1;
    bool m_connected = false;
    std::unordered_map<quint64, Callback> m_pending;
};

} // namespace cv
