#include "thememanager.h"

#include <QApplication>
#include <QFile>

#include "commandstore.h"
#include "framelessdialog.h"
#include "icons.h"

namespace {

QList<ThemeDefinition> createThemes()
{
    // 1. VS Code 经典暗色
    ThemeDefinition dark;
    dark.id = QStringLiteral("dark");
    dark.name = QStringLiteral("Dark (VS Code 经典)");
    dark.isDark = true;
    dark.terminalBg = QColor(0x1E, 0x1E, 0x1E);
    dark.terminalFg = QColor(0xD4, 0xD4, 0xD4);
    dark.selectionBg = QColor(0x26, 0x4F, 0x78);
    dark.infoColor = QColor(0x9C, 0xDC, 0xFE);
    dark.normal[0] = QColor(0x4E, 0x4E, 0x4E); dark.normal[1] = QColor(0xCD, 0x31, 0x31);
    dark.normal[2] = QColor(0x0D, 0xBC, 0x79); dark.normal[3] = QColor(0xE5, 0xE5, 0x11);
    dark.normal[4] = QColor(0x24, 0x72, 0xC8); dark.normal[5] = QColor(0xBC, 0x3F, 0xBC);
    dark.normal[6] = QColor(0x11, 0xA8, 0xCD); dark.normal[7] = QColor(0xE5, 0xE5, 0xE5);
    dark.bright[0] = QColor(0x76, 0x76, 0x76); dark.bright[1] = QColor(0xF1, 0x4C, 0x4C);
    dark.bright[2] = QColor(0x23, 0xD1, 0x8B); dark.bright[3] = QColor(0xF5, 0xF5, 0x43);
    dark.bright[4] = QColor(0x3B, 0x8E, 0xEA); dark.bright[5] = QColor(0xD6, 0x70, 0xD6);
    dark.bright[6] = QColor(0x29, 0xB8, 0xDB); dark.bright[7] = QColor(0xFF, 0xFF, 0xFF);
    dark.windowBg = QColor(0x1E, 0x1E, 0x1E);
    dark.panelBg = QColor(0x24, 0x24, 0x27);
    dark.panelBorder = QColor(0x30, 0x30, 0x34);
    dark.inputBg = QColor(0x2C, 0x2C, 0x30);
    dark.inputBorder = QColor(0x38, 0x38, 0x3E);
    dark.accentColor = QColor(0x00, 0x7A, 0xCC);
    dark.textFg = QColor(0xB8, 0xB8, 0xB8); // 与 style-dark.qss QLabel 原值一致
    dark.dialogBg = QColor(0x25, 0x25, 0x28); // 对话框/菜单原值
    dark.statusBarBg = QColor(0x1E, 0x1E, 0x1E); // 与 style-dark.qss 原值一致
    dark.statusBarFg = QColor(0x9D, 0x9D, 0x9D);
        dark.buttonBg = QColor(0x2C, 0x2C, 0x30); dark.buttonBorder = QColor(0x38, 0x38, 0x3E); // 与基础 QSS 原值一致
        dark.buttonHoverBg = QColor(0x35, 0x35, 0x3A); dark.buttonHoverBorder = QColor(0x4A, 0x4A, 0x52);
        dark.buttonPressedBg = QColor(0x2A, 0x2A, 0x2E);
        dark.toolCheckedBg = QColor(0x09, 0x47, 0x71); dark.toolCheckedBorder = QColor(0x0E, 0x63, 0x9C); // VSCode 蓝选中原值
        dark.listBg = QColor(0x20, 0x20, 0x23); dark.listAltBg = QColor(0x25, 0x25, 0x28); dark.listBorder = QColor(0x30, 0x30, 0x34); // 与基础 QSS 原值一致
        dark.listHoverBg = QColor(0x2C, 0x2C, 0x30); dark.listSelectedBg = QColor(0x09, 0x47, 0x71); dark.listSelectedFg = QColor(0xFF, 0xFF, 0xFF);
        dark.headerBg = QColor(0x24, 0x24, 0x27); dark.headerFg = QColor(0xB8, 0xB8, 0xB8); dark.gridlineColor = QColor(0x2C, 0x2C, 0x30);
        dark.scrollHandleBg = QColor(0x4A, 0x4A, 0x52); dark.scrollHandleHoverBg = QColor(0x5A, 0x5A, 0x62); // 与基础 QSS 原值一致
        dark.scrollTrackBg = QColor(255, 255, 255, 13); // 5% 白叠加，与基础 QSS rgba(255,255,255,0.05) 等值

    // 2. GitHub 经典浅色
    ThemeDefinition light;
    light.id = QStringLiteral("light");
    light.name = QStringLiteral("Light (GitHub 经典)");
    light.isDark = false;
    light.terminalBg = QColor(0xFF, 0xFF, 0xFF);
    light.terminalFg = QColor(0x24, 0x29, 0x2E);
    light.selectionBg = QColor(0xC8, 0xDD, 0xF5);
    light.infoColor = QColor(0x0E, 0x63, 0x9C);
    light.normal[0] = QColor(0x3E, 0x3E, 0x3E); light.normal[1] = QColor(0xC4, 0x2F, 0x2F);
    light.normal[2] = QColor(0x0E, 0x8A, 0x3E); light.normal[3] = QColor(0xB5, 0x89, 0x00);
    light.normal[4] = QColor(0x24, 0x72, 0xC8); light.normal[5] = QColor(0xB0, 0x3A, 0xB0);
    light.normal[6] = QColor(0x11, 0x8A, 0xB3); light.normal[7] = QColor(0x55, 0x55, 0x55);
    light.bright[0] = QColor(0x66, 0x66, 0x66); light.bright[1] = QColor(0xD9, 0x3C, 0x3C);
    light.bright[2] = QColor(0x1F, 0xA8, 0x55); light.bright[3] = QColor(0xC9, 0x9A, 0x0A);
    light.bright[4] = QColor(0x1F, 0x66, 0xC4); light.bright[5] = QColor(0xC5, 0x4E, 0xC5);
    light.bright[6] = QColor(0x14, 0x9C, 0xC8); light.bright[7] = QColor(0x99, 0x99, 0x99);
    light.windowBg = QColor(0xFF, 0xFF, 0xFF);
    light.panelBg = QColor(0xF6, 0xF8, 0xFA);
    light.panelBorder = QColor(0xE1, 0xE4, 0xE8);
    light.inputBg = QColor(0xFF, 0xFF, 0xFF);
    light.inputBorder = QColor(0xD1, 0xD5, 0xDA);
    light.accentColor = QColor(0x03, 0x66, 0xD6);
    light.textFg = QColor(0x5A, 0x5A, 0x5E); // 与 style-light.qss QLabel 原值一致
    light.dialogBg = QColor(0xFF, 0xFF, 0xFF); // 对话框/菜单原值
    light.statusBarBg = QColor(0xF3, 0xF3, 0xF3); // 与 style-light.qss 原值一致
    light.statusBarFg = QColor(0x6A, 0x6A, 0x6E);
    light.buttonBg = QColor(0xFF, 0xFF, 0xFF); light.buttonBorder = QColor(0xC9, 0xC9, 0xCD); // 与基础 QSS 原值一致
    light.buttonHoverBg = QColor(0xF0, 0xF0, 0xF3); light.buttonHoverBorder = QColor(0xA8, 0xA8, 0xAE);
    light.buttonPressedBg = QColor(0xE8, 0xE8, 0xEC);
    light.toolCheckedBg = QColor(0xCD, 0xE4, 0xFB); light.toolCheckedBorder = QColor(0x7F, 0xB6, 0xE8); // 淡蓝选中原值
    light.listBg = QColor(0xFB, 0xFB, 0xFC); light.listAltBg = QColor(0xF3, 0xF3, 0xF5); light.listBorder = QColor(0xDC, 0xDC, 0xDE);
    light.listHoverBg = QColor(0xF0, 0xF0, 0xF3); light.listSelectedBg = QColor(0xD6, 0xE9, 0xFF); light.listSelectedFg = QColor(0x1F, 0x1F, 0x1F);
    light.headerBg = QColor(0xED, 0xED, 0xEF); light.headerFg = QColor(0x3C, 0x40, 0x45); light.gridlineColor = QColor(0xE8, 0xE8, 0xEA);
    light.scrollHandleBg = QColor(0xA9, 0xA9, 0xB0); light.scrollHandleHoverBg = QColor(0x8E, 0x8E, 0x96);
    light.scrollTrackBg = QColor(0, 0, 0, 13);

    // 3. Dracula (吸血鬼)
    ThemeDefinition dracula;
    dracula.id = QStringLiteral("dracula");
    dracula.name = QStringLiteral("Dracula (吸血鬼暗紫)");
    dracula.isDark = true;
    dracula.terminalBg = QColor(0x28, 0x2A, 0x36);
    dracula.terminalFg = QColor(0xC4, 0xB5, 0xFD); // 淡紫正文：Dracula purple 柔化，主题招牌色
    dracula.selectionBg = QColor(0x44, 0x47, 0x5A);
    dracula.infoColor = QColor(0x8B, 0xE9, 0xFD);
    dracula.normal[0] = QColor(0x21, 0x22, 0x2C); dracula.normal[1] = QColor(0xFF, 0x55, 0x55);
    dracula.normal[2] = QColor(0x50, 0xFA, 0x7B); dracula.normal[3] = QColor(0xF1, 0xFA, 0x8C);
    dracula.normal[4] = QColor(0xBD, 0x93, 0xF9); dracula.normal[5] = QColor(0xFF, 0x79, 0xC6);
    dracula.normal[6] = QColor(0x8B, 0xE9, 0xFD); dracula.normal[7] = QColor(0xF8, 0xF8, 0xF2);
    dracula.bright[0] = QColor(0x62, 0x72, 0xA4); dracula.bright[1] = QColor(0xFF, 0x6E, 0x6E);
    dracula.bright[2] = QColor(0x69, 0xFF, 0x94); dracula.bright[3] = QColor(0xFF, 0xFF, 0xA5);
    dracula.bright[4] = QColor(0xD6, 0xAC, 0xFF); dracula.bright[5] = QColor(0xFF, 0x92, 0xDF);
    dracula.bright[6] = QColor(0xA4, 0xFF, 0xFF); dracula.bright[7] = QColor(0xFF, 0xFF, 0xFF);
    dracula.windowBg = QColor(0x28, 0x2A, 0x36); // 终端主体化：背景即画布色
    dracula.panelBg = QColor(0x28, 0x2A, 0x36);
    dracula.panelBorder = QColor(0x44, 0x47, 0x5A);
    dracula.inputBg = QColor(0x34, 0x37, 0x46);
    dracula.inputBorder = QColor(0x62, 0x72, 0xA4);
    dracula.accentColor = QColor(0xBD, 0x93, 0xF9);
    dracula.textFg = QColor(0xF8, 0xF8, 0xF2);
    dracula.dialogBg = QColor(0x28, 0x2A, 0x36);
    dracula.statusBarBg = QColor(0x21, 0x22, 0x2C);
    dracula.statusBarFg = QColor(0xB6, 0xB9, 0xC8);
    dracula.buttonBg = QColor(0x44, 0x47, 0x5A); dracula.buttonBorder = QColor(0x62, 0x72, 0xA4); // 选区紫灰底 + comment 紫边
    dracula.buttonHoverBg = QColor(0x56, 0x59, 0x6E); dracula.buttonHoverBorder = QColor(0x74, 0x82, 0xB4);
    dracula.buttonPressedBg = QColor(0x3B, 0x3D, 0x4F);
    dracula.toolCheckedBg = QColor(0x3B, 0x20, 0x65); dracula.toolCheckedBorder = QColor(0x62, 0x72, 0xA4); // 紫系选中
    dracula.listBg = QColor(0x2D, 0x2F, 0x3C); dracula.listAltBg = QColor(0x28, 0x2A, 0x36); dracula.listBorder = QColor(0x44, 0x47, 0x5A);
    dracula.listHoverBg = QColor(0x3D, 0x3F, 0x52); dracula.listSelectedBg = QColor(0x44, 0x47, 0x5A); dracula.listSelectedFg = QColor(0xF8, 0xF8, 0xF2);
    dracula.headerBg = QColor(0x28, 0x2A, 0x36); dracula.headerFg = QColor(0xB6, 0xB9, 0xC8); dracula.gridlineColor = QColor(0x3B, 0x3D, 0x4F);
    dracula.scrollHandleBg = QColor(0x44, 0x47, 0x5A); dracula.scrollHandleHoverBg = QColor(0x6B, 0x6F, 0x8C);
    dracula.scrollTrackBg = QColor(255, 255, 255, 13);

    // 4. Nord (北欧极光)
    ThemeDefinition nord;
    nord.id = QStringLiteral("nord");
    nord.name = QStringLiteral("Nord (北欧极光蓝)");
    nord.isDark = true;
    nord.terminalBg = QColor(0x2E, 0x34, 0x40);
    nord.terminalFg = QColor(0x9B, 0xB8, 0xD8); // 霜蓝正文：Nord frost 色系，一眼可辨
    nord.selectionBg = QColor(0x43, 0x4C, 0x5E);
    nord.infoColor = QColor(0x88, 0xC0, 0xD0);
    nord.normal[0] = QColor(0x3B, 0x42, 0x52); nord.normal[1] = QColor(0xBF, 0x61, 0x6A);
    nord.normal[2] = QColor(0xA3, 0xBE, 0x8C); nord.normal[3] = QColor(0xEB, 0xCB, 0x8B);
    nord.normal[4] = QColor(0x81, 0xA1, 0xC1); nord.normal[5] = QColor(0xB4, 0x8E, 0xAD);
    nord.normal[6] = QColor(0x88, 0xC0, 0xD0); nord.normal[7] = QColor(0xE5, 0xE9, 0xF0);
    nord.bright[0] = QColor(0x4C, 0x56, 0x6A); nord.bright[1] = QColor(0xD0, 0x87, 0x70);
    nord.bright[2] = QColor(0xB1, 0xD1, 0x99); nord.bright[3] = QColor(0xF0, 0xD3, 0x99);
    nord.bright[4] = QColor(0x8F, 0xAF, 0xCF); nord.bright[5] = QColor(0xC6, 0x9F, 0xBF);
    nord.bright[6] = QColor(0x8F, 0xBC, 0xBB); nord.bright[7] = QColor(0xEC, 0xEF, 0xF4);
    nord.windowBg = QColor(0x2E, 0x34, 0x40); // 终端主体化：背景即画布色
    nord.panelBg = QColor(0x2E, 0x34, 0x40);
    nord.panelBorder = QColor(0x3B, 0x42, 0x52);
    nord.inputBg = QColor(0x3B, 0x42, 0x52);
    nord.inputBorder = QColor(0x4C, 0x56, 0x6A);
    nord.accentColor = QColor(0x88, 0xC0, 0xD0);
    nord.textFg = QColor(0xD8, 0xDE, 0xE9); // Nord4
    nord.dialogBg = QColor(0x2E, 0x34, 0x40);
    nord.statusBarBg = QColor(0x24, 0x29, 0x33);
    nord.statusBarFg = QColor(0x7B, 0x88, 0xA1);
        nord.buttonBg = QColor(0x3B, 0x42, 0x52); nord.buttonBorder = QColor(0x4C, 0x56, 0x6A); // Nord2/Nord3
        nord.buttonHoverBg = QColor(0x43, 0x4C, 0x5E); nord.buttonHoverBorder = QColor(0x5E, 0x6B, 0x7E);
        nord.buttonPressedBg = QColor(0x35, 0x3C, 0x4A);
        nord.toolCheckedBg = QColor(0x43, 0x4C, 0x5E); nord.toolCheckedBorder = QColor(0x81, 0xA1, 0xC1); // Nord 选中
        nord.listBg = QColor(0x34, 0x3B, 0x48); nord.listAltBg = QColor(0x2E, 0x34, 0x40); nord.listBorder = QColor(0x3B, 0x42, 0x52);
        nord.listHoverBg = QColor(0x3B, 0x42, 0x52); nord.listSelectedBg = QColor(0x43, 0x4C, 0x5E); nord.listSelectedFg = QColor(0xEC, 0xEF, 0xF4);
        nord.headerBg = QColor(0x2E, 0x34, 0x40); nord.headerFg = QColor(0xD8, 0xDE, 0xE9); nord.gridlineColor = QColor(0x3B, 0x42, 0x52);
        nord.scrollHandleBg = QColor(0x4C, 0x56, 0x6A); nord.scrollHandleHoverBg = QColor(0x5E, 0x6B, 0x7E);
        nord.scrollTrackBg = QColor(255, 255, 255, 13);

    // 5. Monokai (复古亮黑)
    ThemeDefinition monokai;
    monokai.id = QStringLiteral("monokai");
    monokai.name = QStringLiteral("Monokai (复古暖黑)");
    monokai.isDark = true;
    monokai.terminalBg = QColor(0x27, 0x28, 0x22);
    monokai.terminalFg = QColor(0xED, 0xE0, 0xB3); // 米黄正文：Monokai 暖色招牌
    monokai.selectionBg = QColor(0x49, 0x48, 0x3E);
    monokai.infoColor = QColor(0x66, 0xD9, 0xEF);
    monokai.normal[0] = QColor(0x33, 0x33, 0x33); monokai.normal[1] = QColor(0xF9, 0x26, 0x72);
    monokai.normal[2] = QColor(0xA6, 0xE2, 0x2E); monokai.normal[3] = QColor(0xE6, 0xDB, 0x74);
    monokai.normal[4] = QColor(0x66, 0xD9, 0xEF); monokai.normal[5] = QColor(0xAE, 0x81, 0xFF);
    monokai.normal[6] = QColor(0xA1, 0xEF, 0xE4); monokai.normal[7] = QColor(0xF8, 0xF8, 0xF2);
    monokai.bright[0] = QColor(0x75, 0x71, 0x5E); monokai.bright[1] = QColor(0xFF, 0x5C, 0x8D);
    monokai.bright[2] = QColor(0xB8, 0xF3, 0x40); monokai.bright[3] = QColor(0xFF, 0xE6, 0x8C);
    monokai.bright[4] = QColor(0x8C, 0xE4, 0xFF); monokai.bright[5] = QColor(0xC4, 0x9E, 0xFF);
    monokai.bright[6] = QColor(0xBA, 0xF7, 0xEC); monokai.bright[7] = QColor(0xFF, 0xFF, 0xFF);
    monokai.windowBg = QColor(0x27, 0x28, 0x22); // 终端主体化：背景即画布色
    monokai.panelBg = QColor(0x27, 0x28, 0x22);
    monokai.panelBorder = QColor(0x3E, 0x3D, 0x32);
    monokai.inputBg = QColor(0x38, 0x38, 0x30);
    monokai.inputBorder = QColor(0x49, 0x48, 0x3E);
    monokai.accentColor = QColor(0xFD, 0x97, 0x1F);
    monokai.textFg = QColor(0xF8, 0xF8, 0xF2);
    monokai.dialogBg = QColor(0x27, 0x28, 0x22);
    monokai.statusBarBg = QColor(0x1E, 0x1F, 0x1C);
    monokai.statusBarFg = QColor(0xA6, 0x9F, 0x88);
        monokai.buttonBg = QColor(0x3E, 0x3D, 0x32); monokai.buttonBorder = QColor(0x5B, 0x5A, 0x4A); // 选区色系
        monokai.buttonHoverBg = QColor(0x4A, 0x49, 0x3C); monokai.buttonHoverBorder = QColor(0x6E, 0x6D, 0x5B);
        monokai.buttonPressedBg = QColor(0x34, 0x33, 0x2A);
        monokai.toolCheckedBg = QColor(0x46, 0x50, 0x2C); monokai.toolCheckedBorder = QColor(0x98, 0xCB, 0x00); // 绿系选中
        monokai.listBg = QColor(0x31, 0x30, 0x2A); monokai.listAltBg = QColor(0x2A, 0x2B, 0x24); monokai.listBorder = QColor(0x3E, 0x3D, 0x32);
        monokai.listHoverBg = QColor(0x38, 0x38, 0x30); monokai.listSelectedBg = QColor(0x49, 0x48, 0x3E); monokai.listSelectedFg = QColor(0xF8, 0xF8, 0xF2);
        monokai.headerBg = QColor(0x27, 0x28, 0x22); monokai.headerFg = QColor(0xA6, 0x9F, 0x88); monokai.gridlineColor = QColor(0x38, 0x38, 0x30);
        monokai.scrollHandleBg = QColor(0x49, 0x48, 0x3E); monokai.scrollHandleHoverBg = QColor(0x6E, 0x6D, 0x5B);
        monokai.scrollTrackBg = QColor(255, 255, 255, 13);

    // 6. One Dark Pro (极客深灰)
    ThemeDefinition oneDark;
    oneDark.id = QStringLiteral("one-dark");
    oneDark.name = QStringLiteral("One Dark (现代深灰)");
    oneDark.isDark = true;
    oneDark.terminalBg = QColor(0x21, 0x25, 0x2B);
    oneDark.terminalFg = QColor(0x9F, 0xAD, 0xD3); // 冷蓝正文：One Dark blue 柔化
    oneDark.selectionBg = QColor(0x3E, 0x44, 0x51);
    oneDark.infoColor = QColor(0x61, 0xAF, 0xEF);
    oneDark.normal[0] = QColor(0x28, 0x2C, 0x34); oneDark.normal[1] = QColor(0xE0, 0x6C, 0x75);
    oneDark.normal[2] = QColor(0x98, 0xC3, 0x79); oneDark.normal[3] = QColor(0xE5, 0xC0, 0x7B);
    oneDark.normal[4] = QColor(0x61, 0xAF, 0xEF); oneDark.normal[5] = QColor(0xC6, 0x78, 0xDD);
    oneDark.normal[6] = QColor(0x56, 0xB6, 0xC2); oneDark.normal[7] = QColor(0xAB, 0xB2, 0xBF);
    oneDark.bright[0] = QColor(0x5C, 0x63, 0x70); oneDark.bright[1] = QColor(0xFF, 0x7B, 0x86);
    oneDark.bright[2] = QColor(0xB1, 0xE3, 0x8E); oneDark.bright[3] = QColor(0xFF, 0xD5, 0x88);
    oneDark.bright[4] = QColor(0x73, 0xC2, 0xFF); oneDark.bright[5] = QColor(0xDB, 0x8F, 0xFF);
    oneDark.bright[6] = QColor(0x6D, 0xD8, 0xE7); oneDark.bright[7] = QColor(0xFF, 0xFF, 0xFF);
    oneDark.windowBg = QColor(0x21, 0x25, 0x2B); // 终端主体化：背景即画布色
    oneDark.panelBg = QColor(0x21, 0x25, 0x2B);
    oneDark.panelBorder = QColor(0x2C, 0x31, 0x3A);
    oneDark.inputBg = QColor(0x28, 0x2C, 0x34);
    oneDark.inputBorder = QColor(0x3E, 0x44, 0x51);
    oneDark.accentColor = QColor(0x61, 0xAF, 0xEF);
    oneDark.textFg = QColor(0xAB, 0xB2, 0xBF);
    oneDark.dialogBg = QColor(0x21, 0x25, 0x2B);
    oneDark.statusBarBg = QColor(0x1E, 0x22, 0x27);
    oneDark.statusBarFg = QColor(0x82, 0x89, 0x97);
        oneDark.buttonBg = QColor(0x2C, 0x31, 0x3A); oneDark.buttonBorder = QColor(0x3E, 0x44, 0x51);
        oneDark.buttonHoverBg = QColor(0x35, 0x3B, 0x45); oneDark.buttonHoverBorder = QColor(0x4A, 0x52, 0x61);
        oneDark.buttonPressedBg = QColor(0x26, 0x2A, 0x31);
        oneDark.toolCheckedBg = QColor(0x2C, 0x52, 0x82); oneDark.toolCheckedBorder = QColor(0x3E, 0x6F, 0xA8); // 蓝系选中
        oneDark.listBg = QColor(0x26, 0x2B, 0x33); oneDark.listAltBg = QColor(0x21, 0x25, 0x2B); oneDark.listBorder = QColor(0x2C, 0x31, 0x3A);
        oneDark.listHoverBg = QColor(0x28, 0x2C, 0x34); oneDark.listSelectedBg = QColor(0x3E, 0x44, 0x51); oneDark.listSelectedFg = QColor(0xDC, 0xDF, 0xE4);
        oneDark.headerBg = QColor(0x21, 0x25, 0x2B); oneDark.headerFg = QColor(0xAB, 0xB2, 0xBF); oneDark.gridlineColor = QColor(0x2C, 0x31, 0x3A);
        oneDark.scrollHandleBg = QColor(0x3E, 0x44, 0x51); oneDark.scrollHandleHoverBg = QColor(0x4A, 0x52, 0x61);
        oneDark.scrollTrackBg = QColor(255, 255, 255, 13);

    // 7. Solarized Dark (日光暗青)
    ThemeDefinition solDark;
    solDark.id = QStringLiteral("solarized-dark");
    solDark.name = QStringLiteral("Solarized Dark (日光暗青)");
    solDark.isDark = true;
    solDark.terminalBg = QColor(0x00, 0x2B, 0x36);
    solDark.terminalFg = QColor(0x83, 0x94, 0x96);
    solDark.selectionBg = QColor(0x07, 0x36, 0x42);
    solDark.infoColor = QColor(0x2A, 0xA1, 0x98);
    solDark.normal[0] = QColor(0x07, 0x36, 0x42); solDark.normal[1] = QColor(0xDC, 0x32, 0x2F);
    solDark.normal[2] = QColor(0x85, 0x99, 0x00); solDark.normal[3] = QColor(0xB5, 0x89, 0x00);
    solDark.normal[4] = QColor(0x26, 0x8B, 0xD2); solDark.normal[5] = QColor(0xD3, 0x36, 0x82);
    solDark.normal[6] = QColor(0x2A, 0xA1, 0x98); solDark.normal[7] = QColor(0xEE, 0xE8, 0xD5);
    solDark.bright[0] = QColor(0x00, 0x2B, 0x36); solDark.bright[1] = QColor(0xCB, 0x4B, 0x16);
    solDark.bright[2] = QColor(0x58, 0x6E, 0x75); solDark.bright[3] = QColor(0x65, 0x7B, 0x83);
    solDark.bright[4] = QColor(0x83, 0x94, 0x96); solDark.bright[5] = QColor(0x6C, 0x71, 0xC4);
    solDark.bright[6] = QColor(0x93, 0xA1, 0xA1); solDark.bright[7] = QColor(0xFD, 0xF6, 0xE3);
    solDark.windowBg = QColor(0x00, 0x2B, 0x36); // 终端主体化：背景即画布色
    solDark.panelBg = QColor(0x00, 0x2B, 0x36);
    solDark.panelBorder = QColor(0x07, 0x36, 0x42);
    solDark.inputBg = QColor(0x07, 0x36, 0x42);
    solDark.inputBorder = QColor(0x58, 0x6E, 0x75);
    solDark.accentColor = QColor(0x2A, 0xA1, 0x98);
    solDark.textFg = QColor(0x93, 0xA1, 0xA1); // Solarized base1
    solDark.dialogBg = QColor(0x00, 0x2B, 0x36);
    solDark.statusBarBg = QColor(0x00, 0x21, 0x2B);
    solDark.statusBarFg = QColor(0x93, 0xA1, 0xA1); // Solarized base1
        solDark.buttonBg = QColor(0x07, 0x36, 0x42); solDark.buttonBorder = QColor(0x58, 0x6E, 0x75); // base02 + base01
        solDark.buttonHoverBg = QColor(0x0B, 0x45, 0x54); solDark.buttonHoverBorder = QColor(0x6C, 0x84, 0x94);
        solDark.buttonPressedBg = QColor(0x05, 0x2B, 0x34);
        solDark.toolCheckedBg = QColor(0x0E, 0x45, 0x54); solDark.toolCheckedBorder = QColor(0x26, 0x8B, 0xD2); // Solarized blue
        solDark.listBg = QColor(0x0A, 0x34, 0x40); solDark.listAltBg = QColor(0x07, 0x36, 0x42); solDark.listBorder = QColor(0x0E, 0x45, 0x54);
        solDark.listHoverBg = QColor(0x0B, 0x45, 0x54); solDark.listSelectedBg = QColor(0x0E, 0x45, 0x54); solDark.listSelectedFg = QColor(0xEE, 0xE8, 0xD5);
        solDark.headerBg = QColor(0x00, 0x2B, 0x36); solDark.headerFg = QColor(0x93, 0xA1, 0xA1); solDark.gridlineColor = QColor(0x07, 0x36, 0x42);
        solDark.scrollHandleBg = QColor(0x58, 0x6E, 0x75); solDark.scrollHandleHoverBg = QColor(0x6C, 0x84, 0x94);
        solDark.scrollTrackBg = QColor(255, 255, 255, 13);

    // 8. Solarized Light (日光米黄)
    ThemeDefinition solLight;
    solLight.id = QStringLiteral("solarized-light");
    solLight.name = QStringLiteral("Solarized Light (日光米白)");
    solLight.isDark = false;
    solLight.terminalBg = QColor(0xFD, 0xF6, 0xE3);
    solLight.terminalFg = QColor(0x65, 0x7B, 0x83);
    solLight.selectionBg = QColor(0xEE, 0xE8, 0xD5);
    solLight.infoColor = QColor(0x2A, 0xA1, 0x98);
    solLight.normal[0] = QColor(0xEE, 0xE8, 0xD5); solLight.normal[1] = QColor(0xDC, 0x32, 0x2F);
    solLight.normal[2] = QColor(0x85, 0x99, 0x00); solLight.normal[3] = QColor(0xB5, 0x89, 0x00);
    solLight.normal[4] = QColor(0x26, 0x8B, 0xD2); solLight.normal[5] = QColor(0xD3, 0x36, 0x82);
    solLight.normal[6] = QColor(0x2A, 0xA1, 0x98); solLight.normal[7] = QColor(0x07, 0x36, 0x42);
    solLight.bright[0] = QColor(0xFD, 0xF6, 0xE3); solLight.bright[1] = QColor(0xCB, 0x4B, 0x16);
    solLight.bright[2] = QColor(0x93, 0xA1, 0xA1); solLight.bright[3] = QColor(0x83, 0x94, 0x96);
    solLight.bright[4] = QColor(0x65, 0x7B, 0x83); solLight.bright[5] = QColor(0x6C, 0x71, 0xC4);
    solLight.bright[6] = QColor(0x58, 0x6E, 0x75); solLight.bright[7] = QColor(0x00, 0x2B, 0x36);
    solLight.windowBg = QColor(0xFD, 0xF6, 0xE3); // 终端主体化：背景即画布色
    solLight.panelBg = QColor(0xFD, 0xF6, 0xE3);
    solLight.panelBorder = QColor(0xE0, 0xDA, 0xC6);
    solLight.inputBg = QColor(0xEE, 0xE8, 0xD5);
    solLight.inputBorder = QColor(0xD3, 0xCC, 0xB8);
    solLight.accentColor = QColor(0x26, 0x8B, 0xD2);
    solLight.textFg = QColor(0x58, 0x6E, 0x75); // Solarized base01
    solLight.dialogBg = QColor(0xFD, 0xF6, 0xE3);
    solLight.statusBarBg = QColor(0xF5, 0xEF, 0xDC);
    solLight.statusBarFg = QColor(0x83, 0x94, 0x96); // Solarized base0
        solLight.buttonBg = QColor(0xFD, 0xF6, 0xE3); solLight.buttonBorder = QColor(0xD3, 0xCC, 0xB8);
        solLight.buttonHoverBg = QColor(0xF5, 0xEF, 0xDC); solLight.buttonHoverBorder = QColor(0xB8, 0xB0, 0x9A);
        solLight.buttonPressedBg = QColor(0xED, 0xE6, 0xD0);
        solLight.toolCheckedBg = QColor(0xDC, 0xD9, 0xC8); solLight.toolCheckedBorder = QColor(0x26, 0x8B, 0xD2); // 米色底 + blue 边
        solLight.listBg = QColor(0xF7, 0xF1, 0xDF); solLight.listAltBg = QColor(0xEE, 0xE8, 0xD5); solLight.listBorder = QColor(0xD3, 0xCC, 0xB8);
        solLight.listHoverBg = QColor(0xEF, 0xE9, 0xD6); solLight.listSelectedBg = QColor(0xDC, 0xD9, 0xC8); solLight.listSelectedFg = QColor(0x58, 0x6E, 0x75);
        solLight.headerBg = QColor(0xEE, 0xE8, 0xD5); solLight.headerFg = QColor(0x65, 0x7B, 0x83); solLight.gridlineColor = QColor(0xE3, 0xDC, 0xC6);
        solLight.scrollHandleBg = QColor(0xD3, 0xCC, 0xB8); solLight.scrollHandleHoverBg = QColor(0xB8, 0xB0, 0x9A);
        solLight.scrollTrackBg = QColor(0, 0, 0, 13);

    // 9. Termius (夜航绿)：参考 Termius 默认配色——深夜蓝底 + 亮绿正文，
    // UI 主色 Termius 蓝；SGR 色按 Termius 语义色（error/success/info/primary）配套，
    // 关键词槽位规则随之自动换到 Termius 色系
    ThemeDefinition termius;
    termius.id = QStringLiteral("termius");
    termius.name = QStringLiteral("Termius (夜航绿)");
    termius.isDark = true;
    termius.terminalBg = QColor(0x14, 0x17, 0x29); // Termius 深夜蓝
    termius.terminalFg = QColor(0x00, 0xCC, 0x74); // Termius 招牌亮绿正文
    termius.selectionBg = QColor(0x1E, 0x3E, 0x3A); // 绿系选区：Termius 绿半透明的实色近似
    termius.infoColor = QColor(0xA4, 0xB3, 0xBA);   // Termius disabled 灰蓝：系统消息
    termius.normal[0] = QColor(0x23, 0x29, 0x46); termius.normal[1] = QColor(0xF2, 0x5E, 0x61);
    termius.normal[2] = QColor(0x21, 0xB5, 0x68); termius.normal[3] = QColor(0xFF, 0xD1, 0x66);
    termius.normal[4] = QColor(0x1F, 0x8E, 0xF2); termius.normal[5] = QColor(0x9D, 0x7C, 0xE8);
    termius.normal[6] = QColor(0x21, 0xD0, 0xC9); termius.normal[7] = QColor(0xC4, 0xCD, 0xE0);
    termius.bright[0] = QColor(0x3A, 0x42, 0x66); termius.bright[1] = QColor(0xFF, 0x80, 0x82);
    termius.bright[2] = QColor(0x00, 0xE8, 0x83); termius.bright[3] = QColor(0xFF, 0xE0, 0x8A);
    termius.bright[4] = QColor(0x5A, 0xB0, 0xFF); termius.bright[5] = QColor(0xBC, 0xA4, 0xF5);
    termius.bright[6] = QColor(0x5C, 0xE8, 0xDC); termius.bright[7] = QColor(0xE8, 0xEE, 0xF9);
    termius.windowBg = QColor(0x14, 0x17, 0x29); // Termius main
    termius.panelBg = QColor(0x1A, 0x20, 0x36);
    termius.panelBorder = QColor(0x2A, 0x31, 0x52);
    termius.inputBg = QColor(0x1E, 0x25, 0x40);
    termius.inputBorder = QColor(0x32, 0x3B, 0x5E);
    termius.accentColor = QColor(0x1F, 0x8E, 0xF2); // Termius primary 蓝
    termius.textFg = QColor(0xC9, 0xD3, 0xE8);      // 界面正文冷白
    termius.dialogBg = QColor(0x1A, 0x20, 0x36);
    termius.statusBarBg = QColor(0x10, 0x13, 0x22);
    termius.statusBarFg = QColor(0x7E, 0x8D, 0xAD);
        termius.buttonBg = QColor(0x1E, 0x25, 0x40); termius.buttonBorder = QColor(0x32, 0x3B, 0x5E); // 与输入框同源
        termius.buttonHoverBg = QColor(0x28, 0x32, 0x5A); termius.buttonHoverBorder = QColor(0x46, 0x53, 0x8A);
        termius.buttonPressedBg = QColor(0x17, 0x1E, 0x36);
        termius.toolCheckedBg = QColor(0x16, 0x32, 0x4E); termius.toolCheckedBorder = QColor(0x1F, 0x8E, 0xF2); // Termius 蓝选中
        termius.listBg = QColor(0x1D, 0x24, 0x39); termius.listAltBg = QColor(0x1A, 0x20, 0x36); termius.listBorder = QColor(0x2A, 0x31, 0x52);
        termius.listHoverBg = QColor(0x23, 0x2B, 0x4A); termius.listSelectedBg = QColor(0x16, 0x32, 0x4E); termius.listSelectedFg = QColor(0xDC, 0xE4, 0xF5);
        termius.headerBg = QColor(0x17, 0x1D, 0x2F); termius.headerFg = QColor(0xA4, 0xB3, 0xBA); termius.gridlineColor = QColor(0x23, 0x2B, 0x4A);
        termius.scrollHandleBg = QColor(0x28, 0x50, 0x49); termius.scrollHandleHoverBg = QColor(0x21, 0xB5, 0x68); // 用户钦定：暗绿滑块→悬停招牌亮绿
        termius.scrollTrackBg = QColor(0x2C, 0x2F, 0x3F); // 用户钦定实色轨道

    // 10. Flexoki Dark：stephango/flexoki 官方——近黑纸感底 + 暖白正文，橙色招牌强调
    ThemeDefinition flexokiDark;
    flexokiDark.id = QStringLiteral("flexoki-dark");
    flexokiDark.name = QStringLiteral("Flexoki Dark");
    flexokiDark.isDark = true;
    flexokiDark.terminalBg = QColor(0x10, 0x00, 0x00);
    flexokiDark.terminalFg = QColor(0xCE, 0xCE, 0xC4);
    flexokiDark.selectionBg = QColor(0x40, 0x3E, 0x3D);
    flexokiDark.infoColor = QColor(0x87, 0x84, 0x7F);
    flexokiDark.normal[0] = QColor(0x26, 0x25, 0x24); flexokiDark.normal[1] = QColor(0xD1, 0x4D, 0x41);
    flexokiDark.normal[2] = QColor(0x87, 0x9A, 0x39); flexokiDark.normal[3] = QColor(0xD0, 0xA2, 0x15);
    flexokiDark.normal[4] = QColor(0x43, 0x85, 0xBE); flexokiDark.normal[5] = QColor(0x8B, 0x7E, 0xC8);
    flexokiDark.normal[6] = QColor(0x3A, 0xA9, 0x9F); flexokiDark.normal[7] = QColor(0xCE, 0xCE, 0xC4);
    flexokiDark.bright[0] = QColor(0x57, 0x55, 0x50); flexokiDark.bright[1] = QColor(0xFF, 0x43, 0x40);
    flexokiDark.bright[2] = QColor(0x9B, 0xB7, 0x3D); flexokiDark.bright[3] = QColor(0xEA, 0xC7, 0x3B);
    flexokiDark.bright[4] = QColor(0x68, 0xB0, 0xF4); flexokiDark.bright[5] = QColor(0xA9, 0x9A, 0xE1);
    flexokiDark.bright[6] = QColor(0x45, 0xD9, 0xCF); flexokiDark.bright[7] = QColor(0xF2, 0xF0, 0xE5);
    flexokiDark.windowBg = QColor(0x10, 0x00, 0x00);
    flexokiDark.panelBg = QColor(0x1C, 0x1B, 0x1A);
    flexokiDark.panelBorder = QColor(0x36, 0x35, 0x34);
    flexokiDark.inputBg = QColor(0x26, 0x25, 0x24);
    flexokiDark.inputBorder = QColor(0x40, 0x3E, 0x3D);
    flexokiDark.accentColor = QColor(0xDA, 0x70, 0x2C);
    flexokiDark.textFg = QColor(0xCE, 0xCE, 0xC4);
    flexokiDark.dialogBg = QColor(0x1C, 0x1B, 0x1A);
    flexokiDark.statusBarBg = QColor(0x1C, 0x1B, 0x1A);
    flexokiDark.statusBarFg = QColor(0x87, 0x84, 0x7F);
        flexokiDark.buttonBg = QColor(0x26, 0x25, 0x24); flexokiDark.buttonBorder = QColor(0x40, 0x3E, 0x3D);
        flexokiDark.buttonHoverBg = QColor(0x36, 0x35, 0x34); flexokiDark.buttonHoverBorder = QColor(0x57, 0x55, 0x50);
        flexokiDark.buttonPressedBg = QColor(0x1C, 0x1B, 0x1A);
        flexokiDark.toolCheckedBg = QColor(0x36, 0x35, 0x34); flexokiDark.toolCheckedBorder = QColor(0xDA, 0x70, 0x2C);
        flexokiDark.listBg = QColor(0x1C, 0x1B, 0x1A); flexokiDark.listAltBg = QColor(0x26, 0x25, 0x24); flexokiDark.listBorder = QColor(0x36, 0x35, 0x34);
        flexokiDark.listHoverBg = QColor(0x26, 0x25, 0x24); flexokiDark.listSelectedBg = QColor(0x36, 0x35, 0x34); flexokiDark.listSelectedFg = QColor(0xFF, 0xFC, 0xF0);
        flexokiDark.headerBg = QColor(0x1C, 0x1B, 0x1A); flexokiDark.headerFg = QColor(0x87, 0x84, 0x7F); flexokiDark.gridlineColor = QColor(0x26, 0x25, 0x24);
        flexokiDark.scrollHandleBg = QColor(0x57, 0x55, 0x50); flexokiDark.scrollHandleHoverBg = QColor(0xDA, 0x70, 0x2C);
        flexokiDark.scrollTrackBg = QColor(0, 0, 0, 13);

    // 11. Flexoki Light：同套强调色换纸白底（flexoki 特点：双色模式共强调色）
    ThemeDefinition flexokiLight;
    flexokiLight.id = QStringLiteral("flexoki-light");
    flexokiLight.name = QStringLiteral("Flexoki Light");
    flexokiLight.isDark = false;
    flexokiLight.terminalBg = QColor(0xFF, 0xFC, 0xF0);
    flexokiLight.terminalFg = QColor(0x10, 0x00, 0x00);
    flexokiLight.selectionBg = QColor(0xE6, 0xE4, 0xD9);
    flexokiLight.infoColor = QColor(0x87, 0x84, 0x7F);
    flexokiLight.normal[0] = QColor(0x26, 0x25, 0x24); flexokiLight.normal[1] = QColor(0xD1, 0x4D, 0x41);
    flexokiLight.normal[2] = QColor(0x87, 0x9A, 0x39); flexokiLight.normal[3] = QColor(0xD0, 0xA2, 0x15);
    flexokiLight.normal[4] = QColor(0x43, 0x85, 0xBE); flexokiLight.normal[5] = QColor(0x8B, 0x7E, 0xC8);
    flexokiLight.normal[6] = QColor(0x3A, 0xA9, 0x9F); flexokiLight.normal[7] = QColor(0xB9, 0xB7, 0xA8);
    flexokiLight.bright[0] = QColor(0x57, 0x55, 0x50); flexokiLight.bright[1] = QColor(0xFF, 0x43, 0x40);
    flexokiLight.bright[2] = QColor(0x9B, 0xB7, 0x3D); flexokiLight.bright[3] = QColor(0xEA, 0xC7, 0x3B);
    flexokiLight.bright[4] = QColor(0x68, 0xB0, 0xF4); flexokiLight.bright[5] = QColor(0xA9, 0x9A, 0xE1);
    flexokiLight.bright[6] = QColor(0x45, 0xD9, 0xCF); flexokiLight.bright[7] = QColor(0x6F, 0x69, 0x65);
    flexokiLight.windowBg = QColor(0xFF, 0xFC, 0xF0);
    flexokiLight.panelBg = QColor(0xF2, 0xF0, 0xE5);
    flexokiLight.panelBorder = QColor(0xE6, 0xE4, 0xD9);
    flexokiLight.inputBg = QColor(0xF2, 0xF0, 0xE5);
    flexokiLight.inputBorder = QColor(0xB9, 0xB7, 0xA8);
    flexokiLight.accentColor = QColor(0xDA, 0x70, 0x2C);
    flexokiLight.textFg = QColor(0x40, 0x3E, 0x3D);
    flexokiLight.dialogBg = QColor(0xF2, 0xF0, 0xE5);
    flexokiLight.statusBarBg = QColor(0xF2, 0xF0, 0xE5);
    flexokiLight.statusBarFg = QColor(0x6F, 0x69, 0x65);
        flexokiLight.buttonBg = QColor(0xF2, 0xF0, 0xE5); flexokiLight.buttonBorder = QColor(0xB9, 0xB7, 0xA8);
        flexokiLight.buttonHoverBg = QColor(0xE6, 0xE4, 0xD9); flexokiLight.buttonHoverBorder = QColor(0x87, 0x84, 0x7F);
        flexokiLight.buttonPressedBg = QColor(0xCE, 0xCE, 0xC4);
        flexokiLight.toolCheckedBg = QColor(0xE6, 0xE4, 0xD9); flexokiLight.toolCheckedBorder = QColor(0xDA, 0x70, 0x2C);
        flexokiLight.listBg = QColor(0xF2, 0xF0, 0xE5); flexokiLight.listAltBg = QColor(0xE6, 0xE4, 0xD9); flexokiLight.listBorder = QColor(0xE6, 0xE4, 0xD9);
        flexokiLight.listHoverBg = QColor(0xE6, 0xE4, 0xD9); flexokiLight.listSelectedBg = QColor(0xE6, 0xE4, 0xD9); flexokiLight.listSelectedFg = QColor(0x10, 0x00, 0x00);
        flexokiLight.headerBg = QColor(0xE6, 0xE4, 0xD9); flexokiLight.headerFg = QColor(0x6F, 0x69, 0x65); flexokiLight.gridlineColor = QColor(0xE6, 0xE4, 0xD9);
        flexokiLight.scrollHandleBg = QColor(0xB9, 0xB7, 0xA8); flexokiLight.scrollHandleHoverBg = QColor(0xDA, 0x70, 0x2C);
        flexokiLight.scrollTrackBg = QColor(0, 0, 0, 13);

    // 12. Kanagawa Wave：rebelot/kanagawa.nvim 默认——墨蓝黑底 + 暖米白正文，waveBlue 强调
    ThemeDefinition kanagawaWave;
    kanagawaWave.id = QStringLiteral("kanagawa-wave");
    kanagawaWave.name = QStringLiteral("Kanagawa Wave");
    kanagawaWave.isDark = true;
    kanagawaWave.terminalBg = QColor(0x1F, 0x1F, 0x28);
    kanagawaWave.terminalFg = QColor(0xDC, 0xD7, 0xBA);
    kanagawaWave.selectionBg = QColor(0x2D, 0x4F, 0x67);
    kanagawaWave.infoColor = QColor(0x72, 0x71, 0x69);
    kanagawaWave.normal[0] = QColor(0x09, 0x06, 0x18); kanagawaWave.normal[1] = QColor(0xC3, 0x40, 0x43);
    kanagawaWave.normal[2] = QColor(0x76, 0x94, 0x6A); kanagawaWave.normal[3] = QColor(0xC0, 0xA3, 0x6E);
    kanagawaWave.normal[4] = QColor(0x7E, 0x9C, 0xD8); kanagawaWave.normal[5] = QColor(0x95, 0x7F, 0xB8);
    kanagawaWave.normal[6] = QColor(0x6A, 0x95, 0x89); kanagawaWave.normal[7] = QColor(0xC8, 0xC0, 0x93);
    kanagawaWave.bright[0] = QColor(0x72, 0x71, 0x69); kanagawaWave.bright[1] = QColor(0xE8, 0x24, 0x24);
    kanagawaWave.bright[2] = QColor(0x98, 0xBB, 0x6C); kanagawaWave.bright[3] = QColor(0xFF, 0xA0, 0x66);
    kanagawaWave.bright[4] = QColor(0x7F, 0xB4, 0xCA); kanagawaWave.bright[5] = QColor(0x93, 0x8A, 0xA9);
    kanagawaWave.bright[6] = QColor(0x7A, 0xA8, 0x9F); kanagawaWave.bright[7] = QColor(0xDC, 0xD7, 0xBA);
    kanagawaWave.windowBg = QColor(0x1F, 0x1F, 0x28);
    kanagawaWave.panelBg = QColor(0x1A, 0x1A, 0x26);
    kanagawaWave.panelBorder = QColor(0x2A, 0x2A, 0x37);
    kanagawaWave.inputBg = QColor(0x26, 0x26, 0x33);
    kanagawaWave.inputBorder = QColor(0x36, 0x36, 0x49);
    kanagawaWave.accentColor = QColor(0x7E, 0x9C, 0xD8);
    kanagawaWave.textFg = QColor(0xC8, 0xC0, 0x93);
    kanagawaWave.dialogBg = QColor(0x1A, 0x1A, 0x26);
    kanagawaWave.statusBarBg = QColor(0x16, 0x16, 0x1D);
    kanagawaWave.statusBarFg = QColor(0x72, 0x71, 0x69);
        kanagawaWave.buttonBg = QColor(0x26, 0x26, 0x33); kanagawaWave.buttonBorder = QColor(0x36, 0x36, 0x49);
        kanagawaWave.buttonHoverBg = QColor(0x2E, 0x2E, 0x3C); kanagawaWave.buttonHoverBorder = QColor(0x4A, 0x4A, 0x5F);
        kanagawaWave.buttonPressedBg = QColor(0x1A, 0x1A, 0x26);
        kanagawaWave.toolCheckedBg = QColor(0x2D, 0x4F, 0x67); kanagawaWave.toolCheckedBorder = QColor(0x7E, 0x9C, 0xD8);
        kanagawaWave.listBg = QColor(0x1A, 0x1A, 0x26); kanagawaWave.listAltBg = QColor(0x23, 0x23, 0x2E); kanagawaWave.listBorder = QColor(0x2A, 0x2A, 0x37);
        kanagawaWave.listHoverBg = QColor(0x2A, 0x2A, 0x37); kanagawaWave.listSelectedBg = QColor(0x2D, 0x4F, 0x67); kanagawaWave.listSelectedFg = QColor(0xDC, 0xD7, 0xBA);
        kanagawaWave.headerBg = QColor(0x16, 0x16, 0x1D); kanagawaWave.headerFg = QColor(0x72, 0x71, 0x69); kanagawaWave.gridlineColor = QColor(0x2A, 0x2A, 0x37);
        kanagawaWave.scrollHandleBg = QColor(0x36, 0x36, 0x49); kanagawaWave.scrollHandleHoverBg = QColor(0x7F, 0xB4, 0xCA);
        kanagawaWave.scrollTrackBg = QColor(0, 0, 0, 13);

    // 13. Kanagawa Dragon：kanagawa-dragon 变体——更暗更灰，冷灰绿正文
    ThemeDefinition kanagawaDragon;
    kanagawaDragon.id = QStringLiteral("kanagawa-dragon");
    kanagawaDragon.name = QStringLiteral("Kanagawa Dragon");
    kanagawaDragon.isDark = true;
    kanagawaDragon.terminalBg = QColor(0x18, 0x16, 0x16);
    kanagawaDragon.terminalFg = QColor(0xC5, 0xC9, 0xC5);
    kanagawaDragon.selectionBg = QColor(0x2C, 0x2E, 0x33);
    kanagawaDragon.infoColor = QColor(0x82, 0x82, 0x7C);
    kanagawaDragon.normal[0] = QColor(0x0D, 0x0C, 0x0C); kanagawaDragon.normal[1] = QColor(0xC4, 0x74, 0x6E);
    kanagawaDragon.normal[2] = QColor(0x8A, 0x9A, 0x7B); kanagawaDragon.normal[3] = QColor(0xC4, 0xB2, 0x8A);
    kanagawaDragon.normal[4] = QColor(0x8B, 0xA4, 0xC0); kanagawaDragon.normal[5] = QColor(0xA2, 0x92, 0xA3);
    kanagawaDragon.normal[6] = QColor(0x8E, 0xA4, 0xA2); kanagawaDragon.normal[7] = QColor(0xC8, 0xC0, 0x93);
    kanagawaDragon.bright[0] = QColor(0x7D, 0x7C, 0x76); kanagawaDragon.bright[1] = QColor(0xE4, 0x68, 0x76);
    kanagawaDragon.bright[2] = QColor(0x87, 0xA9, 0x87); kanagawaDragon.bright[3] = QColor(0xE6, 0xC3, 0x84);
    kanagawaDragon.bright[4] = QColor(0x7F, 0xB4, 0xCA); kanagawaDragon.bright[5] = QColor(0x93, 0x8A, 0xA9);
    kanagawaDragon.bright[6] = QColor(0x7A, 0xA8, 0x9F); kanagawaDragon.bright[7] = QColor(0xDC, 0xD7, 0xBA);
    kanagawaDragon.windowBg = QColor(0x18, 0x16, 0x16);
    kanagawaDragon.panelBg = QColor(0x1C, 0x1A, 0x1A);
    kanagawaDragon.panelBorder = QColor(0x2B, 0x29, 0x29);
    kanagawaDragon.inputBg = QColor(0x26, 0x25, 0x25);
    kanagawaDragon.inputBorder = QColor(0x38, 0x36, 0x36);
    kanagawaDragon.accentColor = QColor(0x8B, 0xA4, 0xC0);
    kanagawaDragon.textFg = QColor(0xC5, 0xC9, 0xC5);
    kanagawaDragon.dialogBg = QColor(0x1C, 0x1A, 0x1A);
    kanagawaDragon.statusBarBg = QColor(0x16, 0x15, 0x15);
    kanagawaDragon.statusBarFg = QColor(0x82, 0x82, 0x7C);
        kanagawaDragon.buttonBg = QColor(0x26, 0x25, 0x25); kanagawaDragon.buttonBorder = QColor(0x38, 0x36, 0x36);
        kanagawaDragon.buttonHoverBg = QColor(0x30, 0x2E, 0x2E); kanagawaDragon.buttonHoverBorder = QColor(0x4A, 0x48, 0x48);
        kanagawaDragon.buttonPressedBg = QColor(0x1C, 0x1A, 0x1A);
        kanagawaDragon.toolCheckedBg = QColor(0x2C, 0x2E, 0x33); kanagawaDragon.toolCheckedBorder = QColor(0x8B, 0xA4, 0xC0);
        kanagawaDragon.listBg = QColor(0x1C, 0x1A, 0x1A); kanagawaDragon.listAltBg = QColor(0x23, 0x21, 0x21); kanagawaDragon.listBorder = QColor(0x2B, 0x29, 0x29);
        kanagawaDragon.listHoverBg = QColor(0x2B, 0x29, 0x29); kanagawaDragon.listSelectedBg = QColor(0x2C, 0x2E, 0x33); kanagawaDragon.listSelectedFg = QColor(0xDC, 0xD7, 0xBA);
        kanagawaDragon.headerBg = QColor(0x16, 0x15, 0x15); kanagawaDragon.headerFg = QColor(0x82, 0x82, 0x7C); kanagawaDragon.gridlineColor = QColor(0x2B, 0x29, 0x29);
        kanagawaDragon.scrollHandleBg = QColor(0x38, 0x36, 0x36); kanagawaDragon.scrollHandleHoverBg = QColor(0x7F, 0xB4, 0xCA);
        kanagawaDragon.scrollTrackBg = QColor(0, 0, 0, 13);

    // 14. Kanagawa Lotus：lotus 亮色变体——米黄纸底 + 靛紫正文
    ThemeDefinition kanagawaLotus;
    kanagawaLotus.id = QStringLiteral("kanagawa-lotus");
    kanagawaLotus.name = QStringLiteral("Kanagawa Lotus");
    kanagawaLotus.isDark = false;
    kanagawaLotus.terminalBg = QColor(0xF2, 0xEC, 0xBC);
    kanagawaLotus.terminalFg = QColor(0x54, 0x54, 0x6D);
    kanagawaLotus.selectionBg = QColor(0xD8, 0xD4, 0xB8);
    kanagawaLotus.infoColor = QColor(0x8A, 0x88, 0x67);
    kanagawaLotus.normal[0] = QColor(0x1F, 0x1F, 0x28); kanagawaLotus.normal[1] = QColor(0xC8, 0x40, 0x53);
    kanagawaLotus.normal[2] = QColor(0x48, 0x5E, 0x30); kanagawaLotus.normal[3] = QColor(0x8F, 0x5E, 0x15);
    kanagawaLotus.normal[4] = QColor(0x34, 0x53, 0x9D); kanagawaLotus.normal[5] = QColor(0x66, 0x4A, 0x95);
    kanagawaLotus.normal[6] = QColor(0x0F, 0x4C, 0x6C); kanagawaLotus.normal[7] = QColor(0x43, 0x43, 0x6C);
    kanagawaLotus.bright[0] = QColor(0x6F, 0x6A, 0x50); kanagawaLotus.bright[1] = QColor(0xD9, 0x65, 0x7C);
    kanagawaLotus.bright[2] = QColor(0x6E, 0x7F, 0x4B); kanagawaLotus.bright[3] = QColor(0xB8, 0x89, 0x53);
    kanagawaLotus.bright[4] = QColor(0x45, 0x68, 0xBF); kanagawaLotus.bright[5] = QColor(0x71, 0x68, 0x9E);
    kanagawaLotus.bright[6] = QColor(0x33, 0x6C, 0x80); kanagawaLotus.bright[7] = QColor(0x54, 0x54, 0x6D);
    kanagawaLotus.windowBg = QColor(0xF2, 0xEC, 0xBC);
    kanagawaLotus.panelBg = QColor(0xEC, 0xE6, 0xCC);
    kanagawaLotus.panelBorder = QColor(0xD8, 0xD2, 0xB4);
    kanagawaLotus.inputBg = QColor(0xE9, 0xE3, 0xC8);
    kanagawaLotus.inputBorder = QColor(0xC5, 0xBF, 0xA0);
    kanagawaLotus.accentColor = QColor(0x62, 0x4C, 0x83);
    kanagawaLotus.textFg = QColor(0x54, 0x54, 0x6D);
    kanagawaLotus.dialogBg = QColor(0xEC, 0xE6, 0xCC);
    kanagawaLotus.statusBarBg = QColor(0xE7, 0xE1, 0xC2);
    kanagawaLotus.statusBarFg = QColor(0x8A, 0x88, 0x67);
        kanagawaLotus.buttonBg = QColor(0xE9, 0xE3, 0xC8); kanagawaLotus.buttonBorder = QColor(0xC5, 0xBF, 0xA0);
        kanagawaLotus.buttonHoverBg = QColor(0xE0, 0xDA, 0xB8); kanagawaLotus.buttonHoverBorder = QColor(0xA8, 0xA2, 0x82);
        kanagawaLotus.buttonPressedBg = QColor(0xD8, 0xD2, 0xB4);
        kanagawaLotus.toolCheckedBg = QColor(0xD8, 0xD2, 0xB4); kanagawaLotus.toolCheckedBorder = QColor(0x62, 0x4C, 0x83);
        kanagawaLotus.listBg = QColor(0xEC, 0xE6, 0xCC); kanagawaLotus.listAltBg = QColor(0xE7, 0xE1, 0xC2); kanagawaLotus.listBorder = QColor(0xD8, 0xD2, 0xB4);
        kanagawaLotus.listHoverBg = QColor(0xE7, 0xE1, 0xC2); kanagawaLotus.listSelectedBg = QColor(0xD8, 0xD2, 0xB4); kanagawaLotus.listSelectedFg = QColor(0x1F, 0x1F, 0x28);
        kanagawaLotus.headerBg = QColor(0xE7, 0xE1, 0xC2); kanagawaLotus.headerFg = QColor(0x8A, 0x88, 0x67); kanagawaLotus.gridlineColor = QColor(0xE0, 0xDA, 0xB8);
        kanagawaLotus.scrollHandleBg = QColor(0xC5, 0xBF, 0xA0); kanagawaLotus.scrollHandleHoverBg = QColor(0x62, 0x4C, 0x83);
        kanagawaLotus.scrollTrackBg = QColor(0, 0, 0, 13);

    // 15. Hacker Blue：黑客风电蓝——近黑蓝底 + 电蓝正文
    ThemeDefinition hackerBlue;
    hackerBlue.id = QStringLiteral("hacker-blue");
    hackerBlue.name = QStringLiteral("Hacker Blue");
    hackerBlue.isDark = true;
    hackerBlue.terminalBg = QColor(0x00, 0x10, 0x19);
    hackerBlue.terminalFg = QColor(0x2C, 0xA5, 0xFF);
    hackerBlue.selectionBg = QColor(0x0D, 0x3D, 0x66);
    hackerBlue.infoColor = QColor(0x4A, 0x7A, 0x99);
    hackerBlue.normal[0] = QColor(0x0A, 0x2A, 0x3A); hackerBlue.normal[1] = QColor(0xFF, 0x5C, 0x57);
    hackerBlue.normal[2] = QColor(0x33, 0xE0, 0x6A); hackerBlue.normal[3] = QColor(0xFF, 0xEC, 0x5C);
    hackerBlue.normal[4] = QColor(0x2C, 0xA5, 0xFF); hackerBlue.normal[5] = QColor(0x9C, 0x7B, 0xFF);
    hackerBlue.normal[6] = QColor(0x33, 0xFF, 0xE0); hackerBlue.normal[7] = QColor(0xCD, 0xE3, 0xF2);
    hackerBlue.bright[0] = QColor(0x17, 0x5A, 0x78); hackerBlue.bright[1] = QColor(0xFF, 0x85, 0x80);
    hackerBlue.bright[2] = QColor(0x5C, 0xFF, 0x94); hackerBlue.bright[3] = QColor(0xFF, 0xF2, 0x9E);
    hackerBlue.bright[4] = QColor(0x6F, 0xC4, 0xFF); hackerBlue.bright[5] = QColor(0xB9, 0x9C, 0xFF);
    hackerBlue.bright[6] = QColor(0x7C, 0xFF, 0xE9); hackerBlue.bright[7] = QColor(0xF0, 0xF8, 0xFF);
    hackerBlue.windowBg = QColor(0x00, 0x10, 0x19);
    hackerBlue.panelBg = QColor(0x05, 0x1A, 0x26);
    hackerBlue.panelBorder = QColor(0x0E, 0x2E, 0x42);
    hackerBlue.inputBg = QColor(0x0A, 0x22, 0x33);
    hackerBlue.inputBorder = QColor(0x14, 0x41, 0x5C);
    hackerBlue.accentColor = QColor(0x2C, 0xA5, 0xFF);
    hackerBlue.textFg = QColor(0xBF, 0xD9, 0xED);
    hackerBlue.dialogBg = QColor(0x05, 0x1A, 0x26);
    hackerBlue.statusBarBg = QColor(0x03, 0x0E, 0x18);
    hackerBlue.statusBarFg = QColor(0x4A, 0x7A, 0x99);
        hackerBlue.buttonBg = QColor(0x0A, 0x22, 0x33); hackerBlue.buttonBorder = QColor(0x14, 0x41, 0x5C);
        hackerBlue.buttonHoverBg = QColor(0x12, 0x30, 0x46); hackerBlue.buttonHoverBorder = QColor(0x1E, 0x56, 0x78);
        hackerBlue.buttonPressedBg = QColor(0x05, 0x1A, 0x26);
        hackerBlue.toolCheckedBg = QColor(0x0D, 0x3D, 0x66); hackerBlue.toolCheckedBorder = QColor(0x2C, 0xA5, 0xFF);
        hackerBlue.listBg = QColor(0x05, 0x1A, 0x26); hackerBlue.listAltBg = QColor(0x0A, 0x22, 0x33); hackerBlue.listBorder = QColor(0x0E, 0x2E, 0x42);
        hackerBlue.listHoverBg = QColor(0x0E, 0x2E, 0x42); hackerBlue.listSelectedBg = QColor(0x0D, 0x3D, 0x66); hackerBlue.listSelectedFg = QColor(0xDC, 0xEE, 0xFC);
        hackerBlue.headerBg = QColor(0x03, 0x0E, 0x18); hackerBlue.headerFg = QColor(0x4A, 0x7A, 0x99); hackerBlue.gridlineColor = QColor(0x0E, 0x2E, 0x42);
        hackerBlue.scrollHandleBg = QColor(0x14, 0x41, 0x5C); hackerBlue.scrollHandleHoverBg = QColor(0x2C, 0xA5, 0xFF);
        hackerBlue.scrollTrackBg = QColor(0, 0, 0, 13);

    // 16. Hacker Green：Matrix 风——纯黑绿底 + 经典荧光绿正文
    ThemeDefinition hackerGreen;
    hackerGreen.id = QStringLiteral("hacker-green");
    hackerGreen.name = QStringLiteral("Hacker Green");
    hackerGreen.isDark = true;
    hackerGreen.terminalBg = QColor(0x00, 0x0A, 0x04);
    hackerGreen.terminalFg = QColor(0x00, 0xE6, 0x3D);
    hackerGreen.selectionBg = QColor(0x0B, 0x4A, 0x20);
    hackerGreen.infoColor = QColor(0x3E, 0x7A, 0x52);
    hackerGreen.normal[0] = QColor(0x0A, 0x2A, 0x12); hackerGreen.normal[1] = QColor(0xFF, 0x5C, 0x57);
    hackerGreen.normal[2] = QColor(0x00, 0xE6, 0x3D); hackerGreen.normal[3] = QColor(0xD8, 0xE6, 0x2E);
    hackerGreen.normal[4] = QColor(0x3D, 0xC9, 0xB0); hackerGreen.normal[5] = QColor(0x9C, 0x7B, 0xFF);
    hackerGreen.normal[6] = QColor(0x2E, 0xD6, 0xC1); hackerGreen.normal[7] = QColor(0xC5, 0xE8, 0xCE);
    hackerGreen.bright[0] = QColor(0x17, 0x52, 0x30); hackerGreen.bright[1] = QColor(0xFF, 0x85, 0x80);
    hackerGreen.bright[2] = QColor(0x5C, 0xFF, 0x85); hackerGreen.bright[3] = QColor(0xEA, 0xF5, 0x9A);
    hackerGreen.bright[4] = QColor(0x6F, 0xE3, 0xD6); hackerGreen.bright[5] = QColor(0xB9, 0x9C, 0xFF);
    hackerGreen.bright[6] = QColor(0x7C, 0xF0, 0xE4); hackerGreen.bright[7] = QColor(0xE8, 0xFF, 0xF0);
    hackerGreen.windowBg = QColor(0x00, 0x0A, 0x04);
    hackerGreen.panelBg = QColor(0x06, 0x18, 0x0D);
    hackerGreen.panelBorder = QColor(0x12, 0x3B, 0x1E);
    hackerGreen.inputBg = QColor(0x0A, 0x24, 0x12);
    hackerGreen.inputBorder = QColor(0x1B, 0x52, 0x30);
    hackerGreen.accentColor = QColor(0x00, 0xE6, 0x3D);
    hackerGreen.textFg = QColor(0xC2, 0xE6, 0xCC);
    hackerGreen.dialogBg = QColor(0x06, 0x18, 0x0D);
    hackerGreen.statusBarBg = QColor(0x01, 0x0B, 0x06);
    hackerGreen.statusBarFg = QColor(0x3E, 0x7A, 0x52);
        hackerGreen.buttonBg = QColor(0x0A, 0x24, 0x12); hackerGreen.buttonBorder = QColor(0x1B, 0x52, 0x30);
        hackerGreen.buttonHoverBg = QColor(0x11, 0x33, 0x1E); hackerGreen.buttonHoverBorder = QColor(0x25, 0x66, 0x3C);
        hackerGreen.buttonPressedBg = QColor(0x06, 0x18, 0x0D);
        hackerGreen.toolCheckedBg = QColor(0x0B, 0x4A, 0x20); hackerGreen.toolCheckedBorder = QColor(0x00, 0xE6, 0x3D);
        hackerGreen.listBg = QColor(0x06, 0x18, 0x0D); hackerGreen.listAltBg = QColor(0x0A, 0x24, 0x12); hackerGreen.listBorder = QColor(0x12, 0x3B, 0x1E);
        hackerGreen.listHoverBg = QColor(0x12, 0x3B, 0x1E); hackerGreen.listSelectedBg = QColor(0x0B, 0x4A, 0x20); hackerGreen.listSelectedFg = QColor(0xDC, 0xF5, 0xE2);
        hackerGreen.headerBg = QColor(0x01, 0x0B, 0x06); hackerGreen.headerFg = QColor(0x3E, 0x7A, 0x52); hackerGreen.gridlineColor = QColor(0x12, 0x3B, 0x1E);
        hackerGreen.scrollHandleBg = QColor(0x1B, 0x52, 0x30); hackerGreen.scrollHandleHoverBg = QColor(0x00, 0xE6, 0x3D);
        hackerGreen.scrollTrackBg = QColor(0, 0, 0, 13);

    // 17. Hacker Red：黑客风警示红——黑红底 + 亮红正文
    ThemeDefinition hackerRed;
    hackerRed.id = QStringLiteral("hacker-red");
    hackerRed.name = QStringLiteral("Hacker Red");
    hackerRed.isDark = true;
    hackerRed.terminalBg = QColor(0x10, 0x04, 0x04);
    hackerRed.terminalFg = QColor(0xFF, 0x57, 0x57);
    hackerRed.selectionBg = QColor(0x52, 0x14, 0x14);
    hackerRed.infoColor = QColor(0x8A, 0x4A, 0x4A);
    hackerRed.normal[0] = QColor(0x2A, 0x0E, 0x0E); hackerRed.normal[1] = QColor(0xFF, 0x57, 0x57);
    hackerRed.normal[2] = QColor(0x7D, 0xC5, 0x58); hackerRed.normal[3] = QColor(0xFF, 0xC6, 0x4C);
    hackerRed.normal[4] = QColor(0x5C, 0x9C, 0xFF); hackerRed.normal[5] = QColor(0xB8, 0x5C, 0xFF);
    hackerRed.normal[6] = QColor(0x5C, 0xFF, 0xE0); hackerRed.normal[7] = QColor(0xE8, 0xC5, 0xC5);
    hackerRed.bright[0] = QColor(0x5A, 0x24, 0x24); hackerRed.bright[1] = QColor(0xFF, 0x8A, 0x8A);
    hackerRed.bright[2] = QColor(0x9F, 0xE5, 0x7A); hackerRed.bright[3] = QColor(0xFF, 0xE0, 0x8A);
    hackerRed.bright[4] = QColor(0x8A, 0xB8, 0xFF); hackerRed.bright[5] = QColor(0xD0, 0x8A, 0xFF);
    hackerRed.bright[6] = QColor(0x8A, 0xFF, 0xEC); hackerRed.bright[7] = QColor(0xFF, 0xEA, 0xEA);
    hackerRed.windowBg = QColor(0x10, 0x04, 0x04);
    hackerRed.panelBg = QColor(0x1A, 0x0A, 0x0A);
    hackerRed.panelBorder = QColor(0x33, 0x16, 0x16);
    hackerRed.inputBg = QColor(0x24, 0x0F, 0x0F);
    hackerRed.inputBorder = QColor(0x43, 0x1C, 0x1C);
    hackerRed.accentColor = QColor(0xFF, 0x57, 0x57);
    hackerRed.textFg = QColor(0xE6, 0xC2, 0xC2);
    hackerRed.dialogBg = QColor(0x1A, 0x0A, 0x0A);
    hackerRed.statusBarBg = QColor(0x0C, 0x03, 0x03);
    hackerRed.statusBarFg = QColor(0x8A, 0x4A, 0x4A);
        hackerRed.buttonBg = QColor(0x24, 0x0F, 0x0F); hackerRed.buttonBorder = QColor(0x43, 0x1C, 0x1C);
        hackerRed.buttonHoverBg = QColor(0x33, 0x16, 0x16); hackerRed.buttonHoverBorder = QColor(0x55, 0x25, 0x25);
        hackerRed.buttonPressedBg = QColor(0x1A, 0x0A, 0x0A);
        hackerRed.toolCheckedBg = QColor(0x52, 0x14, 0x14); hackerRed.toolCheckedBorder = QColor(0xFF, 0x57, 0x57);
        hackerRed.listBg = QColor(0x1A, 0x0A, 0x0A); hackerRed.listAltBg = QColor(0x24, 0x0F, 0x0F); hackerRed.listBorder = QColor(0x33, 0x16, 0x16);
        hackerRed.listHoverBg = QColor(0x33, 0x16, 0x16); hackerRed.listSelectedBg = QColor(0x52, 0x14, 0x14); hackerRed.listSelectedFg = QColor(0xFF, 0xE2, 0xE2);
        hackerRed.headerBg = QColor(0x0C, 0x03, 0x03); hackerRed.headerFg = QColor(0x8A, 0x4A, 0x4A); hackerRed.gridlineColor = QColor(0x33, 0x16, 0x16);
        hackerRed.scrollHandleBg = QColor(0x43, 0x1C, 0x1C); hackerRed.scrollHandleHoverBg = QColor(0xFF, 0x57, 0x57);
        hackerRed.scrollTrackBg = QColor(0, 0, 0, 13);

    // 18. Everforest Dark：sainnhe/everforest——苔绿灰底 + 米黄正文，柔和护眼绿系
    ThemeDefinition everforestDark;
    everforestDark.id = QStringLiteral("everforest-dark");
    everforestDark.name = QStringLiteral("Everforest Dark");
    everforestDark.isDark = true;
    everforestDark.terminalBg = QColor(0x2D, 0x35, 0x3B);
    everforestDark.terminalFg = QColor(0xD3, 0xC6, 0xAA);
    everforestDark.selectionBg = QColor(0x3D, 0x48, 0x4D);
    everforestDark.infoColor = QColor(0x85, 0x92, 0x89);
    everforestDark.normal[0] = QColor(0x34, 0x3F, 0x44); everforestDark.normal[1] = QColor(0xE6, 0x7E, 0x80);
    everforestDark.normal[2] = QColor(0xA7, 0xC0, 0x80); everforestDark.normal[3] = QColor(0xDB, 0xBC, 0x7F);
    everforestDark.normal[4] = QColor(0x7F, 0xBB, 0xB3); everforestDark.normal[5] = QColor(0xD6, 0x99, 0xB6);
    everforestDark.normal[6] = QColor(0x83, 0xC5, 0xBE); everforestDark.normal[7] = QColor(0xD3, 0xC6, 0xAA);
    everforestDark.bright[0] = QColor(0x47, 0x52, 0x58); everforestDark.bright[1] = QColor(0xEC, 0x8B, 0x8D);
    everforestDark.bright[2] = QColor(0xB2, 0xCC, 0x8D); everforestDark.bright[3] = QColor(0xE2, 0xC5, 0x83);
    everforestDark.bright[4] = QColor(0x8B, 0xC5, 0xBD); everforestDark.bright[5] = QColor(0xDF, 0xA2, 0xBE);
    everforestDark.bright[6] = QColor(0x8D, 0xCB, 0xC5); everforestDark.bright[7] = QColor(0xE3, 0xD6, 0xB2);
    everforestDark.windowBg = QColor(0x2D, 0x35, 0x3B);
    everforestDark.panelBg = QColor(0x34, 0x3F, 0x44);
    everforestDark.panelBorder = QColor(0x47, 0x52, 0x58);
    everforestDark.inputBg = QColor(0x3D, 0x48, 0x4D);
    everforestDark.inputBorder = QColor(0x47, 0x52, 0x58);
    everforestDark.accentColor = QColor(0xA7, 0xC0, 0x80);
    everforestDark.textFg = QColor(0xD3, 0xC6, 0xAA);
    everforestDark.dialogBg = QColor(0x34, 0x3F, 0x44);
    everforestDark.statusBarBg = QColor(0x27, 0x2E, 0x34);
    everforestDark.statusBarFg = QColor(0x85, 0x92, 0x89);
        everforestDark.buttonBg = QColor(0x3D, 0x48, 0x4D); everforestDark.buttonBorder = QColor(0x47, 0x52, 0x58);
        everforestDark.buttonHoverBg = QColor(0x47, 0x52, 0x58); everforestDark.buttonHoverBorder = QColor(0x59, 0x66, 0x68);
        everforestDark.buttonPressedBg = QColor(0x34, 0x3F, 0x44);
        everforestDark.toolCheckedBg = QColor(0x44, 0x53, 0x3E); everforestDark.toolCheckedBorder = QColor(0xA7, 0xC0, 0x80);
        everforestDark.listBg = QColor(0x34, 0x3F, 0x44); everforestDark.listAltBg = QColor(0x3D, 0x48, 0x4D); everforestDark.listBorder = QColor(0x47, 0x52, 0x58);
        everforestDark.listHoverBg = QColor(0x3D, 0x48, 0x4D); everforestDark.listSelectedBg = QColor(0x44, 0x53, 0x3E); everforestDark.listSelectedFg = QColor(0xE6, 0xE2, 0xCC);
        everforestDark.headerBg = QColor(0x27, 0x2E, 0x34); everforestDark.headerFg = QColor(0x85, 0x92, 0x89); everforestDark.gridlineColor = QColor(0x3D, 0x48, 0x4D);
        everforestDark.scrollHandleBg = QColor(0x47, 0x52, 0x58); everforestDark.scrollHandleHoverBg = QColor(0xA7, 0xC0, 0x80);
        everforestDark.scrollTrackBg = QColor(0, 0, 0, 13);

    // 19. Everforest Light：亮色变体——米黄暖纸底 + 苔绿强调
    ThemeDefinition everforestLight;
    everforestLight.id = QStringLiteral("everforest-light");
    everforestLight.name = QStringLiteral("Everforest Light");
    everforestLight.isDark = false;
    everforestLight.terminalBg = QColor(0xFB, 0xF3, 0xE5);
    everforestLight.terminalFg = QColor(0x5C, 0x6A, 0x72);
    everforestLight.selectionBg = QColor(0xE8, 0xDF, 0xCB);
    everforestLight.infoColor = QColor(0x8B, 0x93, 0x93);
    everforestLight.normal[0] = QColor(0x55, 0x60, 0x6A); everforestLight.normal[1] = QColor(0xC2, 0x5D, 0x5D);
    everforestLight.normal[2] = QColor(0x78, 0x89, 0x3B); everforestLight.normal[3] = QColor(0xB9, 0x8D, 0x52);
    everforestLight.normal[4] = QColor(0x4D, 0x69, 0x97); everforestLight.normal[5] = QColor(0x90, 0x5F, 0x8B);
    everforestLight.normal[6] = QColor(0x4D, 0x92, 0x80); everforestLight.normal[7] = QColor(0x9D, 0xA9, 0xA0);
    everforestLight.bright[0] = QColor(0x7A, 0x84, 0x78); everforestLight.bright[1] = QColor(0xCB, 0x66, 0x5E);
    everforestLight.bright[2] = QColor(0x83, 0x9A, 0x45); everforestLight.bright[3] = QColor(0xC9, 0xA2, 0x5E);
    everforestLight.bright[4] = QColor(0x5D, 0x80, 0xA8); everforestLight.bright[5] = QColor(0x9F, 0x6D, 0xA8);
    everforestLight.bright[6] = QColor(0x5B, 0xA4, 0x8F); everforestLight.bright[7] = QColor(0x5C, 0x6A, 0x72);
    everforestLight.windowBg = QColor(0xFB, 0xF3, 0xE5);
    everforestLight.panelBg = QColor(0xF0, 0xEA, 0xD6);
    everforestLight.panelBorder = QColor(0xDC, 0xD3, 0xBE);
    everforestLight.inputBg = QColor(0xF0, 0xEA, 0xD6);
    everforestLight.inputBorder = QColor(0xC8, 0xBF, 0xA8);
    everforestLight.accentColor = QColor(0x78, 0x89, 0x3B);
    everforestLight.textFg = QColor(0x5C, 0x6A, 0x72);
    everforestLight.dialogBg = QColor(0xF0, 0xEA, 0xD6);
    everforestLight.statusBarBg = QColor(0xE8, 0xE0, 0xCC);
    everforestLight.statusBarFg = QColor(0x7A, 0x84, 0x78);
        everforestLight.buttonBg = QColor(0xF0, 0xEA, 0xD6); everforestLight.buttonBorder = QColor(0xC8, 0xBF, 0xA8);
        everforestLight.buttonHoverBg = QColor(0xE8, 0xDF, 0xCB); everforestLight.buttonHoverBorder = QColor(0xA8, 0x9E, 0x84);
        everforestLight.buttonPressedBg = QColor(0xDC, 0xD3, 0xBE);
        everforestLight.toolCheckedBg = QColor(0xE0, 0xE2, 0xC6); everforestLight.toolCheckedBorder = QColor(0x78, 0x89, 0x3B);
        everforestLight.listBg = QColor(0xF0, 0xEA, 0xD6); everforestLight.listAltBg = QColor(0xE8, 0xE0, 0xCC); everforestLight.listBorder = QColor(0xDC, 0xD3, 0xBE);
        everforestLight.listHoverBg = QColor(0xE8, 0xDF, 0xCB); everforestLight.listSelectedBg = QColor(0xE0, 0xE2, 0xC6); everforestLight.listSelectedFg = QColor(0x41, 0x4B, 0x46);
        everforestLight.headerBg = QColor(0xE8, 0xE0, 0xCC); everforestLight.headerFg = QColor(0x7A, 0x84, 0x78); everforestLight.gridlineColor = QColor(0xE0, 0xD8, 0xC2);
        everforestLight.scrollHandleBg = QColor(0xC8, 0xBF, 0xA8); everforestLight.scrollHandleHoverBg = QColor(0x78, 0x89, 0x3B);
        everforestLight.scrollTrackBg = QColor(0, 0, 0, 13);

    // 20. Night Owl：Sarah Drasner——深蓝夜空底，青蓝紫多彩高亮
    ThemeDefinition nightOwl;
    nightOwl.id = QStringLiteral("night-owl");
    nightOwl.name = QStringLiteral("Night Owl");
    nightOwl.isDark = true;
    nightOwl.terminalBg = QColor(0x01, 0x16, 0x27);
    nightOwl.terminalFg = QColor(0xD6, 0xDE, 0xEB);
    nightOwl.selectionBg = QColor(0x1D, 0x3B, 0x53);
    nightOwl.infoColor = QColor(0x5F, 0x7A, 0x97);
    nightOwl.normal[0] = QColor(0x01, 0x11, 0x1D); nightOwl.normal[1] = QColor(0xEF, 0x53, 0x50);
    nightOwl.normal[2] = QColor(0xAD, 0xDB, 0x67); nightOwl.normal[3] = QColor(0xFF, 0xEB, 0x95);
    nightOwl.normal[4] = QColor(0x82, 0xAA, 0xFF); nightOwl.normal[5] = QColor(0xC7, 0x92, 0xEA);
    nightOwl.normal[6] = QColor(0x7F, 0xDB, 0xCA); nightOwl.normal[7] = QColor(0xD6, 0xDE, 0xEB);
    nightOwl.bright[0] = QColor(0x5F, 0x7A, 0x97); nightOwl.bright[1] = QColor(0xFF, 0x86, 0x9A);
    nightOwl.bright[2] = QColor(0xA5, 0xE0, 0x75); nightOwl.bright[3] = QColor(0xFF, 0xCC, 0x95);
    nightOwl.bright[4] = QColor(0x75, 0xA1, 0xFF); nightOwl.bright[5] = QColor(0xD1, 0x96, 0xF2);
    nightOwl.bright[6] = QColor(0x9B, 0xEF, 0xE0); nightOwl.bright[7] = QColor(0xFF, 0xFF, 0xFF);
    nightOwl.windowBg = QColor(0x01, 0x16, 0x27);
    nightOwl.panelBg = QColor(0x0B, 0x29, 0x42);
    nightOwl.panelBorder = QColor(0x1D, 0x3B, 0x53);
    nightOwl.inputBg = QColor(0x0B, 0x29, 0x42);
    nightOwl.inputBorder = QColor(0x1D, 0x3B, 0x53);
    nightOwl.accentColor = QColor(0x82, 0xAA, 0xFF);
    nightOwl.textFg = QColor(0xD6, 0xDE, 0xEB);
    nightOwl.dialogBg = QColor(0x0B, 0x29, 0x42);
    nightOwl.statusBarBg = QColor(0x01, 0x0E, 0x1D);
    nightOwl.statusBarFg = QColor(0x5F, 0x7A, 0x97);
        nightOwl.buttonBg = QColor(0x0B, 0x29, 0x42); nightOwl.buttonBorder = QColor(0x1D, 0x3B, 0x53);
        nightOwl.buttonHoverBg = QColor(0x12, 0x35, 0x54); nightOwl.buttonHoverBorder = QColor(0x2B, 0x4E, 0x70);
        nightOwl.buttonPressedBg = QColor(0x0B, 0x29, 0x42);
        nightOwl.toolCheckedBg = QColor(0x1D, 0x3B, 0x53); nightOwl.toolCheckedBorder = QColor(0x82, 0xAA, 0xFF);
        nightOwl.listBg = QColor(0x0B, 0x29, 0x42); nightOwl.listAltBg = QColor(0x0E, 0x2F, 0x4C); nightOwl.listBorder = QColor(0x1D, 0x3B, 0x53);
        nightOwl.listHoverBg = QColor(0x12, 0x35, 0x54); nightOwl.listSelectedBg = QColor(0x1D, 0x3B, 0x53); nightOwl.listSelectedFg = QColor(0xE3, 0xEA, 0xF5);
        nightOwl.headerBg = QColor(0x01, 0x0E, 0x1D); nightOwl.headerFg = QColor(0x5F, 0x7A, 0x97); nightOwl.gridlineColor = QColor(0x12, 0x35, 0x54);
        nightOwl.scrollHandleBg = QColor(0x1D, 0x3B, 0x53); nightOwl.scrollHandleHoverBg = QColor(0x82, 0xAA, 0xFF);
        nightOwl.scrollTrackBg = QColor(0, 0, 0, 13);

    // 21. Light Owl：亮色变体——冷白底 + 深蓝灰正文，蓝紫点缀
    ThemeDefinition lightOwl;
    lightOwl.id = QStringLiteral("light-owl");
    lightOwl.name = QStringLiteral("Light Owl");
    lightOwl.isDark = false;
    lightOwl.terminalBg = QColor(0xFB, 0xFB, 0xFB);
    lightOwl.terminalFg = QColor(0x40, 0x3F, 0x53);
    lightOwl.selectionBg = QColor(0xDB, 0xE7, 0xF5);
    lightOwl.infoColor = QColor(0x82, 0x90, 0xA6);
    lightOwl.normal[0] = QColor(0x40, 0x3F, 0x53); lightOwl.normal[1] = QColor(0xD3, 0x42, 0x3E);
    lightOwl.normal[2] = QColor(0x1B, 0x86, 0x6E); lightOwl.normal[3] = QColor(0xDA, 0xA2, 0x28);
    lightOwl.normal[4] = QColor(0x2A, 0x63, 0xF6); lightOwl.normal[5] = QColor(0x7C, 0x4F, 0xBE);
    lightOwl.normal[6] = QColor(0x3C, 0x9F, 0xE8); lightOwl.normal[7] = QColor(0x90, 0xA4, 0xAE);
    lightOwl.bright[0] = QColor(0x82, 0x90, 0xA6); lightOwl.bright[1] = QColor(0xE7, 0x5C, 0x5C);
    lightOwl.bright[2] = QColor(0x2B, 0xB3, 0x89); lightOwl.bright[3] = QColor(0xE5, 0xC5, 0x5C);
    lightOwl.bright[4] = QColor(0x55, 0x77, 0xF0); lightOwl.bright[5] = QColor(0x9A, 0x6B, 0xE0);
    lightOwl.bright[6] = QColor(0x58, 0xAD, 0xE8); lightOwl.bright[7] = QColor(0x5C, 0x67, 0x7D);
    lightOwl.windowBg = QColor(0xFB, 0xFB, 0xFB);
    lightOwl.panelBg = QColor(0xF1, 0xF2, 0xF5);
    lightOwl.panelBorder = QColor(0xE2, 0xE6, 0xEC);
    lightOwl.inputBg = QColor(0xF1, 0xF2, 0xF5);
    lightOwl.inputBorder = QColor(0xC9, 0xD2, 0xDE);
    lightOwl.accentColor = QColor(0x2A, 0x63, 0xF6);
    lightOwl.textFg = QColor(0x40, 0x3F, 0x53);
    lightOwl.dialogBg = QColor(0xF1, 0xF2, 0xF5);
    lightOwl.statusBarBg = QColor(0xED, 0xEE, 0xF2);
    lightOwl.statusBarFg = QColor(0x82, 0x90, 0xA6);
        lightOwl.buttonBg = QColor(0xF1, 0xF2, 0xF5); lightOwl.buttonBorder = QColor(0xC9, 0xD2, 0xDE);
        lightOwl.buttonHoverBg = QColor(0xE7, 0xEA, 0xF0); lightOwl.buttonHoverBorder = QColor(0xA9, 0xB5, 0xC4);
        lightOwl.buttonPressedBg = QColor(0xDE, 0xE3, 0xEA);
        lightOwl.toolCheckedBg = QColor(0xDB, 0xE7, 0xF5); lightOwl.toolCheckedBorder = QColor(0x2A, 0x63, 0xF6);
        lightOwl.listBg = QColor(0xF1, 0xF2, 0xF5); lightOwl.listAltBg = QColor(0xEA, 0xEC, 0xEF); lightOwl.listBorder = QColor(0xE2, 0xE6, 0xEC);
        lightOwl.listHoverBg = QColor(0xE7, 0xEA, 0xF0); lightOwl.listSelectedBg = QColor(0xDB, 0xE7, 0xF5); lightOwl.listSelectedFg = QColor(0x2C, 0x3A, 0x54);
        lightOwl.headerBg = QColor(0xED, 0xEE, 0xF2); lightOwl.headerFg = QColor(0x82, 0x90, 0xA6); lightOwl.gridlineColor = QColor(0xE2, 0xE6, 0xEC);
        lightOwl.scrollHandleBg = QColor(0xC9, 0xD2, 0xDE); lightOwl.scrollHandleHoverBg = QColor(0x2A, 0x63, 0xF6);
        lightOwl.scrollTrackBg = QColor(0, 0, 0, 13);

    // 22. Aura：daltonmenezes/aura——紫黑底 + 霓虹紫/青绿/蜜桃多彩高亮
    ThemeDefinition aura;
    aura.id = QStringLiteral("aura");
    aura.name = QStringLiteral("Aura");
    aura.isDark = true;
    aura.terminalBg = QColor(0x15, 0x14, 0x1B);
    aura.terminalFg = QColor(0xED, 0xEA, 0xE0);
    aura.selectionBg = QColor(0x30, 0x2E, 0x46);
    aura.infoColor = QColor(0x8D, 0x87, 0xA0);
    aura.normal[0] = QColor(0x21, 0x20, 0x2E); aura.normal[1] = QColor(0xFF, 0x67, 0x67);
    aura.normal[2] = QColor(0x61, 0xFF, 0xCA); aura.normal[3] = QColor(0xFF, 0xCA, 0x85);
    aura.normal[4] = QColor(0xA2, 0x77, 0xFF); aura.normal[5] = QColor(0xD6, 0x61, 0xFF);
    aura.normal[6] = QColor(0x00, 0xD2, 0xFF); aura.normal[7] = QColor(0xED, 0xEA, 0xE0);
    aura.bright[0] = QColor(0x57, 0x53, 0x6C); aura.bright[1] = QColor(0xFF, 0x91, 0x91);
    aura.bright[2] = QColor(0x91, 0xFF, 0xDA); aura.bright[3] = QColor(0xFF, 0xDD, 0xB0);
    aura.bright[4] = QColor(0xBD, 0x9E, 0xFF); aura.bright[5] = QColor(0xE0, 0x95, 0xFF);
    aura.bright[6] = QColor(0x62, 0xE4, 0xFF); aura.bright[7] = QColor(0xFF, 0xFF, 0xFF);
    aura.windowBg = QColor(0x15, 0x14, 0x1B);
    aura.panelBg = QColor(0x1C, 0x1B, 0x24);
    aura.panelBorder = QColor(0x2E, 0x2C, 0x3C);
    aura.inputBg = QColor(0x24, 0x22, 0x31);
    aura.inputBorder = QColor(0x3B, 0x39, 0x50);
    aura.accentColor = QColor(0xA2, 0x77, 0xFF);
    aura.textFg = QColor(0xD8, 0xD3, 0xE8);
    aura.dialogBg = QColor(0x1C, 0x1B, 0x24);
    aura.statusBarBg = QColor(0x12, 0x11, 0x18);
    aura.statusBarFg = QColor(0x8D, 0x87, 0xA0);
        aura.buttonBg = QColor(0x24, 0x22, 0x31); aura.buttonBorder = QColor(0x3B, 0x39, 0x50);
        aura.buttonHoverBg = QColor(0x2F, 0x2C, 0x40); aura.buttonHoverBorder = QColor(0x4F, 0x4B, 0x6C);
        aura.buttonPressedBg = QColor(0x1C, 0x1B, 0x24);
        aura.toolCheckedBg = QColor(0x30, 0x2E, 0x46); aura.toolCheckedBorder = QColor(0xA2, 0x77, 0xFF);
        aura.listBg = QColor(0x1C, 0x1B, 0x24); aura.listAltBg = QColor(0x24, 0x22, 0x31); aura.listBorder = QColor(0x2E, 0x2C, 0x3C);
        aura.listHoverBg = QColor(0x2F, 0x2C, 0x40); aura.listSelectedBg = QColor(0x30, 0x2E, 0x46); aura.listSelectedFg = QColor(0xE7, 0xE3, 0xF5);
        aura.headerBg = QColor(0x12, 0x11, 0x18); aura.headerFg = QColor(0x8D, 0x87, 0xA0); aura.gridlineColor = QColor(0x2E, 0x2C, 0x3C);
        aura.scrollHandleBg = QColor(0x3B, 0x39, 0x50); aura.scrollHandleHoverBg = QColor(0xA2, 0x77, 0xFF);
        aura.scrollTrackBg = QColor(0, 0, 0, 13);

    // 23. Rosé Pine：rose-pine/main——深紫黑底，玫瑰金/松青/泡沫蓝柔和高亮
    ThemeDefinition rosePine;
    rosePine.id = QStringLiteral("rose-pine");
    rosePine.name = QStringLiteral("Rosé Pine");
    rosePine.isDark = true;
    rosePine.terminalBg = QColor(0x19, 0x17, 0x24);
    rosePine.terminalFg = QColor(0xE0, 0xDE, 0xF4);
    rosePine.selectionBg = QColor(0x40, 0x3D, 0x58);
    rosePine.infoColor = QColor(0x6E, 0x6A, 0x86);
    rosePine.normal[0] = QColor(0x26, 0x23, 0x3A); rosePine.normal[1] = QColor(0xEB, 0x6F, 0x92);
    rosePine.normal[2] = QColor(0x31, 0x74, 0x8F); rosePine.normal[3] = QColor(0xF6, 0xC1, 0x77);
    rosePine.normal[4] = QColor(0x9C, 0xCF, 0xD8); rosePine.normal[5] = QColor(0xC4, 0xA7, 0xE7);
    rosePine.normal[6] = QColor(0xEB, 0xBC, 0xBA); rosePine.normal[7] = QColor(0xE0, 0xDE, 0xF4);
    rosePine.bright[0] = QColor(0x6E, 0x6A, 0x86); rosePine.bright[1] = QColor(0xEB, 0x6F, 0x92);
    rosePine.bright[2] = QColor(0x31, 0x74, 0x8F); rosePine.bright[3] = QColor(0xF6, 0xC1, 0x77);
    rosePine.bright[4] = QColor(0x9C, 0xCF, 0xD8); rosePine.bright[5] = QColor(0xC4, 0xA7, 0xE7);
    rosePine.bright[6] = QColor(0xEB, 0xBC, 0xBA); rosePine.bright[7] = QColor(0xE0, 0xDE, 0xF4);
    rosePine.windowBg = QColor(0x19, 0x17, 0x24);
    rosePine.panelBg = QColor(0x1F, 0x1D, 0x2E);
    rosePine.panelBorder = QColor(0x31, 0x2F, 0x44);
    rosePine.inputBg = QColor(0x26, 0x23, 0x3A);
    rosePine.inputBorder = QColor(0x40, 0x3D, 0x58);
    rosePine.accentColor = QColor(0xC4, 0xA7, 0xE7);
    rosePine.textFg = QColor(0xE0, 0xDE, 0xF4);
    rosePine.dialogBg = QColor(0x1F, 0x1D, 0x2E);
    rosePine.statusBarBg = QColor(0x13, 0x12, 0x20);
    rosePine.statusBarFg = QColor(0x6E, 0x6A, 0x86);
        rosePine.buttonBg = QColor(0x26, 0x23, 0x3A); rosePine.buttonBorder = QColor(0x40, 0x3D, 0x58);
        rosePine.buttonHoverBg = QColor(0x30, 0x2C, 0x46); rosePine.buttonHoverBorder = QColor(0x51, 0x4C, 0x69);
        rosePine.buttonPressedBg = QColor(0x1F, 0x1D, 0x2E);
        rosePine.toolCheckedBg = QColor(0x40, 0x3D, 0x58); rosePine.toolCheckedBorder = QColor(0xC4, 0xA7, 0xE7);
        rosePine.listBg = QColor(0x1F, 0x1D, 0x2E); rosePine.listAltBg = QColor(0x26, 0x23, 0x3A); rosePine.listBorder = QColor(0x31, 0x2F, 0x44);
        rosePine.listHoverBg = QColor(0x30, 0x2C, 0x46); rosePine.listSelectedBg = QColor(0x40, 0x3D, 0x58); rosePine.listSelectedFg = QColor(0xE0, 0xDE, 0xF4);
        rosePine.headerBg = QColor(0x13, 0x12, 0x20); rosePine.headerFg = QColor(0x6E, 0x6A, 0x86); rosePine.gridlineColor = QColor(0x30, 0x2C, 0x46);
        rosePine.scrollHandleBg = QColor(0x40, 0x3D, 0x58); rosePine.scrollHandleHoverBg = QColor(0xC4, 0xA7, 0xE7);
        rosePine.scrollTrackBg = QColor(0, 0, 0, 13);

    // 24. Rosé Pine Moon：moon 变体——底色稍亮偏蓝紫
    ThemeDefinition rosePineMoon;
    rosePineMoon.id = QStringLiteral("rose-pine-moon");
    rosePineMoon.name = QStringLiteral("Rosé Pine Moon");
    rosePineMoon.isDark = true;
    rosePineMoon.terminalBg = QColor(0x23, 0x21, 0x36);
    rosePineMoon.terminalFg = QColor(0xE0, 0xDE, 0xF4);
    rosePineMoon.selectionBg = QColor(0x43, 0x39, 0x69);
    rosePineMoon.infoColor = QColor(0x81, 0x7C, 0x9C);
    rosePineMoon.normal[0] = QColor(0x2D, 0x27, 0x46); rosePineMoon.normal[1] = QColor(0xEB, 0x6F, 0x92);
    rosePineMoon.normal[2] = QColor(0x3E, 0x5F, 0x6C); rosePineMoon.normal[3] = QColor(0xF6, 0xC1, 0x77);
    rosePineMoon.normal[4] = QColor(0x9C, 0xCF, 0xD8); rosePineMoon.normal[5] = QColor(0xC4, 0xA7, 0xE7);
    rosePineMoon.normal[6] = QColor(0xEB, 0xBC, 0xBA); rosePineMoon.normal[7] = QColor(0xE0, 0xDE, 0xF4);
    rosePineMoon.bright[0] = QColor(0x81, 0x7C, 0x9C); rosePineMoon.bright[1] = QColor(0xEB, 0x6F, 0x92);
    rosePineMoon.bright[2] = QColor(0x3E, 0x5F, 0x6C); rosePineMoon.bright[3] = QColor(0xF6, 0xC1, 0x77);
    rosePineMoon.bright[4] = QColor(0x9C, 0xCF, 0xD8); rosePineMoon.bright[5] = QColor(0xC4, 0xA7, 0xE7);
    rosePineMoon.bright[6] = QColor(0xEB, 0xBC, 0xBA); rosePineMoon.bright[7] = QColor(0xE0, 0xDE, 0xF4);
    rosePineMoon.windowBg = QColor(0x23, 0x21, 0x36);
    rosePineMoon.panelBg = QColor(0x2A, 0x25, 0x44);
    rosePineMoon.panelBorder = QColor(0x3B, 0x34, 0x59);
    rosePineMoon.inputBg = QColor(0x2D, 0x27, 0x46);
    rosePineMoon.inputBorder = QColor(0x43, 0x39, 0x69);
    rosePineMoon.accentColor = QColor(0xC4, 0xA7, 0xE7);
    rosePineMoon.textFg = QColor(0xE0, 0xDE, 0xF4);
    rosePineMoon.dialogBg = QColor(0x2A, 0x25, 0x44);
    rosePineMoon.statusBarBg = QColor(0x1C, 0x19, 0x28);
    rosePineMoon.statusBarFg = QColor(0x81, 0x7C, 0x9C);
        rosePineMoon.buttonBg = QColor(0x2D, 0x27, 0x46); rosePineMoon.buttonBorder = QColor(0x43, 0x39, 0x69);
        rosePineMoon.buttonHoverBg = QColor(0x3B, 0x34, 0x59); rosePineMoon.buttonHoverBorder = QColor(0x57, 0x4C, 0x7C);
        rosePineMoon.buttonPressedBg = QColor(0x2A, 0x25, 0x44);
        rosePineMoon.toolCheckedBg = QColor(0x43, 0x39, 0x69); rosePineMoon.toolCheckedBorder = QColor(0xC4, 0xA7, 0xE7);
        rosePineMoon.listBg = QColor(0x2A, 0x25, 0x44); rosePineMoon.listAltBg = QColor(0x2D, 0x27, 0x46); rosePineMoon.listBorder = QColor(0x3B, 0x34, 0x59);
        rosePineMoon.listHoverBg = QColor(0x3B, 0x34, 0x59); rosePineMoon.listSelectedBg = QColor(0x43, 0x39, 0x69); rosePineMoon.listSelectedFg = QColor(0xE0, 0xDE, 0xF4);
        rosePineMoon.headerBg = QColor(0x1C, 0x19, 0x28); rosePineMoon.headerFg = QColor(0x81, 0x7C, 0x9C); rosePineMoon.gridlineColor = QColor(0x3B, 0x34, 0x59);
        rosePineMoon.scrollHandleBg = QColor(0x43, 0x39, 0x69); rosePineMoon.scrollHandleHoverBg = QColor(0xC4, 0xA7, 0xE7);
        rosePineMoon.scrollTrackBg = QColor(0, 0, 0, 13);

    // 25. Rosé Pine Dawn：dawn 亮色变体——米色纸底 + 靛紫正文
    ThemeDefinition rosePineDawn;
    rosePineDawn.id = QStringLiteral("rose-pine-dawn");
    rosePineDawn.name = QStringLiteral("Rosé Pine Dawn");
    rosePineDawn.isDark = false;
    rosePineDawn.terminalBg = QColor(0xFA, 0xF4, 0xED);
    rosePineDawn.terminalFg = QColor(0x57, 0x52, 0x79);
    rosePineDawn.selectionBg = QColor(0xE4, 0xD9, 0xCF);
    rosePineDawn.infoColor = QColor(0x98, 0x93, 0xA5);
    rosePineDawn.normal[0] = QColor(0x57, 0x52, 0x79); rosePineDawn.normal[1] = QColor(0xB4, 0x63, 0x7A);
    rosePineDawn.normal[2] = QColor(0x28, 0x69, 0x83); rosePineDawn.normal[3] = QColor(0xEA, 0x9D, 0x97);
    rosePineDawn.normal[4] = QColor(0x56, 0x94, 0x9F); rosePineDawn.normal[5] = QColor(0x90, 0x7A, 0xA9);
    rosePineDawn.normal[6] = QColor(0xD7, 0x82, 0x7E); rosePineDawn.normal[7] = QColor(0x98, 0x93, 0xA5);
    rosePineDawn.bright[0] = QColor(0x98, 0x93, 0xA5); rosePineDawn.bright[1] = QColor(0xB4, 0x63, 0x7A);
    rosePineDawn.bright[2] = QColor(0x28, 0x69, 0x83); rosePineDawn.bright[3] = QColor(0xEA, 0x9D, 0x97);
    rosePineDawn.bright[4] = QColor(0x56, 0x94, 0x9F); rosePineDawn.bright[5] = QColor(0x90, 0x7A, 0xA9);
    rosePineDawn.bright[6] = QColor(0xD7, 0x82, 0x7E); rosePineDawn.bright[7] = QColor(0x57, 0x52, 0x79);
    rosePineDawn.windowBg = QColor(0xFA, 0xF4, 0xED);
    rosePineDawn.panelBg = QColor(0xF4, 0xED, 0xE4);
    rosePineDawn.panelBorder = QColor(0xE8, 0xDD, 0xD3);
    rosePineDawn.inputBg = QColor(0xEF, 0xE7, 0xDC);
    rosePineDawn.inputBorder = QColor(0xD8, 0xCC, 0xC0);
    rosePineDawn.accentColor = QColor(0x90, 0x7A, 0xA9);
    rosePineDawn.textFg = QColor(0x57, 0x52, 0x79);
    rosePineDawn.dialogBg = QColor(0xF4, 0xED, 0xE4);
    rosePineDawn.statusBarBg = QColor(0xED, 0xE3, 0xD8);
    rosePineDawn.statusBarFg = QColor(0x98, 0x93, 0xA5);
        rosePineDawn.buttonBg = QColor(0xEF, 0xE7, 0xDC); rosePineDawn.buttonBorder = QColor(0xD8, 0xCC, 0xC0);
        rosePineDawn.buttonHoverBg = QColor(0xE8, 0xDD, 0xD3); rosePineDawn.buttonHoverBorder = QColor(0xB8, 0xAA, 0x9C);
        rosePineDawn.buttonPressedBg = QColor(0xE4, 0xD9, 0xCF);
        rosePineDawn.toolCheckedBg = QColor(0xE4, 0xD9, 0xCF); rosePineDawn.toolCheckedBorder = QColor(0x90, 0x7A, 0xA9);
        rosePineDawn.listBg = QColor(0xF4, 0xED, 0xE4); rosePineDawn.listAltBg = QColor(0xEF, 0xE7, 0xDC); rosePineDawn.listBorder = QColor(0xE8, 0xDD, 0xD3);
        rosePineDawn.listHoverBg = QColor(0xE8, 0xDD, 0xD3); rosePineDawn.listSelectedBg = QColor(0xE4, 0xD9, 0xCF); rosePineDawn.listSelectedFg = QColor(0x57, 0x52, 0x79);
        rosePineDawn.headerBg = QColor(0xED, 0xE3, 0xD8); rosePineDawn.headerFg = QColor(0x98, 0x93, 0xA5); rosePineDawn.gridlineColor = QColor(0xE8, 0xDD, 0xD3);
        rosePineDawn.scrollHandleBg = QColor(0xD8, 0xCC, 0xC0); rosePineDawn.scrollHandleHoverBg = QColor(0x90, 0x7A, 0xA9);
        rosePineDawn.scrollTrackBg = QColor(0, 0, 0, 13);

    // 26. Cobalt2：Wes Bos 经典——深海军蓝底 + 招牌亮黄强调
    ThemeDefinition cobalt2;
    cobalt2.id = QStringLiteral("cobalt2");
    cobalt2.name = QStringLiteral("Cobalt2");
    cobalt2.isDark = true;
    cobalt2.terminalBg = QColor(0x19, 0x35, 0x49);
    cobalt2.terminalFg = QColor(0xFF, 0xFF, 0xFF);
    cobalt2.selectionBg = QColor(0x1F, 0x46, 0x62);
    cobalt2.infoColor = QColor(0x7E, 0xA8, 0xC8);
    cobalt2.normal[0] = QColor(0x00, 0x00, 0x00); cobalt2.normal[1] = QColor(0xFF, 0x4A, 0x57);
    cobalt2.normal[2] = QColor(0x38, 0xDE, 0x21); cobalt2.normal[3] = QColor(0xFF, 0xE5, 0x0A);
    cobalt2.normal[4] = QColor(0x1D, 0x6F, 0xDE); cobalt2.normal[5] = QColor(0xFF, 0x00, 0x5B);
    cobalt2.normal[6] = QColor(0x00, 0xD7, 0xD7); cobalt2.normal[7] = QColor(0xD0, 0xD0, 0xD0);
    cobalt2.bright[0] = QColor(0x5C, 0x5C, 0x5C); cobalt2.bright[1] = QColor(0xF6, 0x6C, 0x66);
    cobalt2.bright[2] = QColor(0x59, 0xE0, 0x4B); cobalt2.bright[3] = QColor(0xFF, 0xEC, 0x59);
    cobalt2.bright[4] = QColor(0x47, 0xA0, 0xFF); cobalt2.bright[5] = QColor(0xFF, 0x5A, 0xC4);
    cobalt2.bright[6] = QColor(0x5B, 0xF9, 0xFF); cobalt2.bright[7] = QColor(0xFF, 0xFF, 0xFF);
    cobalt2.windowBg = QColor(0x19, 0x35, 0x49);
    cobalt2.panelBg = QColor(0x14, 0x2E, 0x40);
    cobalt2.panelBorder = QColor(0x2C, 0x6E, 0x92);
    cobalt2.inputBg = QColor(0x1B, 0x3E, 0x54);
    cobalt2.inputBorder = QColor(0x2C, 0x6E, 0x92);
    cobalt2.accentColor = QColor(0xFF, 0xC6, 0x00);
    cobalt2.textFg = QColor(0xDC, 0xEA, 0xF2);
    cobalt2.dialogBg = QColor(0x14, 0x2E, 0x40);
    cobalt2.statusBarBg = QColor(0x0F, 0x24, 0x32);
    cobalt2.statusBarFg = QColor(0x7E, 0xA8, 0xC8);
        cobalt2.buttonBg = QColor(0x1B, 0x3E, 0x54); cobalt2.buttonBorder = QColor(0x2C, 0x6E, 0x92);
        cobalt2.buttonHoverBg = QColor(0x24, 0x4D, 0x68); cobalt2.buttonHoverBorder = QColor(0x3E, 0x8A, 0xB2);
        cobalt2.buttonPressedBg = QColor(0x14, 0x2E, 0x40);
        cobalt2.toolCheckedBg = QColor(0x1F, 0x46, 0x62); cobalt2.toolCheckedBorder = QColor(0xFF, 0xC6, 0x00);
        cobalt2.listBg = QColor(0x14, 0x2E, 0x40); cobalt2.listAltBg = QColor(0x1B, 0x3E, 0x54); cobalt2.listBorder = QColor(0x2C, 0x6E, 0x92);
        cobalt2.listHoverBg = QColor(0x24, 0x4D, 0x68); cobalt2.listSelectedBg = QColor(0x1F, 0x46, 0x62); cobalt2.listSelectedFg = QColor(0xE8, 0xF2, 0xF8);
        cobalt2.headerBg = QColor(0x0F, 0x24, 0x32); cobalt2.headerFg = QColor(0x7E, 0xA8, 0xC8); cobalt2.gridlineColor = QColor(0x24, 0x4D, 0x68);
        cobalt2.scrollHandleBg = QColor(0x2C, 0x6E, 0x92); cobalt2.scrollHandleHoverBg = QColor(0xFF, 0xC6, 0x00);
        cobalt2.scrollTrackBg = QColor(0, 0, 0, 13);

    // 27. Octocat Dark：GitHub Dark——近黑蓝灰底 + 中性白，绿蓝多彩语义色
    ThemeDefinition octocatDark;
    octocatDark.id = QStringLiteral("octocat-dark");
    octocatDark.name = QStringLiteral("Octocat Dark");
    octocatDark.isDark = true;
    octocatDark.terminalBg = QColor(0x0D, 0x11, 0x17);
    octocatDark.terminalFg = QColor(0xC9, 0xD1, 0xD9);
    octocatDark.selectionBg = QColor(0x2A, 0x4F, 0x6E);
    octocatDark.infoColor = QColor(0x8B, 0x94, 0x9E);
    octocatDark.normal[0] = QColor(0x48, 0x4F, 0x58); octocatDark.normal[1] = QColor(0xFF, 0x7B, 0x72);
    octocatDark.normal[2] = QColor(0x7E, 0xE7, 0x87); octocatDark.normal[3] = QColor(0xF2, 0xCC, 0x60);
    octocatDark.normal[4] = QColor(0x58, 0xA6, 0xFF); octocatDark.normal[5] = QColor(0xBC, 0x8C, 0xFF);
    octocatDark.normal[6] = QColor(0x39, 0xC5, 0xCF); octocatDark.normal[7] = QColor(0xB1, 0xBA, 0xC4);
    octocatDark.bright[0] = QColor(0x6E, 0x76, 0x81); octocatDark.bright[1] = QColor(0xFF, 0xA1, 0x98);
    octocatDark.bright[2] = QColor(0xAF, 0xF5, 0xB4); octocatDark.bright[3] = QColor(0xFB, 0xE3, 0x60);
    octocatDark.bright[4] = QColor(0xA5, 0xD6, 0xFF); octocatDark.bright[5] = QColor(0xD2, 0xA8, 0xFF);
    octocatDark.bright[6] = QColor(0x71, 0xE8, 0xDF); octocatDark.bright[7] = QColor(0xF0, 0xF6, 0xFC);
    octocatDark.windowBg = QColor(0x0D, 0x11, 0x17);
    octocatDark.panelBg = QColor(0x16, 0x1B, 0x22);
    octocatDark.panelBorder = QColor(0x30, 0x36, 0x3D);
    octocatDark.inputBg = QColor(0x16, 0x1B, 0x22);
    octocatDark.inputBorder = QColor(0x30, 0x36, 0x3D);
    octocatDark.accentColor = QColor(0x58, 0xA6, 0xFF);
    octocatDark.textFg = QColor(0xC9, 0xD1, 0xD9);
    octocatDark.dialogBg = QColor(0x16, 0x1B, 0x22);
    octocatDark.statusBarBg = QColor(0x01, 0x04, 0x09);
    octocatDark.statusBarFg = QColor(0x8B, 0x94, 0x9E);
        octocatDark.buttonBg = QColor(0x21, 0x27, 0x2E); octocatDark.buttonBorder = QColor(0x37, 0x3E, 0x46);
        octocatDark.buttonHoverBg = QColor(0x2B, 0x31, 0x39); octocatDark.buttonHoverBorder = QColor(0x48, 0x50, 0x59);
        octocatDark.buttonPressedBg = QColor(0x16, 0x1B, 0x22);
        octocatDark.toolCheckedBg = QColor(0x2A, 0x4F, 0x6E); octocatDark.toolCheckedBorder = QColor(0x58, 0xA6, 0xFF);
        octocatDark.listBg = QColor(0x16, 0x1B, 0x22); octocatDark.listAltBg = QColor(0x1B, 0x21, 0x29); octocatDark.listBorder = QColor(0x30, 0x36, 0x3D);
        octocatDark.listHoverBg = QColor(0x2B, 0x31, 0x39); octocatDark.listSelectedBg = QColor(0x2A, 0x4F, 0x6E); octocatDark.listSelectedFg = QColor(0xE6, 0xED, 0xF3);
        octocatDark.headerBg = QColor(0x01, 0x04, 0x09); octocatDark.headerFg = QColor(0x8B, 0x94, 0x9E); octocatDark.gridlineColor = QColor(0x2B, 0x31, 0x39);
        octocatDark.scrollHandleBg = QColor(0x30, 0x36, 0x3D); octocatDark.scrollHandleHoverBg = QColor(0x58, 0xA6, 0xFF);
        octocatDark.scrollTrackBg = QColor(0, 0, 0, 13);

    // 28. Octocat Light：GitHub Light——纯白底 + GitHub 语义色
    ThemeDefinition octocatLight;
    octocatLight.id = QStringLiteral("octocat-light");
    octocatLight.name = QStringLiteral("Octocat Light");
    octocatLight.isDark = false;
    octocatLight.terminalBg = QColor(0xFF, 0xFF, 0xFF);
    octocatLight.terminalFg = QColor(0x24, 0x29, 0x2F);
    octocatLight.selectionBg = QColor(0xB6, 0xE3, 0xFF);
    octocatLight.infoColor = QColor(0x6E, 0x77, 0x81);
    octocatLight.normal[0] = QColor(0x24, 0x29, 0x2F); octocatLight.normal[1] = QColor(0xCF, 0x22, 0x2E);
    octocatLight.normal[2] = QColor(0x11, 0x63, 0x29); octocatLight.normal[3] = QColor(0x4D, 0x2D, 0x00);
    octocatLight.normal[4] = QColor(0x09, 0x69, 0xDA); octocatLight.normal[5] = QColor(0x82, 0x50, 0xDF);
    octocatLight.normal[6] = QColor(0x1B, 0x7C, 0x83); octocatLight.normal[7] = QColor(0x57, 0x60, 0x6A);
    octocatLight.bright[0] = QColor(0x57, 0x60, 0x6A); octocatLight.bright[1] = QColor(0xA4, 0x0E, 0x26);
    octocatLight.bright[2] = QColor(0x1A, 0x7F, 0x37); octocatLight.bright[3] = QColor(0x6E, 0x36, 0x00);
    octocatLight.bright[4] = QColor(0x21, 0x8A, 0xFF); octocatLight.bright[5] = QColor(0xA4, 0x75, 0xF9);
    octocatLight.bright[6] = QColor(0x2E, 0x9E, 0xBB); octocatLight.bright[7] = QColor(0x8C, 0x95, 0x9F);
    octocatLight.windowBg = QColor(0xFF, 0xFF, 0xFF);
    octocatLight.panelBg = QColor(0xF6, 0xF8, 0xFA);
    octocatLight.panelBorder = QColor(0xD0, 0xD7, 0xDE);
    octocatLight.inputBg = QColor(0xF6, 0xF8, 0xFA);
    octocatLight.inputBorder = QColor(0xD0, 0xD7, 0xDE);
    octocatLight.accentColor = QColor(0x09, 0x69, 0xDA);
    octocatLight.textFg = QColor(0x24, 0x29, 0x2F);
    octocatLight.dialogBg = QColor(0xF6, 0xF8, 0xFA);
    octocatLight.statusBarBg = QColor(0xF6, 0xF8, 0xFA);
    octocatLight.statusBarFg = QColor(0x6E, 0x77, 0x81);
        octocatLight.buttonBg = QColor(0xF6, 0xF8, 0xFA); octocatLight.buttonBorder = QColor(0xD0, 0xD7, 0xDE);
        octocatLight.buttonHoverBg = QColor(0xEC, 0xEF, 0xF3); octocatLight.buttonHoverBorder = QColor(0xAF, 0xB8, 0xC4);
        octocatLight.buttonPressedBg = QColor(0xE4, 0xE8, 0xEC);
        octocatLight.toolCheckedBg = QColor(0xDD, 0xE9, 0xF9); octocatLight.toolCheckedBorder = QColor(0x09, 0x69, 0xDA);
        octocatLight.listBg = QColor(0xF6, 0xF8, 0xFA); octocatLight.listAltBg = QColor(0xEF, 0xF1, 0xF4); octocatLight.listBorder = QColor(0xD0, 0xD7, 0xDE);
        octocatLight.listHoverBg = QColor(0xEC, 0xEF, 0xF3); octocatLight.listSelectedBg = QColor(0xDD, 0xE9, 0xF9); octocatLight.listSelectedFg = QColor(0x1B, 0x1F, 0x24);
        octocatLight.headerBg = QColor(0xF6, 0xF8, 0xFA); octocatLight.headerFg = QColor(0x6E, 0x77, 0x81); octocatLight.gridlineColor = QColor(0xE6, 0xEA, 0xEF);
        octocatLight.scrollHandleBg = QColor(0xD0, 0xD7, 0xDE); octocatLight.scrollHandleHoverBg = QColor(0x09, 0x69, 0xDA);
        octocatLight.scrollTrackBg = QColor(0, 0, 0, 13);

    // 29. Ayu Dark：ayu 官方暗色——深蓝黑底 + 米灰正文，招牌橙强调
    ThemeDefinition ayuDark;
    ayuDark.id = QStringLiteral("ayu-dark");
    ayuDark.name = QStringLiteral("Ayu Dark");
    ayuDark.isDark = true;
    ayuDark.terminalBg = QColor(0x0B, 0x0E, 0x14);
    ayuDark.terminalFg = QColor(0xBF, 0xBD, 0xB6);
    ayuDark.selectionBg = QColor(0x1E, 0x4B, 0x57);
    ayuDark.infoColor = QColor(0x6C, 0x73, 0x80);
    ayuDark.normal[0] = QColor(0x13, 0x17, 0x21); ayuDark.normal[1] = QColor(0xF0, 0x71, 0x78);
    ayuDark.normal[2] = QColor(0xAA, 0xD9, 0x4C); ayuDark.normal[3] = QColor(0xE6, 0xB4, 0x50);
    ayuDark.normal[4] = QColor(0x59, 0xC2, 0xFF); ayuDark.normal[5] = QColor(0xD2, 0xA6, 0xFF);
    ayuDark.normal[6] = QColor(0x95, 0xE6, 0xCB); ayuDark.normal[7] = QColor(0xBF, 0xBD, 0xB6);
    ayuDark.bright[0] = QColor(0x56, 0x5B, 0x66); ayuDark.bright[1] = QColor(0xFF, 0x8A, 0x80);
    ayuDark.bright[2] = QColor(0xB8, 0xCC, 0x52); ayuDark.bright[3] = QColor(0xEF, 0xCB, 0x62);
    ayuDark.bright[4] = QColor(0x73, 0xB8, 0xFF); ayuDark.bright[5] = QColor(0xDC, 0xAF, 0xFF);
    ayuDark.bright[6] = QColor(0xA6, 0xEC, 0xD8); ayuDark.bright[7] = QColor(0xFF, 0xFF, 0xFF);
    ayuDark.windowBg = QColor(0x0B, 0x0E, 0x14);
    ayuDark.panelBg = QColor(0x11, 0x15, 0x1C);
    ayuDark.panelBorder = QColor(0x1E, 0x26, 0x33);
    ayuDark.inputBg = QColor(0x13, 0x17, 0x21);
    ayuDark.inputBorder = QColor(0x2D, 0x36, 0x43);
    ayuDark.accentColor = QColor(0xFF, 0xB4, 0x54);
    ayuDark.textFg = QColor(0xBF, 0xBD, 0xB6);
    ayuDark.dialogBg = QColor(0x11, 0x15, 0x1C);
    ayuDark.statusBarBg = QColor(0x07, 0x09, 0x0D);
    ayuDark.statusBarFg = QColor(0x6C, 0x73, 0x80);
        ayuDark.buttonBg = QColor(0x13, 0x17, 0x21); ayuDark.buttonBorder = QColor(0x2D, 0x36, 0x43);
        ayuDark.buttonHoverBg = QColor(0x1D, 0x24, 0x30); ayuDark.buttonHoverBorder = QColor(0x3E, 0x48, 0x56);
        ayuDark.buttonPressedBg = QColor(0x11, 0x15, 0x1C);
        ayuDark.toolCheckedBg = QColor(0x1E, 0x4B, 0x57); ayuDark.toolCheckedBorder = QColor(0xFF, 0xB4, 0x54);
        ayuDark.listBg = QColor(0x11, 0x15, 0x1C); ayuDark.listAltBg = QColor(0x16, 0x1B, 0x24); ayuDark.listBorder = QColor(0x1E, 0x26, 0x33);
        ayuDark.listHoverBg = QColor(0x1D, 0x24, 0x30); ayuDark.listSelectedBg = QColor(0x1E, 0x4B, 0x57); ayuDark.listSelectedFg = QColor(0xE6, 0xE4, 0xD6);
        ayuDark.headerBg = QColor(0x07, 0x09, 0x0D); ayuDark.headerFg = QColor(0x6C, 0x73, 0x80); ayuDark.gridlineColor = QColor(0x1D, 0x24, 0x30);
        ayuDark.scrollHandleBg = QColor(0x2D, 0x36, 0x43); ayuDark.scrollHandleHoverBg = QColor(0xFF, 0xB4, 0x54);
        ayuDark.scrollTrackBg = QColor(0, 0, 0, 13);

    // 30. Ayu Light：亮色变体——近白底 + 灰蓝正文，橙强调
    ThemeDefinition ayuLight;
    ayuLight.id = QStringLiteral("ayu-light");
    ayuLight.name = QStringLiteral("Ayu Light");
    ayuLight.isDark = false;
    ayuLight.terminalBg = QColor(0xFA, 0xFA, 0xFA);
    ayuLight.terminalFg = QColor(0x5C, 0x61, 0x66);
    ayuLight.selectionBg = QColor(0xEB, 0xE2, 0xAE);
    ayuLight.infoColor = QColor(0x8A, 0x91, 0x99);
    ayuLight.normal[0] = QColor(0x56, 0x5B, 0x66); ayuLight.normal[1] = QColor(0xED, 0x6C, 0x6C);
    ayuLight.normal[2] = QColor(0x86, 0xB3, 0x00); ayuLight.normal[3] = QColor(0xF2, 0xAE, 0x49);
    ayuLight.normal[4] = QColor(0x39, 0x98, 0xF7); ayuLight.normal[5] = QColor(0xA3, 0x7A, 0xCC);
    ayuLight.normal[6] = QColor(0x4C, 0xBF, 0x99); ayuLight.normal[7] = QColor(0x8A, 0x91, 0x99);
    ayuLight.bright[0] = QColor(0x8A, 0x91, 0x99); ayuLight.bright[1] = QColor(0xE0, 0x52, 0x52);
    ayuLight.bright[2] = QColor(0x99, 0xCC, 0x33); ayuLight.bright[3] = QColor(0xF5, 0xB7, 0x57);
    ayuLight.bright[4] = QColor(0x4E, 0xAF, 0xFF); ayuLight.bright[5] = QColor(0xAF, 0x8A, 0xE5);
    ayuLight.bright[6] = QColor(0x59, 0xCB, 0xA8); ayuLight.bright[7] = QColor(0x5C, 0x61, 0x66);
    ayuLight.windowBg = QColor(0xFA, 0xFA, 0xFA);
    ayuLight.panelBg = QColor(0xF1, 0xF2, 0xF3);
    ayuLight.panelBorder = QColor(0xE4, 0xE6, 0xE9);
    ayuLight.inputBg = QColor(0xF1, 0xF2, 0xF3);
    ayuLight.inputBorder = QColor(0xD9, 0xDD, 0xE1);
    ayuLight.accentColor = QColor(0xFA, 0x8D, 0x3E);
    ayuLight.textFg = QColor(0x5C, 0x61, 0x66);
    ayuLight.dialogBg = QColor(0xF1, 0xF2, 0xF3);
    ayuLight.statusBarBg = QColor(0xED, 0xEE, 0xEF);
    ayuLight.statusBarFg = QColor(0x8A, 0x91, 0x99);
        ayuLight.buttonBg = QColor(0xF1, 0xF2, 0xF3); ayuLight.buttonBorder = QColor(0xD9, 0xDD, 0xE1);
        ayuLight.buttonHoverBg = QColor(0xE8, 0xEA, 0xEC); ayuLight.buttonHoverBorder = QColor(0xB5, 0xBC, 0xC4);
        ayuLight.buttonPressedBg = QColor(0xE0, 0xE2, 0xE5);
        ayuLight.toolCheckedBg = QColor(0xEB, 0xE2, 0xAE); ayuLight.toolCheckedBorder = QColor(0xFA, 0x8D, 0x3E);
        ayuLight.listBg = QColor(0xF1, 0xF2, 0xF3); ayuLight.listAltBg = QColor(0xED, 0xEE, 0xEF); ayuLight.listBorder = QColor(0xE4, 0xE6, 0xE9);
        ayuLight.listHoverBg = QColor(0xE8, 0xEA, 0xEC); ayuLight.listSelectedBg = QColor(0xEB, 0xE2, 0xAE); ayuLight.listSelectedFg = QColor(0x4A, 0x50, 0x56);
        ayuLight.headerBg = QColor(0xED, 0xEE, 0xEF); ayuLight.headerFg = QColor(0x8A, 0x91, 0x99); ayuLight.gridlineColor = QColor(0xE4, 0xE6, 0xE9);
        ayuLight.scrollHandleBg = QColor(0xD9, 0xDD, 0xE1); ayuLight.scrollHandleHoverBg = QColor(0xFA, 0x8D, 0x3E);
        ayuLight.scrollTrackBg = QColor(0, 0, 0, 13);

    // 31. Cyberpunk：赛博朋克——紫黑蓝底 + 霓虹青/黄/粉高亮
    ThemeDefinition cyberpunk;
    cyberpunk.id = QStringLiteral("cyberpunk");
    cyberpunk.name = QStringLiteral("Cyberpunk");
    cyberpunk.isDark = true;
    cyberpunk.terminalBg = QColor(0x16, 0x1B, 0x33);
    cyberpunk.terminalFg = QColor(0xC9, 0xC6, 0xE8);
    cyberpunk.selectionBg = QColor(0x2F, 0x3D, 0x74);
    cyberpunk.infoColor = QColor(0x76, 0x7F, 0xAC);
    cyberpunk.normal[0] = QColor(0x21, 0x2A, 0x4E); cyberpunk.normal[1] = QColor(0xFF, 0x4B, 0x64);
    cyberpunk.normal[2] = QColor(0x00, 0xFF, 0x9C); cyberpunk.normal[3] = QColor(0xFD, 0xE8, 0x56);
    cyberpunk.normal[4] = QColor(0x3E, 0xA8, 0xFF); cyberpunk.normal[5] = QColor(0xB2, 0x67, 0xE6);
    cyberpunk.normal[6] = QColor(0x00, 0xE0, 0xE0); cyberpunk.normal[7] = QColor(0xC9, 0xC6, 0xE8);
    cyberpunk.bright[0] = QColor(0x46, 0x51, 0x9A); cyberpunk.bright[1] = QColor(0xFF, 0x7A, 0x8C);
    cyberpunk.bright[2] = QColor(0x5C, 0xFF, 0xBD); cyberpunk.bright[3] = QColor(0xFF, 0xF0, 0x96);
    cyberpunk.bright[4] = QColor(0x7C, 0xC0, 0xFF); cyberpunk.bright[5] = QColor(0xCD, 0x9A, 0xF2);
    cyberpunk.bright[6] = QColor(0x5C, 0xF0, 0xEE); cyberpunk.bright[7] = QColor(0xF0, 0xEF, 0xFA);
    cyberpunk.windowBg = QColor(0x16, 0x1B, 0x33);
    cyberpunk.panelBg = QColor(0x1C, 0x23, 0x44);
    cyberpunk.panelBorder = QColor(0x2C, 0x36, 0x66);
    cyberpunk.inputBg = QColor(0x22, 0x2C, 0x56);
    cyberpunk.inputBorder = QColor(0x34, 0x40, 0x7A);
    cyberpunk.accentColor = QColor(0x00, 0xE0, 0xE0);
    cyberpunk.textFg = QColor(0xC9, 0xC6, 0xE8);
    cyberpunk.dialogBg = QColor(0x1C, 0x23, 0x44);
    cyberpunk.statusBarBg = QColor(0x10, 0x14, 0x28);
    cyberpunk.statusBarFg = QColor(0x76, 0x7F, 0xAC);
        cyberpunk.buttonBg = QColor(0x22, 0x2C, 0x56); cyberpunk.buttonBorder = QColor(0x34, 0x40, 0x7A);
        cyberpunk.buttonHoverBg = QColor(0x2B, 0x36, 0x68); cyberpunk.buttonHoverBorder = QColor(0x44, 0x50, 0x92);
        cyberpunk.buttonPressedBg = QColor(0x1C, 0x23, 0x44);
        cyberpunk.toolCheckedBg = QColor(0x2F, 0x3D, 0x74); cyberpunk.toolCheckedBorder = QColor(0x00, 0xE0, 0xE0);
        cyberpunk.listBg = QColor(0x1C, 0x23, 0x44); cyberpunk.listAltBg = QColor(0x22, 0x2C, 0x56); cyberpunk.listBorder = QColor(0x2C, 0x36, 0x66);
        cyberpunk.listHoverBg = QColor(0x2B, 0x36, 0x68); cyberpunk.listSelectedBg = QColor(0x2F, 0x3D, 0x74); cyberpunk.listSelectedFg = QColor(0xE4, 0xE2, 0xF8);
        cyberpunk.headerBg = QColor(0x10, 0x14, 0x28); cyberpunk.headerFg = QColor(0x76, 0x7F, 0xAC); cyberpunk.gridlineColor = QColor(0x2B, 0x36, 0x68);
        cyberpunk.scrollHandleBg = QColor(0x34, 0x40, 0x7A); cyberpunk.scrollHandleHoverBg = QColor(0x00, 0xE0, 0xE0);
        cyberpunk.scrollTrackBg = QColor(0, 0, 0, 13);

    // 32. Cyberpunk Scarlet：赛博朋克红——黑红底 + 荧光红强调
    ThemeDefinition cyberpunkScarlet;
    cyberpunkScarlet.id = QStringLiteral("cyberpunk-scarlet");
    cyberpunkScarlet.name = QStringLiteral("Cyberpunk Scarlet");
    cyberpunkScarlet.isDark = true;
    cyberpunkScarlet.terminalBg = QColor(0x19, 0x0A, 0x10);
    cyberpunkScarlet.terminalFg = QColor(0xEF, 0xD6, 0xDD);
    cyberpunkScarlet.selectionBg = QColor(0x61, 0x1B, 0x2F);
    cyberpunkScarlet.infoColor = QColor(0x9C, 0x5E, 0x70);
    cyberpunkScarlet.normal[0] = QColor(0x2E, 0x14, 0x1C); cyberpunkScarlet.normal[1] = QColor(0xFF, 0x2B, 0x5C);
    cyberpunkScarlet.normal[2] = QColor(0x62, 0xDF, 0xA0); cyberpunkScarlet.normal[3] = QColor(0xFF, 0xC8, 0x68);
    cyberpunkScarlet.normal[4] = QColor(0x4E, 0x9D, 0xE0); cyberpunkScarlet.normal[5] = QColor(0xB7, 0x5C, 0xF0);
    cyberpunkScarlet.normal[6] = QColor(0x4E, 0xDE, 0xDE); cyberpunkScarlet.normal[7] = QColor(0xEF, 0xD6, 0xDD);
    cyberpunkScarlet.bright[0] = QColor(0x5A, 0x29, 0x40); cyberpunkScarlet.bright[1] = QColor(0xFF, 0x62, 0x85);
    cyberpunkScarlet.bright[2] = QColor(0x8B, 0xEB, 0xC0); cyberpunkScarlet.bright[3] = QColor(0xFF, 0xDF, 0x9E);
    cyberpunkScarlet.bright[4] = QColor(0x7F, 0xC0, 0xFF); cyberpunkScarlet.bright[5] = QColor(0xD2, 0x8F, 0xF5);
    cyberpunkScarlet.bright[6] = QColor(0x7F, 0xF0, 0xF0); cyberpunkScarlet.bright[7] = QColor(0xF8, 0xE8, 0xEE);
    cyberpunkScarlet.windowBg = QColor(0x19, 0x0A, 0x10);
    cyberpunkScarlet.panelBg = QColor(0x24, 0x12, 0x1B);
    cyberpunkScarlet.panelBorder = QColor(0x3F, 0x1E, 0x2F);
    cyberpunkScarlet.inputBg = QColor(0x2E, 0x16, 0x22);
    cyberpunkScarlet.inputBorder = QColor(0x4A, 0x23, 0x38);
    cyberpunkScarlet.accentColor = QColor(0xFF, 0x2B, 0x5C);
    cyberpunkScarlet.textFg = QColor(0xEF, 0xD6, 0xDD);
    cyberpunkScarlet.dialogBg = QColor(0x24, 0x12, 0x1B);
    cyberpunkScarlet.statusBarBg = QColor(0x13, 0x07, 0x10);
    cyberpunkScarlet.statusBarFg = QColor(0x9C, 0x5E, 0x70);
        cyberpunkScarlet.buttonBg = QColor(0x2E, 0x16, 0x22); cyberpunkScarlet.buttonBorder = QColor(0x4A, 0x23, 0x38);
        cyberpunkScarlet.buttonHoverBg = QColor(0x3D, 0x1E, 0x2D); cyberpunkScarlet.buttonHoverBorder = QColor(0x5D, 0x2E, 0x42);
        cyberpunkScarlet.buttonPressedBg = QColor(0x24, 0x12, 0x1B);
        cyberpunkScarlet.toolCheckedBg = QColor(0x61, 0x1B, 0x2F); cyberpunkScarlet.toolCheckedBorder = QColor(0xFF, 0x2B, 0x5C);
        cyberpunkScarlet.listBg = QColor(0x24, 0x12, 0x1B); cyberpunkScarlet.listAltBg = QColor(0x2E, 0x16, 0x22); cyberpunkScarlet.listBorder = QColor(0x3F, 0x1E, 0x2F);
        cyberpunkScarlet.listHoverBg = QColor(0x3D, 0x1E, 0x2D); cyberpunkScarlet.listSelectedBg = QColor(0x61, 0x1B, 0x2F); cyberpunkScarlet.listSelectedFg = QColor(0xF8, 0xE4, 0xEC);
        cyberpunkScarlet.headerBg = QColor(0x13, 0x07, 0x10); cyberpunkScarlet.headerFg = QColor(0x9C, 0x5E, 0x70); cyberpunkScarlet.gridlineColor = QColor(0x3D, 0x1E, 0x2D);
        cyberpunkScarlet.scrollHandleBg = QColor(0x4A, 0x23, 0x38); cyberpunkScarlet.scrollHandleHoverBg = QColor(0xFF, 0x2B, 0x5C);
        cyberpunkScarlet.scrollTrackBg = QColor(0, 0, 0, 13);

    // 33. Romania Night：Termius 风——深藏青底 + 冷白正文，海蓝强调
    ThemeDefinition romaniaNight;
    romaniaNight.id = QStringLiteral("romania-night");
    romaniaNight.name = QStringLiteral("Romania Night");
    romaniaNight.isDark = true;
    romaniaNight.terminalBg = QColor(0x0E, 0x22, 0x33);
    romaniaNight.terminalFg = QColor(0xBF, 0xDC, 0xE8);
    romaniaNight.selectionBg = QColor(0x27, 0x49, 0x60);
    romaniaNight.infoColor = QColor(0x6B, 0x8F, 0xA5);
    romaniaNight.normal[0] = QColor(0x1A, 0x34, 0x46); romaniaNight.normal[1] = QColor(0xE4, 0x62, 0x5E);
    romaniaNight.normal[2] = QColor(0x3F, 0xB8, 0x77); romaniaNight.normal[3] = QColor(0xE8, 0xB8, 0x4F);
    romaniaNight.normal[4] = QColor(0x4B, 0x9C, 0xD9); romaniaNight.normal[5] = QColor(0x9B, 0x78, 0xDB);
    romaniaNight.normal[6] = QColor(0x40, 0xB5, 0xC9); romaniaNight.normal[7] = QColor(0xBF, 0xDC, 0xE8);
    romaniaNight.bright[0] = QColor(0x3D, 0x5F, 0x74); romaniaNight.bright[1] = QColor(0xF2, 0x85, 0x84);
    romaniaNight.bright[2] = QColor(0x5B, 0xD4, 0x9A); romaniaNight.bright[3] = QColor(0xF4, 0xCC, 0x70);
    romaniaNight.bright[4] = QColor(0x71, 0xB6, 0xE8); romaniaNight.bright[5] = QColor(0xB6, 0x92, 0xEA);
    romaniaNight.bright[6] = QColor(0x6B, 0xCB, 0xDD); romaniaNight.bright[7] = QColor(0xDC, 0xEF, 0xF6);
    romaniaNight.windowBg = QColor(0x0E, 0x22, 0x33);
    romaniaNight.panelBg = QColor(0x14, 0x2B, 0x3D);
    romaniaNight.panelBorder = QColor(0x24, 0x45, 0x5C);
    romaniaNight.inputBg = QColor(0x1B, 0x35, 0x47);
    romaniaNight.inputBorder = QColor(0x2E, 0x55, 0x6C);
    romaniaNight.accentColor = QColor(0x4B, 0x9C, 0xD9);
    romaniaNight.textFg = QColor(0xBF, 0xDC, 0xE8);
    romaniaNight.dialogBg = QColor(0x14, 0x2B, 0x3D);
    romaniaNight.statusBarBg = QColor(0x0A, 0x1B, 0x28);
    romaniaNight.statusBarFg = QColor(0x6B, 0x8F, 0xA5);
        romaniaNight.buttonBg = QColor(0x1B, 0x35, 0x47); romaniaNight.buttonBorder = QColor(0x2E, 0x55, 0x6C);
        romaniaNight.buttonHoverBg = QColor(0x24, 0x45, 0x5C); romaniaNight.buttonHoverBorder = QColor(0x3C, 0x67, 0x82);
        romaniaNight.buttonPressedBg = QColor(0x14, 0x2B, 0x3D);
        romaniaNight.toolCheckedBg = QColor(0x27, 0x49, 0x60); romaniaNight.toolCheckedBorder = QColor(0x4B, 0x9C, 0xD9);
        romaniaNight.listBg = QColor(0x14, 0x2B, 0x3D); romaniaNight.listAltBg = QColor(0x1B, 0x35, 0x47); romaniaNight.listBorder = QColor(0x24, 0x45, 0x5C);
        romaniaNight.listHoverBg = QColor(0x24, 0x45, 0x5C); romaniaNight.listSelectedBg = QColor(0x27, 0x49, 0x60); romaniaNight.listSelectedFg = QColor(0xE2, 0xF1, 0xF8);
        romaniaNight.headerBg = QColor(0x0A, 0x1B, 0x28); romaniaNight.headerFg = QColor(0x6B, 0x8F, 0xA5); romaniaNight.gridlineColor = QColor(0x24, 0x45, 0x5C);
        romaniaNight.scrollHandleBg = QColor(0x2E, 0x55, 0x6C); romaniaNight.scrollHandleHoverBg = QColor(0x4B, 0x9C, 0xD9);
        romaniaNight.scrollTrackBg = QColor(0, 0, 0, 13);

    // 34. Romania Day：亮色变体——米白暖底 + 深灰正文
    ThemeDefinition romaniaDay;
    romaniaDay.id = QStringLiteral("romania-day");
    romaniaDay.name = QStringLiteral("Romania Day");
    romaniaDay.isDark = false;
    romaniaDay.terminalBg = QColor(0xF3, 0xEF, 0xE6);
    romaniaDay.terminalFg = QColor(0x3E, 0x46, 0x4B);
    romaniaDay.selectionBg = QColor(0xDB, 0xE2, 0xDF);
    romaniaDay.infoColor = QColor(0x87, 0x91, 0x9A);
    romaniaDay.normal[0] = QColor(0x3E, 0x46, 0x4B); romaniaDay.normal[1] = QColor(0xCC, 0x4C, 0x46);
    romaniaDay.normal[2] = QColor(0x3E, 0x9E, 0x60); romaniaDay.normal[3] = QColor(0xC4, 0x9B, 0x40);
    romaniaDay.normal[4] = QColor(0x3E, 0x78, 0xBA); romaniaDay.normal[5] = QColor(0x8A, 0x63, 0xC4);
    romaniaDay.normal[6] = QColor(0x3D, 0x9E, 0xA8); romaniaDay.normal[7] = QColor(0x87, 0x91, 0x9A);
    romaniaDay.bright[0] = QColor(0x6C, 0x75, 0x7B); romaniaDay.bright[1] = QColor(0xE0, 0x60, 0x5A);
    romaniaDay.bright[2] = QColor(0x52, 0xBA, 0x74); romaniaDay.bright[3] = QColor(0xD8, 0xB3, 0x5C);
    romaniaDay.bright[4] = QColor(0x55, 0x90, 0xD2); romaniaDay.bright[5] = QColor(0xA3, 0x7B, 0xDA);
    romaniaDay.bright[6] = QColor(0x52, 0xB5, 0xC1); romaniaDay.bright[7] = QColor(0x56, 0x5F, 0x64);
    romaniaDay.windowBg = QColor(0xF3, 0xEF, 0xE6);
    romaniaDay.panelBg = QColor(0xEB, 0xE6, 0xDA);
    romaniaDay.panelBorder = QColor(0xD9, 0xD2, 0xC2);
    romaniaDay.inputBg = QColor(0xEB, 0xE6, 0xDA);
    romaniaDay.inputBorder = QColor(0xC9, 0xC1, 0xAF);
    romaniaDay.accentColor = QColor(0x3E, 0x78, 0xBA);
    romaniaDay.textFg = QColor(0x3E, 0x46, 0x4B);
    romaniaDay.dialogBg = QColor(0xEB, 0xE6, 0xDA);
    romaniaDay.statusBarBg = QColor(0xE3, 0xDD, 0xD0);
    romaniaDay.statusBarFg = QColor(0x87, 0x91, 0x9A);
        romaniaDay.buttonBg = QColor(0xEB, 0xE6, 0xDA); romaniaDay.buttonBorder = QColor(0xC9, 0xC1, 0xAF);
        romaniaDay.buttonHoverBg = QColor(0xE0, 0xDA, 0xCA); romaniaDay.buttonHoverBorder = QColor(0xAA, 0xA2, 0x8E);
        romaniaDay.buttonPressedBg = QColor(0xD9, 0xD2, 0xC2);
        romaniaDay.toolCheckedBg = QColor(0xDB, 0xE2, 0xDF); romaniaDay.toolCheckedBorder = QColor(0x3E, 0x78, 0xBA);
        romaniaDay.listBg = QColor(0xEB, 0xE6, 0xDA); romaniaDay.listAltBg = QColor(0xE3, 0xDD, 0xD0); romaniaDay.listBorder = QColor(0xD9, 0xD2, 0xC2);
        romaniaDay.listHoverBg = QColor(0xE0, 0xDA, 0xCA); romaniaDay.listSelectedBg = QColor(0xDB, 0xE2, 0xDF); romaniaDay.listSelectedFg = QColor(0x32, 0x3A, 0x3E);
        romaniaDay.headerBg = QColor(0xE3, 0xDD, 0xD0); romaniaDay.headerFg = QColor(0x87, 0x91, 0x9A); romaniaDay.gridlineColor = QColor(0xE0, 0xDA, 0xCA);
        romaniaDay.scrollHandleBg = QColor(0xC9, 0xC1, 0xAF); romaniaDay.scrollHandleHoverBg = QColor(0x3E, 0x78, 0xBA);
        romaniaDay.scrollTrackBg = QColor(0, 0, 0, 13);

    // 35. Aubergine：VS Code 经典茄子紫——深紫黑底 + 淡紫正文
    ThemeDefinition aubergine;
    aubergine.id = QStringLiteral("aubergine");
    aubergine.name = QStringLiteral("Aubergine");
    aubergine.isDark = true;
    aubergine.terminalBg = QColor(0x20, 0x0B, 0x3E);
    aubergine.terminalFg = QColor(0xD4, 0xCC, 0xE8);
    aubergine.selectionBg = QColor(0x45, 0x28, 0x6E);
    aubergine.infoColor = QColor(0x8D, 0x7D, 0xA8);
    aubergine.normal[0] = QColor(0x2E, 0x1C, 0x4E); aubergine.normal[1] = QColor(0xE0, 0x5C, 0x8A);
    aubergine.normal[2] = QColor(0x4E, 0xC9, 0x8C); aubergine.normal[3] = QColor(0xE8, 0xC4, 0x4C);
    aubergine.normal[4] = QColor(0x8A, 0x8A, 0xE8); aubergine.normal[5] = QColor(0xB8, 0x7C, 0xE6);
    aubergine.normal[6] = QColor(0x4E, 0xCC, 0xCC); aubergine.normal[7] = QColor(0xD4, 0xCC, 0xE8);
    aubergine.bright[0] = QColor(0x49, 0x38, 0x6D); aubergine.bright[1] = QColor(0xF0, 0x85, 0xAC);
    aubergine.bright[2] = QColor(0x6E, 0xDC, 0xA6); aubergine.bright[3] = QColor(0xF2, 0xD6, 0x73);
    aubergine.bright[4] = QColor(0xA3, 0xA3, 0xF0); aubergine.bright[5] = QColor(0xCD, 0x9A, 0xF0);
    aubergine.bright[6] = QColor(0x6E, 0xDD, 0xDD); aubergine.bright[7] = QColor(0xEA, 0xE2, 0xF6);
    aubergine.windowBg = QColor(0x20, 0x0B, 0x3E);
    aubergine.panelBg = QColor(0x28, 0x10, 0x48);
    aubergine.panelBorder = QColor(0x3B, 0x21, 0x59);
    aubergine.inputBg = QColor(0x2E, 0x18, 0x56);
    aubergine.inputBorder = QColor(0x45, 0x28, 0x6E);
    aubergine.accentColor = QColor(0xB8, 0x7C, 0xE6);
    aubergine.textFg = QColor(0xD4, 0xCC, 0xE8);
    aubergine.dialogBg = QColor(0x28, 0x10, 0x48);
    aubergine.statusBarBg = QColor(0x18, 0x08, 0x29);
    aubergine.statusBarFg = QColor(0x8D, 0x7D, 0xA8);
        aubergine.buttonBg = QColor(0x2E, 0x18, 0x56); aubergine.buttonBorder = QColor(0x45, 0x28, 0x6E);
        aubergine.buttonHoverBg = QColor(0x3B, 0x21, 0x59); aubergine.buttonHoverBorder = QColor(0x55, 0x33, 0x7C);
        aubergine.buttonPressedBg = QColor(0x28, 0x10, 0x48);
        aubergine.toolCheckedBg = QColor(0x45, 0x28, 0x6E); aubergine.toolCheckedBorder = QColor(0xB8, 0x7C, 0xE6);
        aubergine.listBg = QColor(0x28, 0x10, 0x48); aubergine.listAltBg = QColor(0x2E, 0x18, 0x56); aubergine.listBorder = QColor(0x3B, 0x21, 0x59);
        aubergine.listHoverBg = QColor(0x3B, 0x21, 0x59); aubergine.listSelectedBg = QColor(0x45, 0x28, 0x6E); aubergine.listSelectedFg = QColor(0xE8, 0xE0, 0xF6);
        aubergine.headerBg = QColor(0x18, 0x08, 0x29); aubergine.headerFg = QColor(0x8D, 0x7D, 0xA8); aubergine.gridlineColor = QColor(0x3B, 0x21, 0x59);
        aubergine.scrollHandleBg = QColor(0x45, 0x28, 0x6E); aubergine.scrollHandleHoverBg = QColor(0xB8, 0x7C, 0xE6);
        aubergine.scrollTrackBg = QColor(0, 0, 0, 13);

    // 36. Peach Fresh：蜜桃亮色——橙桃底 + 深褐正文，暖橙强调
    ThemeDefinition peachFresh;
    peachFresh.id = QStringLiteral("peach-fresh");
    peachFresh.name = QStringLiteral("Peach Fresh");
    peachFresh.isDark = false;
    peachFresh.terminalBg = QColor(0xFF, 0xD9, 0xB3);
    peachFresh.terminalFg = QColor(0x5C, 0x3A, 0x1E);
    peachFresh.selectionBg = QColor(0xF3, 0xBE, 0x8C);
    peachFresh.infoColor = QColor(0xA6, 0x79, 0x5C);
    peachFresh.normal[0] = QColor(0x7C, 0x52, 0x32); peachFresh.normal[1] = QColor(0xE0, 0x48, 0x40);
    peachFresh.normal[2] = QColor(0x3E, 0x9E, 0x50); peachFresh.normal[3] = QColor(0xDF, 0xA8, 0x2E);
    peachFresh.normal[4] = QColor(0x2E, 0x78, 0xCA); peachFresh.normal[5] = QColor(0x9C, 0x5C, 0xCA);
    peachFresh.normal[6] = QColor(0x2E, 0xA8, 0xA2); peachFresh.normal[7] = QColor(0xA6, 0x79, 0x5C);
    peachFresh.bright[0] = QColor(0xB2, 0x81, 0x50); peachFresh.bright[1] = QColor(0xF2, 0x62, 0x5A);
    peachFresh.bright[2] = QColor(0x52, 0xBA, 0x6A); peachFresh.bright[3] = QColor(0xF2, 0xC0, 0x52);
    peachFresh.bright[4] = QColor(0x4C, 0x94, 0xE0); peachFresh.bright[5] = QColor(0xB8, 0x7C, 0xE0);
    peachFresh.bright[6] = QColor(0x4C, 0xC4, 0xBE); peachFresh.bright[7] = QColor(0x5C, 0x3A, 0x1E);
    peachFresh.windowBg = QColor(0xFF, 0xD9, 0xB3);
    peachFresh.panelBg = QColor(0xFA, 0xD0, 0xA8);
    peachFresh.panelBorder = QColor(0xE8, 0xB7, 0x8A);
    peachFresh.inputBg = QColor(0xF5, 0xC9, 0x9E);
    peachFresh.inputBorder = QColor(0xD2, 0xA2, 0x6E);
    peachFresh.accentColor = QColor(0xE0, 0x5E, 0x2E);
    peachFresh.textFg = QColor(0x5C, 0x3A, 0x1E);
    peachFresh.dialogBg = QColor(0xFA, 0xD0, 0xA8);
    peachFresh.statusBarBg = QColor(0xF0, 0xC3, 0x98);
    peachFresh.statusBarFg = QColor(0xA6, 0x79, 0x5C);
        peachFresh.buttonBg = QColor(0xF5, 0xC9, 0x9E); peachFresh.buttonBorder = QColor(0xD2, 0xA2, 0x6E);
        peachFresh.buttonHoverBg = QColor(0xEE, 0xBF, 0x90); peachFresh.buttonHoverBorder = QColor(0xB8, 0x8A, 0x5A);
        peachFresh.buttonPressedBg = QColor(0xE8, 0xB7, 0x8A);
        peachFresh.toolCheckedBg = QColor(0xF3, 0xBE, 0x8C); peachFresh.toolCheckedBorder = QColor(0xE0, 0x5E, 0x2E);
        peachFresh.listBg = QColor(0xFA, 0xD0, 0xA8); peachFresh.listAltBg = QColor(0xF0, 0xC3, 0x98); peachFresh.listBorder = QColor(0xE8, 0xB7, 0x8A);
        peachFresh.listHoverBg = QColor(0xEE, 0xBF, 0x90); peachFresh.listSelectedBg = QColor(0xF3, 0xBE, 0x8C); peachFresh.listSelectedFg = QColor(0x4E, 0x32, 0x18);
        peachFresh.headerBg = QColor(0xF0, 0xC3, 0x98); peachFresh.headerFg = QColor(0xA6, 0x79, 0x5C); peachFresh.gridlineColor = QColor(0xEE, 0xBF, 0x90);
        peachFresh.scrollHandleBg = QColor(0xD2, 0xA2, 0x6E); peachFresh.scrollHandleHoverBg = QColor(0xE0, 0x5E, 0x2E);
        peachFresh.scrollTrackBg = QColor(0, 0, 0, 13);

    // 37. 1984 Dark：Orwell 风——深灰蓝底 + 暖白正文，招牌荧光黄强调
    ThemeDefinition o1984Dark;
    o1984Dark.id = QStringLiteral("1984-dark");
    o1984Dark.name = QStringLiteral("1984 Dark");
    o1984Dark.isDark = true;
    o1984Dark.terminalBg = QColor(0x0D, 0x10, 0x16);
    o1984Dark.terminalFg = QColor(0xE6, 0xE2, 0xD6);
    o1984Dark.selectionBg = QColor(0x2B, 0x32, 0x44);
    o1984Dark.infoColor = QColor(0x74, 0x7E, 0x8A);
    o1984Dark.normal[0] = QColor(0x1A, 0x1F, 0x28); o1984Dark.normal[1] = QColor(0xFF, 0x47, 0x52);
    o1984Dark.normal[2] = QColor(0x00, 0xE2, 0x8C); o1984Dark.normal[3] = QColor(0xFB, 0xED, 0x6E);
    o1984Dark.normal[4] = QColor(0x00, 0xA3, 0xCC); o1984Dark.normal[5] = QColor(0xA5, 0x65, 0xE8);
    o1984Dark.normal[6] = QColor(0x33, 0xC7, 0xCE); o1984Dark.normal[7] = QColor(0xE6, 0xE2, 0xD6);
    o1984Dark.bright[0] = QColor(0x3A, 0x43, 0x50); o1984Dark.bright[1] = QColor(0xFF, 0x6C, 0x74);
    o1984Dark.bright[2] = QColor(0x4C, 0xED, 0xA6); o1984Dark.bright[3] = QColor(0xFF, 0xF4, 0x9B);
    o1984Dark.bright[4] = QColor(0x4C, 0xC0, 0xDE); o1984Dark.bright[5] = QColor(0xBC, 0x8A, 0xF0);
    o1984Dark.bright[6] = QColor(0x66, 0xD9, 0xE0); o1984Dark.bright[7] = QColor(0xF5, 0xF2, 0xEA);
    o1984Dark.windowBg = QColor(0x0D, 0x10, 0x16);
    o1984Dark.panelBg = QColor(0x14, 0x19, 0x22);
    o1984Dark.panelBorder = QColor(0x25, 0x2D, 0x3A);
    o1984Dark.inputBg = QColor(0x1B, 0x22, 0x2E);
    o1984Dark.inputBorder = QColor(0x31, 0x3C, 0x4C);
    o1984Dark.accentColor = QColor(0xFB, 0xED, 0x6E);
    o1984Dark.textFg = QColor(0xE6, 0xE2, 0xD6);
    o1984Dark.dialogBg = QColor(0x14, 0x19, 0x22);
    o1984Dark.statusBarBg = QColor(0x0A, 0x0D, 0x12);
    o1984Dark.statusBarFg = QColor(0x74, 0x7E, 0x8A);
        o1984Dark.buttonBg = QColor(0x1B, 0x22, 0x2E); o1984Dark.buttonBorder = QColor(0x31, 0x3C, 0x4C);
        o1984Dark.buttonHoverBg = QColor(0x25, 0x2E, 0x3C); o1984Dark.buttonHoverBorder = QColor(0x3F, 0x4C, 0x5E);
        o1984Dark.buttonPressedBg = QColor(0x14, 0x19, 0x22);
        o1984Dark.toolCheckedBg = QColor(0x2B, 0x32, 0x44); o1984Dark.toolCheckedBorder = QColor(0xFB, 0xED, 0x6E);
        o1984Dark.listBg = QColor(0x14, 0x19, 0x22); o1984Dark.listAltBg = QColor(0x1B, 0x22, 0x2E); o1984Dark.listBorder = QColor(0x25, 0x2D, 0x3A);
        o1984Dark.listHoverBg = QColor(0x25, 0x2E, 0x3C); o1984Dark.listSelectedBg = QColor(0x2B, 0x32, 0x44); o1984Dark.listSelectedFg = QColor(0xF2, 0xEF, 0xE4);
        o1984Dark.headerBg = QColor(0x0A, 0x0D, 0x12); o1984Dark.headerFg = QColor(0x74, 0x7E, 0x8A); o1984Dark.gridlineColor = QColor(0x25, 0x2E, 0x3C);
        o1984Dark.scrollHandleBg = QColor(0x31, 0x3C, 0x4C); o1984Dark.scrollHandleHoverBg = QColor(0xFB, 0xED, 0x6E);
        o1984Dark.scrollTrackBg = QColor(0, 0, 0, 13);

    // 38. 1984 Light：亮色变体——纸白底 + 深灰正文，暖黄强调
    ThemeDefinition o1984Light;
    o1984Light.id = QStringLiteral("1984-light");
    o1984Light.name = QStringLiteral("1984 Light");
    o1984Light.isDark = false;
    o1984Light.terminalBg = QColor(0xFB, 0xFB, 0xF8);
    o1984Light.terminalFg = QColor(0x23, 0x25, 0x2C);
    o1984Light.selectionBg = QColor(0xE5, 0xE5, 0xD8);
    o1984Light.infoColor = QColor(0x8A, 0x8F, 0x96);
    o1984Light.normal[0] = QColor(0x3A, 0x3E, 0x46); o1984Light.normal[1] = QColor(0xD6, 0x3A, 0x46);
    o1984Light.normal[2] = QColor(0x00, 0xA6, 0x6C); o1984Light.normal[3] = QColor(0xC2, 0xA6, 0x28);
    o1984Light.normal[4] = QColor(0x00, 0x87, 0xA6); o1984Light.normal[5] = QColor(0x8A, 0x4E, 0xC8);
    o1984Light.normal[6] = QColor(0x00, 0xA0, 0xA8); o1984Light.normal[7] = QColor(0x8A, 0x8F, 0x96);
    o1984Light.bright[0] = QColor(0x6B, 0x70, 0x78); o1984Light.bright[1] = QColor(0xE8, 0x50, 0x5A);
    o1984Light.bright[2] = QColor(0x1D, 0xBD, 0x80); o1984Light.bright[3] = QColor(0xE0, 0xBE, 0x52);
    o1984Light.bright[4] = QColor(0x1F, 0xB0, 0xD0); o1984Light.bright[5] = QColor(0xA6, 0x6B, 0xE0);
    o1984Light.bright[6] = QColor(0x40, 0xC4, 0xCC); o1984Light.bright[7] = QColor(0x23, 0x25, 0x2C);
    o1984Light.windowBg = QColor(0xFB, 0xFB, 0xF8);
    o1984Light.panelBg = QColor(0xF0, 0xF0, 0xEA);
    o1984Light.panelBorder = QColor(0xDF, 0xDF, 0xD5);
    o1984Light.inputBg = QColor(0xF0, 0xF0, 0xEA);
    o1984Light.inputBorder = QColor(0xCB, 0xCB, 0xBE);
    o1984Light.accentColor = QColor(0xC2, 0xA6, 0x28);
    o1984Light.textFg = QColor(0x23, 0x25, 0x2C);
    o1984Light.dialogBg = QColor(0xF0, 0xF0, 0xEA);
    o1984Light.statusBarBg = QColor(0xE8, 0xE8, 0xE0);
    o1984Light.statusBarFg = QColor(0x8A, 0x8F, 0x96);
        o1984Light.buttonBg = QColor(0xF0, 0xF0, 0xEA); o1984Light.buttonBorder = QColor(0xCB, 0xCB, 0xBE);
        o1984Light.buttonHoverBg = QColor(0xE6, 0xE6, 0xDC); o1984Light.buttonHoverBorder = QColor(0xAA, 0xAA, 0x9C);
        o1984Light.buttonPressedBg = QColor(0xDF, 0xDF, 0xD5);
        o1984Light.toolCheckedBg = QColor(0xE5, 0xE5, 0xD8); o1984Light.toolCheckedBorder = QColor(0xC2, 0xA6, 0x28);
        o1984Light.listBg = QColor(0xF0, 0xF0, 0xEA); o1984Light.listAltBg = QColor(0xE8, 0xE8, 0xE0); o1984Light.listBorder = QColor(0xDF, 0xDF, 0xD5);
        o1984Light.listHoverBg = QColor(0xE6, 0xE6, 0xDC); o1984Light.listSelectedBg = QColor(0xE5, 0xE5, 0xD8); o1984Light.listSelectedFg = QColor(0x1D, 0x1F, 0x24);
        o1984Light.headerBg = QColor(0xE8, 0xE8, 0xE0); o1984Light.headerFg = QColor(0x8A, 0x8F, 0x96); o1984Light.gridlineColor = QColor(0xE6, 0xE6, 0xDC);
        o1984Light.scrollHandleBg = QColor(0xCB, 0xCB, 0xBE); o1984Light.scrollHandleHoverBg = QColor(0xC2, 0xA6, 0x28);
        o1984Light.scrollTrackBg = QColor(0, 0, 0, 13);

    // 39. Winter Night：冬夜——深蓝灰底 + 冷白正文，冰青强调
    ThemeDefinition winterNight;
    winterNight.id = QStringLiteral("winter-night");
    winterNight.name = QStringLiteral("Winter Night");
    winterNight.isDark = true;
    winterNight.terminalBg = QColor(0x10, 0x17, 0x20);
    winterNight.terminalFg = QColor(0xD6, 0xE0, 0xE8);
    winterNight.selectionBg = QColor(0x25, 0x3A, 0x4E);
    winterNight.infoColor = QColor(0x7A, 0x8C, 0x9E);
    winterNight.normal[0] = QColor(0x1C, 0x27, 0x32); winterNight.normal[1] = QColor(0xE8, 0x6C, 0x7E);
    winterNight.normal[2] = QColor(0x4C, 0xC2, 0x9C); winterNight.normal[3] = QColor(0xE8, 0xC4, 0x6E);
    winterNight.normal[4] = QColor(0x5C, 0xA8, 0xEA); winterNight.normal[5] = QColor(0x9C, 0x7C, 0xE8);
    winterNight.normal[6] = QColor(0x4C, 0xC2, 0xDA); winterNight.normal[7] = QColor(0xD6, 0xE0, 0xE8);
    winterNight.bright[0] = QColor(0x3E, 0x52, 0x68); winterNight.bright[1] = QColor(0xF2, 0x8E, 0xA2);
    winterNight.bright[2] = QColor(0x72, 0xD8, 0xB6); winterNight.bright[3] = QColor(0xF2, 0xD8, 0x90);
    winterNight.bright[4] = QColor(0x86, 0xC2, 0xF4); winterNight.bright[5] = QColor(0xB8, 0x9A, 0xF4);
    winterNight.bright[6] = QColor(0x72, 0xDA, 0xE8); winterNight.bright[7] = QColor(0xED, 0xF4, 0xFA);
    winterNight.windowBg = QColor(0x10, 0x17, 0x20);
    winterNight.panelBg = QColor(0x16, 0x1F, 0x2A);
    winterNight.panelBorder = QColor(0x27, 0x37, 0x48);
    winterNight.inputBg = QColor(0x1D, 0x28, 0x36);
    winterNight.inputBorder = QColor(0x33, 0x46, 0x5A);
    winterNight.accentColor = QColor(0x4C, 0xC2, 0xDA);
    winterNight.textFg = QColor(0xD6, 0xE0, 0xE8);
    winterNight.dialogBg = QColor(0x16, 0x1F, 0x2A);
    winterNight.statusBarBg = QColor(0x0C, 0x12, 0x18);
    winterNight.statusBarFg = QColor(0x7A, 0x8C, 0x9E);
        winterNight.buttonBg = QColor(0x1D, 0x28, 0x36); winterNight.buttonBorder = QColor(0x33, 0x46, 0x5A);
        winterNight.buttonHoverBg = QColor(0x27, 0x37, 0x48); winterNight.buttonHoverBorder = QColor(0x43, 0x5A, 0x72);
        winterNight.buttonPressedBg = QColor(0x16, 0x1F, 0x2A);
        winterNight.toolCheckedBg = QColor(0x25, 0x3A, 0x4E); winterNight.toolCheckedBorder = QColor(0x4C, 0xC2, 0xDA);
        winterNight.listBg = QColor(0x16, 0x1F, 0x2A); winterNight.listAltBg = QColor(0x1D, 0x28, 0x36); winterNight.listBorder = QColor(0x27, 0x37, 0x48);
        winterNight.listHoverBg = QColor(0x27, 0x37, 0x48); winterNight.listSelectedBg = QColor(0x25, 0x3A, 0x4E); winterNight.listSelectedFg = QColor(0xE6, 0xEF, 0xF5);
        winterNight.headerBg = QColor(0x0C, 0x12, 0x18); winterNight.headerFg = QColor(0x7A, 0x8C, 0x9E); winterNight.gridlineColor = QColor(0x27, 0x37, 0x48);
        winterNight.scrollHandleBg = QColor(0x33, 0x46, 0x5A); winterNight.scrollHandleHoverBg = QColor(0x4C, 0xC2, 0xDA);
        winterNight.scrollTrackBg = QColor(0, 0, 0, 13);

    return { dark, light, dracula, nord, monokai, oneDark, solDark, solLight, termius,
             flexokiDark, flexokiLight, kanagawaWave, kanagawaDragon, kanagawaLotus,
             hackerBlue, hackerGreen, hackerRed, everforestDark, everforestLight, nightOwl, lightOwl,
             aura, rosePine, rosePineMoon, rosePineDawn, cobalt2, octocatDark, octocatLight,
             ayuDark, ayuLight, cyberpunk, cyberpunkScarlet, romaniaNight, romaniaDay,
             aubergine, peachFresh, o1984Dark, o1984Light, winterNight };
}

} // namespace

