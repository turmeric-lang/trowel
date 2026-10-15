#pragma once

#include "app/docs_pack.h"
#include "app/tab_content.h"

#include <QString>
#include <QStringList>
#include <QVector>

class QComboBox;
class QLineEdit;
class QTextBrowser;
class QToolButton;
class QTreeWidget;
class QTreeWidgetItem;
class QUrl;

namespace trowel {

// The Turmeric guides and API reference, rendered from the docs pack that ships
// beside the toolchain. Leads on a quickstart link and a "Recently Added" list;
// a search box filters the nav tree down to ranked results.
class DocsView : public TabContent {
    Q_OBJECT
public:
    explicit DocsView(QWidget* parent = nullptr);

    // Load the pack at `dir`. False (and error()) when it isn't one.
    bool loadPack(const QString& dir);
    QString error() const { return pack_.error(); }
    const DocsPack& pack() const { return pack_; }

    // The start page: quickstart, recently added, browse-by-category.
    void showHome();
    // Show a guide or api page by pack path ("guides/quickstart.html") or
    // slug ("quickstart"); `anchor` scrolls to an id inside it.
    bool showArticle(const QString& pathOrSlug, const QString& anchor = {});
    // Fill the search box (which is what filters the nav).
    void setQuery(const QString& query);

    QString currentPath() const { return currentPath_; }  // "" on the home page
    QString query() const;
    QStringList resultPaths() const;      // nav rows currently listed
    QStringList recentPaths() const;      // the home page's Recently Added
    QString plainText() const;            // the article pane, as text
    // Which code variant the articles show: "turmeric" or "sweet-exp".
    QString codeSyntax() const;
    void setCodeSyntax(const QString& syntax);

    Kind kind() const override { return Kind::Docs; }
    QString displayName() const override { return QStringLiteral("Turmeric Docs"); }

private slots:
    void onAnchorClicked(const QUrl& url);
    void onNavCurrentChanged(QTreeWidgetItem* item);
    void onQueryChanged();
    void goBack();
    void goForward();

private:
    struct Location { QString path; QString anchor; };

    void buildNav();
    void fillSearchResults(const QString& query);
    void render(const Location& loc, bool pushHistory);
    QString homeHtml() const;
    QString articleHtml(const DocsEntry& entry) const;
    void updateHistoryButtons();
    void selectNavItem(const QString& path);

    DocsPack pack_;
    QLineEdit* search_ = nullptr;
    QTreeWidget* nav_ = nullptr;
    QTextBrowser* browser_ = nullptr;
    QToolButton* backButton_ = nullptr;
    QToolButton* forwardButton_ = nullptr;
    QToolButton* homeButton_ = nullptr;
    QComboBox* syntaxBox_ = nullptr;

    QString currentPath_;
    QVector<Location> history_;
    int historyPos_ = -1;
    bool syncingNav_ = false;  // a programmatic nav selection must not re-render
    QStringList recent_;
};

}
