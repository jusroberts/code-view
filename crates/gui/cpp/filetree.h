#pragma once
#include <QWidget>
#include <functional>

class QTreeWidget;
class QTreeWidgetItem;
class QLabel;
class QCheckBox;

namespace cv {

class Connection;

// Lazy directory tree served by a daemon, so it works the same for remote hosts.
class FileTree : public QWidget
{
public:
    explicit FileTree(bool dirsOnly = false, QWidget *parent = nullptr);

    void setRoot(Connection *conn, const QString &path);
    Connection *connection() const { return m_conn; }
    QString root() const { return m_root; }
    void refresh();
    // Dot-files and dot-folders; Ctrl+H toggles while the tree has focus.
    void setShowHidden(bool show);
    // Expands the tree down to `path` and selects it once loaded. Call before setRoot().
    void reveal(const QString &path);

    std::function<void(const QString &path)> onOpenFile; // a file was activated
    std::function<void(const QString &path)> onSelect;   // the current item changed
    std::function<void(const QString &path)> onActivateDir; // a folder was double-clicked; unset means expand/collapse it

private:
    void load(QTreeWidgetItem *parent, const QString &path);
    void revealStep(const QList<QTreeWidgetItem *> &items);

    bool m_dirsOnly;
    QTreeWidget *m_tree;
    QLabel *m_rootLabel;
    QCheckBox *m_hiddenBox;
    bool m_showHidden = true;
    Connection *m_conn = nullptr;
    QString m_root;
    QString m_reveal;
    quint64 m_generation = 0;
};

} // namespace cv
