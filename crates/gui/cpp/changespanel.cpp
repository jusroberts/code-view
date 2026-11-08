#include "changespanel.h"
#include "connection.h"

#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QPointer>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace cv {

namespace {
enum Role { PathRole = Qt::UserRole, BaseRole, StatusRole, GroupRole };
enum Group { ClaudeGroup, UncommittedGroup, BranchGroup };

QColor statusColor(const QString &s)
{
    if (s == QLatin1String("M")) return QColor(0xe6, 0x7e, 0x22);
    if (s == QLatin1String("A") || s == QLatin1String("?")) return QColor(0x27, 0xae, 0x60);
    if (s == QLatin1String("D")) return QColor(0xc0, 0x39, 0x2b);
    if (s == QLatin1String("U")) return QColor(0x8e, 0x44, 0xad);
    return QColor(0x29, 0x80, 0xb9);
}

QString statusName(const QString &s)
{
    if (s == QLatin1String("M")) return QStringLiteral("modified");
    if (s == QLatin1String("A")) return QStringLiteral("added");
    if (s == QLatin1String("?")) return QStringLiteral("untracked");
    if (s == QLatin1String("D")) return QStringLiteral("deleted");
    if (s == QLatin1String("R")) return QStringLiteral("renamed");
    if (s == QLatin1String("U")) return QStringLiteral("conflict");
    if (s.isEmpty()) return QStringLiteral("no uncommitted changes");
    return s;
}
} // namespace

ChangesPanel::ChangesPanel(QWidget *parent)
    : QWidget(parent)
{
    m_header = new QLabel;
    m_header->setTextFormat(Qt::PlainText);
    m_header->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    auto *refreshButton = new QToolButton;
    refreshButton->setIcon(QIcon::fromTheme(QStringLiteral("view-refresh")));
    refreshButton->setToolTip(QStringLiteral("Refresh"));
    refreshButton->setAutoRaise(true);
    connect(refreshButton, &QToolButton::clicked, this, [this] { refresh(); });

    m_tree = new QTreeWidget;
    m_tree->setColumnCount(2);
    m_tree->setHeaderHidden(true);
    m_tree->setRootIsDecorated(true);
    m_tree->setUniformRowHeights(true);
    m_tree->setTextElideMode(Qt::ElideMiddle); // keep file names visible
    m_tree->header()->setStretchLastSection(false);
    m_tree->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_tree->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_tree->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_tree, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem *item) { showDiffFor(item); });
    connect(m_tree, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem *item) {
        const QString path = item->data(0, PathRole).toString();
        if (!path.isEmpty() && onOpenFile) onOpenFile(m_conn, path);
    });
    connect(m_tree, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem *item) {
        // Keyboard navigation previews diffs too (clicks are handled above).
        if (item && m_tree->hasFocus() && !(QApplication::mouseButtons() & Qt::LeftButton)) showDiffFor(item);
    });
    connect(m_tree, &QTreeWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
        QTreeWidgetItem *item = m_tree->itemAt(pos);
        if (!item || item->data(0, PathRole).toString().isEmpty()) return;
        const QString path = item->data(0, PathRole).toString();
        const QString base = item->data(0, BaseRole).toString();
        QMenu menu;
        menu.addAction(QIcon::fromTheme(QStringLiteral("vcs-diff")), QStringLiteral("Show Diff"), this,
                       [this, path, base] { showDiff(path, base); });
        menu.addAction(QIcon::fromTheme(QStringLiteral("document-open")), QStringLiteral("Open File"), this,
                       [this, path] { if (onOpenFile) onOpenFile(m_conn, path); });
        const QString status = item->data(0, StatusRole).toString();
        if (item->data(0, GroupRole).toInt() != BranchGroup && !status.isEmpty()) {
            menu.addSeparator();
            menu.addAction(QIcon::fromTheme(QStringLiteral("edit-undo")),
                           status == QLatin1String("?") ? QStringLiteral("Delete Untracked File…") : QStringLiteral("Discard Changes…"),
                           this, [this, path, status] { revert(path, status); });
        }
        menu.exec(m_tree->viewport()->mapToGlobal(pos));
    });

    m_debounce = new QTimer(this);
    m_debounce->setSingleShot(true);
    m_debounce->setInterval(400);
    connect(m_debounce, &QTimer::timeout, this, [this] { refresh(); });

    auto *header = new QHBoxLayout;
    header->addWidget(m_header, 1);
    header->addWidget(refreshButton);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addLayout(header);
    layout->addWidget(m_tree);
}

