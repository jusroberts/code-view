#pragma once
#include <KTextEditor/CodeCompletionModel>
#include <KTextEditor/TextHintInterface>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QPointer>
#include <QRect>
#include <functional>
#include <map>

namespace KTextEditor {
class Document;
class MovingRange;
class View;
}
class QTimer;

namespace cv {

class Connection;
class LspManager;
struct Language;

struct MissingServer {
    QString language;       // e.g. "dart"
    QStringList servers;    // commands Code View looks for
    QString installCommand; // a command that installs one on that host, if known
    QString connId;
    QString extension;
};

// One language server running in a daemon (local or remote). Speaks JSON-RPC through
// lsp_send requests; replies come back as lsp_message events.
class LspServer
{
public:
    using Callback = std::function<void(const QJsonValue &result, const QJsonObject &error)>;

    LspServer(LspManager *manager, const QString &connId, quint64 id, const QString &root, const QString &name);

    void initialize();
    void request(const QString &method, const QJsonObject &params, Callback cb);
    void notify(const QString &method, const QJsonObject &params);
    void handle(const QJsonObject &msg);
    bool ready() const { return m_ready; }
    bool failed() const { return m_failed; }
    QString name() const { return m_name; }
    QString connId() const { return m_connId; }
    quint64 id() const { return m_id; }

private:
    void send(const QJsonObject &msg);

    LspManager *m_manager;
    QString m_connId;
    quint64 m_id;
    QString m_root;
    QString m_name;
    bool m_ready = false;
    bool m_failed = false;
    QList<QJsonObject> m_queued; // sent once initialized
    qint64 m_nextId = 1;
    std::map<qint64, Callback> m_pending;
};

// Connects open editor documents to language servers: completion, hover,
// diagnostics and go-to-definition.
class LspManager : public KTextEditor::CodeCompletionModel, public KTextEditor::TextHintProvider
{
public:
    explicit LspManager(QObject *parent = nullptr);
    ~LspManager() override;

    // `path` is the file's path on the host `conn` refers to.
    void attachDocument(KTextEditor::Document *doc, Connection *conn, const QString &path);
    void attachView(KTextEditor::View *view);
    void gotoDefinition(KTextEditor::View *view);
    bool hasServer(KTextEditor::Document *doc); // a language server is ready for this document
    // True while hover info is showing for the symbol under `pos` in `w` (a view's text area).
    bool hoverShowingAt(QWidget *w, const QPoint &pos) const;
    // One line for the status bar: which server serves the document, or why none does.
    QString describe(KTextEditor::Document *doc);
    // True if the document's language has no server on its host.
    bool missingServer(KTextEditor::Document *doc, MissingServer *out);
    void retry(KTextEditor::Document *doc); // look for a server again (after installing one)

    // From the daemon.
    void handleMessage(Connection *conn, quint64 server, const QJsonObject &msg);
    void handleExit(Connection *conn, quint64 server, const QString &error);
    void connectionLost(Connection *conn);
    void reconnected(Connection *conn); // restart servers for that host's open documents

    std::function<void(Connection *conn, const QString &path, int line, int column)> onOpenLocation;
    std::function<void(const QString &message)> onStatus;
    std::function<void()> onStateChanged; // describe()/hasServer() may have changed

    // CodeCompletionModel
    QVariant data(const QModelIndex &index, int role) const override;
    void completionInvoked(KTextEditor::View *view, const KTextEditor::Range &range, InvocationType type) override;
    void executeCompletionItem(KTextEditor::View *view, const KTextEditor::Range &word, const QModelIndex &index) const override;

    // TextHintProvider
    QString textHint(KTextEditor::View *view, const KTextEditor::Cursor &position) override;

private:
    friend class LspServer;
    struct Diagnostic {
        KTextEditor::MovingRange *range;
        QString message;
        int severity;
    };
    struct Doc {
        QPointer<KTextEditor::Document> doc;
        QString connId;
        QString path;
        QString uri;
        QString languageId;
        LspServer *server = nullptr;
        QString problem; // why there's no server
        QString installCommand;
        int version = 0;
        bool dirty = false;
        QTimer *debounce = nullptr;
        QList<Diagnostic> diagnostics;
    };
    struct Item {
        QString label;
        QString detail;
        QString insert;
        int kind = 0;
        KTextEditor::Range replace = KTextEditor::Range::invalid(); // from the item's textEdit
    };

    Doc *docFor(KTextEditor::Document *doc);
    void start(Doc *d);
    void opened(Doc *d);
    void flush(Doc *d);
    void clearDiagnostics(Doc *d);
    void publishDiagnostics(const QString &uri, const QJsonArray &diagnostics);
    QJsonObject position(Doc *d, const KTextEditor::Cursor &c);
    void status(const QString &message);
    void stateChanged();
    void findInstallCommand(Doc *d, const Language *lang);

    QHash<KTextEditor::Document *, Doc *> m_docs;
    QHash<QString, LspServer *> m_servers; // connId + '/' + server id
    QSet<QString> m_reported;              // "no server" messages already shown
    QList<Item> m_items;
    quint64 m_completionGen = 0;
    quint64 m_hoverGen = 0;
    QPointer<KTextEditor::View> m_hoverView; // where the hover tooltip is showing
    KTextEditor::Range m_hoverRange = KTextEditor::Range::invalid();
    QPointer<QWidget> m_hoverWidget; // the view's text area
    QRect m_hoverArea;               // the symbol, in m_hoverWidget coordinates
};

} // namespace cv
