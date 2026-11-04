#include "thememanager.h"

#include <QApplication>
#include <QFile>

#include "commandstore.h"
#include "framelessdialog.h"
#include "icons.h"

ThemeManager::ThemeManager(CommandStore *store, QObject *parent)
    : QObject(parent)
    , m_store(store)
{
}

bool ThemeManager::isDark() const
{
    return m_store->isDarkTheme();
}

void ThemeManager::toggleTheme()
{
    m_store->setDarkTheme(!m_store->isDarkTheme());
    const bool dark = m_store->isDarkTheme();
    applyNow();
    emit themeChanged(dark);
}

void ThemeManager::applyNow()
{
    const bool dark = m_store->isDarkTheme();
    setIconThemeDark(dark);
    QFile f(dark ? QStringLiteral(":/style-dark.qss") : QStringLiteral(":/style-light.qss"));
    if (f.open(QIODevice::ReadOnly | QIODevice::Text))
        qApp->setStyleSheet(QString::fromUtf8(f.readAll()));
    // 公共对话框实例化时自动取用（message/question 这类静态弹窗拿不到主窗口引用）
    FramelessDialog::setThemeHint(dark);
}
