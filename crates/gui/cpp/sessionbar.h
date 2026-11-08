#pragma once
#include <QWidget>
#include <functional>

class QListWidget;
class QListWidgetItem;
class QToolButton;

namespace cv {

// Vertical session list on the far left. Collapsed it shows initials badges,
// expanded it shows full names. Selecting an entry switches the whole window.
class SessionBar : public QWidget
{
public:
    explicit SessionBar(QWidget *parent = nullptr);

    // Adds or updates an entry. `host` is empty for local sessions.
    void setEntry(const QString &key, const QString &name, const QString &host, const QString &tooltip, const QString &status);
    void removeEntry(const QString &key);
    QString currentKey() const;
    void setCurrentKey(const QString &key);
    QString keyAt(int row) const; // empty if out of range
    int count() const;

    void setCollapsed(bool collapsed);
    bool isCollapsed() const { return m_collapsed; }

    std::function<void(const QString &key)> onCurrentChanged;
    std::function<void()> onNew;
    std::function<void(const QString &key)> onRename;
    std::function<void(const QString &key)> onClose;

private:
    QListWidgetItem *find(const QString &key) const;
    void refreshItem(QListWidgetItem *item);
    void applyWidth();

    QListWidget *m_list;
    QToolButton *m_toggle;
    QToolButton *m_add;
    bool m_collapsed = false;
};

} // namespace cv
