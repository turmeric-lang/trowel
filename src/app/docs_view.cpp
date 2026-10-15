#include "app/docs_view.h"

#include <QApplication>
#include <QComboBox>
#include <QDesktopServices>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QPalette>
#include <QRegularExpression>
#include <QScrollBar>
#include <QSplitter>
#include <QTextBrowser>
#include <QTextDocument>
#include <QToolButton>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>

namespace trowel {

namespace {

constexpr int kPathRole = Qt::UserRole;
constexpr int kRecentCount = 10;
const char* kQuickstart = "guides/quickstart.html";

QString Esc(const QString& s) { return s.toHtmlEscaped(); }

// The pack's code blocks carry BOTH syntaxes and rely on a script to hide one.
// QTextBrowser runs no script, so choose here.
QString PickCodeVariant(QString html, const QString& syntax) {
    // The toggle bar is dead chrome without its script.
    static const QRegularExpression bar(
        R"(<div class="code-card-bar">.*?</div></div>)",
        QRegularExpression::DotMatchesEverythingOption);
    html.remove(bar);

    const QString drop = syntax == "sweet-exp" ? "turmeric-version" : "sweet-exp-version";
    const QRegularExpression variant(
        QStringLiteral(R"(<div class="code-version %1"[^>]*>.*?</pre></div>)").arg(drop),
        QRegularExpression::DotMatchesEverythingOption);
    html.remove(variant);

    static const QRegularExpression controls(R"(<(button|input)\b[^>]*>(.*?</button>)?)",
                                             QRegularExpression::DotMatchesEverythingOption);
    html.remove(controls);
    return html;
}

QString ArticleStyle() {
    const QPalette p = QApplication::palette();
    const QString fg = p.color(QPalette::Text).name();
    const QString link = p.color(QPalette::Link).name();
    const QString code = p.color(QPalette::AlternateBase).name();
    const QString mid = p.color(QPalette::Mid).name();
    return QStringLiteral(
        "body { color:%1; }"
        "a { color:%2; }"
        "h1 { margin-top:0; }"
        "h2 { margin-top:1.4em; }"
        "pre { background-color:%3; font-family:Menlo,monospace; padding:6px; }"
        "code { font-family:Menlo,monospace; background-color:%3; }"
        "table { border-collapse:collapse; }"
        "td, th { padding:3px 8px; border:1px solid %4; }"
        ".module-path, .def-since { color:%4; }"
        ".def-section-label { font-weight:bold; margin-top:8px; }"
        ".kind-badge { font-size:small; color:%4; }"
        ".def-card { margin-top:14px; }"
        ".home-meta { color:%4; }"
        ".summary { font-size:large; }"
    ).arg(fg, link, code, mid);
}

}

DocsView::DocsView(QWidget* parent) : TabContent(parent) {
    auto* outer = new QHBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    auto* split = new QSplitter(Qt::Horizontal, this);
    outer->addWidget(split);

    // Left: search + nav.
    auto* left = new QWidget(split);
    auto* leftLay = new QVBoxLayout(left);
    leftLay->setContentsMargins(6, 6, 0, 6);
    search_ = new QLineEdit(left);
    search_->setPlaceholderText("Search guides and API…");
    search_->setClearButtonEnabled(true);
    leftLay->addWidget(search_);
    nav_ = new QTreeWidget(left);
    nav_->setHeaderHidden(true);
    nav_->setUniformRowHeights(true);
    leftLay->addWidget(nav_, 1);

    // Right: toolbar + article.
    auto* right = new QWidget(split);
    auto* rightLay = new QVBoxLayout(right);
    rightLay->setContentsMargins(0, 6, 6, 6);
    auto* bar = new QHBoxLayout;
    backButton_ = new QToolButton(right);
    backButton_->setText("‹");
    backButton_->setToolTip("Back");
    forwardButton_ = new QToolButton(right);
    forwardButton_->setText("›");
    forwardButton_->setToolTip("Forward");
    homeButton_ = new QToolButton(right);
    homeButton_->setText("Home");
    syntaxBox_ = new QComboBox(right);
    syntaxBox_->addItem("Turmeric code", "turmeric");
    syntaxBox_->addItem("Sweet-exp code", "sweet-exp");
    bar->addWidget(backButton_);
    bar->addWidget(forwardButton_);
    bar->addWidget(homeButton_);
    bar->addStretch(1);
    bar->addWidget(syntaxBox_);
    rightLay->addLayout(bar);
    browser_ = new QTextBrowser(right);
    browser_->setOpenLinks(false);
    browser_->document()->setDefaultStyleSheet(ArticleStyle());
    rightLay->addWidget(browser_, 1);

    split->addWidget(left);
    split->addWidget(right);
    split->setStretchFactor(0, 0);
    split->setStretchFactor(1, 1);
    split->setSizes({260, 740});

    connect(browser_, &QTextBrowser::anchorClicked, this, &DocsView::onAnchorClicked);
    connect(nav_, &QTreeWidget::currentItemChanged, this,
            [this](QTreeWidgetItem* cur, QTreeWidgetItem*) { onNavCurrentChanged(cur); });
    connect(search_, &QLineEdit::textChanged, this, &DocsView::onQueryChanged);
    connect(search_, &QLineEdit::returnPressed, this, [this] {
        // Enter opens the best hit, which is the first row.
        if (QTreeWidgetItem* first = nav_->topLevelItem(0))
            onNavCurrentChanged(first);
    });
    connect(backButton_, &QToolButton::clicked, this, &DocsView::goBack);
    connect(forwardButton_, &QToolButton::clicked, this, &DocsView::goForward);
    connect(homeButton_, &QToolButton::clicked, this, &DocsView::showHome);
    connect(syntaxBox_, &QComboBox::currentIndexChanged, this, [this](int) {
        // Re-render in place; the choice is a view setting, not navigation.
        if (historyPos_ >= 0) render(history_[historyPos_], /*pushHistory=*/false);
    });
    updateHistoryButtons();
}

bool DocsView::loadPack(const QString& dir) {
    if (!pack_.load(dir)) return false;
    recent_.clear();
    for (const DocsEntry* e : pack_.recentlyAdded(kRecentCount)) recent_ << e->path;
    buildNav();
    showHome();
    return true;
}

void DocsView::buildNav() {
    nav_->clear();
    const struct { DocsEntry::Section sec; const char* label; } sections[] = {
        {DocsEntry::Section::Guide, "Guides"},
        {DocsEntry::Section::Api, "API Reference"},
    };
    for (const auto& s : sections) {
        auto* top = new QTreeWidgetItem(nav_, {QString::fromLatin1(s.label)});
        QFont f = top->font(0);
        f.setBold(true);
        top->setFont(0, f);
        for (const QString& cat : pack_.categories(s.sec)) {
            auto* catItem = new QTreeWidgetItem(top, {cat});
            for (const DocsEntry& e : pack_.entries()) {
                if (e.section != s.sec || e.category != cat) continue;
                auto* it = new QTreeWidgetItem(catItem, {e.title});
                it->setData(0, kPathRole, e.path);
            }
        }
        // Guides open on their first level so the nav isn't a blank wall;
        // the API list is long and a lookup tool, so it starts folded.
        if (s.sec == DocsEntry::Section::Guide) top->setExpanded(true);
    }
}

void DocsView::fillSearchResults(const QString& query) {
    nav_->clear();
    const auto hits = pack_.search(query);
    for (const DocsEntry* e : hits) {
        const QString tag = e->section == DocsEntry::Section::Api ? "API" : e->category;
        auto* it = new QTreeWidgetItem(nav_, {QStringLiteral("%1  —  %2").arg(e->title, tag)});
        it->setData(0, kPathRole, e->path);
    }
    if (hits.isEmpty()) {
        auto* none = new QTreeWidgetItem(nav_, {"No matches"});
        none->setFlags(Qt::NoItemFlags);
    }
}

void DocsView::onQueryChanged() {
    const QString q = search_->text().simplified();
    syncingNav_ = true;
    if (q.isEmpty()) {
        buildNav();
        if (!currentPath_.isEmpty()) selectNavItem(currentPath_);
    } else {
        fillSearchResults(q);
    }
    syncingNav_ = false;
}

void DocsView::onNavCurrentChanged(QTreeWidgetItem* item) {
    if (syncingNav_ || !item) return;
    const QString path = item->data(0, kPathRole).toString();
    if (path.isEmpty()) return;  // a section or category row
    showArticle(path);
}

void DocsView::selectNavItem(const QString& path) {
    QTreeWidgetItemIterator it(nav_);
    while (*it) {
        if ((*it)->data(0, kPathRole).toString() == path) {
            syncingNav_ = true;
            nav_->setCurrentItem(*it);
            nav_->scrollToItem(*it);
            syncingNav_ = false;
            return;
        }
        ++it;
    }
}

void DocsView::setQuery(const QString& query) { search_->setText(query); }
QString DocsView::query() const { return search_->text(); }

QStringList DocsView::resultPaths() const {
    QStringList out;
    QTreeWidgetItemIterator it(nav_);
    while (*it) {
        const QString p = (*it)->data(0, kPathRole).toString();
        if (!p.isEmpty()) out << p;
        ++it;
    }
    return out;
}

QStringList DocsView::recentPaths() const { return recent_; }
QString DocsView::plainText() const { return browser_->toPlainText(); }
QString DocsView::codeSyntax() const { return syntaxBox_->currentData().toString(); }

void DocsView::setCodeSyntax(const QString& syntax) {
    const int i = syntaxBox_->findData(syntax);
    if (i >= 0) syntaxBox_->setCurrentIndex(i);
}

void DocsView::showHome() {
    currentPath_.clear();
    render({QString(), QString()}, /*pushHistory=*/true);
}

bool DocsView::showArticle(const QString& pathOrSlug, const QString& anchor) {
    const DocsEntry* e = pack_.find(pathOrSlug);
    if (!e) return false;
    render({e->path, anchor}, /*pushHistory=*/true);
    return true;
}

void DocsView::render(const Location& loc, bool pushHistory) {
    const DocsEntry* e = loc.path.isEmpty() ? nullptr : pack_.find(loc.path);
    if (!loc.path.isEmpty() && !e) return;

    if (pushHistory) {
        // Navigating after going back discards the forward branch, as a browser does.
        const bool same = historyPos_ >= 0 && history_[historyPos_].path == loc.path
                          && history_[historyPos_].anchor == loc.anchor;
        if (!same) {
            history_.resize(historyPos_ + 1);
            history_.append(loc);
            historyPos_ = history_.size() - 1;
        }
    }

    currentPath_ = loc.path;
    browser_->setHtml(e ? articleHtml(*e) : homeHtml());
    if (!loc.anchor.isEmpty()) browser_->scrollToAnchor(loc.anchor);
    else browser_->verticalScrollBar()->setValue(0);

    if (e && search_->text().isEmpty()) selectNavItem(e->path);
    updateHistoryButtons();
    emit displayNameChanged();
}

QString DocsView::articleHtml(const DocsEntry& entry) const {
    const QString body = pack_.articleHtml(entry);
    if (body.isEmpty())
        return QStringLiteral("<p>Could not read <code>%1</code> from the docs pack.</p>")
            .arg(Esc(entry.path));
    return PickCodeVariant(body, codeSyntax());
}

QString DocsView::homeHtml() const {
    QString h;
    h += "<h1>Turmeric Documentation</h1>";
    int guides = 0, modules = 0;
    for (const DocsEntry& e : pack_.entries())
        (e.section == DocsEntry::Section::Guide ? guides : modules)++;
    h += QStringLiteral("<p class='home-meta'>Version %1 · %2 guides, %3 API modules</p>")
             .arg(Esc(pack_.version())).arg(guides).arg(modules);

    if (const DocsEntry* q = pack_.find(kQuickstart)) {
        h += QStringLiteral("<h2>Start here</h2><p class='summary'><a href='%1'>%2</a></p><p>%3</p>")
                 .arg(Esc(q->path), Esc(q->title), Esc(q->description));
    }

    h += "<h2>Recently Added</h2><ul>";
    for (const QString& path : recent_) {
        const DocsEntry* e = pack_.find(path);
        if (!e) continue;
        const QString kind = e->section == DocsEntry::Section::Api ? "API" : e->category;
        h += QStringLiteral("<li><a href='%1'>%2</a> <span class='home-meta'>%3 · %4</span></li>")
                 .arg(Esc(e->path), Esc(e->title), Esc(kind), Esc(e->added));
    }
    h += "</ul>";

    h += "<h2>Browse</h2>";
    for (const QString& cat : pack_.categories(DocsEntry::Section::Guide)) {
        h += QStringLiteral("<p><b>%1</b><br>").arg(Esc(cat));
        QStringList links;
        for (const DocsEntry& e : pack_.entries())
            if (e.section == DocsEntry::Section::Guide && e.category == cat)
                links << QStringLiteral("<a href='%1'>%2</a>").arg(Esc(e.path), Esc(e.title));
        h += links.join(" · ") + "</p>";
    }
    return h;
}

void DocsView::onAnchorClicked(const QUrl& url) {
    const QString href = url.toString();
    if (url.scheme() == "http" || url.scheme() == "https" || url.scheme() == "mailto") {
        QDesktopServices::openUrl(url);
        return;
    }
    // "#frag" alone stays in the article; resolveLink returns the current path.
    QString anchor;
    const int hash = href.indexOf('#');
    if (hash >= 0) anchor = href.mid(hash + 1);

    const QString target = DocsPack::resolveLink(currentPath_, href);
    if (target.isEmpty()) return;
    if (target == currentPath_) {
        if (!anchor.isEmpty()) browser_->scrollToAnchor(anchor);
        return;
    }
    showArticle(target, anchor);
}

void DocsView::goBack() {
    if (historyPos_ <= 0) return;
    --historyPos_;
    render(history_[historyPos_], /*pushHistory=*/false);
}

void DocsView::goForward() {
    if (historyPos_ + 1 >= history_.size()) return;
    ++historyPos_;
    render(history_[historyPos_], /*pushHistory=*/false);
}

void DocsView::updateHistoryButtons() {
    backButton_->setEnabled(historyPos_ > 0);
    forwardButton_->setEnabled(historyPos_ + 1 < history_.size());
}

}
