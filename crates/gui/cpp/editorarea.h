#pragma once
#include <QTabWidget>
#include <functional>

namespace KTextEditor {
class Document;
class View;
}

namespace cv {

class Connection;

// Tabbed KTextEditor (Kate's editor part) views.
// Local files open directly from disk. Remote files are downloaded into a cache
// file and opened from there; every save is uploaded back through the daemon.
class EditorArea : public QTabWidget
{
public:
    explicit EditorArea(QWidget *parent = nullptr);

    void openFile(Connection *conn, const QString &path);
    // Reloads a file Claude edited, unless it has unsaved changes.
    void reloadIfUnmodified(Connection *conn, const QString &path);
    // Shows a unified diff in a read-only tab; `key` identifies the tab so it's reused.
    void showDiff(const QString &key, const QString &title, const QString &tooltip, const QString &diff);
    void saveCurrent();
    bool closeTab(int index);
    // Asks about unsaved changes in every tab; false if the user cancelled.
    bool queryCloseAll();

    std::function<void()> onSaved; // any document was saved

private:
    KTextEditor::View *viewAt(int index) const;
    int indexOfLocal(const QString &canonicalPath) const;
    int indexOfRemote(Connection *conn, const QString &path) const;
    void addView(KTextEditor::Document *doc, const QString &tooltip);
    void openRemote(Connection *conn, const QString &path, const QString &content);
    void upload(KTextEditor::Document *doc);
    void updateTitle(KTextEditor::View *view);
};

} // namespace cv
