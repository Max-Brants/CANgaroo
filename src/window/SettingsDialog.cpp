#include "SettingsDialog.h"

#include <QVBoxLayout>
#include <QFormLayout>
#include <QComboBox>
#include <QCheckBox>
#include <QSpinBox>
#include <QDialogButtonBox>
#include <QStyleFactory>
#include <QApplication>
#include <QActionGroup>

SettingsDialog::SettingsDialog(QSettings &settings, QActionGroup *languageGroup, QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Settings"));
    setMinimumWidth(350);

    auto *mainLayout = new QVBoxLayout(this);
    auto *form = new QFormLayout;

    // --- Theme ---
    m_themeCombo = new QComboBox(this);
    QStringList styles = QStyleFactory::keys();
    m_themeCombo->addItems(styles);

    const QString currentStyle = settings.value("ui/applicationStyle", QApplication::style()->name()).toString();
    for (int i = 0; i < styles.size(); ++i)
    {
        if (styles[i].compare(currentStyle, Qt::CaseInsensitive) == 0)
        {
            m_themeCombo->setCurrentIndex(i);
            break;
        }
    }
    form->addRow(tr("Theme:"), m_themeCombo);

    // --- Language ---
    m_languageCombo = new QComboBox(this);
    QString savedLocale = settings.value("ui/language", "en_US").toString();
    int langIdx = 0;

    if (languageGroup)
    {
        int i = 0;
        for (QAction *action : languageGroup->actions())
        {
            m_languageCombo->addItem(action->text(), action->data());
            if (action->data().toString() == savedLocale)
            {
                langIdx = i;
            }
            ++i;
        }
    }
    m_languageCombo->setCurrentIndex(langIdx);
    form->addRow(tr("Language:"), m_languageCombo);

    // --- Font size ---
    m_fontSizeSpin = new QSpinBox(this);
    m_fontSizeSpin->setRange(6, 24);
    int defaultSize = QApplication::font().pointSize();
    if (defaultSize < 6) { defaultSize = 9; }
    m_fontSizeSpin->setValue(settings.value("ui/fontSize", defaultSize).toInt());
    m_fontSizeSpin->setSuffix(" pt");
    form->addRow(tr("Font size:"), m_fontSizeSpin);

    // --- Restore window ---
    m_restoreWindowCheck = new QCheckBox(tr("Restore window layout on startup"), this);
    m_restoreWindowCheck->setChecked(settings.value("ui/restoreWindowGeometry", false).toBool());
    form->addRow(m_restoreWindowCheck);

    // --- Clear trace on measurement start ---
    m_clearTraceOnStartCheck = new QCheckBox(tr("Clear trace on measurement start"), this);
    m_clearTraceOnStartCheck->setChecked(settings.value("ui/clearTraceOnStart", true).toBool());
    form->addRow(m_clearTraceOnStartCheck);

    // --- UDS 29-bit decoding ---
    m_uds29BitCheck = new QCheckBox(tr("Decode UDS on 29-bit (extended) CAN IDs"), this);
    m_uds29BitCheck->setChecked(settings.value("decoder/uds29Bit", true).toBool());
    form->addRow(m_uds29BitCheck);

    // --- Skip save-workspace prompt ---
    m_skipSaveWorkspacePromptCheck = new QCheckBox(tr("Do not ask to save workspace on close (always discard)"), this);
    m_skipSaveWorkspacePromptCheck->setChecked(settings.value("ui/skipSaveWorkspacePrompt", false).toBool());
    form->addRow(m_skipSaveWorkspacePromptCheck);

    mainLayout->addLayout(form);
    mainLayout->addSpacing(10);

    // --- Buttons ---
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    mainLayout->addWidget(buttons);
}

QString SettingsDialog::selectedTheme() const
{
    return m_themeCombo->currentText();
}

QString SettingsDialog::selectedLanguage() const
{
    return m_languageCombo->currentData().toString();
}

bool SettingsDialog::restoreWindowEnabled() const
{
    return m_restoreWindowCheck->isChecked();
}

bool SettingsDialog::clearTraceOnStart() const
{
    return m_clearTraceOnStartCheck->isChecked();
}

int SettingsDialog::selectedFontSize() const
{
    return m_fontSizeSpin->value();
}

bool SettingsDialog::uds29BitEnabled() const
{
    return m_uds29BitCheck->isChecked();
}

bool SettingsDialog::skipSaveWorkspacePrompt() const
{
    return m_skipSaveWorkspacePromptCheck->isChecked();
}
