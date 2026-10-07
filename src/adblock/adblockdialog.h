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

#ifndef ADBLOCKDIALOG_H
#define ADBLOCKDIALOG_H

#include <qdialog.h>
#include <qpointer.h>
#include "ui_adblockdialog.h"

class AdBlockSubscription;
class QTimer;
class QTreeWidgetItem;
class AdBlockDialog : public QDialog, public Ui_AdBlockDialog
{
    Q_OBJECT

public:
    AdBlockDialog(QWidget *parent = nullptr);

    // Selects a subscription's row so a freshly added list is in view
    // (used after an abp: subscribe prompt is accepted).
    void selectSubscription(AdBlockSubscription *subscription);

public slots:
    void addCustomRule(const QString &rule = QString());
    void done(int result) override;

private slots:
    void learnAboutWritingFilters();
    void aboutToShowActionMenu();
    void updateSubscription();
    void browseSubscriptions();
    void showPresets();
    void removeSubscription();
    void repopulateSubscriptions();
    void subscriptionItemChanged(QTreeWidgetItem *item, int column);
    void subscriptionSelectionChanged();
    void fillRulesView();
    void customRulesTextChanged();
    void commitCustomRules();
    void customRulesChangedExternally();

private:
    AdBlockSubscription *selectedSubscription() const;
    AdBlockSubscription *customSubscription();
    void loadCustomRulesText();
    void focusCustomRules();

    QPointer<AdBlockSubscription> m_customRules;
    QTimer *m_customCommitTimer;
    QString m_customCommittedText;
    bool m_refreshing;

};

#endif // ADBLOCKDIALOG_H