ThemeManager::ThemeManager(CommandStore *store, QObject *parent)
    : QObject(parent)
    , m_store(store)
{
}

const QList<ThemeDefinition> &ThemeManager::availableThemes()
{
    static const QList<ThemeDefinition> s_themes = createThemes();
    return s_themes;
}

ThemeDefinition ThemeManager::themeById(const QString &id)
{
    const auto &list = availableThemes();
    for (const auto &t : list) {
        if (t.id == id)
            return t;
    }
    return list.first(); // 默认 dark
}

ThemeDefinition ThemeManager::currentTheme() const
{
    return themeById(currentThemeId());
}

QString ThemeManager::currentThemeId() const
{
    const QString id = m_store->theme();
    return id.isEmpty() ? QStringLiteral("dark") : id;
}

void ThemeManager::setTheme(const QString &id)
{
    m_store->setTheme(id);
    const ThemeDefinition theme = currentTheme();
    applyNow();
    emit themeChanged(theme.isDark);
    emit themeUpdated(theme);
}

bool ThemeManager::isDark() const
{
    return currentTheme().isDark;
}

void ThemeManager::toggleTheme()
{
    if (isDark()) {
        setTheme(QStringLiteral("light"));
    } else {
        setTheme(QStringLiteral("dark"));
    }
}

