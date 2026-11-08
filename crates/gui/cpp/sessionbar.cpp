#include "sessionbar.h"

#include <QBoxLayout>
#include <QListWidget>
#include <QMenu>
#include <QPainter>
#include <QRegularExpression>
#include <QSettings>
#include <QToolButton>

namespace cv {

namespace {
enum Role { KeyRole = Qt::UserRole, NameRole, HostRole, StatusRole, TipRole };
constexpr int kBadge = 32;
constexpr int kCollapsedWidth = 52;
constexpr int kExpandedWidth = 220;

// "code-view" -> "CV", "bookshop" -> "Bo".
QString initials(const QString &name)
{
    const QStringList words = name.split(QRegularExpression(QStringLiteral("[^\\p{L}\\p{N}]+")), Qt::SkipEmptyParts);
    if (words.isEmpty()) return QStringLiteral("?");
    if (words.size() == 1) return words[0].left(1).toUpper() + words[0].mid(1, 1).toLower();
    return (words[0].left(1) + words[1].left(1)).toUpper();
}

QIcon badge(const QString &name, const QString &status)
{
    const qreal dpr = 2.0;
    QPixmap pm(QSize(kBadge, kBadge) * dpr);
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    const QColor bg = QColor::fromHsv(int(qHash(name) % 360), 140, 170);
    p.setBrush(bg);
    p.setPen(Qt::NoPen);
    p.drawRoundedRect(QRectF(0, 0, kBadge, kBadge), 7, 7);
    QFont f;
    f.setBold(true);
    f.setPixelSize(13);
    p.setFont(f);
    p.setPen(Qt::white);
    p.drawText(QRectF(0, 0, kBadge, kBadge), Qt::AlignCenter, initials(name));
    QColor dot;
    if (status == QLatin1String("busy")) dot = QColor(0x3d, 0xae, 0xe9);
    else if (status == QLatin1String("waiting_permission")) dot = QColor(0xf6, 0x74, 0x00);
    if (dot.isValid()) {
        p.setBrush(dot);
        p.setPen(QPen(Qt::white, 1.5));
        p.drawEllipse(QRectF(kBadge - 11, kBadge - 11, 10, 10));
    }
    return QIcon(pm);
}
} // namespace

SessionBar::SessionBar(QWidget *parent)
    : QWidget(parent)
{
    m_toggle = new QToolButton;
    m_toggle->setAutoRaise(true);
    connect(m_toggle, &QToolButton::clicked, this, [this] { setCollapsed(!m_collapsed); });
    m_add = new QToolButton;
    m_add->setAutoRaise(true);
    m_add->setIcon(QIcon::fromTheme(QStringLiteral("list-add")));
    m_add->setToolTip(QStringLiteral("New session (Ctrl+Shift+N)"));
    connect(m_add, &QToolButton::clicked, this, [this] { if (onNew) onNew(); });

    m_list = new QListWidget;
    m_list->setIconSize(QSize(kBadge, kBadge));
    m_list->setFrameShape(QFrame::NoFrame);
    m_list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_list->setDragDropMode(QAbstractItemView::InternalMove);
    m_list->setSpacing(2);
    m_list->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_list, &QListWidget::currentItemChanged, this, [this](QListWidgetItem *item) {
        if (onCurrentChanged) onCurrentChanged(item ? item->data(KeyRole).toString() : QString());
    });
    connect(m_list, &QListWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
        QListWidgetItem *item = m_list->itemAt(pos);
        if (!item) return;
        const QString key = item->data(KeyRole).toString();
        QMenu menu;
        menu.addAction(QIcon::fromTheme(QStringLiteral("edit-rename")), QStringLiteral("Rename…"), this,
                       [this, key] { if (onRename) onRename(key); });
        menu.addAction(QIcon::fromTheme(QStringLiteral("tab-close")), QStringLiteral("Close…"), this,
                       [this, key] { if (onClose) onClose(key); });
        menu.exec(m_list->viewport()->mapToGlobal(pos));
    });

    auto *buttons = new QBoxLayout(QBoxLayout::LeftToRight);
    buttons->setObjectName(QStringLiteral("buttons"));
    buttons->addWidget(m_toggle);
    buttons->addStretch();
    buttons->addWidget(m_add);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(2, 2, 2, 2);
    layout->addLayout(buttons);
    layout->addWidget(m_list, 1);

    setCollapsed(QSettings().value(QStringLiteral("sessionBarCollapsed"), false).toBool());
}

