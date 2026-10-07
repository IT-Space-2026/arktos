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

#include "adblockpresetsdialog.h"

#include "adblockmanager.h"
#include "adblockpresets.h"
#include "adblocksubscription.h"

#include <qboxlayout.h>
#include <qcheckbox.h>
#include <qdialogbuttonbox.h>
#include <qgroupbox.h>
#include <qlabel.h>
#include <qscrollarea.h>
#include <qurl.h>

AdBlockPresetsDialog::AdBlockPresetsDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Filter List Presets"));

    QVBoxLayout *layout = new QVBoxLayout(this);

    QLabel *intro = new QLabel(
        tr("Tick a list to subscribe to it; Arora downloads it once and "
           "refreshes it about once a week. Unticking disables the "
           "subscription. Lists already subscribed to (including the "
           "defaults) show as ticked."), this);
    intro->setWordWrap(true);
    layout->addWidget(intro);

    const QList<AdBlockListPreset> presets = AdBlockPresets::all();
    AdBlockManager *manager = AdBlockManager::instance();

    QScrollArea *scroll = new QScrollArea(this);
    QWidget *contents = new QWidget(scroll);
    QVBoxLayout *contentsLayout = new QVBoxLayout(contents);

    QGroupBox *group = nullptr;
    QString currentCategory;
    int index = 0;
    for (const AdBlockListPreset &preset : presets) {
        if (preset.category != currentCategory) {
            currentCategory = preset.category;
            group = new QGroupBox(currentCategory, contents);
            group->setLayout(new QVBoxLayout);
            contentsLayout->addWidget(group);
        }
        QCheckBox *box = new QCheckBox(preset.title, group);
        box->setToolTip(preset.description + QLatin1Char('\n')
                        + preset.location);
        box->setAccessibleName(preset.title);
        box->setAccessibleDescription(preset.description);
        box->setProperty("presetIndex", index++);
        const AdBlockSubscription *existing =
            manager->subscriptionForLocation(QUrl(preset.location));
        box->setChecked(existing && existing->isEnabled());
        connect(box, &QCheckBox::toggled,
                this, &AdBlockPresetsDialog::presetToggled);
        group->layout()->addWidget(box);

        QLabel *description = new QLabel(preset.description, group);
        description->setWordWrap(true);
        description->setContentsMargins(20, 0, 0, 4);
        group->layout()->addWidget(description);
    }
    contentsLayout->addStretch(1);
    scroll->setWidget(contents);
    scroll->setWidgetResizable(true);
    layout->addWidget(scroll);

    QDialogButtonBox *buttonBox = new QDialogButtonBox(
        QDialogButtonBox::Ok, this);
    connect(buttonBox, &QDialogButtonBox::accepted, this, &QDialog::accept);
    layout->addWidget(buttonBox);

    resize(520, 460);
}

void AdBlockPresetsDialog::presetToggled(bool checked)
{
    QCheckBox *box = qobject_cast<QCheckBox*>(sender());
    if (!box)
        return;
    const int index = box->property("presetIndex").toInt();
    const AdBlockListPreset preset = AdBlockPresets::all().at(index);
    const QUrl location(preset.location);

    AdBlockManager *manager = AdBlockManager::instance();
    if (checked) {
        // Adds + enables the subscription and counts as the user's
        // consent to remote-list downloads (TELEM01).
        manager->subscribeRemoteList(location, preset.title);
    } else {
        // Disable rather than remove so the cached rules and any
        // custom state survive; removal lives in the main dialog.
        AdBlockSubscription *subscription =
            manager->subscriptionForLocation(location);
        if (subscription)
            subscription->setEnabled(false);
    }
}
