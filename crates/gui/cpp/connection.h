#pragma once
#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <functional>
#include <unordered_map>

namespace cv {

using Callback = std::function<void(bool ok, const QJsonValue &data, const QString &error)>;

// One daemon: the local one, or a remote one reached via `ssh <target> codeviewd bridge`.
// Request/response bookkeeping happens here on the GUI thread; the transport is in Rust.
class Connection
{
public:
    static constexpr const char *kLocalId = "local";

    // `sshTarget` empty means the local daemon. The id must be unique.
    Connection(const QString &id, const QString &label, const QString &sshTarget);
    ~Connection();
    Connection(const Connection &) = delete;
    Connection &operator=(const Connection &) = delete;

    static Connection *find(const QString &id);

    QString id() const { return m_id; }
    QString label() const { return m_label; }
    QString sshTarget() const { return m_sshTarget; }
    bool isLocal() const { return m_sshTarget.isEmpty(); }
    bool connected() const { return m_connected; }
    QString detail() const { return m_detail; } // last disconnect reason
    QString hostname() const { return m_hostname.isEmpty() ? m_label : m_hostname; }
    QString home() const { return m_home; }

    // (Re)starts the transport; it reconnects by itself after failures.
    void open();

    // Sends {"op": op, ...args}; `cb` runs on the GUI thread with the response.
    void request(const QString &op, QJsonObject args = {}, Callback cb = {});

    std::function<void(const QJsonObject &event)> onEvent;
    std::function<void()> onStateChanged;

    void handleLine(const QByteArray &line);
    void handleConnection(bool connected, const QString &detail);

private:
    QString m_id, m_label, m_sshTarget;
    QString m_hostname, m_home, m_detail;
    quint64 m_nextId = 1;
    bool m_connected = false;
    std::unordered_map<quint64, Callback> m_pending;
};

} // namespace cv