void ThemeManager::applyNow()
{
    const ThemeDefinition theme = currentTheme();
    setIconThemeDark(theme.isDark);

    // 轨道色可能是半透明（内置主题 5% 黑/白叠加）：QColor::name() 不带 alpha，
    // 半透明时改用 QSS rgba() 格式；不透明时用 #hex（Termius 实色轨道）。
    // alpha 用 0-255 整数：Qt QSS 的 rgba() 不认小数 alpha，写小数整条规则会丢
    const auto cssColor = [](const QColor &c) {
        return c.alpha() < 255
            ? QStringLiteral("rgba(%1,%2,%3,%4)").arg(c.red()).arg(c.green())
                  .arg(c.blue()).arg(c.alpha())
            : c.name();
    };
    const QString trackCss = cssColor(theme.scrollTrackBg);
    // 命令栏浮层毛玻璃底色：panelBg 先抬一档再半透明（0-255 整数 alpha）——
    // dracula 等主题 panelBg 与终端底同色，不抬档的半透明叠加会彻底隐形；
    // 暗色更亮、亮色更暗，浮层永远是可辨的「悬浮面」。Qt 子控件无法调用系统
    // 模糊，半透明实色 + 左缘分隔线即磨砂质感（底下透出终端底色的淡影）
    QColor overlayColor = theme.isDark ? theme.panelBg.lighter(112)
                                       : theme.panelBg.darker(104);
    overlayColor.setAlpha(200);
    const QString overlayCss = cssColor(overlayColor);

    // 侧边栏三页签选中/hover 色块（修复用户反馈 2026-11-16）：不能沿用 buttonHoverBg——
    // 本模板恒拼在静态 qss 之后（同特异性后写胜出），%16 在亮色系与浮层底（overlayCss
    // = panelBg ± 一档）撞色，选中态隐形（GitHub Light #F0F0F3 vs ≈#EDEEF0）。
    // 暗色仍用 buttonHoverBg（与标题栏 hover 同源，视觉已验收）；亮色从 panelBg 再深
    // 一档（darker(112)，浮层只 darker(104)），恒比所在浮层底深约 8% 亮度
    const QColor cbTabBg = theme.isDark ? theme.buttonHoverBg
                                        : theme.panelBg.darker(112);
    const QString cbTabBgCss = cssColor(cbTabBg);

    QFile f(theme.isDark ? QStringLiteral(":/style-dark.qss") : QStringLiteral(":/style-light.qss"));
    QString qss;
    if (f.open(QIODevice::ReadOnly | QIODevice::Text))
        qss = QString::fromUtf8(f.readAll());

    // 针对当前主题动态注入色彩覆盖。基础 QSS 只备了 dark/light 两套色，
    // 第三方主题（及部分强调色）全靠这里覆盖；同特异性时后写的规则胜出。
    // 特异性注意：子选择器（如 QStatusBar QLabel、QComboBox QAbstractItemView）
    // 高于单一类型选择器，必须单独写；带 #id 的旧规则（如 QDialog#FramelessDialog）
    // 也要用同样的 #id 选择器才能压住
    const QString overrideQss = QStringLiteral(
        "\n/* ---- 动态主题覆盖 [%1] ---- */\n"
        "QMainWindow, QWidget#centralwidget { background-color: %2; color: %12; }\n"
        // 高级参数行仍是玻璃浮层（用户定稿 2026-11-15）：无边框线半透明底，圆角对齐
        // CommandBar；参数栏已嵌入标题栏（透明融入，不吃面板色，底色由 #TitleBar 承载）
        "QFrame#advancedBar { background-color: %34; border: none; border-radius: 8px; color: %12; }\n"
        "QFrame#connectionBar { background: transparent; border: none; }\n"
        "QFrame#terminalPanel { background-color: %33; border: none; color: %12; }\n"
        "CommandBar { background-color: %34; border-radius: 8px; }\n"
        "CommandBar QScrollArea, CommandBar QScrollArea > QWidget > QWidget { background: transparent; border: none; }\n"
        // 命令页列表去卡片化（用户定稿：去边框+透明底，融入毛玻璃卡片整体）；
        // 两级选择器特异性压住全局 QListWidget 规则，::item:hover/selected 仍生效保留行反馈
        "CommandBar QListWidget { background-color: transparent; alternate-background-color: transparent; border: none; }\n"
                // 行容器圆角 8px（用户定稿 2026-11-15）：命令/主题列表行与卡片、输入控件同基准——
                // 两级选择器压住全局 ::item 的 3px，::item:hover/selected 背景仍生效保留行反馈；
                // 主题行 48px、命令行内嵌文本框 ~28px，8px 圆角不药丸化
        "CommandBar QListWidget::item { border-radius: 8px; }\n"
        // MCP 页文本区主题适配（用户定稿 2026-11-14）：全局 QPlainTextEdit/QTextEdit 死色
        // （#202023）不随主题也不融卡片——提示词说明（QTextEdit，富文本布局行距才能生效）
        // 透明融入卡片；配置 JSON 用输入框风格做代码块；:focus 变体压住全局聚焦边框
        "CommandBar QPlainTextEdit, CommandBar QTextEdit, "
        "CommandBar QPlainTextEdit:focus, CommandBar QTextEdit:focus "
        "{ background-color: transparent; border: none; color: %12; }\n"
        "CommandBar QPlainTextEdit#commandBarMcpConfig { background-color: %5; border: 1px solid %6; border-radius: 8px; }\n" // 圆角基准 8px 同 qss（用户定稿 2026-11-15）
        // 三页签选中/hover 态（用户定稿 2026-11-16）：与标题栏收放按钮 hover 同款无边框
        // 圆角色块；%35 专用色（暗色=buttonHoverBg 同源标题栏 hover，亮色=panelBg 派生，
        // 原因见上方 cbTabBg 注释），覆盖全局 checked 蓝底蓝边框
        "CommandBar QToolButton[cbTab=true], CommandBar QToolButton[cbTab=true]:hover, "
        "CommandBar QToolButton[cbTab=true]:checked { border: none; }\n"
        "CommandBar QToolButton[cbTab=true]:hover, CommandBar QToolButton[cbTab=true]:checked "
        "{ background-color: %35; }\n"
        "QLabel { color: %12; }\n"
        "QComboBox, QLineEdit { background-color: %5; border: 1px solid %6; color: %7; }\n"
        "QSpinBox { background-color: %5; border: 1px solid %6; color: %7; }\n"
        "QFrame QComboBox, QDialog QComboBox, QFrame QLineEdit, QDialog QLineEdit { background-color: %5; border: 1px solid %6; color: %7; }\n"
        "QComboBox:focus, QLineEdit:focus, QSpinBox:focus { border-color: %8; background-color: %5; }\n"
        "QComboBox:hover, QLineEdit:hover, QSpinBox:hover { background-color: %5; border-color: %8; }\n"
        "QComboBox:disabled, QLineEdit:disabled, QSpinBox:disabled { background-color: %5; border-color: %6; }\n"
        "QComboBox QAbstractItemView { background-color: %5; color: %7; selection-background-color: %9; selection-color: %26; }\n"
        "QScrollBar::handle:vertical, QScrollBar::handle:horizontal { background-color: %30; }\n"
        "QScrollBar::handle:vertical:hover, QScrollBar::handle:horizontal:hover { background-color: %31; }\n"
        "QScrollBar[flash=\"true\"]:vertical, QScrollBar[flash=\"true\"]:horizontal { background-color: %32; }\n"
        "QDialog { background-color: %13; color: %12; }\n"
        "QDialog#FramelessDialog { background-color: %13; }\n"
        "QMenu { background-color: %13; border: 1px solid %6; }\n"
        "QWidget#TitleBar { background-color: %10; }\n"
        // 标题栏自有按钮主题适配（用户指出：hover/按下/文字色在静态 qss 里硬编码只适配
        // dark/light，第三方主题不变）：静态 qss 已按 objectName 收窄（参数栏嵌入标题栏后
        // 宽泛 #TitleBar QToolButton 规则会吃掉参数栏按钮的 id 样式与选中态），override
        // 必须同构列举；hover/pressed 不含 close（红色 Win 惯例由静态专属规则保留）
        "QWidget#TitleBar QToolButton#TitleStatusBtn, QWidget#TitleBar QToolButton#TitleSidebarBtn,"
        " QWidget#TitleBar QToolButton#TitleMinBtn, QWidget#TitleBar QToolButton#TitleMaxBtn,"
        " QWidget#TitleBar QToolButton#TitleCloseBtn { color: %12; }\n"
        "QWidget#TitleBar QToolButton#TitleStatusBtn:hover, QWidget#TitleBar QToolButton#TitleSidebarBtn:hover,"
        " QWidget#TitleBar QToolButton#TitleMinBtn:hover, QWidget#TitleBar QToolButton#TitleMaxBtn:hover"
        " { background-color: %16; }\n"
        "QWidget#TitleBar QToolButton#TitleStatusBtn:pressed, QWidget#TitleBar QToolButton#TitleSidebarBtn:pressed,"
        " QWidget#TitleBar QToolButton#TitleMinBtn:pressed, QWidget#TitleBar QToolButton#TitleMaxBtn:pressed"
        " { background-color: %18; }\n"
        "QListWidget, QTreeWidget, QTreeView, QTableWidget, QTableView { background-color: %21; alternate-background-color: %22; border: 1px solid %23; gridline-color: %28; color: %12; }\n"
        "QTreeView::item:hover, QTreeWidget::item:hover, QTableView::item:hover, QTableWidget::item:hover, QListWidget::item:hover { background-color: %24; }\n"
        "QTreeView::item:selected, QTreeWidget::item:selected, QTableView::item:selected, QTableWidget::item:selected, QListWidget::item:selected { background-color: %25; color: %26; }\n"
        "QHeaderView::section { background-color: %27; color: %29; border: none; border-right: 1px solid %23; border-bottom: 1px solid %23; }\n"
        "QTableCornerButton::section { background-color: %27; border: none; }\n"
        "QFrame#pathBar { background-color: %5; border: 1px solid %15; }\n"
        "QStatusBar { background-color: %10; border-top: 1px solid %8; }\n"
        "QStatusBar QLabel { color: %11; }\n"
        "QPushButton { background-color: %14; border: 1px solid %15; }\n"
        "QWidget QPushButton { background-color: %14; border: 1px solid %15; }\n"
        "QPushButton:hover { background-color: %16; border-color: %17; }\n"
        "QPushButton:pressed { background-color: %18; }\n"
        "QToolButton#settingsBtn, QToolButton#pwToggleBtn { background-color: %14; border: 1px solid %15; }\n"
        "QToolButton#settingsBtn:hover, QToolButton#pwToggleBtn:hover { background-color: %16; border-color: %17; }\n"
        "QToolButton#settingsBtn:pressed, QToolButton#pwToggleBtn:pressed { background-color: %18; }\n"
        "QToolButton#paneTool { background-color: %14; border: 1px solid %15; }\n"
        "QToolButton#paneTool:hover { background-color: %16; border-color: %17; }\n"
        "QToolButton#paneTool:pressed { background-color: %18; }\n"
        "QToolButton#modeBtn, QToolButton#mcpBtn, QToolButton#keyBrowseBtn { background-color: %14; border: 1px solid %15; }\n"
        "QToolButton#modeBtn:hover, QToolButton#mcpBtn:hover, QToolButton#keyBrowseBtn:hover { background-color: %16; border-color: %17; }\n"
        "QToolButton#modeBtn:pressed, QToolButton#mcpBtn:pressed, QToolButton#keyBrowseBtn:pressed { background-color: %18; }\n"
        "QToolButton { background-color: transparent; border: 1px solid transparent; }\n"
        "QWidget QToolButton { background-color: transparent; border: 1px solid transparent; }\n"
        "QToolButton:hover { background-color: %16; border-color: %17; }\n"
        "QToolButton:pressed { background-color: %18; }\n"
        "QToolButton:checked { background-color: %19; border-color: %20; }\n"
        "QToolButton#modeBtn:checked, QToolButton#mcpBtn:checked { background-color: %19; border-color: %20; }\n"
        "* { selection-background-color: %9; selection-color: %26; }\n"
        // arg 位置守卫：QString::arg 多参等价链式替换「最小编号 marker」，
        // 模板中若删除某个 %n 的使用而参数列表未同步缩短，之后所有参数全部错位。
        // connectionBar 改用 %34 后 %3/%4 暂无消费者，靠注释占位保号（arg 对注释同样替换）
        "/* arg marker guard: %3 %4 */\n"
    ).arg(theme.id,
         theme.windowBg.name(),
         theme.panelBg.name(),
         theme.panelBorder.name(),
         theme.inputBg.name(),
         theme.inputBorder.name(),
         theme.terminalFg.name(),
         theme.accentColor.name(),
         theme.selectionBg.name(),
         theme.statusBarBg.name(),
         theme.statusBarFg.name(),
         theme.textFg.name(),
         theme.dialogBg.name(),
         theme.buttonBg.name(),
         theme.buttonBorder.name(),
         theme.buttonHoverBg.name(),
         theme.buttonHoverBorder.name(),
         theme.buttonPressedBg.name(),
         theme.toolCheckedBg.name(),
         theme.toolCheckedBorder.name(),
         theme.listBg.name(),
         theme.listAltBg.name(),
         theme.listBorder.name(),
         theme.listHoverBg.name(),
         theme.listSelectedBg.name(),
         theme.listSelectedFg.name(),
         theme.headerBg.name(),
         theme.gridlineColor.name(),
         theme.headerFg.name(),
         theme.scrollHandleBg.name(),
         theme.scrollHandleHoverBg.name(),
         trackCss,
         theme.terminalBg.name(),
         overlayCss,
         cbTabBgCss);

    qApp->setStyleSheet(qss + overrideQss);
    FramelessDialog::setThemeHint(theme.isDark);
}
