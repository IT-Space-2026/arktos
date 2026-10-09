/*
 * Copyright 2026 The Arora Authors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA  02110-1301  USA
 */

#include "commandpalette.h"

#include "bookmarknode.h"
#include "bookmarksmanager.h"
#include "browserapplication.h"
#include "browsermainwindow.h"
#include "settings.h"
#include "tabbar.h"
#include "tabwidget.h"
#include "webview.h"

#include <qaction.h>
#include <qboxlayout.h>
#include <qevent.h>
#include <qhash.h>
#include <qlineedit.h>
#include <qlistwidget.h>
#include <qmenubar.h>
#include <qpointer.h>
#include <qsettings.h>
#include <qstyleditemdelegate.h>
#include <qstyle.h>
#include <qtimer.h>
#include <qmenu.h>
#include <qpainter.h>

namespace {

const int HintRole = Qt::UserRole + 1;

// Right-aligned hint text (shortcut, url, ...) in the muted text color;
// the main text is elided to leave the hint its room.
class PaletteDelegate : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        QStyleOptionViewItem opt = option;
        initStyleOption(&opt, index);
        const QString hint = index.data(HintRole).toString();
        if (hint.isEmpty()) {
            QStyledItemDelegate::paint(painter, opt, index);
            return;
        }
        const QFontMetrics fm(opt.font);
        const int hintWidth = fm.horizontalAdvance(hint) + 14;
        opt.text = fm.elidedText(opt.text, Qt::ElideRight,
                                 opt.rect.width() - hintWidth - 8);
        QStyledItemDelegate::paint(painter, opt, index);

        const bool selected = opt.state & QStyle::State_Selected;
        const QPalette::ColorRole role = selected
            ? QPalette::HighlightedText : QPalette::PlaceholderText;
        painter->save();
        painter->setPen(opt.palette.color(QPalette::Active, role));
        QRect hintRect = opt.rect.adjusted(0, 0, -8, 0);
        painter->drawText(hintRect, Qt::AlignRight | Qt::AlignVCenter, hint);
        painter->restore();
    }
};

// Strips the & keyboard mnemonics from action/menu text; "&&" is a
// literal ampersand.
QString cleanActionText(const QString &text)
{
    QString out;
    out.reserve(text.size());
    for (int i = 0; i < text.size(); ++i) {
        if (text.at(i) == QLatin1Char('&') && i + 1 < text.size()
            && text.at(i + 1) != QLatin1Char('&'))
            continue;
        out += text.at(i);
        if (text.at(i) == QLatin1Char('&'))
            ++i;
    }
    return out;
}

// Match-only vocabulary for commands whose real names don't carry the
// word a user would type.  Keyed by the cleaned action text.
QString commandKeywords(const QString &cleanText)
{
    static const QHash<QString, QString> aliases = {
        {QStringLiteral("Clear Private Data"),
         QStringLiteral("clear history cookies cache erase browsing data")},
        {QStringLiteral("Private Browsing..."),
         QStringLiteral("incognito private window mode")},
        {QStringLiteral("New Tor Window"),
         QStringLiteral("onion anonymous tor private")},
        {QStringLiteral("Page Source"),
         QStringLiteral("view source html markup")},
        {QStringLiteral("Web Inspector"),
         QStringLiteral("devtools developer tools inspect element")},
        {QStringLiteral("Find"),
         QStringLiteral("search in page text")},
        {QStringLiteral("Show All Bookmarks..."),
         QStringLiteral("library bookmarks manager")},
        {QStringLiteral("Options..."),
         QStringLiteral("preferences settings configure")},
        {QStringLiteral("Downloads"),
         QStringLiteral("download manager files")},
        {QStringLiteral("Reader Mode"),
         QStringLiteral("read article clutter-free")},
        {QStringLiteral("Restore Last Session"),
         QStringLiteral("reopen tabs session restore")},
        {QStringLiteral("Full Screen"),
         QStringLiteral("fullscreen f11")},
        {QStringLiteral("Zoom Text Only"),
         QStringLiteral("text zoom fonts")},
        {QStringLiteral("Ad Block..."),
         QStringLiteral("adblock advertisements filters ublock")},
    };
    return aliases.value(cleanText);
}

} // namespace

