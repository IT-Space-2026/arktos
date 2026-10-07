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

#ifndef ADBLOCKPRESETSDIALOG_H
#define ADBLOCKPRESETSDIALOG_H

#include <qdialog.h>

// ADB03: "Preset Filter Lists" dialog — one checkbox per curated
// community list (adblockpresets.cpp), grouped by category.  Ticking a
// box subscribes + enables the list (and counts as the user's consent
// to remote-list downloads); unticking disables the subscription, which
// stays removable from the main AdBlock Configuration tree.
class AdBlockPresetsDialog : public QDialog
{
    Q_OBJECT

public:
    AdBlockPresetsDialog(QWidget *parent = nullptr);

private slots:
    void presetToggled(bool checked);
};

#endif // ADBLOCKPRESETSDIALOG_H
