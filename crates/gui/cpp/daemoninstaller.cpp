#include "daemoninstaller.h"

#include <QDialog>
#include <QDialogButtonBox>
#include <QFontDatabase>
#include <QLabel>
#include <QPlainTextEdit>
#include <QProcess>
#include <QProcessEnvironment>
#include <QPushButton>
#include <QScrollBar>
#include <QVBoxLayout>

namespace cv {

namespace {
// Runs on the host with `sh -s`. REPO is substituted before sending.
const char *kScript = R"SH(set -e
REPO='@REPO@'
SRC="$HOME/.local/share/codeview/src"
[ -f "$HOME/.cargo/env" ] && . "$HOME/.cargo/env"
export PATH="$HOME/.cargo/bin:$PATH"
command -v git >/dev/null || { echo "git isn't installed on this host. Install it, then try again."; exit 2; }
if ! command -v cargo >/dev/null; then
    echo "Rust isn't installed on this host. Install it there with:"
    echo "  curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh"
    echo "then try again."
    exit 3
fi
if [ -d "$SRC/.git" ]; then
    echo "Fetching the latest source…"
    git -C "$SRC" fetch --depth 1 origin main
    git -C "$SRC" reset --hard FETCH_HEAD
else
    echo "Downloading the source to $SRC…"
    mkdir -p "$(dirname "$SRC")"
    git clone --depth 1 "$REPO" "$SRC"
fi
echo "Building codeviewd $(git -C "$SRC" rev-parse --short=12 HEAD) (the first build takes a few minutes)…"
cargo install --locked --force --path "$SRC/crates/daemon"
echo "Restarting codeviewd (sessions carry on after the restart)…"
pkill -x -u "$(id -u)" codeviewd || true
echo "Done."
)SH";
} // namespace

bool runDaemonInstaller(QWidget *parent, const QString &label, const QString &sshTarget)
{
    QDialog dlg(parent);
    dlg.setWindowTitle(QStringLiteral("Install codeviewd on %1").arg(label));
    auto *status = new QLabel(QStringLiteral("Building codeviewd from %1 on %2…").arg(QLatin1String(kRepoUrl), label));
    status->setWordWrap(true);
    auto *log = new QPlainTextEdit;
    log->setReadOnly(true);
    log->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    log->setMaximumBlockCount(5000);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel);
    auto *layout = new QVBoxLayout(&dlg);
    layout->addWidget(status);
    layout->addWidget(log, 1);
    layout->addWidget(buttons);
    dlg.resize(820, 480);

    QProcess proc;
    proc.setProcessChannelMode(QProcess::MergedChannels);
    const QString ssh = QProcessEnvironment::systemEnvironment().value(QStringLiteral("CODEVIEW_SSH"), QStringLiteral("ssh"));
    auto append = [log, &proc] {
        log->moveCursor(QTextCursor::End);
        log->insertPlainText(QString::fromUtf8(proc.readAll()));
        log->verticalScrollBar()->setValue(log->verticalScrollBar()->maximum());
    };
    QObject::connect(&proc, &QProcess::readyRead, &dlg, append);
    bool ok = false;
    QObject::connect(&proc, &QProcess::finished, &dlg, [&](int code, QProcess::ExitStatus st) {
        append();
        ok = st == QProcess::NormalExit && code == 0;
        status->setText(ok ? QStringLiteral("codeviewd is installed and restarting on %1.").arg(label)
                           : QStringLiteral("The install failed (see the output below)."));
        buttons->setStandardButtons(QDialogButtonBox::Close);
    });
    QObject::connect(&proc, &QProcess::errorOccurred, &dlg, [&](QProcess::ProcessError e) {
        if (e != QProcess::FailedToStart) return;
        status->setText(QStringLiteral("Couldn't run %1: %2").arg(ssh, proc.errorString()));
        buttons->setStandardButtons(QDialogButtonBox::Close);
    });
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);

    proc.start(ssh, {QStringLiteral("-T"), QStringLiteral("-o"), QStringLiteral("BatchMode=yes"), sshTarget, QStringLiteral("sh -s")});
    proc.write(QString::fromUtf8(kScript).replace(QLatin1String("@REPO@"), QLatin1String(kRepoUrl)).toUtf8());
    proc.closeWriteChannel();
    dlg.exec();
    if (proc.state() != QProcess::NotRunning) { // cancelled
        proc.kill();
        proc.waitForFinished(3000);
    }
    return ok;
}

} // namespace cv
