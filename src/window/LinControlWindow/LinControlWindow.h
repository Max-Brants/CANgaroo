#pragma once

#include "core/ConfigurableWidget.h"

class Backend;
class BusInterface;
class QVBoxLayout;
class QLabel;

class LinControlWindow : public ConfigurableWidget
{
    Q_OBJECT

public:
    explicit LinControlWindow(QWidget *parent, Backend &backend);
    ~LinControlWindow();

protected:
    void retranslateUi() override;

private slots:
    void rebuildRows();
    void clearRows();

private:
    Backend &_backend;
    QWidget *_rowContainer;
    QVBoxLayout *_rowLayout;
    QLabel *_placeholder;
};
