#pragma once

#include <QDialog>
#include <QSettings>

class QComboBox;
class QCheckBox;
class QSpinBox;
class QActionGroup;

class SettingsDialog : public QDialog
{
    Q_OBJECT

public:
    explicit SettingsDialog(QSettings &settings, QActionGroup *languageGroup, QWidget *parent = nullptr);

    QString selectedTheme() const;
    QString selectedLanguage() const;
    bool restoreWindowEnabled() const;
    bool clearTraceOnStart() const;
    int selectedFontSize() const;
    bool uds29BitEnabled() const;
    bool skipSaveWorkspacePrompt() const;

private:
    QComboBox *m_themeCombo;
    QComboBox *m_languageCombo;
    QCheckBox *m_restoreWindowCheck;
    QCheckBox *m_clearTraceOnStartCheck;
    QCheckBox *m_uds29BitCheck;
    QCheckBox *m_skipSaveWorkspacePromptCheck;
    QSpinBox *m_fontSizeSpin;
};
