/*
  Copyright (c) 2026 Schildkroet

  This file is part of cangaroo.

  cangaroo is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 2 of the License, or
  (at your option) any later version.

  cangaroo is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with cangaroo.  If not, see <http://www.gnu.org/licenses/>.
*/

#pragma once

#include <QDialog>
#include <QSet>

#include "driver/CanDriver.h"

class QCheckBox;
class QListWidget;
class Backend;
class QDomDocument;
class QDomElement;

class TraceFilterDialog : public QDialog
{
    Q_OBJECT

public:
    explicit TraceFilterDialog(Backend &backend, QWidget *parent = nullptr);

    bool showTx() const;
    bool showRx() const;
    QSet<uint32_t> hiddenMessageIds() const;
    QSet<uint8_t>  hiddenLinFrameIds() const;
    QSet<BusInterfaceId> hiddenInterfaces() const;

    void setShowTx(bool show);
    void setShowRx(bool show);
    void setHiddenMessageIds(const QSet<uint32_t> &ids);
    void setHiddenLinFrameIds(const QSet<uint8_t> &ids);
    void setHiddenInterfaces(const QSet<BusInterfaceId> &ids);

    bool saveXML(QDomDocument &xml, QDomElement &root) const;
    bool loadXML(QDomElement &el);

private slots:
    void clearFilters();
    void selectAllMessages();
    void deselectAllMessages();

private:
    void populateMessages(Backend &backend);
    void populateInterfaces(Backend &backend);

    QCheckBox *m_showTx;
    QCheckBox *m_showRx;
    QListWidget *m_messageList;
    QListWidget *m_interfaceList;
};
