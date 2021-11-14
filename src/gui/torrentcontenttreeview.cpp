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

void TorrentContentTreeView::setupDownloadPriorityMenu(QMenu *const menu, const bool createSubMenu) const
{
    const auto model = qobject_cast<TorrentContentFilterModel *>(this->model());
    Q_ASSERT(model);

    const auto getSelectedRows = [this]() -> QModelIndexList
    {
        return selectionModel()->selectedRows(TorrentContentModelItem::COL_PRIO);
    };

    const auto applyPriorities = [getSelectedRows, model](const BitTorrent::DownloadPriority priority) -> auto
    {
        return [getSelectedRows, model, priority]() -> void
        {
            for (const QModelIndex &index : asConst(getSelectedRows()))
                model->setData(index, static_cast<int>(priority));

            emit model->filteredFilesChanged();
        };
    };
    const auto applyPrioritiesByOrder = [getSelectedRows, model]() -> void
    {
        // Equally distribute the selected items into groups and for each group assign
        // a download priority that will apply to each item. The number of groups depends on how
        // many "download priority" are available to be assigned

        const QModelIndexList selectedRows = getSelectedRows();
        const qsizetype priorityGroups = 3;
        const auto priorityGroupSize = std::max<qsizetype>((selectedRows.length() / priorityGroups), 1);

        for (qsizetype i = 0; i < selectedRows.length(); ++i)
        {
            auto priority = BitTorrent::DownloadPriority::Ignored;
            switch (i / priorityGroupSize)
            {
            case 0:
                priority = BitTorrent::DownloadPriority::Maximum;
                break;
            case 1:
                priority = BitTorrent::DownloadPriority::High;
                break;
            default:
            case 2:
                priority = BitTorrent::DownloadPriority::Normal;
                break;
            }

            model->setData(selectedRows[i], static_cast<int>(priority));
        }

        emit model->filteredFilesChanged();
    };

    if (createSubMenu)
    {
        QMenu *const priorityMenu = menu->addMenu(tr("Priority"));
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