void SessionBar::setCollapsed(bool collapsed)
{
    m_collapsed = collapsed;
    QSettings().setValue(QStringLiteral("sessionBarCollapsed"), collapsed);
    m_toggle->setIcon(QIcon::fromTheme(collapsed ? QStringLiteral("sidebar-expand-left") : QStringLiteral("sidebar-collapse-left")));
    m_toggle->setToolTip(collapsed ? QStringLiteral("Expand sessions") : QStringLiteral("Collapse sessions"));
    // Collapsed, the buttons stack vertically to fit the narrow bar.
    if (auto *buttons = findChild<QBoxLayout *>(QStringLiteral("buttons"))) {
        buttons->setDirection(collapsed ? QBoxLayout::TopToBottom : QBoxLayout::LeftToRight);
        // Match the session badges' row width (the list insets items by its spacing).
        const int inset = collapsed ? m_list->spacing() : 0;
        buttons->setContentsMargins(inset, 0, inset, 0);
    }
    for (QToolButton *b : {m_toggle, m_add}) {
        b->setSizePolicy(collapsed ? QSizePolicy::Expanding : QSizePolicy::Preferred, QSizePolicy::Fixed);
        if (collapsed) b->setFixedHeight(kBadge + 6);
        else b->setFixedHeight(b->sizeHint().height());
    }
    for (int i = 0; i < m_list->count(); ++i) refreshItem(m_list->item(i));
    applyWidth();
}

void SessionBar::applyWidth()
{
    setFixedWidth(m_collapsed ? kCollapsedWidth : kExpandedWidth);
}

QListWidgetItem *SessionBar::find(const QString &key) const
{
    for (int i = 0; i < m_list->count(); ++i)
        if (m_list->item(i)->data(KeyRole).toString() == key) return m_list->item(i);
    return nullptr;
}

void SessionBar::refreshItem(QListWidgetItem *item)
{
    const QString name = item->data(NameRole).toString();
    const QString host = item->data(HostRole).toString();
    const QString status = item->data(StatusRole).toString();
    item->setIcon(badge(name, status));
    item->setText(m_collapsed ? QString() : host.isEmpty() ? name : QStringLiteral("%1\n@%2").arg(name, host));
    QString tip = host.isEmpty() ? name : QStringLiteral("%1 @%2").arg(name, host);
    if (status == QLatin1String("busy")) tip += QStringLiteral(" (working)");
    else if (status == QLatin1String("waiting_permission")) tip += QStringLiteral(" (waiting for permission)");
    item->setToolTip(tip + QLatin1Char('\n') + item->data(TipRole).toString());
}

void SessionBar::setEntry(const QString &key, const QString &name, const QString &host, const QString &tooltip, const QString &status)
{
    QListWidgetItem *item = find(key);
    if (!item) {
        item = new QListWidgetItem(m_list);
        item->setData(KeyRole, key);
        item->setFlags(item->flags() | Qt::ItemIsDragEnabled);
    }
    item->setData(NameRole, name);
    item->setData(HostRole, host);
    item->setData(StatusRole, status);
    item->setData(TipRole, tooltip);
    refreshItem(item);
}

void SessionBar::removeEntry(const QString &key)
{
    delete find(key);
}

QString SessionBar::currentKey() const
{
    QListWidgetItem *item = m_list->currentItem();
    return item ? item->data(KeyRole).toString() : QString();
}

void SessionBar::setCurrentKey(const QString &key)
{
    if (QListWidgetItem *item = find(key)) m_list->setCurrentItem(item);
}

QString SessionBar::keyAt(int row) const
{
    QListWidgetItem *item = m_list->item(row);
    return item ? item->data(KeyRole).toString() : QString();
}

int SessionBar::count() const { return m_list->count(); }

} // namespace cv