CommandPalette::CommandPalette(BrowserMainWindow *window)
    : QFrame(window, Qt::Popup | Qt::FramelessWindowHint)
    , m_window(window)
    , m_edit(new QLineEdit(this))
    , m_list(new QListWidget(this))
{
    setFrameShape(QFrame::StyledPanel);
    setFrameShadow(QFrame::Raised);

    m_edit->setPlaceholderText(tr("Type a command, tab or bookmark name..."));
    m_edit->installEventFilter(this);

    m_list->setUniformItemSizes(true);
    m_list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_list->setItemDelegate(new PaletteDelegate(m_list));

    QVBoxLayout *layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->setSpacing(4);
    layout->addWidget(m_edit);
    layout->addWidget(m_list);

    connect(m_edit, &QLineEdit::textChanged,
            this, [this](const QString &) { refilter(); });
    connect(m_list, &QListWidget::itemActivated,
            this, [this](QListWidgetItem *item) { executeItem(item); });
    connect(m_list, &QListWidget::itemClicked,
            this, [this](QListWidgetItem *item) { executeItem(item); });
}

int CommandPalette::fuzzyScore(const QString &query, const QString &candidate)
{
    const QString needle = query.toLower();
    const QString hay = candidate.toLower();
    if (needle.isEmpty())
        return 0;
    if (hay.isEmpty())
        return -1;

    int score = 0;
    int qi = 0;
    int lastMatch = -2;
    for (int i = 0; i < hay.size() && qi < needle.size(); ++i) {
        if (hay.at(i) != needle.at(qi))
            continue;
        score += 10;
        if (i == 0)
            score += 15;
        else if (!hay.at(i - 1).isLetterOrNumber())
            score += 12;   // word boundary
        if (i == lastMatch + 1)
            score += 8;    // consecutive run
        lastMatch = i;
        ++qi;
    }
    if (qi < needle.size())
        return -1;

    // A contiguous substring reads as a deliberate hit; shorter
    // candidates rank tighter.
    if (hay.contains(needle))
        score += 25;
    score -= (hay.size() - needle.size()) / 10;
    return score;
}

QStringList CommandPalette::mruIds()
{
    return QSettings().value(QLatin1String("commandPalette/mru")).toStringList();
}

void CommandPalette::recordMru(const QString &id)
{
    if (id.isEmpty())
        return;
    QStringList mru = mruIds();
    mru.removeAll(id);
    mru.prepend(id);
    while (mru.size() > 32)
        mru.removeLast();
    QSettings().setValue(QLatin1String("commandPalette/mru"), mru);
}

void CommandPalette::openPalette()
{
    open(AllItems);
}

void CommandPalette::openTabSearch()
{
    open(TabsOnly);
}

void CommandPalette::open(Mode mode)
{
    m_mode = mode;
    rebuild();
    m_edit->setPlaceholderText(mode == TabsOnly
        ? tr("Search open tabs...")
        : tr("Type a command, tab or bookmark name..."));
    m_edit->clear();
    refilter();
    updateGeometry();
    show();
    raise();
    m_edit->setFocus();
}

void CommandPalette::updateGeometry()
{
    const int rows = qBound(1, m_list->count(), 10);
    const int rowHeight = m_list->sizeHintForRow(0) > 0
        ? m_list->sizeHintForRow(0)
        : m_list->fontMetrics().height() + 6;
    const int width = qBound(420, m_window->width() * 3 / 4, 720);
    const int height = m_edit->sizeHint().height()
        + rows * rowHeight + 2 * m_list->frameWidth()
        + layout()->contentsMargins().top()
        + layout()->contentsMargins().bottom() + 12;
    setFixedSize(width, height);
    const QPoint topCenter(
        m_window->rect().center().x() - width / 2,
        qMax(20, m_window->height() / 8));
    move(m_window->mapToGlobal(topCenter));
}

void CommandPalette::collectMenuActions(const QList<QAction*> &actions,
                                        const QString &path,
                                        QSet<QAction*> *seen)
{
    for (QAction *action : actions) {
        if (seen->contains(action))
            continue;
        seen->insert(action);
        if (action->isSeparator() || !action->isVisible())
            continue;
        const QString text = cleanActionText(action->text());
        if (QMenu *menu = action->menu()) {
            collectMenuActions(menu->actions(),
                               path.isEmpty() ? text : path + QLatin1String("/") + text,
                               seen);
            // An action carrying a menu still triggers (Back/Forward
            // pull-down buttons) — keep those searchable too.  Menu
            // TITLE actions (the menu's own menuAction, e.g. "File")
            // trigger nothing, so they only contribute their path.
            if (menu->menuAction() == action)
                continue;
        }
        if (text.isEmpty())
            continue;

        Item item;
        item.text = text;
        item.category = tr("Command", "command-palette item kind");
        item.hint = action->shortcut().toString(QKeySequence::NativeText);
        item.matchText = text + QLatin1Char(' ') + path;
        const QString keywords = commandKeywords(text);
        if (!keywords.isEmpty())
            item.matchText += QLatin1Char(' ') + keywords;
        const QString tip = action->toolTip();
        if (!tip.isEmpty() && tip != text)
            item.matchText += QLatin1Char(' ') + cleanActionText(tip);
        item.id = QLatin1String("cmd:") + path + QLatin1Char('/') + text;
        item.icon = action->icon();
        item.action = action;
        m_items.append(item);
    }
}

