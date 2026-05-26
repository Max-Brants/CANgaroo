#pragma once

#include <QString>

class CanOpenDb;

class CanOpenEdsParser
{
public:
    bool parseFile(const QString &path, CanOpenDb &db, QString *errorMsg = nullptr) const;
};
