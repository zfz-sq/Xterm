#pragma once

#include <QObject>

class QScrollBar;
class QWidget;

// Termius 式滚动条显隐：待机（flash=false）时滑块透明且鼠标穿透——视觉上不存在，
// 点击不拦截；用户滚动时浮现「轨道 + 滑块」，无操作 2s 后隐去；内容不足一屏
// （maximum<=0）不浮现，滚动中 range 收缩到不可滚立即隐去；悬停滑块变色（主题注入
// 常态/悬停两色）即吸附提示。
//
// 浮现触发源（用户定稿 2026-11-14）：仅鼠标滚轮与鼠标从窗口外移入视图——由宿主在
// 对应事件里调 noteUserActivity 打开「用户滚动门」。日志信息流等程序化滚动
// （syncBar 的 setValue）虽也走 valueChanged，但门未开不浮现，避免输出刷屏时
// 滚动条常驻闪烁。鼠标移出应用窗口时由宿主调 noteMouseLeft 立即隐去。
//
// 机制：dynamic property "flash" + QSS 属性选择器驱动，unpolish/polish 刷新；
// widget 始终可见（不 setVisible），只切换绘制透明度——零布局回流（reflow）。
// 滑块长度由 QSS 钳制在 [48px, 100px]，内容多少都不致过短或占满全条。
//
// 渐进增强：未 attach 的滚动条不带 flash 属性、不匹配 [flash="false"]，保持常显。
namespace ScrollFlash {

// 接管单条滚动条（挂为 bar 的子对象，随 bar 生命周期释放）
void attach(QScrollBar *bar);

// 接管 root 子树下当前已存在的全部滚动条（动态后建的需再次调用）
void attachTree(QWidget *root);

// 用户滚动意图：滚轮滚动 / 鼠标移入视图时由宿主调用。打开用户滚动门并浮现
// （内容不足一屏时内部自动钳为隐藏）；门随 2s 无操作超时关闭。
void noteUserActivity(QScrollBar *bar);

// 鼠标移出应用窗口（用户定稿 2026-11-14）：立即隐去并关门，不等 2s 待机。
// 用户正按住滑块拖拽时不打断（isSliderDown 保护），松手后由 2s 待机接管。
void noteMouseLeft(QScrollBar *bar);

} // namespace ScrollFlash
