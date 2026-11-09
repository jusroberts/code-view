#include "chatview.h"
#include "connection.h"

#include <QBuffer>
#include <QDesktopServices>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFile>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QEvent>
#include <QComboBox>
#include <QLabel>
#include <QMimeData>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollBar>
#include <QSplitter>
#include <QTextBrowser>
#include <QTextCursor>
#include <QToolButton>
#include <QVBoxLayout>

namespace cv {

namespace {
// The chat splitter ignores a wrapped label's height-for-width, so the label
// claims the height its text needs at its current width.
struct FitHeight : QObject {
    using QObject::QObject;
    bool eventFilter(QObject *o, QEvent *e) override
    {
        if (e->type() == QEvent::Resize) {
            auto *l = static_cast<QLabel *>(o);
            l->setMinimumHeight(qMax(0, l->heightForWidth(l->width())));
        }
        return false;
    }
};

constexpr int kMaxImageSide = 2000;           // larger images are scaled down
constexpr qsizetype kMaxImageBytes = 3500000; // stay well under the API's 5 MB limit

QString esc(const QString &s) { return s.toHtmlEscaped(); }

QString firstLines(const QString &text, int lines, int maxChars)
{
    QStringList parts = text.split(QLatin1Char('\n'));
    QString out = parts.mid(0, lines).join(QLatin1Char('\n'));
    if (out.size() > maxChars) out = out.left(maxChars) + QStringLiteral("…");
    else if (parts.size() > lines) out += QStringLiteral("\n…");
    return out;
}

bool looksLikeAuthProblem(const QString &text)
{
    static const QRegularExpression re(QStringLiteral("(/login|log ?in|not logged|authenticat|oauth|api key)"),
                                       QRegularExpression::CaseInsensitiveOption);
    return re.match(text).hasMatch();
}

bool hasImageData(const QMimeData *m)
{
    if (m->hasImage()) return true;
    for (const QUrl &u : m->urls())
        if (u.isLocalFile() && !QImageReader::imageFormat(u.toLocalFile()).isEmpty()) return true;
    return false;
}

} // namespace

// ---- PromptEdit ----

void PromptEdit::keyPressEvent(QKeyEvent *e)
{
    if ((e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) && !(e->modifiers() & Qt::ShiftModifier)) {
        if (onSubmit) onSubmit();
        return;
    }
    QPlainTextEdit::keyPressEvent(e);
}

bool PromptEdit::canInsertFromMimeData(const QMimeData *source) const
{
    return hasImageData(source) || source->hasUrls() || QPlainTextEdit::canInsertFromMimeData(source);
}

void PromptEdit::insertFromMimeData(const QMimeData *source)
{
    if (onMime && onMime(source)) return;
    QPlainTextEdit::insertFromMimeData(source);
}

// ---- ChatView ----

QList<QPair<QString, QString>> permissionModes()
{
    return {
        {QStringLiteral("Ask before every change"), QStringLiteral("manual")},
        {QStringLiteral("Accept file edits automatically"), QStringLiteral("acceptEdits")},
        {QStringLiteral("Auto"), QStringLiteral("auto")},
        {QStringLiteral("Plan only (no changes)"), QStringLiteral("plan")},
        {QStringLiteral("Bypass all permission checks"), QStringLiteral("bypassPermissions")},
    };
}

ChatView::ChatView(Connection *conn, const QJsonObject &info, QWidget *parent)
    : QWidget(parent)
    , m_conn(conn)
    , m_info(info)
{
    setAcceptDrops(true);

    m_transcript = new QTextBrowser;
    m_transcript->setOpenLinks(false);
    m_transcript->setAcceptDrops(false); // drops go to the ChatView
    m_transcript->document()->setDocumentMargin(8);
    connect(m_transcript, &QTextBrowser::anchorClicked, this, [this](const QUrl &url) {
        if (url.isLocalFile()) {
            if (onOpenFile) onOpenFile(url.toLocalFile());
        } else {
            QDesktopServices::openUrl(url);
        }
    });

    // Permission prompt bar.
    m_permBar = new QFrame;
    m_permBar->setFrameShape(QFrame::StyledPanel);
    m_permBar->setAutoFillBackground(true);
    m_permBar->setBackgroundRole(QPalette::ToolTipBase);
    m_permLabel = new QLabel;
    m_permLabel->setWordWrap(true);
    m_permLabel->setTextFormat(Qt::RichText);
    m_permLabel->setForegroundRole(QPalette::ToolTipText);
    m_permLabel->installEventFilter(new FitHeight(m_permLabel));
    auto *allow = new QPushButton(QIcon::fromTheme(QStringLiteral("dialog-ok")), QStringLiteral("&Allow"));
    auto *deny = new QPushButton(QIcon::fromTheme(QStringLiteral("dialog-cancel")), QStringLiteral("&Deny"));
    connect(allow, &QPushButton::clicked, this, [this] { answer(true); });
    connect(deny, &QPushButton::clicked, this, [this] { answer(false); });
    auto *permLayout = new QHBoxLayout(m_permBar);
    permLayout->addWidget(m_permLabel, 1);
    permLayout->addWidget(allow);
    permLayout->addWidget(deny);
    m_permBar->hide();

    // Attachment thumbnails.
    m_attachBar = new QWidget;
    m_attachLayout = new QHBoxLayout(m_attachBar);
    m_attachLayout->setContentsMargins(0, 0, 0, 0);
    m_attachLayout->addStretch();
    m_attachBar->hide();

    m_input = new PromptEdit;
    m_input->setPlaceholderText(QStringLiteral("Message Claude — Enter to send, Shift+Enter for a new line. "
                                               "Paste or drop images here."));
    m_input->onSubmit = [this] { send(); };
    m_input->onMime = [this](const QMimeData *m) { return takeMime(m); };

    m_statusLabel = new QLabel;
    m_sendButton = new QPushButton(QIcon::fromTheme(QStringLiteral("document-send")), QStringLiteral("Send"));
    m_stopButton = new QPushButton(QIcon::fromTheme(QStringLiteral("process-stop")), QStringLiteral("Stop"));
    m_stopButton->setToolTip(QStringLiteral("Interrupt Claude"));
    connect(m_sendButton, &QPushButton::clicked, this, [this] { send(); });
    connect(m_stopButton, &QPushButton::clicked, this, [this] {
        m_conn->request(QStringLiteral("interrupt"), {{QStringLiteral("session"), id()}});
    });
    // Permission mode, changeable while the session runs.
    m_modeBox = new QComboBox;
    m_modeBox->setToolTip(QStringLiteral("Permission mode: what Claude may do without asking"));
    m_modeBox->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    for (const auto &[label, mode] : permissionModes()) m_modeBox->addItem(label, mode);
    connect(m_modeBox, &QComboBox::activated, this, [this] {
        const QString mode = m_modeBox->currentData().toString();
        if (mode.isEmpty() || mode == m_info.value(QStringLiteral("permission_mode")).toString()) return;
        QPointer<ChatView> self(this);
        m_conn->request(QStringLiteral("set_permission_mode"), {{QStringLiteral("session"), id()}, {QStringLiteral("mode"), mode}},
                        [self](bool ok, const QJsonValue &, const QString &error) {
                            if (!self || ok) return; // the session list update shows the new mode
                            QMessageBox::warning(self, QStringLiteral("Permission Mode"), error);
                            self->updateMode();
                        });
    });
    auto *buttons = new QHBoxLayout;
    buttons->addWidget(m_statusLabel, 1);
    buttons->addWidget(m_modeBox);
    buttons->addWidget(m_stopButton);
    buttons->addWidget(m_sendButton);

    auto *bottom = new QWidget;
    auto *bottomLayout = new QVBoxLayout(bottom);
    bottomLayout->setContentsMargins(0, 0, 0, 0);
    bottomLayout->addWidget(m_permBar);
    bottomLayout->addWidget(m_attachBar);
    bottomLayout->addWidget(m_input, 1);
    bottomLayout->addLayout(buttons);

    auto *split = new QSplitter(Qt::Vertical);
    split->addWidget(m_transcript);
    split->addWidget(bottom);
    split->setStretchFactor(0, 4);
    split->setStretchFactor(1, 1);
    split->setChildrenCollapsible(false);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->addWidget(split);

    updateStatus();
    updateMode();
}

void ChatView::setInfo(const QJsonObject &info)
{
    m_info = info;
    updateStatus();
    updateMode();
}

void ChatView::updateMode()
{
    const QString mode = m_info.value(QStringLiteral("permission_mode")).toString();
    // "My Claude settings" is only listed while it's in effect: a running session can't go back to it.
    const bool hasSettingsItem = m_modeBox->itemData(0).toString().isEmpty();
    if (mode.isEmpty() && !hasSettingsItem) m_modeBox->insertItem(0, QStringLiteral("My Claude settings"), QString());
    if (!mode.isEmpty() && hasSettingsItem) m_modeBox->removeItem(0);
    const int i = m_modeBox->findData(mode);
    if (i >= 0) m_modeBox->setCurrentIndex(i);
}

void ChatView::focusPrompt() { m_input->setFocus(); }

void ChatView::updateStatus()
{
    const QString s = status();
    if (s == QLatin1String("busy")) m_statusLabel->setText(QStringLiteral("Claude is working…"));
    else if (s == QLatin1String("waiting_permission")) m_statusLabel->setText(QStringLiteral("Waiting for your approval"));
    else m_statusLabel->clear();
    m_stopButton->setEnabled(s != QLatin1String("idle"));
}

// ---- history & events ----

void ChatView::loadHistory()
{
    m_historyLoaded = false;
    m_buffered.clear();
    m_lastSeq = -1;
    m_pending.clear();
    m_toolNames.clear();
    m_editedFiles.clear();
    m_touched.clear();
    if (onTouchedChanged) onTouchedChanged();
    m_transcript->clear();
    m_liveStart = -1;
    updatePermissionBar();

    const quint64 generation = ++m_loadGeneration;
    QPointer<ChatView> self(this);
    m_conn->request(
        QStringLiteral("get_history"), {{QStringLiteral("session"), id()}},
        [self, generation](bool ok, const QJsonValue &data, const QString &error) {
            if (!self || generation != self->m_loadGeneration) return;
            if (!ok) {
                self->appendHtml(QStringLiteral("<p style='color:#c0392b'>Couldn't load history: %1</p>").arg(esc(error)));
                return;
            }
            for (const QJsonValue &v : data.toArray()) {
                const QJsonObject e = v.toObject();
                self->m_lastSeq = e.value(QStringLiteral("seq")).toInteger();
                self->render(e.value(QStringLiteral("event")).toObject(), false);
            }
            self->m_historyLoaded = true;
            const auto buffered = std::move(self->m_buffered);
            for (const auto &[seq, ev] : buffered) self->handleEvent(seq, ev);
            self->updatePermissionBar();
            self->scrollToBottom();
        });
}

void ChatView::handleEvent(qint64 seq, const QJsonObject &event)
{
    if (!m_historyLoaded) {
        m_buffered.append({seq, event});
        return;
    }
    if (seq <= m_lastSeq) return;
    m_lastSeq = seq;
    render(event, true);
}

void ChatView::render(const QJsonObject &ev, bool live)
{
    const QString type = ev.value(QStringLiteral("type")).toString();
    auto str = [&ev](const char *key) { return ev.value(QLatin1String(key)).toString(); };

    if (type == QLatin1String("text_delta")) {
        appendDelta(str("text"));
    } else if (type == QLatin1String("text")) {
        finishLive(str("text"));
    } else if (type == QLatin1String("user")) {
        QString html = esc(str("text")).replace(QLatin1Char('\n'), QStringLiteral("<br>"));
        const int images = ev.value(QStringLiteral("images")).toInt();
        if (images > 0)
            html += QStringLiteral("<br><i>📎 %1 image%2</i>").arg(images).arg(images == 1 ? "" : "s");
        const QString bg = palette().color(QPalette::AlternateBase).name();
        appendHtml(QStringLiteral("<table width='100%' cellpadding='6' bgcolor='%1'><tr><td>%2</td></tr></table>")
                       .arg(bg, html));
    } else if (type == QLatin1String("tool_use")) {
        const QString toolId = str("id"), name = str("name");
        const QJsonObject input = ev.value(QStringLiteral("input")).toObject();
        m_toolNames.insert(toolId, name);
        static const QStringList editTools{QStringLiteral("Edit"), QStringLiteral("Write"),
                                           QStringLiteral("MultiEdit"), QStringLiteral("NotebookEdit")};
        if (editTools.contains(name)) {
            const QString path = input.value(QStringLiteral("file_path")).toString(
                input.value(QStringLiteral("notebook_path")).toString());
            if (!path.isEmpty()) m_editedFiles.insert(toolId, path);
        }
        appendHtml(QStringLiteral("<p style='color:gray'><tt>⏺ <b>%1</b> %2</tt></p>").arg(esc(name), toolSummary(name, input)));
    } else if (type == QLatin1String("tool_result")) {
        const QString toolId = str("tool_use_id");
        const bool isError = ev.value(QStringLiteral("is_error")).toBool();
        const QString content = str("content").trimmed();
        if (isError) {
            appendHtml(QStringLiteral("<pre style='color:#c0392b; margin-left:16px'>↳ %1</pre>").arg(esc(firstLines(content, 4, 400))));
        } else if (m_toolNames.value(toolId) == QLatin1String("Read")) {
            appendHtml(QStringLiteral("<p style='color:gray; margin-left:16px'><tt>↳ read %1 lines</tt></p>").arg(content.count(QLatin1Char('\n')) + 1));
        } else if (!content.isEmpty()) {
            appendHtml(QStringLiteral("<pre style='color:gray; margin-left:16px'>↳ %1</pre>").arg(esc(firstLines(content, 6, 600))));
        }
        const QString edited = m_editedFiles.take(toolId);
        if (!isError && !edited.isEmpty()) {
            if (!m_touched.contains(edited)) {
                m_touched.append(edited);
                if (onTouchedChanged) onTouchedChanged();
            }
            if (live && onFileChanged) onFileChanged(edited);
        }
    } else if (type == QLatin1String("permission_request")) {
        const QString tool = str("tool_name");
        const QJsonObject input = ev.value(QStringLiteral("input")).toObject();
        m_pending.append({str("request_id"), tool, input});
        appendHtml(QStringLiteral("<p style='color:#d68910'><tt>⚠ Permission requested: <b>%1</b> %2</tt></p>")
                       .arg(esc(tool), toolSummary(tool, input)));
        if (live) updatePermissionBar();
    } else if (type == QLatin1String("permission_resolved")) {
        const QString rid = str("request_id");
        m_pending.removeIf([&rid](const Pending &p) { return p.requestId == rid; });
        const bool allowed = ev.value(QStringLiteral("allowed")).toBool();
        appendHtml(allowed ? QStringLiteral("<p style='color:#27ae60'><tt>✓ Allowed</tt></p>")
                           : QStringLiteral("<p style='color:#c0392b'><tt>✗ Denied</tt></p>"));
        if (live) updatePermissionBar();
    } else if (type == QLatin1String("turn_complete")) {
        const bool isError = ev.value(QStringLiteral("is_error")).toBool();
        if (isError) {
            const QString result = str("result");
            if (result == QLatin1String("Interrupted.")) {
                appendHtml(QStringLiteral("<p style='color:gray'><i>Interrupted.</i></p>"));
                return;
            }
            appendHtml(QStringLiteral("<p style='color:#c0392b'>%1</p>").arg(esc(result.isEmpty() ? QStringLiteral("The turn failed.") : result)));
            if (live && looksLikeAuthProblem(result) && onAuthProblem) onAuthProblem();
        } else {
            const double secs = ev.value(QStringLiteral("duration_ms")).toDouble() / 1000.0;
            appendHtml(QStringLiteral("<p style='color:gray'><small>Done in %1 s</small></p>").arg(secs, 0, 'f', 1));
        }
    } else if (type == QLatin1String("info")) {
        appendHtml(QStringLiteral("<p style='color:gray'><i>%1</i></p>").arg(esc(str("message"))));
    } else if (type == QLatin1String("error")) {
        const QString msg = str("message");
        appendHtml(QStringLiteral("<pre style='color:#c0392b'>%1</pre>").arg(esc(msg)));
        if (live && looksLikeAuthProblem(msg) && onAuthProblem) onAuthProblem();
    }
}

QString ChatView::displayPath(const QString &path) const
{
    const QString root = cwd() + QLatin1Char('/');
    return path.startsWith(root) ? path.mid(root.size()) : path;
}

QString ChatView::toolSummary(const QString &name, const QJsonObject &input) const
{
    auto s = [&input](const char *key) { return input.value(QLatin1String(key)).toString(); };
    const QString path = s("file_path").isEmpty() ? s("notebook_path") : s("file_path");
    if (!path.isEmpty()) {
        return QStringLiteral("<a href='%1'>%2</a>")
            .arg(esc(QUrl::fromLocalFile(path).toString()), esc(displayPath(path)));
    }
    QString text;
    if (name == QLatin1String("Bash")) text = s("command");
    else if (name == QLatin1String("Grep") || name == QLatin1String("Glob")) text = s("pattern") + (s("path").isEmpty() ? QString() : QStringLiteral("  in ") + displayPath(s("path")));
    else if (name == QLatin1String("WebFetch")) text = s("url");
    else if (name == QLatin1String("WebSearch")) text = s("query");
    else if (!s("description").isEmpty()) text = s("description");
    else text = QString::fromUtf8(QJsonDocument(input).toJson(QJsonDocument::Compact));
    return esc(firstLines(text, 1, 200));
}

// ---- transcript rendering ----

bool ChatView::atBottom() const
{
    const QScrollBar *sb = m_transcript->verticalScrollBar();
    return sb->value() >= sb->maximum() - 4;
}

void ChatView::scrollToBottom()
{
    m_transcript->verticalScrollBar()->setValue(m_transcript->verticalScrollBar()->maximum());
}

void ChatView::appendHtml(const QString &html)
{
    m_liveStart = -1;
    const bool follow = atBottom();
    QTextCursor c(m_transcript->document());
    c.movePosition(QTextCursor::End);
    if (!m_transcript->document()->isEmpty()) c.insertBlock(QTextBlockFormat(), QTextCharFormat());
    c.insertHtml(html);
    if (follow) scrollToBottom();
}

void ChatView::appendMarkdown(const QString &md)
{
    const bool follow = atBottom();
    QTextCursor c(m_transcript->document());
    c.movePosition(QTextCursor::End);
    if (!m_transcript->document()->isEmpty()) c.insertBlock(QTextBlockFormat(), QTextCharFormat());
    c.insertMarkdown(md, QTextDocument::MarkdownDialectGitHub);
    if (follow) scrollToBottom();
}

void ChatView::appendDelta(const QString &text)
{
    const bool follow = atBottom();
    QTextCursor c(m_transcript->document());
    c.movePosition(QTextCursor::End);
    if (m_liveStart < 0) {
        if (!m_transcript->document()->isEmpty()) c.insertBlock(QTextBlockFormat(), QTextCharFormat());
        m_liveStart = c.position();
    }
    c.insertText(text, QTextCharFormat());
    if (follow) scrollToBottom();
}

void ChatView::finishLive(const QString &finalText)
{
    if (m_liveStart < 0) {
        appendMarkdown(finalText);
        return;
    }
    // Replace the streamed plain text with the rendered markdown.
    const bool follow = atBottom();
    QTextCursor c(m_transcript->document());
    c.setPosition(m_liveStart);
    c.movePosition(QTextCursor::End, QTextCursor::KeepAnchor);
    c.removeSelectedText();
    c.insertMarkdown(finalText, QTextDocument::MarkdownDialectGitHub);
    m_liveStart = -1;
    if (follow) scrollToBottom();
}

// ---- permissions ----

void ChatView::updatePermissionBar()
{
    if (m_pending.isEmpty()) {
        m_permBar->hide();
        return;
    }
    const Pending &p = m_pending.first();
    QString text = QStringLiteral("Allow <b>%1</b>? %2").arg(esc(p.tool), toolSummary(p.tool, p.input));
    if (p.tool == QLatin1String("Bash") && !p.input.value(QStringLiteral("command")).toString().isEmpty())
        text = QStringLiteral("Allow <b>Bash</b>?<pre style='white-space: pre-wrap'>%1</pre>").arg(esc(firstLines(p.input.value(QStringLiteral("command")).toString(), 12, 1500)));
    if (m_pending.size() > 1) text += QStringLiteral(" <i>(+%1 more)</i>").arg(m_pending.size() - 1);
    m_permLabel->setText(text);
    m_permLabel->setMinimumHeight(qMax(0, m_permLabel->heightForWidth(m_permLabel->width())));
    m_permBar->show();
}

void ChatView::answer(bool allow)
{
    if (m_pending.isEmpty()) return;
    const QString rid = m_pending.first().requestId;
    QPointer<ChatView> self(this);
    m_conn->request(
        QStringLiteral("answer_permission"),
        {{QStringLiteral("session"), id()}, {QStringLiteral("request_id"), rid}, {QStringLiteral("allow"), allow}},
        [self](bool ok, const QJsonValue &, const QString &error) {
            if (self && !ok) self->appendHtml(QStringLiteral("<p style='color:#c0392b'>%1</p>").arg(esc(error)));
        });
}

// ---- attachments ----

void ChatView::dragEnterEvent(QDragEnterEvent *e)
{
    if (e->mimeData()->hasImage() || e->mimeData()->hasUrls()) e->acceptProposedAction();
}

void ChatView::dropEvent(QDropEvent *e)
{
    if (takeMime(e->mimeData())) e->acceptProposedAction();
}

bool ChatView::takeMime(const QMimeData *mime)
{
    bool handled = false;
    QStringList otherPaths;
    for (const QUrl &u : mime->urls()) {
        if (!u.isLocalFile()) continue;
        if (addImageFile(u.toLocalFile())) handled = true;
        else otherPaths << u.toLocalFile();
    }
    if (!handled && mime->hasImage()) {
        addImage(qvariant_cast<QImage>(mime->imageData()));
        handled = true;
    }
    // Non-image files: insert their paths so Claude can read them.
    if (!otherPaths.isEmpty()) {
        m_input->insertPlainText(otherPaths.join(QLatin1Char(' ')) + QLatin1Char(' '));
        handled = true;
    }
    if (handled) m_input->setFocus();
    return handled;
}

bool ChatView::addImageFile(const QString &path)
{
    const QByteArray format = QImageReader::imageFormat(path).toLower();
    if (format.isEmpty()) return false;
    QImageReader reader(path);
    const QSize size = reader.size();
    static const QHash<QByteArray, QString> direct{{"png", QStringLiteral("image/png")},
                                                   {"jpeg", QStringLiteral("image/jpeg")},
                                                   {"gif", QStringLiteral("image/gif")},
                                                   {"webp", QStringLiteral("image/webp")}};
    // Send the original bytes when the API accepts the format and size as-is.
    if (direct.contains(format) && QFileInfo(path).size() <= kMaxImageBytes && size.isValid() &&
        qMax(size.width(), size.height()) <= kMaxImageSide) {
        QFile f(path);
        if (f.open(QIODevice::ReadOnly)) {
            const QByteArray bytes = f.readAll();
            QPixmap thumb;
            thumb.loadFromData(bytes);
            addAttachment({direct.value(format), bytes.toBase64(), thumb});
            return true;
        }
    }
    const QImage image = reader.read();
    if (image.isNull()) return false;
    addImage(image);
    return true;
}

void ChatView::addImage(const QImage &original)
{
    if (original.isNull()) return;
    QImage image = original;
    if (qMax(image.width(), image.height()) > kMaxImageSide)
        image = image.scaled(kMaxImageSide, kMaxImageSide, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    QByteArray bytes;
    QBuffer buf(&bytes);
    buf.open(QIODevice::WriteOnly);
    image.save(&buf, "PNG");
    QString mediaType = QStringLiteral("image/png");
    if (bytes.size() > kMaxImageBytes) {
        bytes.clear();
        buf.seek(0);
        image.convertToFormat(QImage::Format_RGB888).save(&buf, "JPEG", 85);
        mediaType = QStringLiteral("image/jpeg");
    }
    addAttachment({mediaType, bytes.toBase64(), QPixmap::fromImage(image)});
}

void ChatView::addAttachment(Attachment a)
{
    a.thumb = a.thumb.scaled(64, 64, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    m_attachments.append(std::move(a));
    rebuildAttachments();
}

void ChatView::rebuildAttachments()
{
    while (m_attachLayout->count() > 1) delete m_attachLayout->takeAt(0)->widget();
    for (int i = 0; i < m_attachments.size(); ++i) {
        auto *b = new QToolButton;
        b->setIcon(QIcon(m_attachments[i].thumb));
        b->setIconSize(QSize(64, 64));
        b->setToolTip(QStringLiteral("Click to remove"));
        connect(b, &QToolButton::clicked, this, [this, i] {
            m_attachments.removeAt(i);
            rebuildAttachments();
        });
        m_attachLayout->insertWidget(i, b);
    }
    m_attachBar->setVisible(!m_attachments.isEmpty());
}

void ChatView::setDraft(const QString &text)
{
    m_input->setPlainText(text);
    m_input->setFocus();
    m_input->moveCursor(QTextCursor::End);
}

void ChatView::send()
{
    const QString text = m_input->toPlainText().trimmed();
    if (text.isEmpty() && m_attachments.isEmpty()) return;
    QJsonArray images;
    for (const Attachment &a : m_attachments)
        images.append(QJsonObject{{QStringLiteral("media_type"), a.mediaType},
                                  {QStringLiteral("data"), QString::fromLatin1(a.base64)}});
    m_sendButton->setEnabled(false);
    QPointer<ChatView> self(this);
    m_conn->request(
        QStringLiteral("send_message"),
        {{QStringLiteral("session"), id()}, {QStringLiteral("text"), text}, {QStringLiteral("images"), images}},
        [self](bool ok, const QJsonValue &, const QString &error) {
            if (!self) return;
            self->m_sendButton->setEnabled(true);
            if (!ok) {
                self->appendHtml(QStringLiteral("<p style='color:#c0392b'>Message not sent: %1</p>").arg(esc(error)));
                return;
            }
            self->m_input->clear();
            self->m_attachments.clear();
            self->rebuildAttachments();
        });
}

} // namespace cv
