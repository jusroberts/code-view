#pragma once
#include <QHash>
#include <QObject>

class QAction;
class QDockWidget;
class QMainWindow;
class QToolBar;

namespace cv {

// JetBrains-style tool windows: panels stay docked in the main window, and their
// title bar has a minimize button instead of float/close. A minimized panel becomes
// a button with rotated text on a strip at the window edge; clicking it restores the panel.
class DockStripes : public QObject
{
public:
    explicit DockStripes(QMainWindow *window);

    void manage(QDockWidget *dock);
    void setMinimized(QDockWidget *dock, bool minimized);
    bool isMinimized(QDockWidget *dock) const { return m_buttons.contains(dock); }

private:
    QMainWindow *m_window;
    QToolBar *m_left;
    QToolBar *m_right;
    QAction *m_leftSpacer; // panels from the bottom area go below it
    QHash<QDockWidget *, QAction *> m_buttons;
};

} // namespace cv
