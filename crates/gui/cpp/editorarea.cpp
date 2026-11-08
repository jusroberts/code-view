#include "editorarea.h"

#include <KTextEditor/Document>
#include <KTextEditor/Editor>
#include <KTextEditor/View>
#include <QFileInfo>
#include <QIcon>
#include <QMessageBox>
#include <QMimeDatabase>

namespace cv {

EditorArea::EditorArea(QWidget *parent)
    : QTabWidget(parent)
{
    setTabsClosable(true);
    setMovable(true);
    setDocumentMode(true);
    connect(this, &QTabWidget::tabCloseRequested, this, [this](int i) { closeTab(i); });
}

KTextEditor::View *EditorArea::viewAt(int index) const { return qobject_cast<KTextEditor::View *>(widget(index)); }

int EditorArea::indexOf(const QString &canonicalPath) const
{
    for (int i = 0; i < count(); ++i)
        if (auto *v = viewAt(i); v && v->document()->url().toLocalFile() == canonicalPath) return i;
    return -1;
}

void EditorArea::openFile(const QString &path)
{
    const QFileInfo fi(path);
    const QString canonical = fi.canonicalFilePath();
    if (canonical.isEmpty() || !fi.isFile()) {
        QMessageBox::warning(this, QStringLiteral("Open file"), QStringLiteral("%1 doesn't exist.").arg(path));
        return;
    }
    if (const int i = indexOf(canonical); i >= 0) {
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
    auto *view = doc->createView(this);
    const QMimeType mime = QMimeDatabase().mimeTypeForFile(canonical);
    const int i = addTab(view, QIcon::fromTheme(mime.iconName(), QIcon::fromTheme(mime.genericIconName())), fi.fileName());
    setTabToolTip(i, canonical);
    connect(doc, &KTextEditor::Document::modifiedChanged, this, [this, view] { updateTitle(view); });
    setCurrentIndex(i);
    view->setFocus();
}

void EditorArea::updateTitle(KTextEditor::View *view)
{
    const int i = QTabWidget::indexOf(view);
    if (i < 0) return;
    const QString name = view->document()->documentName();
    setTabText(i, view->document()->isModified() ? name + QStringLiteral(" *") : name);
}

void EditorArea::reloadIfUnmodified(const QString &path)
{
    const int i = indexOf(QFileInfo(path).canonicalFilePath());
    if (i < 0) return;
    KTextEditor::Document *doc = viewAt(i)->document();
    if (!doc->isModified()) doc->documentReload();
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
