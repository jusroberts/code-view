#include "editorarea.h"
#include "connection.h"

#include <KTextEditor/Document>
#include <KTextEditor/Editor>
#include <KTextEditor/View>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QIcon>
#include <QJsonObject>
#include <QMessageBox>
#include <QMimeDatabase>
#include <QPointer>
#include <QRegularExpression>
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
}

KTextEditor::View *EditorArea::viewAt(int index) const { return qobject_cast<KTextEditor::View *>(widget(index)); }

int EditorArea::indexOfLocal(const QString &canonicalPath) const
{
    for (int i = 0; i < count(); ++i) {
        KTextEditor::Document *d = viewAt(i)->document();
        if (!d->property(kConnProp).isValid() && d->url().toLocalFile() == canonicalPath) return i;
    }
    return -1;
}

int EditorArea::indexOfRemote(Connection *conn, const QString &path) const
{
    for (int i = 0; i < count(); ++i) {
        KTextEditor::Document *d = viewAt(i)->document();
        if (d->property(kConnProp).toString() == conn->id() && d->property(kPathProp).toString() == path) return i;
    }
    return -1;
}

void EditorArea::openFile(Connection *conn, const QString &path)
{
    if (conn && !conn->isLocal()) {
        if (const int i = indexOfRemote(conn, path); i >= 0) {
            setCurrentIndex(i);
            viewAt(i)->setFocus();
            return;
        }
        QPointer<EditorArea> self(this);
        conn->request(QStringLiteral("read_file"), {{QStringLiteral("path"), path}},
                      [self, id = conn->id(), path](bool ok, const QJsonValue &data, const QString &error) {
                          Connection *c = Connection::find(id);
                          if (!self || !c) return;
                          if (!ok) QMessageBox::warning(self, QStringLiteral("Open file"), error);
                          else self->openRemote(c, path, data[QStringLiteral("content")].toString());
                      });
        return;
    }

    const QFileInfo fi(path);
    const QString canonical = fi.canonicalFilePath();
    if (canonical.isEmpty() || !fi.isFile()) {
        QMessageBox::warning(this, QStringLiteral("Open file"), QStringLiteral("%1 doesn't exist.").arg(path));
        return;
    }
    if (const int i = indexOfLocal(canonical); i >= 0) {
        setCurrentIndex(i);
        viewAt(i)->setFocus();
        return;
    }
    auto *doc = KTextEditor::Editor::instance()->createDocument(this);
    if (!doc->openUrl(QUrl::fromLocalFile(canonical))) {
        delete doc;
        QMessageBox::warning(this, QStringLiteral("Open file"), QStringLiteral("Couldn't open %1.").arg(canonical));
        return;
    }
    addView(doc, canonical);
}

void EditorArea::openRemote(Connection *conn, const QString &path, const QString &content)
{
    if (const int i = indexOfRemote(conn, path); i >= 0) { // opened twice while loading
        setCurrentIndex(i);
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
    addView(doc, conn->label() + QLatin1Char(':') + path);
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
    const QMimeType mime = QMimeDatabase().mimeTypeForFile(doc->url().toLocalFile());
    const int i = addTab(view, QIcon::fromTheme(mime.iconName(), QIcon::fromTheme(mime.genericIconName())), QString());
    setTabToolTip(i, tooltip);
    updateTitle(view);
    connect(doc, &KTextEditor::Document::modifiedChanged, this, [this, view] { updateTitle(view); });
    connect(doc, &KTextEditor::Document::documentSavedOrUploaded, this, [this] { if (onSaved) onSaved(); });
    setCurrentIndex(i);
    view->setFocus();
}

void EditorArea::showDiff(const QString &key, const QString &title, const QString &tooltip, const QString &diff)
{
    KTextEditor::Document *doc = nullptr;
    for (int i = 0; i < count() && !doc; ++i)
        if (viewAt(i)->document()->property(kDiffProp).toString() == key) {
            doc = viewAt(i)->document();
            setCurrentIndex(i);
        }
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
    if (fresh) {
        auto *view = doc->createView(this);
        view->setConfigValue(QStringLiteral("dynamic-word-wrap"), false);
        const int i = addTab(view, QIcon::fromTheme(QStringLiteral("vcs-diff")), QString());
        setTabToolTip(i, tooltip);
        updateTitle(view);
        setCurrentIndex(i);
    }
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
        const int i = indexOfLocal(QFileInfo(path).canonicalFilePath());
        if (i >= 0 && !viewAt(i)->document()->isModified()) viewAt(i)->document()->documentReload();
        return;
    }
    const int i = indexOfRemote(conn, path);
    if (i < 0 || viewAt(i)->document()->isModified()) return;
    QPointer<KTextEditor::Document> doc(viewAt(i)->document());
    conn->request(QStringLiteral("read_file"), {{QStringLiteral("path"), path}},
                  [doc](bool ok, const QJsonValue &data, const QString &) {
                      if (!ok || !doc || doc->isModified()) return;
                      if (writeFile(doc->url().toLocalFile(), data[QStringLiteral("content")].toString())) doc->documentReload();
                  });
}

void EditorArea::saveCurrent()
{
    if (auto *v = viewAt(currentIndex())) v->document()->documentSave();
}

bool EditorArea::closeTab(int index)
{
    auto *view = viewAt(index);
    if (!view) return false;
    KTextEditor::Document *doc = view->document();
    if (!doc->queryClose()) return false;
    removeTab(index);
    delete doc; // also deletes its views
    return true;
}

bool EditorArea::queryCloseAll()
{
    for (int i = 0; i < count(); ++i) {
        if (auto *v = viewAt(i); v && v->document()->isModified()) {
            setCurrentIndex(i);
            if (!v->document()->queryClose()) return false;
        }
    }
    return true;
}

} // namespace cv
