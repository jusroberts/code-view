#include "editorarea.h"
#include "connection.h"
#include "lsp.h"

#include <KTextEditor/Document>
#include <KTextEditor/Editor>
#include <KTextEditor/View>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <KActionCollection>
#include <QAction>
#include <QIcon>
#include <QMenu>
#include <QJsonObject>
#include <QMessageBox>
#include <QMimeDatabase>
#include <QPointer>
#include <QRegularExpression>
#include <QSet>
#include <QTimer>
#include <QStandardPaths>

namespace cv {

namespace {
// Properties stored on remote documents.
constexpr const char *kConnProp = "cv_conn";
constexpr const char *kPathProp = "cv_path";
constexpr const char *kLabelProp = "cv_label";
// Properties stored on diff documents.
constexpr const char *kDiffProp = "cv_diff";
constexpr const char *kTitleProp = "cv_title";
// Property stored on views (tabs): the session they belong to; empty means every session.
constexpr const char *kSessionProp = "cv_session";

QString cachePathFor(Connection *conn, const QString &remotePath)
{
    QString host = conn->id();
    host.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9._@-]")), QStringLiteral("_"));
    return QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + QStringLiteral("/remote/") + host +
           QDir::cleanPath(QLatin1Char('/') + remotePath);
}

bool writeFile(const QString &path, const QString &content)
{
    QDir().mkpath(QFileInfo(path).path());
    QFile f(path);
    return f.open(QIODevice::WriteOnly | QIODevice::Truncate) && f.write(content.toUtf8()) >= 0;
}
} // namespace

EditorArea::EditorArea(QWidget *parent)
    : QTabWidget(parent)
{
    setTabsClosable(true);
    setMovable(true);
    setDocumentMode(true);
    connect(this, &QTabWidget::tabCloseRequested, this, [this](int i) { closeTab(i); });

    m_lsp = new LspManager; // deleted after the documents (views hold pointers to it)
    m_lsp->onOpenLocation = [this](Connection *conn, const QString &path, int line, int column) { openFile(conn, path, line, column); };

    auto *gotoDef = m_gotoDef = new QAction(QStringLiteral("Go to Definition"), this);
    gotoDef->setShortcut(Qt::Key_F12);
    gotoDef->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    connect(gotoDef, &QAction::triggered, this, [this] { gotoDefinition(); });
    addAction(gotoDef);
    m_lsp->onStateChanged = [this] { updateCodeIntel(); };
    connect(this, &QTabWidget::currentChanged, this, [this] { updateCodeIntel(); });
}

QString EditorArea::codeIntelStatus() const
{
    KTextEditor::View *v = currentView();
    return v ? m_lsp->describe(v->document()) : QString();
}

void EditorArea::updateCodeIntel()
{
    KTextEditor::View *v = currentView();
    m_gotoDef->setEnabled(v && m_lsp->hasServer(v->document()));
    if (onCodeIntelChanged) onCodeIntelChanged();
}

EditorArea::~EditorArea()
{
    QSet<KTextEditor::Document *> docs; // a document can have a view in several sessions
    for (int i = 0; i < count(); ++i)
        if (auto *v = viewAt(i)) docs << v->document();
    qDeleteAll(docs);
    delete m_lsp;
}

void EditorArea::gotoDefinition()
{
    if (auto *v = currentView()) m_lsp->gotoDefinition(v);
}

void EditorArea::focusAt(int index, int line, int column)
{
    setCurrentIndex(index);
    KTextEditor::View *v = viewAt(index);
    if (line >= 0) v->setCursorPosition({line, column});
    v->setFocus();
}

KTextEditor::View *EditorArea::viewAt(int index) const { return qobject_cast<KTextEditor::View *>(widget(index)); }

KTextEditor::View *EditorArea::currentView() const
{
    const int i = currentIndex();
    return i >= 0 && isTabVisible(i) ? viewAt(i) : nullptr;
}

KTextEditor::Document *EditorArea::findDoc(const std::function<bool(KTextEditor::Document *)> &match) const
{
    for (int i = 0; i < count(); ++i)
        if (KTextEditor::Document *d = viewAt(i)->document(); match(d)) return d;
    return nullptr;
}

