#ifndef ICONS_H
#define ICONS_H

#include <QIcon>
#include <QString>

// 工具栏图标按当前主题取 resources/icons/light 或 dark。
// 窗口图标 app.png 不走这里。
void setIconThemeDark(bool dark);
QIcon themedIcon(const QString &name);

#endif // ICONS_H
