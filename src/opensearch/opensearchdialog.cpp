/*
 * Copyright © 2026-2027 Arktos Browser - IT-Space
 * Copyright 2009 Arora Browser
 * Copyright 2009 Arora Browser
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

#include "opensearchdialog.h"

#include "indexcheckbox.h"
#include "opensearchenginemodel.h"
#include "opensearchmanager.h"
#include "toolbarsearch.h"

#include <qfiledialog.h>
#include <qmessagebox.h>

OpenSearchDialog::OpenSearchDialog(QWidget *parent)
    : QDialog(parent)
    , m_model(nullptr)
{
    setModal(true);
    setupUi(this);

    m_model = new OpenSearchEngineModel(ToolbarSearch::openSearchManager(), this);
    m_tableView->setModel(m_model);
    m_tableView->horizontalHeader()->resizeSection(0, 200);
    m_tableView->horizontalHeader()->setStretchLastSection(true);
    m_tableView->verticalHeader()->hide();
    m_tableView->verticalHeader()->setDefaultSectionSize(1.2 * fontMetrics().height());
    m_tableView->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_tableView->setShowGrid(false);
    m_tableView->setAlternatingRowColors(true);

    // CA12/M3: the Suggestions column's check affordance is a real
    // QCheckBox index widget, not the style-painted indicator (the
    // MAC04 class, added to this view after that fix — see
    // utils/indexcheckbox.h).  Index widgets die on model reset and
    // row insert, so they are (re)installed on every shape change.
    m_tableView->setItemDelegate(new IndexCheckBoxDelegate(2, m_tableView));
    connect(m_model, &QAbstractItemModel::modelReset,
            this, &OpenSearchDialog::syncSuggestionBoxes);
    connect(m_model, &QAbstractItemModel::rowsInserted,
            this, &OpenSearchDialog::syncSuggestionBoxes);
    syncSuggestionBoxes();

    connect(m_closeButton, &QPushButton::clicked, this, &QDialog::close);
    connect(m_addButton, &QPushButton::clicked, this, &OpenSearchDialog::addButtonClicked);
    connect(m_deleteButton, &QPushButton::clicked, this, &OpenSearchDialog::deleteButtonClicked);
    connect(m_restoreButton, &QPushButton::clicked, this, &OpenSearchDialog::restoreButtonClicked);
}

void OpenSearchDialog::syncSuggestionBoxes()
{
    for (int row = 0; row < m_model->rowCount(); ++row) {
        const QModelIndex index = m_model->index(row, 2);
        if (!(m_model->flags(index) & Qt::ItemIsUserCheckable)
                || m_tableView->indexWidget(index))
            continue;
        installIndexCheckBox(m_tableView, index);
    }
}

void OpenSearchDialog::addButtonClicked()
{
    QStringList fileNames = QFileDialog::getOpenFileNames(this,
                                                          tr("Open File"),
                                                          QString(),
                                                          tr("OpenSearch") + QLatin1String(" (*.xml)"));

    for (const QString &fileName : fileNames) {
        if (!ToolbarSearch::openSearchManager()->addEngine(fileName)) {
            QMessageBox::critical(this, tr("Error"),
                    tr("%1 is not a valid OpenSearch 1.1 description or is already on your list.").arg(fileName));
        }
    }
}

void OpenSearchDialog::deleteButtonClicked()
{
    if (m_tableView->model()->rowCount() == 1) {
        QMessageBox::critical(this, tr("Error"),
                tr("You must have at least one search engine in here."));
        return;
    }

    m_tableView->removeSelected();
}

void OpenSearchDialog::restoreButtonClicked()
{
    ToolbarSearch::openSearchManager()->restoreDefaults();
}