KTextEditor::Document *EditorArea::localDoc(const QString &canonicalPath) const
{
    return findDoc([&](KTextEditor::Document *d) {
        return !d->property(kConnProp).isValid() && !d->property(kDiffProp).isValid() && d->url().toLocalFile() == canonicalPath;
    });
}

KTextEditor::Document *EditorArea::remoteDoc(Connection *conn, const QString &path) const
{
    return findDoc([&](KTextEditor::Document *d) {
        return d->property(kConnProp).toString() == conn->id() && d->property(kPathProp).toString() == path;
    });
}

int EditorArea::indexInSession(KTextEditor::Document *doc) const
{
    for (int i = 0; i < count(); ++i)
        if (viewAt(i)->document() == doc && isTabVisible(i)) return i;
    return -1;
}

int EditorArea::addTabForSession(QWidget *view, const QIcon &icon, const QString &tooltip)
{
    view->setProperty(kSessionProp, m_session);
    const int i = addTab(view, icon, QString());
    setTabToolTip(i, tooltip);
    return i;
}

void EditorArea::setSession(const QString &key)
{
    if (key == m_session) return;
    m_lastCurrent.insert(m_session, currentView());
    m_session = key;
    QTimer::singleShot(0, this, [this] { updateCodeIntel(); });
    for (int i = 0; i < count(); ++i) {
        const QString s = widget(i)->property(kSessionProp).toString();
        setTabVisible(i, s.isEmpty() || s == key);
    }
    if (QWidget *w = m_lastCurrent.value(key); w && isTabVisible(indexOf(w))) setCurrentWidget(w);
    else for (int i = 0; i < count(); ++i)
        if (isTabVisible(i)) {
            setCurrentIndex(i);
            break;
        }
}

bool EditorArea::hasVisibleTabs() const
{
    for (int i = 0; i < count(); ++i)
        if (isTabVisible(i)) return true;
    return false;
}

bool EditorArea::closeSession(const QString &key)
{
    for (int i = count() - 1; i >= 0; --i)
        if (widget(i)->property(kSessionProp).toString() == key && !closeTab(i)) return false;
    return true;
}

void EditorArea::removeSession(const QString &key)
{
    m_lastCurrent.remove(key);
    for (int i = count() - 1; i >= 0; --i) {
        KTextEditor::View *v = viewAt(i);
        if (v->property(kSessionProp).toString() != key) continue;
        KTextEditor::Document *doc = v->document();
        if (doc->views().size() > 1) {
            removeTab(i);
            delete v;
        } else if (doc->isModified()) {
            v->setProperty(kSessionProp, QString());
            setTabVisible(i, true);
        } else {
            removeTab(i);
            delete doc;
        }
    }
}

void EditorArea::openFile(Connection *conn, const QString &path, int line, int column)
{
    if (conn && !conn->isLocal()) {
        if (KTextEditor::Document *doc = remoteDoc(conn, path)) {
            int i = indexInSession(doc);
            if (i < 0) {
                addView(doc, conn->label() + QLatin1Char(':') + path);
                i = currentIndex();
            }
            focusAt(i, line, column);
            return;
        }
        QPointer<EditorArea> self(this);
        conn->request(QStringLiteral("read_file"), {{QStringLiteral("path"), path}},
                      [self, id = conn->id(), path, line, column](bool ok, const QJsonValue &data, const QString &error) {
                          Connection *c = Connection::find(id);
                          if (!self || !c) return;
                          if (!ok) QMessageBox::warning(self, QStringLiteral("Open file"), error);
                          else self->openRemote(c, path, data[QStringLiteral("content")].toString(), line, column);
                      });
        return;
    }

    const QFileInfo fi(path);
    const QString canonical = fi.canonicalFilePath();
    if (canonical.isEmpty() || !fi.isFile()) {
        QMessageBox::warning(this, QStringLiteral("Open file"), QStringLiteral("%1 doesn't exist.").arg(path));
        return;
    }
    if (KTextEditor::Document *doc = localDoc(canonical)) {
        int i = indexInSession(doc);
        if (i < 0) {
            addView(doc, canonical);
            i = currentIndex();
        }
        focusAt(i, line, column);
        return;
    }
    auto *doc = KTextEditor::Editor::instance()->createDocument(this);
    if (!doc->openUrl(QUrl::fromLocalFile(canonical))) {
        delete doc;
        QMessageBox::warning(this, QStringLiteral("Open file"), QStringLiteral("Couldn't open %1.").arg(canonical));
        return;
    }
    m_lsp->attachDocument(doc, conn ? conn : Connection::find(QString::fromLatin1(Connection::kLocalId)), canonical);
    addView(doc, canonical);
    if (line >= 0) focusAt(currentIndex(), line, column);
}

