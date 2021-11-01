/*
 * Bittorrent Client using Qt and libtorrent.
 * Copyright (C) 2014  Ivan Sorokin <vanyacpp@gmail.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 *
 * In addition, as a special exception, the copyright holders give permission to
 * link this program with the OpenSSL project's "OpenSSL" library (or with
 * modified versions of it that use the same license as the "OpenSSL" library),
 * and distribute the linked executables. You must obey the GNU General Public
 * License in all respects for all of the code used other than "OpenSSL".  If you
 * modify file(s), you may extend this exception to your version of the file(s),
 * but you are not obligated to do so. If you do not wish to do so, delete this
 * exception statement from your version.
 */

#include "torrentcontenttreeview.h"

#include <QDir>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QModelIndexList>
#include <QTableView>
#include <QThread>

#include "base/bittorrent/abstractfilestorage.h"
#include "base/bittorrent/common.h"
#include "base/bittorrent/session.h"
#include "base/bittorrent/torrent.h"
#include "base/bittorrent/torrentinfo.h"
#include "base/exceptions.h"
#include "base/global.h"
#include "base/utils/fs.h"
#include "autoexpandabledialog.h"
#include "raisedmessagebox.h"
#include "torrentcontentfiltermodel.h"
#include "torrentcontentmodelitem.h"

namespace
{
    QString getFullPath(const QModelIndex &idx)
    {
        QStringList paths;
        for (QModelIndex i = idx; i.isValid(); i = i.parent())
            paths.prepend(i.data().toString());
        return paths.join(QLatin1Char {'/'});
    }
}

TorrentContentTreeView::TorrentContentTreeView(QWidget *parent)
    : QTreeView(parent)
{
    setExpandsOnDoubleClick(false);

    // This hack fixes reordering of first column with Qt5.
    // https://github.com/qtproject/qtbase/commit/e0fc088c0c8bc61dbcaf5928b24986cd61a22777
    QTableView unused;
    unused.setVerticalHeader(header());
    header()->setParent(this);
    header()->setStretchLastSection(false);
    header()->setTextElideMode(Qt::ElideRight);
    unused.setVerticalHeader(new QHeaderView(Qt::Horizontal));
}

void TorrentContentTreeView::keyPressEvent(QKeyEvent *event)
{
    if ((event->key() != Qt::Key_Space) && (event->key() != Qt::Key_Select))
    {
        QTreeView::keyPressEvent(event);
        return;
    }

    event->accept();

    QModelIndex current = currentNameCell();

    QVariant value = current.data(Qt::CheckStateRole);
    if (!value.isValid())
    {
        Q_ASSERT(false);
        return;
    }

    Qt::CheckState state = (static_cast<Qt::CheckState>(value.toInt()) == Qt::Checked
                            ? Qt::Unchecked : Qt::Checked);

    const QModelIndexList selection = selectionModel()->selectedRows(TorrentContentModelItem::COL_NAME);

    for (const QModelIndex &index : selection)
    {
        Q_ASSERT(index.column() == TorrentContentModelItem::COL_NAME);
        model()->setData(index, state, Qt::CheckStateRole);
    }
}

void TorrentContentTreeView::renameSelectedFile(BitTorrent::AbstractFileStorage &fileStorage)
{
    const QModelIndexList selectedIndexes = selectionModel()->selectedRows(0);
    if (selectedIndexes.size() != 1) return;

    const QPersistentModelIndex modelIndex = selectedIndexes.first();
    if (!modelIndex.isValid()) return;

    auto model = dynamic_cast<TorrentContentFilterModel *>(TorrentContentTreeView::model());
    if (!model) return;

    const bool isFile = (model->itemType(modelIndex) == TorrentContentModelItem::FileType);

    // Ask for new name
    bool ok = false;
    QString newName = AutoExpandableDialog::getText(this, tr("Renaming"), tr("New name:"), QLineEdit::Normal
            , modelIndex.data().toString(), &ok, isFile).trimmed();
    if (!ok || !modelIndex.isValid()) return;

    const QString oldName = modelIndex.data().toString();
    if (newName == oldName)
        return;  // Name did not change

    const QString parentPath = getFullPath(modelIndex.parent());
    const QString oldPath {parentPath.isEmpty() ? oldName : parentPath + QLatin1Char {'/'} + oldName};
    const QString newPath {parentPath.isEmpty() ? newName : parentPath + QLatin1Char {'/'} + newName};

    try
    {
        if (isFile)
            fileStorage.renameFile(oldPath, newPath);
        else
            fileStorage.renameFolder(oldPath, newPath);

        model->setData(modelIndex, newName);
    }
    catch (const RuntimeError &error)
    {
        RaisedMessageBox::warning(this, tr("Rename error"), error.message(), QMessageBox::Ok);
    }
}

