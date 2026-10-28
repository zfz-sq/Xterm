#ifndef FRAMELESSDIALOG_H
#define FRAMELESSDIALOG_H

#include <QDialog>

class CaptionButton;
class QLabel;
class QLayout;
class QWidget;

/*
 * 公共主题对话框：无边框 + 自绘标题栏（标题 + 关闭），跟随全局 QSS 换肤，
 * 暗色主题不再露出不适配的原生标题栏。标题栏控件复用主窗口 TitleBar 的
 * objectName（#TitleBar/#TitleLabel/#TitleCloseBtn），样式一处维护两处生效。
 * 关闭按钮走 close()（模态结束 exec、非模态触发 WA_DeleteOnClose），Esc 由
 * QDialog 处理；标题栏可拖动移动窗口。
 * message()/question() 是 QMessageBox::information/warning/question 的替身，
 * 供各处弹窗统一换肤用。
 */
class FramelessDialog : public QDialog
{
    Q_OBJECT

public:
    explicit FramelessDialog(QWidget *parent = nullptr);

    void setDialogTitle(const QString &title);
    // 内容区布局交进来（边距由调用方定）；重复调用会替换旧内容
    void setContentLayout(QLayout *layout);
    // 标题栏按钮字形深浅，与 MainWindow::applyTheme 联动
    void applyTheme(bool dark);
    // 全局主题提示：主窗口 applyTheme 时同步一次，之后新建的实例自动取用
    // （背景/标题栏底色走全局 QSS 自动生效，只有自画字形色需要这个布尔）
    static void setThemeHint(bool dark) { s_darkHint = dark; }

    // 信息/警告弹窗（一个「确定」按钮），exec 模态
    static void message(QWidget *parent, const QString &title, const QString &text);
    // 询问弹窗，返回是否点了 acceptText（默认「是 / 否」），exec 模态
    static bool question(QWidget *parent, const QString &title, const QString &text,
                         const QString &acceptText = QString(),
                         const QString &rejectText = QString());
    // 单行文本输入（QInputDialog::getText 的替身），ok=false 表示取消/Esc/关闭
    static QString getText(QWidget *parent, const QString &title, const QString &label,
                           const QString &initial = QString(), bool *ok = nullptr);
    // 取色弹窗（QColorDialog::getColor 的主题化替身）：标准色板 + 十六进制自选。
    // 取消 / Esc / 关闭返回无效 QColor。
    static QColor getColor(QWidget *parent, const QColor &initial, const QString &title);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    // message/question 共用的组装：按钮文案对应返回下标（-1 = Esc/关闭）
    static int runMessageBox(QWidget *parent, const QString &title, const QString &text,
                             const QStringList &buttons);

    QWidget *m_bar = nullptr;
    QLabel *m_title = nullptr;
    CaptionButton *m_close = nullptr;
    QWidget *m_body = nullptr;
    bool m_dragging = false;
    QPoint m_dragOrigin;
    static bool s_darkHint;
};

#endif // FRAMELESSDIALOG_H
