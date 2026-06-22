/*

  Copyright (c) 2015, 2016 Hubert Denkmair <hubert@denkmair.de>

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

#include "MeasurementNetwork.h"
#include "MeasurementInterface.h"

#include "core/Backend.h"
#include "core/DBC/CanOpenDb.h"
#include "core/DBC/LinDb.h"


MeasurementNetwork::MeasurementNetwork()
{
}

void MeasurementNetwork::cloneFrom(MeasurementNetwork &origin)
{
    _name = origin._name;
    for (auto *omi : origin._interfaces) {
        MeasurementInterface *mi = new MeasurementInterface();
        mi->cloneFrom(*omi);
        _interfaces.append(mi);
    }
    _canDbs = origin._canDbs;
    _linDbs = origin._linDbs;
    _canOpenDbs = origin._canOpenDbs;
}

void MeasurementNetwork::addInterface(MeasurementInterface *intf)
{
    _interfaces.append(intf);
}

void MeasurementNetwork::removeInterface(MeasurementInterface *intf)
{
    if (_interfaces.removeAll(intf) > 0) {
        delete intf;
    }
}

QList<MeasurementInterface *> MeasurementNetwork::interfaces()
{
    return _interfaces;
}

MeasurementInterface *MeasurementNetwork::addBusInterface(BusInterfaceId busif)
{
    MeasurementInterface *mi = new MeasurementInterface();
    mi->setBusInterface(busif);
    mi->setResolved(true);
    addInterface(mi);
    return mi;
}

BusInterfaceIdList MeasurementNetwork::getReferencedBusInterfaces()
{
    BusInterfaceIdList list;
    for (auto *mi : _interfaces) {
        list << mi->busInterface();
    }
    return list;
}

void MeasurementNetwork::addCanDb(QSharedPointer<CanDb> candb)
{
    _canDbs.append(candb);
}

bool MeasurementNetwork::reloadCanDbs(Backend *backend, QStringList *errors)
{
    bool allSuccess = true;
    for (pCanDb &db : _canDbs) {
        QString errorMsg;
        pCanDb newDb = backend->loadDbc(db->getPath(), &errorMsg);
        if (newDb) {
            db->updateFrom(newDb.data());
        } else {
            allSuccess = false;
            if (errors) {
                errors->append(QString("%1: %2").arg(db->getPath(), errorMsg));
            }
        }
    }
    return allSuccess;
}

void MeasurementNetwork::addLinDb(pLinDb lindb)
{
    _linDbs.append(lindb);
}

bool MeasurementNetwork::reloadLinDbs(Backend *backend, QStringList *errors)
{
    bool allSuccess = true;
    for (pLinDb &db : _linDbs) {
        QString errorMsg;
        pLinDb newDb = backend->loadLdf(db->path(), &errorMsg);
        if (newDb) {
            db = newDb;
        } else {
            allSuccess = false;
            if (errors)
                errors->append(QString("%1: %2").arg(db->path(), errorMsg));
        }
    }
    return allSuccess;
}

void MeasurementNetwork::addCanOpenDb(pCanOpenDb canOpenDb)
{
    _canOpenDbs.append(canOpenDb);
}

bool MeasurementNetwork::reloadCanOpenDbs(Backend *backend, QStringList *errors)
{
    bool allSuccess = true;
    for (pCanOpenDb &db : _canOpenDbs) {
        QString errorMsg;
        const int previousNodeId = db->configuredNodeId();
        pCanOpenDb newDb = backend->loadEds(db->path(), &errorMsg);
        if (newDb) {
            // Reloading re-parses the EDS from disk, which would otherwise reset the node ID
            // to the file's static default and discard any override the user configured.
            if (previousNodeId >= 0)
                newDb->setConfiguredNodeId(previousNodeId);
            db = newDb;
        } else {
            allSuccess = false;
            if (errors)
                errors->append(QString("%1: %2").arg(db->path(), errorMsg));
        }
    }
    return allSuccess;
}

QString MeasurementNetwork::name() const
{
    return _name;
}

void MeasurementNetwork::setName(const QString &name)
{
    _name = name;
}

bool MeasurementNetwork::saveXML(Backend &backend, QDomDocument &xml, QDomElement &root)
{
    root.setAttribute("name", _name);

    QDomElement interfacesNode = xml.createElement("interfaces");
    for (auto *intf : _interfaces) {
        QDomElement intfNode = xml.createElement("interface");
        if (!intf->saveXML(backend, xml, intfNode)) {
            return false;
        }
        interfacesNode.appendChild(intfNode);
    }
    root.appendChild(interfacesNode);

    QDomElement candbsNode = xml.createElement("databases");
    for (const auto &candb : _canDbs) {
        QDomElement dbNode = xml.createElement("database");
        dbNode.setAttribute("db-type", "dbc");
        if (!candb->saveXML(backend, xml, dbNode)) {
            return false;
        }
        candbsNode.appendChild(dbNode);
    }
    for (const auto &lindb : _linDbs) {
        QDomElement dbNode = xml.createElement("database");
        dbNode.setAttribute("db-type", "ldf");
        dbNode.setAttribute("filename", lindb->path());
        candbsNode.appendChild(dbNode);
    }
    for (const auto &canOpenDb : _canOpenDbs) {
        QDomElement dbNode = xml.createElement("database");
        dbNode.setAttribute("db-type", "eds");
        dbNode.setAttribute("filename", canOpenDb->path());
        if (canOpenDb->configuredNodeId() >= 0)
            dbNode.setAttribute("node-id", canOpenDb->configuredNodeId());
        candbsNode.appendChild(dbNode);
    }
    root.appendChild(candbsNode);


    return true;
}

bool MeasurementNetwork::loadXML(Backend &backend, QDomElement el)
{
    setName(el.attribute("name", "unnamed network"));

    QDomNodeList ifList = el.firstChildElement("interfaces").elementsByTagName("interface");
    for (int i=0; i<ifList.length(); i++) {
        QDomElement elIntf = ifList.item(i).toElement();
        const QString driverName = elIntf.attribute("driver");
        const QString deviceName = elIntf.attribute("name");

        MeasurementInterface *mi = new MeasurementInterface();
        mi->loadXML(backend, elIntf);

        BusInterface *intf = backend.getInterfaceByDriverAndName(driverName, deviceName);
        if (intf) {
            mi->setBusInterface(intf->getId());
            mi->setResolved(true);
        } else {
            mi->setBusInterface(InvalidBusInterfaceId);
            mi->setResolved(false);
            log_warning(QString("Interface %1/%2 not found; settings preserved but interface is unavailable.").arg(driverName, deviceName));
        }

        addInterface(mi);
    }


    QDomNodeList dbList = el.firstChildElement("databases").elementsByTagName("database");
    for (int i=0; i<dbList.length(); i++) {
        QDomElement elDb = dbList.item(i).toElement();
        QString filename = elDb.attribute("filename", QString());
        QString dbType   = elDb.attribute("db-type", "dbc");
        if (filename.isEmpty()) {
            log_error(QString("Unable to load database: empty filename"));
            continue;
        }
        if (dbType == "ldf") {
            pLinDb lindb = backend.loadLdf(filename);
            if (lindb) addLinDb(lindb);
            else log_error(QString("Unable to load LDF: %1").arg(filename));
        } else if (dbType == "eds") {
            pCanOpenDb canOpenDb = backend.loadEds(filename);
            if (canOpenDb) {
                if (elDb.hasAttribute("node-id"))
                    canOpenDb->setConfiguredNodeId(elDb.attribute("node-id").toInt());
                addCanOpenDb(canOpenDb);
            } else {
                log_error(QString("Unable to load EDS: %1").arg(filename));
            }
        } else {
            addCanDb(backend.loadDbc(filename));
        }
    }

    return true;
}

