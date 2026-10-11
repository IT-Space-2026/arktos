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

#ifndef ADDRESSDIALOG_H
#define ADDRESSDIALOG_H

#include <qdialog.h>
#include "ui_addressdialog.h"

#include "autofillmanager.h"

// ADDR01: editor for a single AutoFillManager::Address — the
// "Add..."/"Edit..." surface of the AutoFill prefs page.  Save stays
// disabled until at least Name or Street is non-empty (the same rule
// Address::isValid() encodes).
class AddressDialog : public QDialog, public Ui_AddressDialog
{
    Q_OBJECT

public:
    AddressDialog(QWidget *parent = nullptr,
                  Qt::WindowFlags flags = Qt::WindowFlags());

    void setAddress(const AutoFillManager::Address &address);
    AutoFillManager::Address address() const;

    // One-line row text for the prefs list:
    // "Name — Street, City, State ZIP — Country".
    static QString summaryText(const AutoFillManager::Address &address);
    // Address::country stores the ISO territory code; this resolves it
    // to the display name, falling back to the stored string itself.
    static QString countryDisplayName(const QString &storedCountry);

private slots:
    void updateSaveButton();
};

#endif // ADDRESSDIALOG_H
