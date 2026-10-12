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

#include "addressdialog.h"

#include <qdialogbuttonbox.h>
#include <qlocale.h>
#include <qpushbutton.h>

#include <algorithm>

AddressDialog::AddressDialog(QWidget *parent, Qt::WindowFlags flags)
    : QDialog(parent, flags)
{
    setupUi(this);

    // Every territory QLocale knows, sorted by display name; itemData
    // carries the ISO code that Address::country stores.
    QList<QPair<QString, QString> > territories;
    for (int i = int(QLocale::AnyTerritory) + 1;
         i <= int(QLocale::LastTerritory); ++i) {
        const QLocale::Territory territory = QLocale::Territory(i);
        const QString name = QLocale::territoryToString(territory);
        if (name.isEmpty())
            continue;
        territories.append(qMakePair(
            name, QLocale::territoryToCode(territory)));
    }
    std::sort(territories.begin(), territories.end(),
              [](const QPair<QString, QString> &a,
                 const QPair<QString, QString> &b) {
        return a.first.localeAwareCompare(b.first) < 0;
    });
    for (const QPair<QString, QString> &territory : territories)
        countryCombo->addItem(territory.first, territory.second);

    // Default to the system territory; a territory-less locale (the
    // bare "C" locale) falls back to the mockup's United States.
    QLocale::Territory system = QLocale().territory();
    if (system == QLocale::AnyTerritory)
        system = QLocale::UnitedStates;
    const int index = countryCombo->findData(
        QLocale::territoryToCode(system));
    countryCombo->setCurrentIndex(index >= 0 ? index : 0);

    connect(nameEdit, &QLineEdit::textChanged,
            this, &AddressDialog::updateSaveButton);
    connect(streetEdit, &QPlainTextEdit::textChanged,
            this, &AddressDialog::updateSaveButton);
    updateSaveButton();
}

void AddressDialog::setAddress(const AutoFillManager::Address &address)
{
    int index = countryCombo->findData(address.country);
    if (index < 0) {
        // A stored value that is not a known territory code (hand
        // edited or written by another build) — surface it rather
        // than silently resetting to the default country.
        countryCombo->addItem(countryDisplayName(address.country),
                              address.country);
        index = countryCombo->count() - 1;
    }
    countryCombo->setCurrentIndex(index);
    nameEdit->setText(address.name);
    organizationEdit->setText(address.organization);
    streetEdit->setPlainText(address.street);
    cityEdit->setText(address.city);
    stateEdit->setText(address.state);
    zipEdit->setText(address.zip);
    phoneEdit->setText(address.phone);
    emailEdit->setText(address.email);
    updateSaveButton();
}

AutoFillManager::Address AddressDialog::address() const
{
    AutoFillManager::Address result;
    result.country = countryCombo->currentData().toString();
    result.name = nameEdit->text().trimmed();
    result.organization = organizationEdit->text().trimmed();
    result.street = streetEdit->toPlainText().trimmed();
    result.city = cityEdit->text().trimmed();
    result.state = stateEdit->text().trimmed();
    result.zip = zipEdit->text().trimmed();
    result.phone = phoneEdit->text().trimmed();
    result.email = emailEdit->text().trimmed();
    return result;
}

void AddressDialog::updateSaveButton()
{
    // The documented rule: an address needs at least a name or a
    // street to be worth saving.
    const bool saveable = !nameEdit->text().trimmed().isEmpty()
        || !streetEdit->toPlainText().trimmed().isEmpty();
    buttonBox->button(QDialogButtonBox::Save)->setEnabled(saveable);
}

QString AddressDialog::summaryText(const AutoFillManager::Address &address)
{
    QString who = address.name;
    if (who.isEmpty())
        who = address.organization.isEmpty()
            ? address.email : address.organization;

    QStringList place;
    const QString streetLine =
        address.street.section(QLatin1Char('\n'), 0, 0).trimmed();
    if (!streetLine.isEmpty())
        place << streetLine;
    if (!address.city.trimmed().isEmpty())
        place << address.city.trimmed();
    const QString stateZip =
        (address.state.trimmed() + QLatin1Char(' ')
         + address.zip.trimmed()).trimmed();
    if (!stateZip.isEmpty())
        place << stateZip;

    QStringList sections;
    if (!who.isEmpty())
        sections << who;
    if (!place.isEmpty())
        sections << place.join(QLatin1String(", "));
    const QString country = countryDisplayName(address.country);
    if (!country.isEmpty())
        sections << country;
    return sections.join(QLatin1String(" \u2014 "));
}

QString AddressDialog::countryDisplayName(const QString &storedCountry)
{
    if (storedCountry.isEmpty())
        return QString();
    const QLocale::Territory territory =
        QLocale::codeToTerritory(storedCountry);
    if (territory != QLocale::AnyTerritory) {
        const QString name = QLocale::territoryToString(territory);
        if (!name.isEmpty())
            return name;
    }
    return storedCountry;
}