void ChangesPanel::setSession(Connection *conn, const QString &cwd)
{
    if (conn == m_conn && cwd == m_cwd) return;
    m_conn = conn;
    m_cwd = cwd;
    m_status = {};
    m_error.clear();
    rebuild();
    refresh();
}

void ChangesPanel::setClaudeFiles(const QStringList &files)
{
    if (files == m_claudeFiles) return;
    m_claudeFiles = files;
    rebuild();
}

void ChangesPanel::scheduleRefresh() { m_debounce->start(); }

void ChangesPanel::refresh()
{
    m_debounce->stop();
    if (!m_conn || m_cwd.isEmpty()) return;
    const quint64 generation = ++m_generation;
    QPointer<ChangesPanel> self(this);
    m_conn->request(QStringLiteral("git_status"), {{QStringLiteral("cwd"), m_cwd}},
                    [self, generation](bool ok, const QJsonValue &data, const QString &error) {
                        if (!self || generation != self->m_generation) return;
                        self->m_status = ok ? data.toObject() : QJsonObject{};
                        self->m_error = ok ? QString() : error;
                        self->rebuild();
                    });
}

void ChangesPanel::rebuild()
{
    // Keep the selection and collapsed groups across refreshes.
    QString selPath;
    int selGroup = -1;
    if (QTreeWidgetItem *cur = m_tree->currentItem()) {
        selPath = cur->data(0, PathRole).toString();
        selGroup = cur->data(0, GroupRole).toInt();
    }
    QSet<int> collapsed;
    for (int i = 0; i < m_tree->topLevelItemCount(); ++i)
        if (!m_tree->topLevelItem(i)->isExpanded()) collapsed.insert(m_tree->topLevelItem(i)->data(0, GroupRole).toInt());

    const QSignalBlocker block(m_tree);
    m_tree->clear();
    if (!m_conn) {
        m_header->setText(QStringLiteral("No session"));
        return;
    }

    QHash<QString, QString> uncommitted; // path -> status
    for (const QJsonValue &v : m_status.value(QStringLiteral("uncommitted")).toArray())
        uncommitted.insert(v[QStringLiteral("path")].toString(), v[QStringLiteral("status")].toString());
    const QString root = m_status.value(QStringLiteral("root")).toString(m_cwd);
    auto relative = [&root](const QString &path) {
        const QString rel = QDir(root).relativeFilePath(path);
        return rel.startsWith(QLatin1String("..")) ? path : rel;
    };

    QTreeWidgetItem *select = nullptr;
    auto addGroup = [this, &collapsed](int group, const QString &title) {
        auto *g = new QTreeWidgetItem(m_tree, {title});
        g->setData(0, GroupRole, group);
        g->setFirstColumnSpanned(true);
        QFont f = g->font(0);
        f.setBold(true);
        g->setFont(0, f);
        g->setFlags(Qt::ItemIsEnabled);
        g->setExpanded(!collapsed.contains(group));
        return g;
    };
    auto addFile = [&](QTreeWidgetItem *g, const QString &path, const QString &rel, const QString &status, const QString &base) {
        auto *item = new QTreeWidgetItem(g, {status.isEmpty() ? QStringLiteral("·") : status, rel});
        item->setData(0, PathRole, path);
        item->setData(0, BaseRole, base);
        item->setData(0, StatusRole, status);
        item->setData(0, GroupRole, g->data(0, GroupRole));
        item->setForeground(0, statusColor(status));
        item->setTextAlignment(0, Qt::AlignCenter);
        item->setToolTip(1, QStringLiteral("%1 (%2)").arg(path, statusName(status)));
        if (status == QLatin1String("D")) {
            QFont f = item->font(1);
            f.setStrikeOut(true);
            item->setFont(1, f);
        }
        if (path == selPath && g->data(0, GroupRole).toInt() == selGroup) select = item;
    };

    QTreeWidgetItem *claude = addGroup(ClaudeGroup, QStringLiteral("Edited by Claude (%1)").arg(m_claudeFiles.size()));
    for (const QString &path : std::as_const(m_claudeFiles)) addFile(claude, path, relative(path), uncommitted.value(path), QString());

    if (!m_error.isEmpty()) {
        m_header->setText(m_error.contains(QLatin1String("not a git repository")) ? QStringLiteral("Not a git repository")
                                                                                   : QStringLiteral("git: ") + m_error);
        m_header->setToolTip(m_error);
    } else if (!m_status.isEmpty()) {
        const QString branch = m_status.value(QStringLiteral("branch")).toString(QStringLiteral("(detached)"));
        m_header->setText(QStringLiteral("%1 · %2 uncommitted").arg(branch).arg(uncommitted.size()));
        m_header->setToolTip(root);

        QTreeWidgetItem *unc = addGroup(UncommittedGroup, QStringLiteral("Uncommitted (%1)").arg(uncommitted.size()));
        for (const QJsonValue &v : m_status.value(QStringLiteral("uncommitted")).toArray())
            addFile(unc, v[QStringLiteral("path")].toString(), v[QStringLiteral("rel")].toString(), v[QStringLiteral("status")].toString(), QString());

        const QString base = m_status.value(QStringLiteral("base")).toString();
        if (base == branch) { // on the base branch itself: the branch group would just repeat Uncommitted
            if (select) m_tree->setCurrentItem(select);
            return;
        }
        const QString mergeBase = m_status.value(QStringLiteral("merge_base")).toString();
        const QJsonArray branchFiles = m_status.value(QStringLiteral("branch_files")).toArray();
        QTreeWidgetItem *br = addGroup(BranchGroup, base.isEmpty() ? QStringLiteral("Branch (no main or master to compare with)")
                                                                   : QStringLiteral("Branch vs %1 (%2)").arg(base).arg(branchFiles.size()));
        br->setToolTip(0, mergeBase.isEmpty() ? QString() : QStringLiteral("Everything changed since %1 (merge-base %2), including uncommitted edits")
                                                                .arg(base, mergeBase.left(10)));
        for (const QJsonValue &v : branchFiles)
            addFile(br, v[QStringLiteral("path")].toString(), v[QStringLiteral("rel")].toString(), v[QStringLiteral("status")].toString(), mergeBase);
    } else {
        m_header->setText(QStringLiteral("Loading…"));
    }
    if (select) m_tree->setCurrentItem(select);
}

