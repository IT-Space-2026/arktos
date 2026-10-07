/**
 * Copyright (c) 2009, Benjamin C. Meyer <ben@meyerhome.net>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the Benjamin Meyer nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include "adblockdialog.h"

#include "adblockmanager.h"
#include "adblockpresetsdialog.h"
#include "adblockrule.h"
#include "adblocksubscription.h"
#include "aroraicon.h"
#include "safetext.h"
#include "searchlineedit.h"

#include <qdesktopservices.h>
#include <qfontdatabase.h>
#include <qheaderview.h>
#include <qlocale.h>
#include <qmenu.h>
#include <qtextcursor.h>
#include <qtimer.h>
#include <qtreewidget.h>
#include <qurl.h>

// Debounce for the custom-rules text pane: committing emits
// rulesChanged, which rebuilds the matcher's whole rule snapshot —
// far too expensive per keystroke on a populated EasyList.  The text
// is committed 600ms after typing stops and unconditionally when the
// dialog closes, so edits apply live like the old per-row editing.
// "Dirty" is a diff against m_customCommittedText, not the document's
// own modified flag (setPlainText resets that flag even when the new
// text differs).
static const int CustomRulesCommitDelayMs = 600;

AdBlockDialog::AdBlockDialog(QWidget *parent)
    : QDialog(parent)
    , m_customRules(nullptr)
    , m_customCommitTimer(nullptr)
    , m_refreshing(false)
{
    setupUi(this);

    AdBlockManager *manager = AdBlockManager::instance();
    adblockCheckBox->setChecked(manager->isEnabled());
    connect(adblockCheckBox, &QCheckBox::toggled,
            manager, &AdBlockManager::setEnabled);

    // One checkable row per subscription; remote lists are never
    // expanded into per-rule rows (a 60k-rule list was unusable and
    // slow, and the comment/metadata lines were never toggleable).
    // Titles and locations come from abp:subscribe urls — page-
    // controlled — so the tree paints them literally.
    subscriptionsTree->setItemDelegate(
        new PlainTextItemDelegate(subscriptionsTree));
    subscriptionsTree->header()->setSectionResizeMode(
        0, QHeaderView::Stretch);
    connect(manager, &AdBlockManager::rulesChanged,
            this, &AdBlockDialog::repopulateSubscriptions);
    connect(subscriptionsTree, &QTreeWidget::itemChanged,
            this, &AdBlockDialog::subscriptionItemChanged);
    connect(subscriptionsTree, &QTreeWidget::itemSelectionChanged,
            this, &AdBlockDialog::subscriptionSelectionChanged);
    connect(updateSubscriptionButton, &QPushButton::clicked,
            this, &AdBlockDialog::updateSubscription);
    connect(removeSubscriptionButton, &QPushButton::clicked,
            this, &AdBlockDialog::removeSubscription);

    // The rules viewer is opt-in — walking a remote list is the cost
    // the flat subscription table exists to avoid.
    const QFont fixed = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    rulesView->setFont(fixed);
    customRulesEdit->setFont(fixed);
    connect(rulesGroup, &QGroupBox::toggled,
            this, [this](bool) { fillRulesView(); });
    connect(search, &SearchLineEdit::textChanged,
            this, [this](const QString &) { fillRulesView(); });

    QMenu *menu = new QMenu(this);
    connect(menu, &QMenu::aboutToShow,
            this, &AdBlockDialog::aboutToShowActionMenu);
    actionToolButton->setMenu(menu);
    actionToolButton->setIcon(AroraIcon::get(QLatin1String("system-run")));
    actionToolButton->setPopupMode(QToolButton::InstantPopup);

    m_customCommitTimer = new QTimer(this);
    m_customCommitTimer->setSingleShot(true);
    m_customCommitTimer->setInterval(CustomRulesCommitDelayMs);
    connect(m_customCommitTimer, &QTimer::timeout,
            this, &AdBlockDialog::commitCustomRules);
    connect(customRulesEdit, &QPlainTextEdit::textChanged,
            this, &AdBlockDialog::customRulesTextChanged);

    // Loading the custom rules first means its rulesChanged reaches
    // the initial repopulate below as well.
    loadCustomRulesText();
    repopulateSubscriptions();
}

void AdBlockDialog::aboutToShowActionMenu()
{
    QMenu *menu = actionToolButton->menu();
    menu->clear();

    QAction *addRule = menu->addAction(tr("Add Custom Rule"));
    connect(addRule, &QAction::triggered,
            this, &AdBlockDialog::focusCustomRules);

    QAction *learnRule = menu->addAction(tr("Learn more about writing rules..."));
    connect(learnRule, &QAction::triggered, this, &AdBlockDialog::learnAboutWritingFilters);

    menu->addSeparator();

    AdBlockSubscription *subscription = selectedSubscription();
    const bool remote = subscription
        && subscription != AdBlockManager::instance()->customRules();

    QAction *updateSubscription = menu->addAction(tr("Update Subscription"));
    connect(updateSubscription, &QAction::triggered, this, &AdBlockDialog::updateSubscription);
    updateSubscription->setEnabled(remote);

    QAction *presets = menu->addAction(tr("Preset Filter Lists..."));
    connect(presets, &QAction::triggered, this, &AdBlockDialog::showPresets);

    QAction *addSubscription = menu->addAction(tr("Browse Subscriptions..."));
    connect(addSubscription, &QAction::triggered, this, &AdBlockDialog::browseSubscriptions);

    menu->addSeparator();

    QAction *removeSubscription = menu->addAction(tr("Remove Subscription"));
    connect(removeSubscription, &QAction::triggered, this, &AdBlockDialog::removeSubscription);
    removeSubscription->setEnabled(remote);
}

void AdBlockDialog::addCustomRule(const QString &rule)
{
    // Commit any pending edit first — the new rule would otherwise be
    // clobbered when the text pane is later written back.
    commitCustomRules();
    AdBlockSubscription *subscription = customSubscription();
    subscription->addRule(AdBlockRule(rule));
    loadCustomRulesText();
    focusCustomRules();
}

void AdBlockDialog::done(int result)
{
    // OK, window close and reject all land here — a pending custom-
    // rules edit must never be silently dropped.
    m_customCommitTimer->stop();
    commitCustomRules();
    QDialog::done(result);
}

void AdBlockDialog::updateSubscription()
{
    AdBlockSubscription *subscription = selectedSubscription();
    if (!subscription)
        return;
    // Pressing Update is explicit consent to download remote lists.
    AdBlockManager::setRemoteListsConsent(AdBlockManager::RemoteListsGranted);
    subscription->updateNow();
}

void AdBlockDialog::browseSubscriptions()
{
    QUrl url(QLatin1String("http://adblockplus.org/en/subscriptions"));
    QDesktopServices::openUrl(url);
}

// ADB03: the curated community-list catalog; ticking a preset
// subscribes + enables it (with the user's consent to fetch remote
// lists), unticking disables it.
void AdBlockDialog::showPresets()
{
    AdBlockPresetsDialog dialog(this);
    dialog.exec();
}

void AdBlockDialog::learnAboutWritingFilters()
{
    QUrl url(QLatin1String("http://adblockplus.org/en/filters"));
    QDesktopServices::openUrl(url);
}

void AdBlockDialog::removeSubscription()
{
    AdBlockSubscription *subscription = selectedSubscription();
    if (!subscription)
        return;
    AdBlockManager::instance()->removeSubscription(subscription);
}

void AdBlockDialog::repopulateSubscriptions()
{
    if (m_refreshing)
        return;
    m_refreshing = true;

    AdBlockSubscription *selected = selectedSubscription();
    AdBlockManager *manager = AdBlockManager::instance();
    subscriptionsTree->clear();
    const QLocale locale;
    const QList<AdBlockSubscription*> subscriptions = manager->subscriptions();
    for (AdBlockSubscription *subscription : subscriptions) {
        if (!subscription)
            continue;
        QTreeWidgetItem *item = new QTreeWidgetItem(subscriptionsTree);
        item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable
                       | Qt::ItemIsUserCheckable);
        item->setCheckState(0, subscription->isEnabled()
                            ? Qt::Checked : Qt::Unchecked);
        item->setText(0, subscription->title());
        item->setText(1, locale.toString(subscription->ruleCount()));
        item->setTextAlignment(1, Qt::AlignRight | Qt::AlignVCenter);
        item->setText(2, subscription->lastUpdate().isValid()
                      ? locale.toString(subscription->lastUpdate(),
                                        QLocale::ShortFormat)
                      : tr("Never"));
        const QString tip = tr("%1\n%2").arg(
            subscription->title(), subscription->location().toString());
        item->setToolTip(0, SafeText::escaped(tip));
        item->setStatusTip(0, SafeText::escaped(tip));
        item->setData(0, Qt::UserRole, QVariant::fromValue(
            reinterpret_cast<qintptr>(subscription)));
        if (subscription == selected)
            subscriptionsTree->setCurrentItem(item);
    }
    m_refreshing = false;
    subscriptionSelectionChanged();
}

AdBlockSubscription *AdBlockDialog::selectedSubscription() const
{
    QTreeWidgetItem *item = subscriptionsTree->currentItem();
    if (!item)
        return nullptr;
    return reinterpret_cast<AdBlockSubscription*>(static_cast<qintptr>(
        item->data(0, Qt::UserRole).value<qintptr>()));
}

void AdBlockDialog::selectSubscription(AdBlockSubscription *subscription)
{
    for (int i = 0; i < subscriptionsTree->topLevelItemCount(); ++i) {
        QTreeWidgetItem *item = subscriptionsTree->topLevelItem(i);
        if (reinterpret_cast<AdBlockSubscription*>(static_cast<qintptr>(
                item->data(0, Qt::UserRole).value<qintptr>()))
            == subscription) {
            subscriptionsTree->setCurrentItem(item);
            subscriptionsTree->scrollToItem(item);
            return;
        }
    }
}

void AdBlockDialog::subscriptionItemChanged(QTreeWidgetItem *item,
                                            int column)
{
    if (m_refreshing || column != 0 || !item)
        return;
    AdBlockSubscription *subscription =
        reinterpret_cast<AdBlockSubscription*>(static_cast<qintptr>(
            item->data(0, Qt::UserRole).value<qintptr>()));
    if (!subscription)
        return;
    // setEnabled emits rulesChanged; m_refreshing keeps the repopulate
    // it triggers from clearing the row underneath this handler.
    m_refreshing = true;
    subscription->setEnabled(item->checkState(0) == Qt::Checked);
    m_refreshing = false;
}

void AdBlockDialog::subscriptionSelectionChanged()
{
    AdBlockSubscription *subscription = selectedSubscription();
    // The custom-rules list is file-backed, is edited through the text
    // pane below, and is recreated on demand — updating it would only
    // re-read its own file and removing it is meaningless.
    const bool remote = subscription
        && subscription != AdBlockManager::instance()->customRules();
    updateSubscriptionButton->setEnabled(remote);
    removeSubscriptionButton->setEnabled(remote);
    fillRulesView();
}

void AdBlockDialog::fillRulesView()
{
    rulesView->clear();
    if (!rulesGroup->isChecked())
        return;
    AdBlockSubscription *subscription = selectedSubscription();
    if (!subscription)
        return;
    QStringList filters = subscription->ruleFilters();
    const QString needle = search->text();
    if (!needle.isEmpty())
        filters = filters.filter(needle, Qt::CaseInsensitive);
    rulesView->setPlainText(filters.join(QLatin1Char('\n')));
}

AdBlockSubscription *AdBlockDialog::customSubscription()
{
    AdBlockSubscription *subscription =
        AdBlockManager::instance()->customRules();
    if (subscription != m_customRules) {
        m_customRules = subscription;
        connect(subscription, &AdBlockSubscription::rulesChanged,
                this, &AdBlockDialog::customRulesChangedExternally,
                Qt::UniqueConnection);
    }
    return subscription;
}

void AdBlockDialog::loadCustomRulesText()
{
    const QStringList filters = customSubscription()->ruleFilters();
    m_customCommittedText = filters.join(QLatin1Char('\n'));
    customRulesEdit->setPlainText(m_customCommittedText);
}

void AdBlockDialog::customRulesTextChanged()
{
    if (customRulesEdit->toPlainText() == m_customCommittedText)
        return;
    m_customCommitTimer->start();
}

void AdBlockDialog::commitCustomRules()
{
    const QString text = customRulesEdit->toPlainText();
    if (text == m_customCommittedText)
        return;
    QList<AdBlockRule> rules;
    if (!text.isEmpty()) {
        // Lines are preserved verbatim — comments (! ...) and blanks
        // round-trip through the rules file just like a hand-edited
        // list.  The parser keeps inert lines harmless.
        const QStringList lines = text.split(QLatin1Char('\n'));
        rules.reserve(lines.count());
        for (const QString &line : lines)
            rules.append(AdBlockRule(line));
    }
    customSubscription()->setRules(rules);
    m_customCommittedText = text;
}

void AdBlockDialog::customRulesChangedExternally()
{
    // An in-flight edit wins: reloading now would discard the user's
    // text, and committing it writes the pane wholesale anyway.  The
    // shield panel / context-menu paths append through addCustomRule,
    // which commits first, so this only matters for external edits.
    if (customRulesEdit->toPlainText() != m_customCommittedText)
        return;
    loadCustomRulesText();
}

void AdBlockDialog::focusCustomRules()
{
    customRulesEdit->setFocus();
    QTextCursor cursor = customRulesEdit->textCursor();
    cursor.movePosition(QTextCursor::End);
    customRulesEdit->setTextCursor(cursor);
}