void CommandPalette::collectCommands()
{
    QSet<QAction*> seen;
    // Menubar tree first (carries the menu path), then the actions the
    // window itself registered — those without a menu entry (Web
    // Search, Show Menu Bar) only exist on the window's action list.
    collectMenuActions(m_window->menuBar()->actions(), QString(), &seen);
    collectMenuActions(m_window->actions(), QString(), &seen);
}

void CommandPalette::collectTabs()
{
    QList<BrowserMainWindow*> windows;
    if (BrowserApplication *app = BrowserApplication::instance())
        windows = app->mainWindows();
    if (!windows.contains(m_window))
        windows.append(m_window);
    const bool multipleWindows = windows.size() > 1;

    for (BrowserMainWindow *window : windows) {
        TabWidget *tabWidget = window->tabWidget();
        if (!tabWidget)
            continue;
        for (int i = 0; i < tabWidget->count(); ++i) {
            WebView *view = tabWidget->webView(i);
            if (!view)
                continue;
            QString title = view->title();
            if (title.isEmpty())
                title = tabWidget->tabText(i);
            const QString url = view->url().toString();
            if (title.isEmpty())
                title = url.isEmpty() ? tr("New Tab") : url;

            Item item;
            item.text = title;
            item.category = tr("Tab", "command-palette item kind");
            item.hint = url;
            if (multipleWindows)
                item.hint = cleanActionText(window->windowTitle())
                    + QLatin1String(" — ") + url;
            if (tabWidget->isTabSleeping(i))
                item.hint = tr("sleeping") + QLatin1String(" — ") + item.hint;
            item.matchText = title + QLatin1Char(' ') + url;
            item.id = QLatin1String("tab:") + url;
            item.icon = view->icon();
            item.view = view;
            m_items.append(item);
        }
    }
}

void CommandPalette::collectBookmarks(BookmarkNode *node)
{
    if (!node)
        return;
    if (node->type() == BookmarkNode::Bookmark && !node->url.isEmpty()) {
        const QUrl url(node->url);
        Item item;
        item.text = node->title.isEmpty() ? node->url : node->title;
        item.category = tr("Bookmark", "command-palette item kind");
        item.hint = node->url;
        item.matchText = item.text + QLatin1Char(' ') + node->url;
        item.id = QLatin1String("bm:") + node->url;
        if (BrowserApplication::instance())
            item.icon = BrowserApplication::icon(url);
        item.url = url;
        m_items.append(item);
    }
    const QList<BookmarkNode*> children = node->children();
    for (BookmarkNode *child : children)
        collectBookmarks(child);
}

void CommandPalette::collectSettingsPages()
{
    const int count = SettingsDialog::pageCount();
    for (int i = 0; i < count; ++i) {
        const SettingsDialog::Page page = SettingsDialog::Page(i);
        const QString name = SettingsDialog::pageTitle(page);
        Item item;
        item.text = tr("Preferences: %1").arg(name);
        item.category = tr("Settings", "command-palette item kind");
        item.matchText = item.text
            + QLatin1String(" settings preferences options section ") + name;
        item.id = QLatin1String("pref:") + QString::number(i);
        item.settingsPage = i;
        m_items.append(item);
    }
}

void CommandPalette::rebuild()
{
    m_items.clear();
    if (m_mode == TabsOnly) {
        collectTabs();
        return;
    }
    collectCommands();
    collectTabs();
    collectBookmarks(BookmarksManager::instance()->bookmarks());
    collectSettingsPages();
}