void EditorArea::openRemote(Connection *conn, const QString &path, const QString &content, int line, int column)
{
    if (remoteDoc(conn, path)) { // opened twice while loading
        openFile(conn, path, line, column);
        return;
    }
    const QString cache = cachePathFor(conn, path);
    auto *doc = KTextEditor::Editor::instance()->createDocument(this);
    if (!writeFile(cache, content) || !doc->openUrl(QUrl::fromLocalFile(cache))) {
        delete doc;
        QMessageBox::warning(this, QStringLiteral("Open file"), QStringLiteral("Couldn't open %1.").arg(path));
        return;
    }
    doc->setProperty(kConnProp, conn->id());
    doc->setProperty(kPathProp, path);
    doc->setProperty(kLabelProp, conn->label());
    connect(doc, &KTextEditor::Document::documentSavedOrUploaded, this, [this](KTextEditor::Document *d, bool) { upload(d); });
    m_lsp->attachDocument(doc, conn, path);
    addView(doc, conn->label() + QLatin1Char(':') + path);
    if (line >= 0) focusAt(currentIndex(), line, column);
}

void EditorArea::upload(KTextEditor::Document *doc)
{
    const QString id = doc->property(kConnProp).toString();
    const QString path = doc->property(kPathProp).toString();
    Connection *conn = Connection::find(id);
    QFile f(doc->url().toLocalFile());
    if (!conn || !f.open(QIODevice::ReadOnly)) {
        doc->setModified(true);
        QMessageBox::warning(this, QStringLiteral("Save"), QStringLiteral("%1 wasn't saved to the host.").arg(path));
        return;
    }
    QPointer<EditorArea> self(this);
    QPointer<KTextEditor::Document> d(doc);
    conn->request(QStringLiteral("write_file"),
                  {{QStringLiteral("path"), path}, {QStringLiteral("content"), QString::fromUtf8(f.readAll())}},
                  [self, d, path](bool ok, const QJsonValue &, const QString &error) {
                      if (ok || !self) return;
                      if (d) d->setModified(true);
                      QMessageBox::warning(self, QStringLiteral("Save"), QStringLiteral("%1 wasn't saved to the host:\n%2").arg(path, error));
                  });
}

void EditorArea::addView(KTextEditor::Document *doc, const QString &tooltip)
{
    auto *view = doc->createView(this);
    m_lsp->attachView(view);
    // Without a KXMLGUI host the view has no context menu; give it a small one.
    auto *menu = new QMenu(view);
    QAction *def = menu->addAction(QIcon::fromTheme(QStringLiteral("go-jump-definition")), QStringLiteral("Go to Definition (F12)"), this,
                                   [this, view] { m_lsp->gotoDefinition(view); });
    connect(menu, &QMenu::aboutToShow, def, [this, def, doc] { def->setEnabled(m_lsp->hasServer(doc)); });
    menu->addSeparator();
    for (const char *name : std::initializer_list<const char *>{"edit_undo", "edit_redo", nullptr, "edit_cut", "edit_copy", "edit_paste", nullptr, "edit_select_all"}) {
        if (!name) menu->addSeparator();
        else if (QAction *a = view->actionCollection()->action(QString::fromLatin1(name))) menu->addAction(a);
    }
    view->setContextMenu(menu);
    const QMimeType mime = QMimeDatabase().mimeTypeForFile(doc->url().toLocalFile());
    const int i = addTabForSession(view, QIcon::fromTheme(mime.iconName(), QIcon::fromTheme(mime.genericIconName())), tooltip);
    updateTitle(view);
    connect(doc, &KTextEditor::Document::modifiedChanged, this, [this, view] { updateTitle(view); });
    connect(doc, &KTextEditor::Document::documentSavedOrUploaded, this, [this] { if (onSaved) onSaved(); });
    setCurrentIndex(i);
    view->setFocus();
}

