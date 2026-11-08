#pragma once
#include <QWidget>
#include <functional>

class QTreeWidget;
class QTreeWidgetItem;
class QLabel;

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

    std::function<void(const QString &path)> onOpenFile; // a file was activated
    std::function<void(const QString &path)> onSelect;   // the current item changed

private:
    void load(QTreeWidgetItem *parent, const QString &path);

    bool m_dirsOnly;
    QTreeWidget *m_tree;
    QLabel *m_rootLabel;
    Connection *m_conn = nullptr;
    QString m_root;
    quint64 m_generation = 0;
};

} // namespace cv