void ChangesPanel::showDiffFor(QTreeWidgetItem *item)
{
    showDiff(item->data(0, PathRole).toString(), item->data(0, BaseRole).toString());
}

void ChangesPanel::showDiff(const QString &path, const QString &base)
{
    if (path.isEmpty() || !onShowDiff) return;
    const QString name = QFileInfo(path).fileName();
    const QString title = base.isEmpty() ? QStringLiteral("Δ %1").arg(name)
                                         : QStringLiteral("Δ %1 (vs %2)").arg(name, m_status.value(QStringLiteral("base")).toString());
    if (!m_error.isEmpty()) { // not a repo: nothing to diff against, just open it
        if (onOpenFile) onOpenFile(m_conn, path);
        return;
    }
    onShowDiff(m_conn, path, base, title);
}

void ChangesPanel::revert(const QString &path, const QString &status)
{
    const bool untracked = status == QLatin1String("?");
    const QString question = untracked ? QStringLiteral("Delete %1? It isn't tracked by git, so this can't be undone.").arg(path)
                                       : QStringLiteral("Discard all uncommitted changes to %1?").arg(path);
    if (QMessageBox::warning(this, QStringLiteral("Discard Changes"), question, QMessageBox::Discard | QMessageBox::Cancel,
                             QMessageBox::Cancel) != QMessageBox::Discard)
        return;
    QPointer<ChangesPanel> self(this);
    m_conn->request(QStringLiteral("git_revert"), {{QStringLiteral("cwd"), m_cwd}, {QStringLiteral("path"), path}},
                    [self, id = m_conn->id(), path](bool ok, const QJsonValue &, const QString &error) {
                        if (!self) return;
                        if (!ok) QMessageBox::warning(self, QStringLiteral("Discard Changes"), error);
                        else if (self->onReverted) self->onReverted(Connection::find(id), path);
                        self->refresh();
                    });
}

} // namespace cv
