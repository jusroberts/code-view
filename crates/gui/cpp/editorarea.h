#pragma once
#include <QTabWidget>

namespace KTextEditor {
class Document;
class View;
}

namespace cv {

// Tabbed KTextEditor (Kate's editor part) views. Files are opened directly
// from disk, so highlighting, saving and on-disk change detection are Kate's.
class EditorArea : public QTabWidget
{
public:
    explicit EditorArea(QWidget *parent = nullptr);

    void openFile(const QString &path);
    // Reloads a file Claude edited, unless it has unsaved changes.
    void reloadIfUnmodified(const QString &path);
    void saveCurrent();
    bool closeTab(int index);
    // Asks about unsaved changes in every tab; false if the user cancelled.
    bool queryCloseAll();

private:
    KTextEditor::View *viewAt(int index) const;
    int indexOf(const QString &canonicalPath) const;
    void updateTitle(KTextEditor::View *view);
};

} // namespace cv
