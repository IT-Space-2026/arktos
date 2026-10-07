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

#include "adblockmodel.h"
#include "adblockmanager.h"
#include "adblocksubscription.h"
#include "aroraicon.h"
#include "treesortfilterproxymodel.h"

#include <qdesktopservices.h>
#include <qmenu.h>
#include <qurl.h>

#include <qdebug.h>

AdBlockDialog::AdBlockDialog(QWidget *parent)
    : QDialog(parent)
{
    setupUi(this);
    m_adBlockModel = new AdBlockModel(this);
    m_proxyModel = new TreeSortFilterProxyModel(this);
    m_proxyModel->setSourceModel(m_adBlockModel);
    treeView->setModel(m_proxyModel);
    connect(search, &SearchLineEdit::textChanged,
            m_proxyModel, &TreeSortFilterProxyModel::setFilterFixedString);

    AdBlockManager *manager = AdBlockManager::instance();
    adblockCheckBox->setChecked(manager->isEnabled());
    connect(adblockCheckBox, &QCheckBox::toggled,
            AdBlockManager::instance(), &AdBlockManager::setEnabled);

    QMenu *menu = new QMenu(this);
    connect(menu, &QMenu::aboutToShow,
            this, &AdBlockDialog::aboutToShowActionMenu);
    actionToolButton->setMenu(menu);
    actionToolButton->setIcon(AroraIcon::get(QLatin1String("system-run")));
    actionToolButton->setPopupMode(QToolButton::InstantPopup);

    AdBlockSubscription *subscription = manager->customRules();
    QModelIndex subscriptionIndex = m_adBlockModel->index(subscription);
    treeView->expand(m_proxyModel->mapFromSource(subscriptionIndex));
}

void AdBlockDialog::aboutToShowActionMenu()
{
    QMenu *menu = actionToolButton->menu();
    menu->clear();

    QAction *addRule = menu->addAction(tr("Add Custom Rule"));
    connect(addRule, &QAction::triggered, this, [this]() { addCustomRule(); });

    QAction *learnRule = menu->addAction(tr("Learn more about writing rules..."));
    connect(learnRule, &QAction::triggered, this, &AdBlockDialog::learnAboutWritingFilters);

    menu->addSeparator();

    QModelIndex idx = m_proxyModel->mapToSource(treeView->currentIndex());

    QAction *updateSubscription = menu->addAction(tr("Update Subscription"));
    connect(updateSubscription, &QAction::triggered, this, &AdBlockDialog::updateSubscription);
    if (!idx.isValid())
        updateSubscription->setEnabled(false);

    QAction *addSubscription = menu->addAction(tr("Browse Subscriptions..."));
    connect(addSubscription, &QAction::triggered, this, &AdBlockDialog::browseSubscriptions);

    menu->addSeparator();

    QAction *removeSubscription = menu->addAction(tr("Remove Subscription"));
    connect(removeSubscription, &QAction::triggered, this, &AdBlockDialog::removeSubscription);
    if (!idx.isValid())
        removeSubscription->setEnabled(false);
}

void AdBlockDialog::addCustomRule(const QString &rule)
{
    AdBlockManager *manager = AdBlockManager::instance();
    AdBlockSubscription *subscription = manager->customRules();
    Q_ASSERT(subscription);
    // addRule emits rulesChanged synchronously, which resets the
    // model through AdBlockManager — no processEvents() needed (it
    // would also let arbitrary handlers re-enter this slot).
    subscription->addRule(AdBlockRule(rule));

    QModelIndex parent = m_adBlockModel->index(subscription);
    int x = m_adBlockModel->rowCount(parent);
    QModelIndex ruleIndex = m_adBlockModel->index(x - 1, 0, parent);
    treeView->expand(m_proxyModel->mapFromSource(parent));
    treeView->edit(m_proxyModel->mapFromSource(ruleIndex));
}

void AdBlockDialog::updateSubscription()
{
    QModelIndex idx = m_proxyModel->mapToSource(treeView->currentIndex());
    if (!idx.isValid())
        return;
    if (idx.parent().isValid())
        idx = idx.parent();
    AdBlockSubscription *subscription = (AdBlockSubscription*)m_adBlockModel->subscription(idx);
    // Pressing Update is explicit consent to download remote lists.
    AdBlockManager::setRemoteListsConsent(AdBlockManager::RemoteListsGranted);
    subscription->updateNow();
}

void AdBlockDialog::browseSubscriptions()
{
    QUrl url(QLatin1String("http://adblockplus.org/en/subscriptions"));
    QDesktopServices::openUrl(url);
}

void AdBlockDialog::learnAboutWritingFilters()
{
    QUrl url(QLatin1String("http://adblockplus.org/en/filters"));
    QDesktopServices::openUrl(url);
}

void AdBlockDialog::removeSubscription()
{
    QModelIndex idx = m_proxyModel->mapToSource(treeView->currentIndex());
    if (!idx.isValid())
        return;
    if (idx.parent().isValid())
        idx = idx.parent();
    AdBlockSubscription *subscription = (AdBlockSubscription*)m_adBlockModel->subscription(idx);
    AdBlockManager::instance()->removeSubscription(subscription);
}

