#pragma once

#include <QWidget>

class QLineEdit;
class QPushButton;
class QCheckBox;
class QLabel;

namespace trowel {

// A find/replace bar that sits at the bottom of the editor pane.
// The bar is a widget; EditorView owns it and connects signals.
class FindBar : public QWidget {
    Q_OBJECT
public:
    explicit FindBar(QWidget* parent = nullptr);

    void setReplaceMode(bool replace);
    bool replaceMode() const { return replaceMode_; }

    QString query() const;
    void setQuery(const QString& text);

    bool matchCase() const;
    bool wholeWord() const;
    bool regex() const;
    bool inSelection() const;

    void setFlags(bool matchCase, bool wholeWord, bool regex, bool inSelection);

    void setMatchCount(int current, int total);
    void setError(const QString& error);

    void open(bool replace);
    void closeBar();

    QLineEdit* queryField() const { return queryEdit_; }
    QLineEdit* replaceField() const { return replaceEdit_; }

signals:
    void searchChanged();       // query or flags changed
    void findNextRequested();
    void findPreviousRequested();
    void replaceRequested();
    void replaceAllRequested();
    void closed();
    void selectAllOccurrencesRequested();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    QLineEdit* queryEdit_ = nullptr;
    QLineEdit* replaceEdit_ = nullptr;
    QCheckBox* matchCaseCheck_ = nullptr;
    QCheckBox* wholeWordCheck_ = nullptr;
    QCheckBox* regexCheck_ = nullptr;
    QCheckBox* inSelectionCheck_ = nullptr;
    QLabel* countLabel_ = nullptr;
    QPushButton* nextBtn_ = nullptr;
    QPushButton* prevBtn_ = nullptr;
    QPushButton* replaceBtn_ = nullptr;
    QPushButton* replaceAllBtn_ = nullptr;
    QPushButton* closeBtn_ = nullptr;
    QWidget* replaceRow_ = nullptr;

    bool replaceMode_ = false;
};

}  // namespace trowel
