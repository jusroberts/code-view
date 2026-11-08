#pragma once
#include <QWidget>
#include <functional>

class QTreeWidget;
class QTreeWidgetItem;
class QLabel;

namespace cv {

// Lazy directory tree served by the daemon (so it will work for remote hosts too).
class FileTree : public QWidget
{
public:
    explicit FileTree(QWidget *parent = nullptr);

    void setRoot(const QString &path);
    QString root() const { return m_root; }
    void refresh();

    std::function<void(const QString &path)> onOpenFile;

private:
    void load(QTreeWidgetItem *parent, const QString &path);

    QTreeWidget *m_tree;
    QLabel *m_rootLabel;
    QString m_root;
    quint64 m_generation = 0;
};

} // namespace cv
