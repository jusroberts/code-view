#pragma once
#include <QString>

class QWidget;

namespace cv {

// Where remote daemons are built from.
inline constexpr const char *kRepoUrl = "https://github.com/jusroberts/code-view.git";

// Installs or updates codeviewd on a host over SSH: fetches the repo's main branch,
// builds the daemon with cargo, and restarts it. Shows the output in a dialog;
// returns true if it succeeded.
bool runDaemonInstaller(QWidget *parent, const QString &label, const QString &sshTarget);

} // namespace cv