void TorrentContentTreeView::setupDownloadPriorityMenu(QMenu *menu, bool createSubMenu)
{
    auto model = qobject_cast<TorrentContentFilterModel *>(TorrentContentTreeView::model());
    Q_ASSERT(model);

    const auto getSelectedRows = [this]() { return selectionModel()->selectedRows(); };

    const auto applyPriorities = [&](BitTorrent::DownloadPriority priority)
    {
        return [model, getSelectedRows, priority]()
        {
            model->changeFilePriorities(getSelectedRows(), [priority]() { return priority; });
        };
    };
    const auto applyPrioritiesByOrder = [model, getSelectedRows]()
    {
        // If a signle folder is selected then distribute priorities over sub-items.
        // Otherwise distribute priorities over all selected items.
        QModelIndexList rows = getSelectedRows();
        if (rows.length() == 1 && model->itemType(rows[0]) == TorrentContentModelItem::FolderType)
        {
            const QModelIndex parent = rows[0];
            const qsizetype rowCount = model->rowCount(parent);
            if (rowCount == 0)
                rows = {};
            else
                rows = QItemSelectionRange(model->index(0, 0, parent), model->index(rowCount - 1, 0, parent)).indexes();
        }

        // Equally distribute items into groups and for each group assign a download
        // priority that will apply to each item. The number of groups depends on how
        // many "download priority" are available to be assigned

        const qsizetype priorityGroups = 3;
        const qsizetype rowCount = rows.count();
        const qsizetype maxPrioGroupSize = std::max<qsizetype>(rowCount / priorityGroups, 1);
        const qsizetype highPrioGroupSize = std::max<qsizetype>((rowCount - maxPrioGroupSize) / (priorityGroups - 1), 1);

        model->changeFilePriorities(rows, [maxPrioGroupSize, highPrioGroupSize, i = qsizetype(0)]() mutable
        {
            ++i;
            if (i <= maxPrioGroupSize)
                return BitTorrent::DownloadPriority::Maximum;
            else if (i <= maxPrioGroupSize + highPrioGroupSize)
                return BitTorrent::DownloadPriority::High;
            else
                return BitTorrent::DownloadPriority::Normal;
        });
    };

    if (createSubMenu)
    {
        QMenu *priorityMenu = menu->addMenu(tr("Priority"));
        priorityMenu->addAction(tr("Do not download"), priorityMenu, applyPriorities(BitTorrent::DownloadPriority::Ignored));
        priorityMenu->addAction(tr("Normal"),          priorityMenu, applyPriorities(BitTorrent::DownloadPriority::Normal));
        priorityMenu->addAction(tr("High"),            priorityMenu, applyPriorities(BitTorrent::DownloadPriority::High));
        priorityMenu->addAction(tr("Maximum"),         priorityMenu, applyPriorities(BitTorrent::DownloadPriority::Maximum));
        priorityMenu->addSeparator();
        priorityMenu->addAction(tr("By shown file order"), priorityMenu, applyPrioritiesByOrder);
    }
    else
    {
        menu->addAction(tr("Do not download"),  menu, applyPriorities(BitTorrent::DownloadPriority::Ignored));
        menu->addAction(tr("Normal priority"),  menu, applyPriorities(BitTorrent::DownloadPriority::Normal));
        menu->addAction(tr("High priority"),    menu, applyPriorities(BitTorrent::DownloadPriority::High));
        menu->addAction(tr("Maximum priority"), menu, applyPriorities(BitTorrent::DownloadPriority::Maximum));
        menu->addSeparator();
        menu->addAction(tr("Priority by shown file order"), menu, applyPrioritiesByOrder);
    }
}

QModelIndex TorrentContentTreeView::currentNameCell()
{
    QModelIndex current = currentIndex();
    if (!current.isValid())
    {
        Q_ASSERT(false);
        return {};
    }

    return model()->index(current.row(), TorrentContentModelItem::COL_NAME, current.parent());
}
