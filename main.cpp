#include "mainwindow.h"

#include <QApplication>
#include <QIcon>

int main(int argc, char *argv[])
{
    QApplication a(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("Xterm"));
    QCoreApplication::setApplicationName(QStringLiteral("Xterm"));
    QCoreApplication::setApplicationVersion(QStringLiteral("0.1.0"));

    a.setWindowIcon(QIcon(QStringLiteral(":/icons/app.png")));

    // UI_AnimateCombo 默认开启：下拉框以 QRollEffect「卷帘」逐帧展开（~150ms），
    // 弹窗每帧改尺寸重绘，观感是迟滞/卡顿。关闭后弹窗一帧直接显示。
    QApplication::setEffectEnabled(Qt::UI_AnimateCombo, false);

    // 主题（深/亮）由 MainWindow 启动时按配置应用
    MainWindow w;
    w.show();
    return QCoreApplication::exec();
}
