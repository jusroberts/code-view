#include "dockstripes.h"

#include <QDockWidget>
#include <QHBoxLayout>
#include <QLabel>
#include <QMainWindow>
#include <QStyleOptionToolButton>
#include <QStylePainter>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>

namespace cv {

namespace {

// A tool button whose text runs vertically (rotated by `angle`, 90 or -90 degrees).
class StripeButton : public QToolButton
{
public:
    StripeButton(const QString &text, int angle)
        : m_angle(angle)
    {
        setText(text);
        setAutoRaise(true);
        setCursor(Qt::PointingHandCursor);
    }
    QSize sizeHint() const override
    {
        const QFontMetrics fm(font());
        return {fm.height() + 12, fm.horizontalAdvance(text()) + 24};
    }
    QSize minimumSizeHint() const override { return sizeHint(); }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QStylePainter p(this);
        QStyleOptionToolButton opt;
        initStyleOption(&opt);
        opt.text.clear();
        p.drawComplexControl(QStyle::CC_ToolButton, opt);
        if (m_angle > 0) p.translate(width(), 0);
        else p.translate(0, height());
        p.rotate(m_angle);
        p.setPen(palette().color(QPalette::ButtonText));
        p.drawText(QRect(0, 0, height(), width()), Qt::AlignCenter, text());
    }

private:
    int m_angle;
};

QToolBar *makeStripe(QMainWindow *window, Qt::ToolBarArea area, const QString &name)
{
    auto *bar = new QToolBar(name, window);
    bar->setObjectName(name);
    bar->setMovable(false);
    bar->setFloatable(false);
    bar->toggleViewAction()->setVisible(false);
    bar->setContextMenuPolicy(Qt::PreventContextMenu);
    window->addToolBar(area, bar);
    bar->hide(); // shown while it holds a button
    return bar;
}

} // namespace

DockStripes::DockStripes(QMainWindow *window)
    : QObject(window)
    , m_window(window)
{
    m_left = makeStripe(window, Qt::LeftToolBarArea, QStringLiteral("leftStripe"));
    m_right = makeStripe(window, Qt::RightToolBarArea, QStringLiteral("rightStripe"));
    auto *spacer = new QWidget;
    spacer->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
    m_leftSpacer = m_left->addWidget(spacer);
}

void DockStripes::manage(QDockWidget *dock)
{
    m_docks.append(dock);
    dock->setFeatures(QDockWidget::DockWidgetMovable); // no floating, no close
    auto *bar = new QWidget;
    auto *layout = new QHBoxLayout(bar);
    layout->setContentsMargins(8, 2, 2, 2);
    auto *title = new QLabel(dock->windowTitle());
    connect(dock, &QDockWidget::windowTitleChanged, title, &QLabel::setText);
    auto *minimize = new QToolButton;
    minimize->setAutoRaise(true);
    minimize->setIcon(QIcon::fromTheme(QStringLiteral("window-minimize")));
    minimize->setToolTip(QStringLiteral("Minimize"));
    connect(minimize, &QToolButton::clicked, this, [this, dock] { setMinimized(dock, true); });
    layout->addWidget(title, 1);
    layout->addWidget(minimize);
    dock->setTitleBarWidget(bar);
}

QVariantMap DockStripes::saveState() const
{
    QStringList minimized;
    QVariantMap sizes;
    for (QDockWidget *d : m_docks) {
        if (!isMinimized(d)) continue;
        minimized << d->objectName();
        if (m_sizes.contains(d)) sizes.insert(d->objectName(), m_sizes.value(d));
    }
    return {{QStringLiteral("minimized"), minimized}, {QStringLiteral("sizes"), sizes}};
}

void DockStripes::restoreState(const QVariantMap &state)
{
    const QStringList minimized = state.value(QStringLiteral("minimized")).toStringList();
    const QVariantMap sizes = state.value(QStringLiteral("sizes")).toMap();
    for (QDockWidget *d : std::as_const(m_docks)) {
        const bool want = minimized.contains(d->objectName());
        if (want == isMinimized(d)) continue;
        setMinimized(d, want);
        if (want && sizes.contains(d->objectName())) m_sizes.insert(d, sizes.value(d->objectName()).toInt());
    }
    // A panel the saved layout hid without minimizing would otherwise be unreachable.
    for (QDockWidget *d : std::as_const(m_docks))
        if (!isMinimized(d) && d->isHidden()) d->show();
    m_left->setVisible(m_left->actions().size() > 1);
    m_right->setVisible(!m_right->actions().isEmpty());
}

void DockStripes::setMinimized(QDockWidget *dock, bool minimized)
{
    if (minimized == isMinimized(dock)) {
        if (!minimized) dock->raise(); // already open: bring a tabbed panel to the front
        return;
    }
    if (!minimized) {
        QAction *a = m_buttons.take(dock);
        QToolBar *stripe = qobject_cast<QToolBar *>(a->parent());
        delete a; // also deletes the button
        if (stripe == m_right ? m_right->actions().isEmpty() : m_left->actions().size() == 1) stripe->hide();
        dock->show();
        dock->raise();
        // Qt gives a panel restored into an empty dock area its minimum size; put back the old size
        // once the layout has placed it.
        if (const int size = m_sizes.take(dock); size > 0)
            QTimer::singleShot(0, dock, [this, dock, size] {
                const bool bottom = m_window->dockWidgetArea(dock) == Qt::BottomDockWidgetArea;
                m_window->resizeDocks({dock}, {size}, bottom ? Qt::Vertical : Qt::Horizontal);
            });
        return;
    }
    const Qt::DockWidgetArea area = m_window->dockWidgetArea(dock);
    if (dock->isVisible()) m_sizes.insert(dock, area == Qt::BottomDockWidgetArea ? dock->height() : dock->width());
    QToolBar *stripe = area == Qt::RightDockWidgetArea ? m_right : m_left;
    auto *button = new StripeButton(dock->windowTitle().section(QStringLiteral(" — "), 0, 0), stripe == m_right ? 90 : -90);
    button->setToolTip(QStringLiteral("Show %1").arg(dock->windowTitle()));
    connect(button, &QToolButton::clicked, this, [this, dock] { setMinimized(dock, false); });
    QAction *a = stripe == m_left && area != Qt::BottomDockWidgetArea ? m_left->insertWidget(m_leftSpacer, button)
                                                                       : stripe->addWidget(button);
    m_buttons.insert(dock, a);
    dock->hide();
    stripe->show();
}

} // namespace cv