void EditorArea::showDiff(const QString &key, const QString &title, const QString &tooltip, const QString &diff)
{
    KTextEditor::Document *doc = findDoc([&](KTextEditor::Document *d) { return d->property(kDiffProp).toString() == key; });
    const bool fresh = !doc;
    if (fresh) {
        doc = KTextEditor::Editor::instance()->createDocument(this);
        doc->setProperty(kDiffProp, key);
        doc->setProperty(kTitleProp, title);
        doc->setHighlightingMode(QStringLiteral("Diff"));
    }
    doc->setReadWrite(true);
    doc->setText(diff.isEmpty() ? QStringLiteral("No changes.\n") : diff);
    doc->setReadWrite(false);
    doc->setModified(false);
    if (const int i = indexInSession(doc); i >= 0) setCurrentIndex(i);
    else addDiffView(doc, tooltip);
}

KTextEditor::View *EditorArea::addDiffView(KTextEditor::Document *doc, const QString &tooltip)
{
    auto *view = doc->createView(this);
    view->setConfigValue(QStringLiteral("dynamic-word-wrap"), false);
    const int i = addTabForSession(view, QIcon::fromTheme(QStringLiteral("vcs-diff")), tooltip);
    updateTitle(view);
    setCurrentIndex(i);
    return view;
}

void EditorArea::updateTitle(KTextEditor::View *view)
{
    const int i = QTabWidget::indexOf(view);
    if (i < 0) return;
    KTextEditor::Document *doc = view->document();
    QString name = doc->property(kTitleProp).toString();
    if (name.isEmpty()) name = doc->documentName();
    if (const QString label = doc->property(kLabelProp).toString(); !label.isEmpty()) name += QStringLiteral(" @") + label;
    setTabText(i, doc->isModified() ? name + QStringLiteral(" *") : name);
}

void EditorArea::reloadIfUnmodified(Connection *conn, const QString &path)
{
    if (!conn || conn->isLocal()) {
        KTextEditor::Document *d = localDoc(QFileInfo(path).canonicalFilePath());
        if (d && !d->isModified()) d->documentReload();
        return;
    }
    KTextEditor::Document *d = remoteDoc(conn, path);
    if (!d || d->isModified()) return;
    QPointer<KTextEditor::Document> doc(d);
    conn->request(QStringLiteral("read_file"), {{QStringLiteral("path"), path}},
                  [doc](bool ok, const QJsonValue &data, const QString &) {
                      if (!ok || !doc || doc->isModified()) return;
                      if (writeFile(doc->url().toLocalFile(), data[QStringLiteral("content")].toString())) doc->documentReload();
                  });
}

void EditorArea::saveCurrent()
{
    if (auto *v = currentView()) v->document()->documentSave();
}

bool EditorArea::closeTab(int index)
{
    auto *view = viewAt(index);
    if (!view) return false;
    KTextEditor::Document *doc = view->document();
    if (doc->views().size() > 1) { // still open in another session
        removeTab(index);
        delete view;
        return true;
    }
    if (!doc->queryClose()) return false;
    removeTab(index);
    delete doc; // also deletes its views
    return true;
}

bool EditorArea::queryCloseAll()
{
    QSet<KTextEditor::Document *> asked;
    for (int i = 0; i < count(); ++i) {
        KTextEditor::Document *d = viewAt(i)->document();
        if (!d->isModified() || asked.contains(d)) continue;
        asked.insert(d);
        if (isTabVisible(i)) setCurrentIndex(i);
        if (!d->queryClose()) return false;
    }
    return true;
}

} // namespace cv