void CommandPalette::refilter()
{
    const QString queryText = m_edit->text();
    const QStringList mru = mruIds();

    QList<QPair<int, int> > scored;   // (item index, score)
    scored.reserve(m_items.size());
    for (int i = 0; i < m_items.size(); ++i) {
        const Item &item = m_items.at(i);
        int score = fuzzyScore(queryText, item.matchText);
        if (score < 0)
            continue;
        const int mruIndex = mru.indexOf(item.id);
        if (mruIndex >= 0)
            score += 60 - mruIndex;
        scored.append(QPair<int, int>(i, score));
    }
    std::stable_sort(scored.begin(), scored.end(),
                     [](const QPair<int, int> &a, const QPair<int, int> &b) {
        return a.second > b.second;
    });

    m_list->clear();
    const int cap = 100;
    for (int i = 0; i < scored.size() && i < cap; ++i) {
        const Item &item = m_items.at(scored.at(i).first);
        QListWidgetItem *row = new QListWidgetItem(item.icon, item.text,
                                                 m_list);
        row->setData(Qt::UserRole, scored.at(i).first);
        row->setData(HintRole, item.hint);
        row->setToolTip(item.category + QLatin1String(" — ")
                        + (item.hint.isEmpty() ? item.text : item.hint));
        bool enabled = true;
        if (item.action)
            enabled = item.action->isEnabled();
        if (!enabled)
            row->setFlags(row->flags() & ~Qt::ItemIsEnabled);
    }
    if (m_list->count() > 0)
        m_list->setCurrentRow(0);
    updateGeometry();
}

bool CommandPalette::eventFilter(QObject *object, QEvent *event)
{
    if (object == m_edit && event->type() == QEvent::KeyPress) {
        QKeyEvent *keyEvent = static_cast<QKeyEvent*>(event);
        const int row = m_list->currentRow();
        switch (keyEvent->key()) {
        case Qt::Key_Up:
            if (row > 0)
                m_list->setCurrentRow(row - 1);
            return true;
        case Qt::Key_Down:
            if (row + 1 < m_list->count())
                m_list->setCurrentRow(row + 1);
            return true;
        case Qt::Key_PageUp:
            m_list->setCurrentRow(qMax(0, row - 10));
            return true;
        case Qt::Key_PageDown:
            m_list->setCurrentRow(qMin(m_list->count() - 1, row + 10));
            return true;
        case Qt::Key_Return:
        case Qt::Key_Enter:
            executeCurrent();
            return true;
        case Qt::Key_Escape:
            close();
            return true;
        default:
            break;
        }
    }
    return QFrame::eventFilter(object, event);
}

void CommandPalette::executeItem(QListWidgetItem *item)
{
    if (!item || !(item->flags() & Qt::ItemIsEnabled))
        return;
    const int index = item->data(Qt::UserRole).toInt();
    if (index < 0 || index >= m_items.size())
        return;
    executeItem(m_items.at(index));
}

void CommandPalette::executeItem(const Item &item)
{
    if (item.action && !item.action->isEnabled())
        return;

    recordMru(item.id);
    // Let the popup finish closing before the payload runs — several
    // commands (Private Browsing, Clear Private Data, Preferences)
    // exec() modal dialogs.
    hide();
    BrowserMainWindow *window = m_window;
    QTimer::singleShot(0, window, [window, item]() {
        if (item.action) {
            item.action->trigger();
        } else if (item.view) {
            WebView *view = item.view;
            if (BrowserMainWindow *owner =
                    BrowserMainWindow::parentWindow(view)) {
                const int index = owner->tabWidget()->webViewIndex(view);
                if (index >= 0) {
                    owner->tabWidget()->setCurrentIndex(index);
                    owner->raise();
                    owner->activateWindow();
                    view->setFocus();
                }
            }
        } else if (!item.url.isEmpty()) {
            window->tabWidget()->loadUrlFromUser(item.url, item.text);
        } else if (item.settingsPage >= 0) {
            SettingsDialog dialog(window);
            dialog.openAtPage(SettingsDialog::Page(item.settingsPage));
            dialog.exec();
        }
    });
}

void CommandPalette::setQuery(const QString &query)
{
    m_edit->setText(query);
}

QString CommandPalette::query() const
{
    return m_edit->text();
}

int CommandPalette::visibleCount() const
{
    return m_list->count();
}

int CommandPalette::currentRow() const
{
    return m_list->currentRow();
}

QString CommandPalette::itemText(int row) const
{
    QListWidgetItem *item = m_list->item(row);
    return item ? item->text() : QString();
}

QString CommandPalette::itemCategory(int row) const
{
    QListWidgetItem *item = m_list->item(row);
    if (!item)
        return QString();
    const int index = item->data(Qt::UserRole).toInt();
    if (index < 0 || index >= m_items.size())
        return QString();
    return m_items.at(index).category;
}

bool CommandPalette::executeRow(int row)
{
    QListWidgetItem *item = m_list->item(row);
    if (!item)
        return false;
    executeItem(item);
    return true;
}

bool CommandPalette::executeCurrent()
{
    return executeRow(m_list->currentRow());
}
