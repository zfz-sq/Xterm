#include "icons.h"

namespace {
bool g_dark = true; // 与 CommandStore 默认主题一致，applyTheme 会立刻覆盖
}

void setIconThemeDark(bool dark)
{
    g_dark = dark;
}

QIcon themedIcon(const QString &name)
{
    return QIcon(QStringLiteral(":/icons/")
                 + QLatin1String(g_dark ? "dark/" : "light/")
                 + name);
}
