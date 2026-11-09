#include "filetree.h"
#include "connection.h"

#include <QCheckBox>
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QJsonArray>
#include <QLabel>
#include <QMimeDatabase>
#include <QPointer>
#include <QShortcut>
#include <QToolButton>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QVBoxLayout>

namespace cv {

namespace {
enum Role { PathRole = Qt::UserRole, IsDirRole, LoadedRole, DotRole };
const QStringList kHidden{QStringLiteral(".git")};
} // namespace

FileTree::FileTree(bool dirsOnly, QWidget *parent)
    : QWidget(parent)
    , m_dirsOnly(dirsOnly)
{
    m_rootLabel = new QLabel;
    m_rootLabel->setTextFormat(Qt::PlainText);
    m_rootLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    auto *refreshButton = new QToolButton;
    refreshButton->setIcon(QIcon::fromTheme(QStringLiteral("view-refresh")));
    refreshButton->setToolTip(QStringLiteral("Refresh"));
    refreshButton->setAutoRaise(true);
    connect(refreshButton, &QToolButton::clicked, this, [this] { refresh(); });
    m_hiddenBox = new QCheckBox(QStringLiteral("Show hidden"));
    m_hiddenBox->setChecked(m_showHidden);
    m_hiddenBox->setToolTip(QStringLiteral("Show files and folders starting with a dot (Ctrl+H)"));
    connect(m_hiddenBox, &QCheckBox::toggled, this, [this](bool on) { setShowHidden(on); });
    auto *hiddenShortcut = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_H), this);
    hiddenShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(hiddenShortcut, &QShortcut::activated, m_hiddenBox, &QCheckBox::toggle);

    m_tree = new QTreeWidget;
    m_tree->setHeaderHidden(true);
    m_tree->setUniformRowHeights(true);
    m_tree->setExpandsOnDoubleClick(false); // itemActivated toggles folders (also on Enter)
    connect(m_tree, &QTreeWidget::itemExpanded, this, [this](QTreeWidgetItem *item) {
        if (!item->data(0, LoadedRole).toBool()) load(item, item->data(0, PathRole).toString());
    });
    connect(m_tree, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem *item) {
        if (item && onSelect) onSelect(item->data(0, PathRole).toString());
    });
    connect(m_tree, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem *item) {
        if (item->data(0, IsDirRole).toBool()) {
            if (onActivateDir) onActivateDir(item->data(0, PathRole).toString());
            else item->setExpanded(!item->isExpanded());
        }
        else if (onOpenFile) onOpenFile(item->data(0, PathRole).toString());
    });

    auto *header = new QHBoxLayout;
    header->addWidget(m_rootLabel, 1);
    header->addWidget(m_hiddenBox);
    header->addWidget(refreshButton);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addLayout(header);
    layout->addWidget(m_tree);
}

void FileTree::setRoot(Connection *conn, const QString &path)
{
    if (conn == m_conn && path == m_root) return;
    m_conn = conn;
    m_root = path;
    refresh();
}

void FileTree::setShowHidden(bool show)
{
    m_showHidden = show;
    m_hiddenBox->setChecked(show);
    for (QTreeWidgetItemIterator it(m_tree); *it; ++it) {
        if ((*it)->data(0, DotRole).toBool()) (*it)->setHidden(!show);
    }
}

void FileTree::reveal(const QString &path)
{
    m_reveal = QDir::cleanPath(path);
}

void FileTree::revealStep(const QList<QTreeWidgetItem *> &items)
{
    if (m_reveal.isEmpty()) return;
    for (QTreeWidgetItem *item : items) {
        QString p = item->data(0, PathRole).toString();
        if (p == m_reveal) {
            m_reveal.clear();
            item->setHidden(false);
            m_tree->setCurrentItem(item);
            m_tree->scrollToItem(item, QAbstractItemView::PositionAtCenter);
            return;
        }
        if (!p.endsWith(QLatin1Char('/'))) p += QLatin1Char('/');
        if (m_reveal.startsWith(p)) {
            item->setHidden(false); // a dot-folder on the way is shown anyway
            item->setExpanded(true); // loads its children, which continue the walk
            return;
        }
    }
    m_reveal.clear(); // path no longer exists
}

void FileTree::refresh()
{
    ++m_generation;
    m_tree->clear();
    // Show just the folder name; long paths would force the panel wide.
    const bool remote = m_conn && !m_conn->isLocal();
    const QString name = QFileInfo(m_root).fileName().isEmpty() ? m_root : QFileInfo(m_root).fileName();
    m_rootLabel->setText(remote ? name + QStringLiteral(" @") + m_conn->label() : name);
    m_rootLabel->setToolTip(remote ? m_conn->label() + QLatin1Char(':') + m_root : m_root);
    if (m_conn && !m_root.isEmpty()) load(nullptr, m_root);
}

void FileTree::load(QTreeWidgetItem *parent, const QString &path)
{
    QPointer<FileTree> self(this);
    const quint64 generation = m_generation;
    if (parent) parent->setData(0, LoadedRole, true);
    m_conn->request(
        QStringLiteral("list_dir"), {{QStringLiteral("path"), path}},
        [self, generation, parent](bool ok, const QJsonValue &data, const QString &error) {
            if (!self || generation != self->m_generation) return; // tree was cleared; `parent` is gone
            if (!ok) {
                auto *item = new QTreeWidgetItem({error});
                item->setIcon(0, QIcon::fromTheme(QStringLiteral("dialog-error")));
                if (parent) parent->addChild(item);
                else self->m_tree->addTopLevelItem(item);
                return;
            }
            static const QMimeDatabase mimeDb;
            QList<QTreeWidgetItem *> items;
            for (const QJsonValue &v : data.toArray()) {
                const QJsonObject e = v.toObject();
                const QString name = e.value(QStringLiteral("name")).toString();
                if (kHidden.contains(name)) continue;
                const bool isDir = e.value(QStringLiteral("is_dir")).toBool();
                if (self->m_dirsOnly && !isDir) continue;
                auto *item = new QTreeWidgetItem({name});
                item->setData(0, PathRole, e.value(QStringLiteral("path")).toString());
                item->setData(0, IsDirRole, isDir);
                if (name.startsWith(QLatin1Char('.'))) item->setData(0, DotRole, true);
                if (isDir) {
                    item->setIcon(0, QIcon::fromTheme(QStringLiteral("folder")));
                    item->setChildIndicatorPolicy(QTreeWidgetItem::ShowIndicator);
                } else {
                    const QMimeType mime = mimeDb.mimeTypeForFile(name, QMimeDatabase::MatchExtension);
                    item->setIcon(0, QIcon::fromTheme(mime.iconName(), QIcon::fromTheme(mime.genericIconName(),
                                                                                         QIcon::fromTheme(QStringLiteral("text-plain")))));
                }
                items << item;
            }
            if (parent) {
                parent->addChildren(items);
                if (items.isEmpty()) parent->setChildIndicatorPolicy(QTreeWidgetItem::DontShowIndicator);
            } else {
                self->m_tree->addTopLevelItems(items);
            }
            // Hiding only works once items are in the tree.
            for (QTreeWidgetItem *item : items) {
                if (item->data(0, DotRole).toBool()) item->setHidden(!self->m_showHidden);
            }
            self->revealStep(items);
        });
}

} // namespace cv
