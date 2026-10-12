/*
 * Copyright 2008 Jason A. Donenfeld <Jason@zx2c4.com>
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

#ifndef CLEARPRIVATEDATA_H
#define CLEARPRIVATEDATA_H

#include <qdialog.h>

class QCheckBox;
class QJsonArray;

class ClearPrivateData : public QDialog
{
    Q_OBJECT

public:
    // CLR01: the category bits the plan operates on — identical
    // values to rustcore's RC_CLEAR_* (static_assert'd in the .cpp
    // under ARORA_RUSTCORE).  The dialog's seven checkboxes are
    // History..Icons; Session/FullWipe belong to the on-exit wipe.
    enum ClearCategory {
        ClearHistory = 0x01,
        ClearDownloads = 0x02,
        ClearSearch = 0x04,
        ClearCookies = 0x08,
        ClearSiteData = 0x10,
        ClearCache = 0x20,
        ClearIcons = 0x40,
        ClearSession = 0x80,
        ClearFullWipe = 0x100,
        ClearAll = 0xff
    };

    ClearPrivateData(QWidget *parent = nullptr);

    // The "which stores get wiped" decision, returned as the ordered
    // operation list the executor walks.  plan() decides through
    // rustcore's rc_clearplan under ARORA_RUSTCORE; planQt() is the
    // no-rust implementation, the FFI-failure fallback and the
    // parity-test seam — both emit the identical op vocabulary
    // documented in rustcore.h.  `otr` scopes the plan to the live
    // off-the-record context so its ops never touch a persistent
    // store.
    static QJsonArray plan(const QString &period, quint32 categories, bool otr);
    static QJsonArray planQt(const QString &period, quint32 categories, bool otr);

    // Executes one plan's ops in order against the adapters (engine
    // profile stores, rustcore stores, shell stores).  Unknown op
    // names are skipped with a warning — a plan the executor does
    // not understand degrades to less clearing, never to random
    // deletion.
    static void executePlan(const QJsonArray &ops);

public slots:
    void accept() override;

private:
    QCheckBox *m_browsingHistory;
    QCheckBox *m_downloadHistory;
    QCheckBox *m_searchHistory;
    QCheckBox *m_autoFill;
    QCheckBox *m_cookies;
    QCheckBox *m_siteData;
    QCheckBox *m_cache;
    QCheckBox *m_favIcons;
};

#endif // CLEARPRIVATEDATA_H

