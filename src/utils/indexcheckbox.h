/*
 * Copyright 2026 Arktos Browser - IT-Space
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

#ifndef INDEXCHECKBOX_H
#define INDEXCHECKBOX_H

#include "safetext.h"

#include <qabstractitemview.h>
#include <qcheckbox.h>
#include <qitemselectionmodel.h>

// CA12/M3 — the MAC04/10 recurring class: on macOS the style-painted
// item check indicator is the one platform-style-dependent render
// step in the sheet->ChromeProxyStyle->QMacStyle stack, and rows
// whose toggle rides CheckStateRole can show an invisible or inert
// box (MAC04 extensions list, MAC10 checkable group boxes — both
// already converted to real widgets; CA06 re-found the class in the
// adblock subscriptions tree, the sidebar panels list and the
// OpenSearch suggestions column, one added AFTER the original fix).
// This header is the systemic fix: every CheckStateRole-bearing view
// installs a REAL QCheckBox as the index widget instead of relying
// on PE_Indicator*Check, and every such view's delegate is an
// IndexCheckBoxDelegate so the painted cell content can't double-draw
// underneath the widget.  New checkable views get the same helper —
// do not hand a checkable item to the style again.
//
// Two pieces per view:
//   1. IndexCheckBoxDelegate(column) — strips the painted display/
//      check/decoration content for the widget-bearing column (the
//      MAC11 lesson: clearing the feature flags alone is not enough —
//      QCommonStyle gates item text on !option->text.isEmpty(), so
//      the paintable content itself must be emptied).
//   2. installIndexCheckBox() — creates the QCheckBox and mirrors
//      CheckStateRole in both directions through the model: toggling
//      writes model->setData(CheckStateRole) so the existing
//      itemChanged/dataChanged handlers run unchanged, and a
//      dataChanged hook moves the box when the state is written
//      programmatically (setCheckState, loadFromSettings, model
//      resets that re-report data).
//
// Lifetime note: index widgets do not survive row removal or a model
// reset — a takeItem/insertItem reorder or beginResetModel drops the
// widget for the affected cells.  Callers whose views reorder or
// reset must re-run installIndexCheckBox on the affected indexes
// (SettingsDialog::installSidebarPanelBoxes and
// OpenSearchDialog::syncSuggestionBoxes are the examples).

class IndexCheckBoxDelegate : public PlainTextItemDelegate
{
public:
    explicit IndexCheckBoxDelegate(int column, QObject *parent = nullptr)
        : PlainTextItemDelegate(parent)
        , m_column(column)
    {
    }

    void initStyleOption(QStyleOptionViewItem *option,
                         const QModelIndex &index) const override
    {
        PlainTextItemDelegate::initStyleOption(option, index);
        if (index.column() != m_column)
            return;
        option->features &= ~(QStyleOptionViewItem::HasDisplay
                              | QStyleOptionViewItem::HasCheckIndicator
                              | QStyleOptionViewItem::HasDecoration);
        option->text.clear();
        option->icon = QIcon();
        option->checkState = Qt::Unchecked;
    }

private:
    int m_column;
};

// Install a real QCheckBox as the index widget for `index`, seeded
// from the index's CheckStateRole.  The painted item text the box
// replaces used to switch to HighlightedText on selection, so the
// box follows the same cue (the MAC04 extensionSelectionChanged fix,
// folded in here so every view gets it).
inline QCheckBox *installIndexCheckBox(QAbstractItemView *view,
                                       const QModelIndex &index,
                                       const QString &text = QString(),
                                       const QIcon &icon = QIcon(),
                                       const QString &toolTip = QString())
{
    QCheckBox *box = new QCheckBox(text);
    box->setFont(view->font());
    if (!icon.isNull())
        box->setIcon(icon);
    if (!toolTip.isEmpty())
        box->setToolTip(toolTip);
    const QPersistentModelIndex persistent(index);
    QAbstractItemModel *model = view->model();
    box->setChecked(model->data(index, Qt::CheckStateRole).toInt()
                    == Qt::Checked);
    QObject::connect(box, &QCheckBox::toggled, view,
                     [model, persistent](bool on) {
        if (persistent.isValid())
            model->setData(persistent,
                           on ? Qt::Checked : Qt::Unchecked,
                           Qt::CheckStateRole);
    });
    QObject::connect(model, &QAbstractItemModel::dataChanged, box,
                     [box, persistent](const QModelIndex &topLeft,
                                       const QModelIndex &bottomRight,
                                       const QVector<int> &) {
        if (!persistent.isValid()
                || persistent.parent() != topLeft.parent()
                || persistent.row() < topLeft.row()
                || persistent.row() > bottomRight.row()
                || persistent.column() < topLeft.column()
                || persistent.column() > bottomRight.column())
            return;
        box->setChecked(persistent.model()->data(
                            persistent, Qt::CheckStateRole).toInt()
                        == Qt::Checked);
    });
    if (QItemSelectionModel *selection = view->selectionModel()) {
        const auto syncPalette = [view, box, persistent]() {
            const bool selected = persistent.isValid()
                && (view->selectionModel()->isSelected(persistent)
                    || view->selectionModel()->isRowSelected(
                        persistent.row(), persistent.parent()));
            QPalette palette = box->palette();
            palette.setColor(QPalette::WindowText,
                view->palette().color(selected
                    ? QPalette::HighlightedText : QPalette::Text));
            box->setPalette(palette);
        };
        QObject::connect(selection,
                         &QItemSelectionModel::selectionChanged,
                         box, [syncPalette](const QItemSelection &,
                                            const QItemSelection &) {
            syncPalette();
        });
        syncPalette();
    }
    view->setIndexWidget(index, box);
    return box;
}

#endif // INDEXCHECKBOX_H
