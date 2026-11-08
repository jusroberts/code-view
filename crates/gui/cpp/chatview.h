#pragma once
#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QWidget>
#include <functional>

class QTextBrowser;
class QLabel;
class QPushButton;
class QFrame;
class QHBoxLayout;
class QMimeData;

namespace cv {

class Connection;

// Prompt box: Enter sends, Shift+Enter adds a newline, pasted/dropped images become attachments.
class PromptEdit : public QPlainTextEdit
{
public:
    using QPlainTextEdit::QPlainTextEdit;
    std::function<void()> onSubmit;
    std::function<bool(const QMimeData *)> onMime; // returns true if it consumed the data

protected:
    void keyPressEvent(QKeyEvent *e) override;
    bool canInsertFromMimeData(const QMimeData *source) const override;
    void insertFromMimeData(const QMimeData *source) override;
};

// One agent session: transcript, permission prompts and the prompt box.
class ChatView : public QWidget
{
public:
    ChatView(Connection *conn, const QJsonObject &info, QWidget *parent = nullptr);

    Connection *connection() const { return m_conn; }

    QString id() const { return m_info.value(QStringLiteral("id")).toString(); }
    QString name() const { return m_info.value(QStringLiteral("name")).toString(); }
    QString cwd() const { return m_info.value(QStringLiteral("cwd")).toString(); }
    QString status() const { return m_info.value(QStringLiteral("status")).toString(); }
    void setInfo(const QJsonObject &info);

    // Clears the transcript and reloads it from the daemon.
    void loadHistory();
    void handleEvent(qint64 seq, const QJsonObject &event);
    void focusPrompt();

    std::function<void(const QString &path)> onOpenFile;
    std::function<void(const QString &path)> onFileChanged; // Claude edited a file
    std::function<void()> onAuthProblem;
    std::function<void()> onTouchedChanged; // touchedFiles() changed

    // Files Claude edited successfully in this session, in first-edit order.
    const QStringList &touchedFiles() const { return m_touched; }

protected:
    void dragEnterEvent(QDragEnterEvent *e) override;
    void dropEvent(QDropEvent *e) override;

private:
    struct Attachment {
        QString mediaType;
        QByteArray base64;
        QPixmap thumb;
    };
    struct Pending {
        QString requestId;
        QString tool;
        QJsonObject input;
    };

    void render(const QJsonObject &ev, bool live);
    void appendHtml(const QString &html);
    void appendMarkdown(const QString &md);
    void appendDelta(const QString &text);
    void finishLive(const QString &finalText);
    bool atBottom() const;
    void scrollToBottom();

    bool takeMime(const QMimeData *mime);
    bool addImageFile(const QString &path);
    void addImage(const QImage &image);
    void addAttachment(Attachment a);
    void rebuildAttachments();
    void send();
    void answer(bool allow);
    void updatePermissionBar();
    void updateStatus();
    QString toolSummary(const QString &name, const QJsonObject &input) const;
    QString displayPath(const QString &path) const;

    Connection *m_conn;
    QJsonObject m_info;
    QTextBrowser *m_transcript;
    PromptEdit *m_input;
    QWidget *m_attachBar;
    QHBoxLayout *m_attachLayout;
    QFrame *m_permBar;
    QLabel *m_permLabel;
    QLabel *m_statusLabel;
    QPushButton *m_sendButton;
    QPushButton *m_stopButton;

    QList<Attachment> m_attachments;
    QList<Pending> m_pending;
    QHash<QString, QString> m_toolNames;   // tool_use_id -> tool name
    QHash<QString, QString> m_editedFiles; // tool_use_id -> file path (Edit/Write)
    QStringList m_touched;

    bool m_historyLoaded = false;
    quint64 m_loadGeneration = 0;
    qint64 m_lastSeq = -1;
    QList<QPair<qint64, QJsonObject>> m_buffered;
    int m_liveStart = -1; // document position where streaming text began
};

} // namespace cv
