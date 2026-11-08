#include "lsp.h"
#include "connection.h"

#include <KTextEditor/Attribute>
#include <KTextEditor/Document>
#include <KTextEditor/MovingRange>
#include <KTextEditor/View>
#include <QCursor>
#include <QFileInfo>
#include <QIcon>
#include <QPointer>
#include <QRegularExpression>
#include <QTextDocument>
#include <QTimer>
#include <QToolTip>
#include <QUrl>

namespace cv {

namespace {

struct Language {
    QString id;
    QList<QStringList> candidates; // argv lists, first found on the host wins
};

// Language servers by file extension.
const Language *languageFor(const QString &path)
{
    static const QHash<QString, Language> byExt = [] {
        QHash<QString, Language> m;
        const Language rust{QStringLiteral("rust"), {{QStringLiteral("rust-analyzer")}}};
        const QList<QStringList> clangd{{QStringLiteral("clangd")}};
        const QList<QStringList> python{{QStringLiteral("pyright-langserver"), QStringLiteral("--stdio")},
                                        {QStringLiteral("basedpyright-langserver"), QStringLiteral("--stdio")},
                                        {QStringLiteral("pylsp")}};
        const QList<QStringList> ts{{QStringLiteral("typescript-language-server"), QStringLiteral("--stdio")}};
        m.insert(QStringLiteral("rs"), rust);
        for (const char *e : {"c", "h"}) m.insert(QLatin1String(e), {QStringLiteral("c"), clangd});
        for (const char *e : {"cpp", "cc", "cxx", "hpp", "hh", "hxx"}) m.insert(QLatin1String(e), {QStringLiteral("cpp"), clangd});
        m.insert(QStringLiteral("py"), {QStringLiteral("python"), python});
        m.insert(QStringLiteral("ts"), {QStringLiteral("typescript"), ts});
        m.insert(QStringLiteral("tsx"), {QStringLiteral("typescriptreact"), ts});
        for (const char *e : {"js", "mjs", "cjs"}) m.insert(QLatin1String(e), {QStringLiteral("javascript"), ts});
        m.insert(QStringLiteral("jsx"), {QStringLiteral("javascriptreact"), ts});
        m.insert(QStringLiteral("go"), {QStringLiteral("go"), {{QStringLiteral("gopls")}}});
        m.insert(QStringLiteral("kt"), {QStringLiteral("kotlin"), {{QStringLiteral("kotlin-language-server")}}});
        m.insert(QStringLiteral("lua"), {QStringLiteral("lua"), {{QStringLiteral("lua-language-server")}}});
        m.insert(QStringLiteral("zig"), {QStringLiteral("zig"), {{QStringLiteral("zls")}}});
        for (const char *e : {"sh", "bash"})
            m.insert(QLatin1String(e), {QStringLiteral("shellscript"), {{QStringLiteral("bash-language-server"), QStringLiteral("start")}}});
        return m;
    }();
    const auto it = byExt.find(QFileInfo(path).suffix().toLower());
    return it == byExt.end() ? nullptr : &it.value();
}

QString uriFor(const QString &path) { return QUrl::fromLocalFile(path).toString(QUrl::FullyEncoded); }
QString pathFor(const QString &uri) { return QUrl(uri).toLocalFile(); }

KTextEditor::Cursor cursorFrom(const QJsonValue &pos)
{
    return {pos[QStringLiteral("line")].toInt(), pos[QStringLiteral("character")].toInt()};
}

KTextEditor::Range rangeFrom(const QJsonValue &range)
{
    return {cursorFrom(range[QStringLiteral("start")]), cursorFrom(range[QStringLiteral("end")])};
}

// Completion/hover text may contain snippet syntax or markdown; keep it readable.
QString stripSnippet(QString s)
{
    static const QRegularExpression placeholder(QStringLiteral(R"(\$\{\d+:([^}]*)\})"));
    static const QRegularExpression tabstop(QStringLiteral(R"(\$\{?\d+\}?)"));
    s.replace(placeholder, QStringLiteral("\\1"));
    s.replace(tabstop, QString());
    return s;
}

QString hoverHtml(const QJsonValue &contents)
{
    QString md;
    auto add = [&md](const QJsonValue &v) {
        if (v.isString()) md += v.toString();
        else if (v.isObject() && v[QStringLiteral("language")].isString())
            md += QStringLiteral("```%1\n%2\n```").arg(v[QStringLiteral("language")].toString(), v[QStringLiteral("value")].toString());
        else md += v[QStringLiteral("value")].toString();
        md += QStringLiteral("\n\n");
    };
    if (contents.isArray()) for (const QJsonValue &v : contents.toArray()) add(v);
    else add(contents);
    md = md.trimmed();
    if (md.isEmpty()) return {};
    QTextDocument doc;
    doc.setMarkdown(md);
    return doc.toHtml();
}

QIcon kindIcon(int kind)
{
    // LSP CompletionItemKind
    switch (kind) {
    case 2: case 3: case 4: return QIcon::fromTheme(QStringLiteral("code-function"));
    case 5: case 10: return QIcon::fromTheme(QStringLiteral("code-variable"));
    case 6: case 12: case 21: return QIcon::fromTheme(QStringLiteral("code-variable"));
    case 7: case 8: case 22: case 25: return QIcon::fromTheme(QStringLiteral("code-class"));
    case 9: return QIcon::fromTheme(QStringLiteral("code-block"));
    case 13: case 20: return QIcon::fromTheme(QStringLiteral("code-typedef"));
    case 14: return QIcon::fromTheme(QStringLiteral("code-context"));
    default: return {};
    }
}

} // namespace

// ---- LspServer ----

LspServer::LspServer(LspManager *manager, const QString &connId, quint64 id, const QString &root, const QString &name)
    : m_manager(manager)
    , m_connId(connId)
    , m_id(id)
    , m_root(root)
    , m_name(name)
{
}

void LspServer::send(const QJsonObject &msg)
{
    Connection *conn = Connection::find(m_connId);
    if (!conn) return;
    conn->request(QStringLiteral("lsp_send"), {{QStringLiteral("server"), static_cast<qint64>(m_id)}, {QStringLiteral("message"), msg}});
}

void LspServer::initialize()
{
    const QJsonObject capabilities{
        {QStringLiteral("textDocument"),
         QJsonObject{
             {QStringLiteral("synchronization"), QJsonObject{{QStringLiteral("didSave"), false}}},
             {QStringLiteral("completion"), QJsonObject{{QStringLiteral("completionItem"), QJsonObject{{QStringLiteral("snippetSupport"), false}}}}},
             {QStringLiteral("hover"), QJsonObject{{QStringLiteral("contentFormat"), QJsonArray{QStringLiteral("markdown"), QStringLiteral("plaintext")}}}},
             {QStringLiteral("definition"), QJsonObject{{QStringLiteral("linkSupport"), true}}},
             {QStringLiteral("publishDiagnostics"), QJsonObject{}},
         }},
        {QStringLiteral("workspace"), QJsonObject{{QStringLiteral("configuration"), true}, {QStringLiteral("workspaceFolders"), true}}},
    };
    const QJsonObject params{
        {QStringLiteral("processId"), QJsonValue::Null},
        {QStringLiteral("clientInfo"), QJsonObject{{QStringLiteral("name"), QStringLiteral("Code View")}}},
        {QStringLiteral("rootUri"), uriFor(m_root)},
        {QStringLiteral("workspaceFolders"), QJsonArray{QJsonObject{{QStringLiteral("uri"), uriFor(m_root)}, {QStringLiteral("name"), QFileInfo(m_root).fileName()}}}},
        {QStringLiteral("capabilities"), capabilities},
    };
    // Sent directly: everything else waits in m_queued until the server is initialized.
    const qint64 id = m_nextId++;
    m_pending[id] = [this](const QJsonValue &, const QJsonObject &error) {
        if (!error.isEmpty()) {
            m_manager->status(QStringLiteral("%1 failed to start: %2").arg(m_name, error[QStringLiteral("message")].toString()));
            return;
        }
        send({{QStringLiteral("jsonrpc"), QStringLiteral("2.0")}, {QStringLiteral("method"), QStringLiteral("initialized")},
              {QStringLiteral("params"), QJsonObject{}}});
        m_ready = true;
        for (const QJsonObject &m : std::as_const(m_queued)) send(m);
        m_queued.clear();
        m_manager->status(QStringLiteral("%1 ready").arg(m_name));
    };
    send({{QStringLiteral("jsonrpc"), QStringLiteral("2.0")}, {QStringLiteral("id"), id}, {QStringLiteral("method"), QStringLiteral("initialize")},
          {QStringLiteral("params"), params}});
}

void LspServer::request(const QString &method, const QJsonObject &params, Callback cb)
{
    const qint64 id = m_nextId++;
    m_pending[id] = std::move(cb);
    const QJsonObject msg{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")}, {QStringLiteral("id"), id},
                          {QStringLiteral("method"), method}, {QStringLiteral("params"), params}};
    if (m_ready) send(msg);
    else m_queued.append(msg);
}

void LspServer::notify(const QString &method, const QJsonObject &params)
{
    const QJsonObject msg{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")}, {QStringLiteral("method"), method}, {QStringLiteral("params"), params}};
    if (m_ready) send(msg);
    else m_queued.append(msg);
}

void LspServer::handle(const QJsonObject &msg)
{
    const QString method = msg.value(QStringLiteral("method")).toString();
    const QJsonValue id = msg.value(QStringLiteral("id"));
    if (method.isEmpty()) { // a response
        const auto it = m_pending.find(id.toInteger());
        if (it == m_pending.end()) return;
        Callback cb = std::move(it->second);
        m_pending.erase(it);
        if (cb) cb(msg.value(QStringLiteral("result")), msg.value(QStringLiteral("error")).toObject());
        return;
    }
    if (id.isUndefined()) { // a notification
        if (method == QLatin1String("textDocument/publishDiagnostics")) {
            const QJsonObject p = msg.value(QStringLiteral("params")).toObject();
            m_manager->publishDiagnostics(p.value(QStringLiteral("uri")).toString(), p.value(QStringLiteral("diagnostics")).toArray());
        }
        return;
    }
    // A request from the server: answer the ones that matter, refuse the rest.
    QJsonObject reply{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")}, {QStringLiteral("id"), id}};
    if (method == QLatin1String("workspace/configuration")) {
        QJsonArray nulls;
        for (int i = 0; i < msg[QStringLiteral("params")][QStringLiteral("items")].toArray().size(); ++i) nulls.append(QJsonValue::Null);
        reply.insert(QStringLiteral("result"), nulls);
    } else if (method == QLatin1String("workspace/workspaceFolders")) {
        reply.insert(QStringLiteral("result"), QJsonArray{QJsonObject{{QStringLiteral("uri"), uriFor(m_root)}, {QStringLiteral("name"), QFileInfo(m_root).fileName()}}});
    } else if (method == QLatin1String("client/registerCapability") || method == QLatin1String("window/workDoneProgress/create") ||
               method == QLatin1String("client/unregisterCapability")) {
        reply.insert(QStringLiteral("result"), QJsonValue::Null);
    } else {
        reply.insert(QStringLiteral("error"), QJsonObject{{QStringLiteral("code"), -32601}, {QStringLiteral("message"), QStringLiteral("not supported")}});
    }
    send(reply);
}

// ---- LspManager ----

LspManager::LspManager(QObject *parent)
    : KTextEditor::CodeCompletionModel(parent)
{
}

LspManager::~LspManager()
{
    for (Doc *d : std::as_const(m_docs)) {
        clearDiagnostics(d);
        delete d;
    }
    qDeleteAll(m_servers);
}

void LspManager::status(const QString &message)
{
    if (onStatus) onStatus(message);
}

LspManager::Doc *LspManager::docFor(KTextEditor::Document *doc) { return m_docs.value(doc); }

void LspManager::attachDocument(KTextEditor::Document *doc, Connection *conn, const QString &path)
{
    const Language *lang = languageFor(path);
    if (!lang || !conn || m_docs.contains(doc)) return;
    auto *d = new Doc;
    d->doc = doc;
    d->connId = conn->id();
    d->path = path;
    d->uri = uriFor(path);
    d->languageId = lang->id;
    d->debounce = new QTimer(doc);
    d->debounce->setSingleShot(true);
    d->debounce->setInterval(250);
    m_docs.insert(doc, d);

    connect(d->debounce, &QTimer::timeout, this, [this, doc] { if (Doc *x = docFor(doc)) flush(x); });
    connect(doc, &KTextEditor::Document::textChanged, this, [this, doc] {
        if (Doc *x = docFor(doc)) {
            x->dirty = true;
            x->debounce->start();
        }
    });
    auto dropRanges = [this, doc] { if (Doc *x = docFor(doc)) clearDiagnostics(x); };
    connect(doc, &KTextEditor::Document::aboutToInvalidateMovingInterfaceContent, this, dropRanges);
    connect(doc, &KTextEditor::Document::aboutToDeleteMovingInterfaceContent, this, dropRanges);
    connect(doc, &QObject::destroyed, this, [this, doc] {
        Doc *x = m_docs.take(doc);
        if (!x) return;
        if (x->server) x->server->notify(QStringLiteral("textDocument/didClose"),
                                         {{QStringLiteral("textDocument"), QJsonObject{{QStringLiteral("uri"), x->uri}}}});
        delete x; // its moving ranges went with the document
    });
    doc->setMarkIcon(KTextEditor::Document::Error, QIcon::fromTheme(QStringLiteral("dialog-error")));
    doc->setMarkIcon(KTextEditor::Document::Warning, QIcon::fromTheme(QStringLiteral("dialog-warning")));
    start(d);
}

void LspManager::start(Doc *d)
{
    const Language *lang = languageFor(d->path);
    Connection *conn = Connection::find(d->connId);
    if (!lang || !conn || !conn->connected()) return;
    KTextEditor::Document *doc = d->doc;
    const QString path = d->path;
    QJsonArray candidates;
    for (const QStringList &c : lang->candidates) candidates.append(QJsonArray::fromStringList(c));
    QPointer<KTextEditor::Document> guard(doc);
    conn->request(QStringLiteral("lsp_start"), {{QStringLiteral("file"), path}, {QStringLiteral("candidates"), candidates}},
                  [this, guard, connId = conn->id(), lang](bool ok, const QJsonValue &data, const QString &error) {
                      Doc *d = guard ? docFor(guard) : nullptr;
                      if (!d) return;
                      if (!ok) {
                          // Say it once per host and language, not for every file.
                          if (const QString key = connId + QLatin1Char('/') + lang->id; !m_reported.contains(key)) {
                              m_reported.insert(key);
                              Connection *c = Connection::find(connId);
                              status(QStringLiteral("No %1 code intelligence%2: %3")
                                         .arg(lang->id, c && !c->isLocal() ? QStringLiteral(" on ") + c->label() : QString(), error));
                          }
                          return;
                      }
                      const quint64 id = data[QStringLiteral("server")].toInteger();
                      const QString key = connId + QLatin1Char('/') + QString::number(id);
                      LspServer *server = m_servers.value(key);
                      if (!server) {
                          const QString name = data[QStringLiteral("command")][0].toString();
                          server = new LspServer(this, connId, id, data[QStringLiteral("root")].toString(), QFileInfo(name).fileName());
                          m_servers.insert(key, server);
                          status(QStringLiteral("Starting %1…").arg(server->name()));
                          server->initialize();
                      }
                      d->server = server;
                      opened(d);
                  });
}

void LspManager::opened(Doc *d)
{
    d->version = 1;
    d->dirty = false;
    d->server->notify(QStringLiteral("textDocument/didOpen"),
                      {{QStringLiteral("textDocument"),
                        QJsonObject{{QStringLiteral("uri"), d->uri}, {QStringLiteral("languageId"), d->languageId},
                                    {QStringLiteral("version"), d->version}, {QStringLiteral("text"), d->doc->text()}}}});
}

void LspManager::flush(Doc *d)
{
    d->debounce->stop();
    if (!d->dirty || !d->server || !d->doc) return;
    d->dirty = false;
    d->server->notify(QStringLiteral("textDocument/didChange"),
                      {{QStringLiteral("textDocument"), QJsonObject{{QStringLiteral("uri"), d->uri}, {QStringLiteral("version"), ++d->version}}},
                       {QStringLiteral("contentChanges"), QJsonArray{QJsonObject{{QStringLiteral("text"), d->doc->text()}}}}});
}

QJsonObject LspManager::position(Doc *d, const KTextEditor::Cursor &c)
{
    flush(d); // the server must see what the user sees
    return {{QStringLiteral("textDocument"), QJsonObject{{QStringLiteral("uri"), d->uri}}},
            {QStringLiteral("position"), QJsonObject{{QStringLiteral("line"), c.line()}, {QStringLiteral("character"), c.column()}}}};
}

void LspManager::attachView(KTextEditor::View *view)
{
    view->registerCompletionModel(this);
    view->registerTextHintProvider(this);
    view->setConfigValue(QStringLiteral("icon-bar"), true); // error/warning marks
}

void LspManager::handleMessage(Connection *conn, quint64 server, const QJsonObject &msg)
{
    if (LspServer *s = m_servers.value(conn->id() + QLatin1Char('/') + QString::number(server))) s->handle(msg);
}

void LspManager::handleExit(Connection *conn, quint64 server, const QString &error)
{
    LspServer *s = m_servers.take(conn->id() + QLatin1Char('/') + QString::number(server));
    if (!s) return;
    for (Doc *d : std::as_const(m_docs))
        if (d->server == s) {
            d->server = nullptr;
            clearDiagnostics(d);
        }
    status(error.isEmpty() ? QStringLiteral("%1 stopped").arg(s->name()) : QStringLiteral("%1 stopped: %2").arg(s->name(), error));
    delete s;
}

void LspManager::reconnected(Connection *conn)
{
    for (Doc *d : std::as_const(m_docs))
        if (d->connId == conn->id() && !d->server && d->doc) start(d);
}

void LspManager::connectionLost(Connection *conn)
{
    // The daemon stops a client's servers when it disconnects. Reattach open documents later.
    for (auto it = m_servers.begin(); it != m_servers.end();) {
        if (it.value()->connId() == conn->id()) {
            for (Doc *d : std::as_const(m_docs))
                if (d->server == it.value()) {
                    d->server = nullptr;
                    clearDiagnostics(d);
                }
            delete it.value();
            it = m_servers.erase(it);
        } else {
            ++it;
        }
    }
}

// ---- diagnostics ----

void LspManager::clearDiagnostics(Doc *d)
{
    if (d->doc) {
        const auto marks = d->doc->marks();
        for (auto it = marks.cbegin(); it != marks.cend(); ++it)
            d->doc->removeMark(it.key(), KTextEditor::Document::Error | KTextEditor::Document::Warning);
    }
    for (const Diagnostic &diag : std::as_const(d->diagnostics)) delete diag.range;
    d->diagnostics.clear();
}

void LspManager::publishDiagnostics(const QString &uri, const QJsonArray &diagnostics)
{
    for (Doc *d : std::as_const(m_docs)) {
        if (d->uri != uri || !d->doc) continue;
        clearDiagnostics(d);
        for (const QJsonValue &v : diagnostics) {
            const int severity = v[QStringLiteral("severity")].toInt(1); // 1 error, 2 warning, 3 info, 4 hint
            if (severity > 3) continue;
            KTextEditor::Range r = rangeFrom(v[QStringLiteral("range")]);
            if (r.isEmpty()) r.setEnd({r.end().line(), r.end().column() + 1}); // make zero-width ranges visible
            KTextEditor::Attribute::Ptr attr(new KTextEditor::Attribute);
            attr->setUnderlineStyle(QTextCharFormat::WaveUnderline);
            attr->setUnderlineColor(severity == 1 ? QColor(0xda, 0x44, 0x53) : severity == 2 ? QColor(0xf6, 0x74, 0x00) : QColor(0x3d, 0xae, 0xe9));
            KTextEditor::MovingRange *range = d->doc->newMovingRange(r);
            range->setAttribute(attr);
            QString message = v[QStringLiteral("message")].toString();
            if (const QString source = v[QStringLiteral("source")].toString(); !source.isEmpty()) message = source + QStringLiteral(": ") + message;
            d->diagnostics.append({range, message, severity});
            if (severity <= 2) d->doc->addMark(r.start().line(), severity == 1 ? KTextEditor::Document::Error : KTextEditor::Document::Warning);
        }
    }
}

// ---- hover ----

QString LspManager::textHint(KTextEditor::View *view, const KTextEditor::Cursor &pos)
{
    Doc *d = docFor(view->document());
    if (!d) return {};
    QStringList messages;
    for (const Diagnostic &diag : std::as_const(d->diagnostics)) {
        const KTextEditor::Range r = diag.range->toRange();
        if (r.contains(pos) || (r.start().line() == pos.line() && r.end() == pos)) messages << diag.message;
    }
    if (!messages.isEmpty()) return messages.join(QStringLiteral("\n\n"));
    if (!d->server) return {};

    // Hover info arrives later; show it as a tooltip if the mouse hasn't moved on.
    const quint64 gen = ++m_hoverGen;
    QPointer<KTextEditor::View> v(view);
    const QPoint mouse = QCursor::pos();
    d->server->request(QStringLiteral("textDocument/hover"), position(d, pos),
                       [this, gen, v, mouse](const QJsonValue &result, const QJsonObject &) {
                           if (!v || gen != m_hoverGen || (QCursor::pos() - mouse).manhattanLength() > 8) return;
                           const QString html = hoverHtml(result[QStringLiteral("contents")]);
                           if (!html.isEmpty()) QToolTip::showText(mouse, html, v);
                       });
    return {};
}

// ---- completion ----

void LspManager::completionInvoked(KTextEditor::View *view, const KTextEditor::Range &, InvocationType)
{
    beginResetModel();
    m_items.clear();
    setRowCount(0);
    endResetModel();
    Doc *d = docFor(view->document());
    if (!d || !d->server) return;
    const quint64 gen = ++m_completionGen;
    d->server->request(QStringLiteral("textDocument/completion"), position(d, view->cursorPosition()),
                       [this, gen](const QJsonValue &result, const QJsonObject &) {
                           if (gen != m_completionGen) return;
                           const QJsonArray list = result.isArray() ? result.toArray() : result[QStringLiteral("items")].toArray();
                           QList<Item> items;
                           for (const QJsonValue &v : list) {
                               Item it;
                               it.label = v[QStringLiteral("label")].toString().trimmed();
                               it.detail = v[QStringLiteral("detail")].toString();
                               it.kind = v[QStringLiteral("kind")].toInt();
                               const QJsonValue edit = v[QStringLiteral("textEdit")];
                               if (edit.isObject()) {
                                   it.insert = edit[QStringLiteral("newText")].toString();
                                   // Plain TextEdit has "range"; InsertReplaceEdit has "insert"/"replace".
                                   it.replace = rangeFrom(edit[QStringLiteral("range")].isObject() ? edit[QStringLiteral("range")] : edit[QStringLiteral("replace")]);
                               } else {
                                   it.insert = v[QStringLiteral("insertText")].toString(it.label);
                               }
                               it.insert = stripSnippet(it.insert);
                               items << it;
                           }
                           beginResetModel();
                           m_items = items;
                           setRowCount(m_items.size());
                           endResetModel();
                       });
}

QVariant LspManager::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() >= m_items.size()) return {};
    const Item &it = m_items.at(index.row());
    if (role == Qt::DisplayRole) {
        if (index.column() == Name) return it.label;
        if (index.column() == Postfix) return it.detail.left(80);
    } else if (role == Qt::DecorationRole && index.column() == Icon) {
        return kindIcon(it.kind);
    } else if (role == CompletionRole) {
        return int(NoProperty);
    }
    return {};
}

void LspManager::executeCompletionItem(KTextEditor::View *view, const KTextEditor::Range &word, const QModelIndex &index) const
{
    if (!index.isValid() || index.row() >= m_items.size()) return;
    const Item &it = m_items.at(index.row());
    KTextEditor::Range r = word;
    if (it.replace.isValid()) {
        // The server's range ends where the cursor was when it answered; extend to the current word end.
        r = KTextEditor::Range(it.replace.start(), qMax(it.replace.end(), word.end()));
    }
    view->document()->replaceText(r, it.insert);
}

// ---- go to definition ----

void LspManager::gotoDefinition(KTextEditor::View *view)
{
    Doc *d = docFor(view->document());
    if (!d || !d->server) {
        status(QStringLiteral("No code intelligence for this file"));
        return;
    }
    const QString connId = d->connId;
    d->server->request(QStringLiteral("textDocument/definition"), position(d, view->cursorPosition()),
                       [this, connId](const QJsonValue &result, const QJsonObject &error) {
                           const QJsonArray all = result.toArray();
                           const QJsonValue loc = result.isArray() ? (all.isEmpty() ? QJsonValue() : all.at(0)) : result;
                           if (!error.isEmpty() || !loc.isObject()) {
                               status(QStringLiteral("No definition found"));
                               return;
                           }
                           // Location has uri/range; LocationLink has targetUri/targetSelectionRange.
                           const QString uri = loc[QStringLiteral("uri")].toString(loc[QStringLiteral("targetUri")].toString());
                           const QJsonValue range = loc[QStringLiteral("range")].isObject() ? loc[QStringLiteral("range")]
                                                                                             : loc[QStringLiteral("targetSelectionRange")];
                           const KTextEditor::Cursor c = cursorFrom(range[QStringLiteral("start")]);
                           if (onOpenLocation) onOpenLocation(Connection::find(connId), pathFor(uri), c.line(), c.column());
                       });
}

} // namespace cv
