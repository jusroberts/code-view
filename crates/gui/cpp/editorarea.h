#pragma once
#include <QHash>
#include <QPointer>
#include <QTabWidget>
#include <functional>

namespace KTextEditor {
class Document;
class View;
}

namespace cv {

class Connection;
class LspManager;

// Tabbed KTextEditor (Kate's editor part) views.
// Local files open directly from disk. Remote files are downloaded into a cache
// file and opened from there; every save is uploaded back through the daemon.
class EditorArea : public QTabWidget
{
public:
    explicit EditorArea(QWidget *parent = nullptr);
    ~EditorArea() override;

    // Opens (or switches to) a file; `line`/`column` (0-based) move the cursor when line >= 0.
    void openFile(Connection *conn, const QString &path, int line = -1, int column = 0);
    LspManager *lsp() const { return m_lsp; }
    void gotoDefinition();
    // Reloads a file Claude edited, unless it has unsaved changes.
    void reloadIfUnmodified(Connection *conn, const QString &path);
    // Shows a unified diff in a read-only tab; `key` identifies the tab so it's reused.
    void showDiff(const QString &key, const QString &title, const QString &tooltip, const QString &diff);
    void saveCurrent();
    bool closeTab(int index);
    // Each session has its own tabs; only the current session's are shown.
    void setSession(const QString &key);
    bool hasVisibleTabs() const;
    // Closes a session's tabs, asking about unsaved changes; false if the user cancelled.
    bool closeSession(const QString &key);
    // A session went away: drops its tabs, but keeps unsaved ones (shown in every session).
    void removeSession(const QString &key);
    // Asks about unsaved changes in every tab; false if the user cancelled.
    bool queryCloseAll();

    std::function<void()> onSaved; // any document was saved

private:
    KTextEditor::View *viewAt(int index) const;
    KTextEditor::View *currentView() const; // null when the current tab belongs to another session
    // Documents are shared between sessions; each session has its own view (tab) of one.
    KTextEditor::Document *findDoc(const std::function<bool(KTextEditor::Document *)> &match) const;
    KTextEditor::Document *localDoc(const QString &canonicalPath) const;
    KTextEditor::Document *remoteDoc(Connection *conn, const QString &path) const;
    int indexInSession(KTextEditor::Document *doc) const; // this session's tab for doc, or -1
    int addTabForSession(QWidget *view, const QIcon &icon, const QString &tooltip);
    void addView(KTextEditor::Document *doc, const QString &tooltip);
    KTextEditor::View *addDiffView(KTextEditor::Document *doc, const QString &tooltip);
    void openRemote(Connection *conn, const QString &path, const QString &content, int line, int column);
    void focusAt(int index, int line, int column);
    void upload(KTextEditor::Document *doc);
    void updateTitle(KTextEditor::View *view);

    LspManager *m_lsp;
    QString m_session;
    QHash<QString, QPointer<QWidget>> m_lastCurrent; // session -> its last current tab
};

} // namespace cv
