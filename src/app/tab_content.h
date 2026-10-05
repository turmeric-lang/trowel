#pragma once

#include <QString>
#include <QWidget>

namespace trowel {

// Abstract base for anything that can live inside a MainWindow tab.
// Concrete kinds: EditorView (editable file buffer), DirectoryView (netrw-style
// directory browser).
class TabContent : public QWidget {
    Q_OBJECT
public:
    enum class Kind { Editor, Directory };

    explicit TabContent(QWidget* parent = nullptr) : QWidget(parent) {}

    virtual Kind kind() const = 0;
    virtual QString displayName() const = 0;
    virtual QString filePath() const { return {}; }
    virtual bool isModified() const { return false; }
    virtual bool isEmpty() const { return false; }

signals:
    void displayNameChanged();
    void modifiedChanged(bool modified);
    void filePathChanged(const QString& path);
};

}
