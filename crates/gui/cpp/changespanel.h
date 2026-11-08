#pragma once
#include <QJsonObject>
#include <QWidget>
#include <functional>

class QLabel;
class QTreeWidget;
class QTreeWidgetItem;
class QTimer;

namespace cv {

class Connection;

// Review panel for the current session: files Claude edited, uncommitted changes,
// and everything changed on the current branch. Selecting a file shows its diff.
class ChangesPanel : public QWidget
{
public:
    explicit ChangesPanel(QWidget *parent = nullptr);

    void setSession(Connection *conn, const QString &cwd);
    void setClaudeFiles(const QStringList &files);
    // Re-reads git status (debounced, so it's cheap to call often).
    void scheduleRefresh();
    void refresh();

    // Shows a diff: `base` is a commit, or empty for "against HEAD".
    std::function<void(Connection *conn, const QString &path, const QString &base, const QString &title)> onShowDiff;
    std::function<void(Connection *conn, const QString &path)> onOpenFile;
    std::function<void(Connection *conn, const QString &path)> onReverted;

private:
    void rebuild();
    void showDiffFor(QTreeWidgetItem *item);
    void showDiff(const QString &path, const QString &base);
    void revert(const QString &path, const QString &status);

    QLabel *m_header;
    QTreeWidget *m_tree;
    QTimer *m_debounce;
    Connection *m_conn = nullptr;
    QString m_cwd;
    QStringList m_claudeFiles;
    QJsonObject m_status; // last git_status reply
    QString m_error;
    quint64 m_generation = 0;
};

} // namespace cv
