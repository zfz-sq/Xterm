#include "framelessdialog.h"

#include <QEvent>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPushButton>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWindow>

#include <iterator> // std::size（色板数组）

#include "titlebar.h"

namespace {
constexpr int kBarHeight = 30; // 与主窗口 TitleBar 同高，视觉上同一根栏
}

bool FramelessDialog::s_darkHint = false;

FramelessDialog::FramelessDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowFlags(windowFlags() | Qt::FramelessWindowHint);
    applyWin11RoundedCorners(this);
    setObjectName(QStringLiteral("FramelessDialog"));

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // 标题栏：objectName 对齐 TitleBar，QSS 里同一组规则直接命中
    m_bar = new QWidget(this);
    m_bar->setObjectName(QStringLiteral("TitleBar"));
    m_bar->setFixedHeight(kBarHeight);
    m_bar->installEventFilter(this);

    auto *barLay = new QHBoxLayout(m_bar);
    barLay->setContentsMargins(0, 0, 0, 0);
    barLay->setSpacing(0);
    m_title = new QLabel(m_bar);
    m_title->setObjectName(QStringLiteral("TitleLabel"));
    barLay->addWidget(m_title);
    barLay->addStretch(1);
    m_close = new CaptionButton(CaptionButton::Close, m_bar);
    m_close->setObjectName(QStringLiteral("TitleCloseBtn"));
    m_close->setToolTip(tr("关闭"));
    m_close->setGlyphColor(CaptionButton::captionForeground(s_darkHint));
    // close() 对两种用法都正确：模态 exec 会像 reject 一样结束循环，
    // 非模态配合 WA_DeleteOnClose 还能正常销毁（reject 只隐藏不删）
    connect(m_close, &QToolButton::clicked, this, &FramelessDialog::close);
    barLay->addWidget(m_close);
    root->addWidget(m_bar);
}

void FramelessDialog::setDialogTitle(const QString &title)
{
    m_title->setText(title);
    setWindowTitle(title); // 任务栏 / Alt+Tab 也用得上
}

void FramelessDialog::setContentLayout(QLayout *content)
{
    delete m_body; // 幂等：重复调用替换旧内容
    m_body = new QWidget(this);
    m_body->setLayout(content);
    if (auto *root = qobject_cast<QVBoxLayout *>(layout()))
        root->addWidget(m_body);
}

void FramelessDialog::applyTheme(bool dark)
{
    setThemeHint(dark);
    m_close->setGlyphColor(CaptionButton::captionForeground(dark));
}

int FramelessDialog::runMessageBox(QWidget *parent, const QString &title,
                                   const QString &text, const QStringList &buttons)
{
    FramelessDialog dlg(parent);
    dlg.setDialogTitle(title);
    auto *content = new QWidget;
    auto *lay = new QVBoxLayout(content);
    lay->setContentsMargins(20, 16, 20, 14);
    auto *label = new QLabel(text);
    label->setWordWrap(true);
    lay->addWidget(label);
    lay->addSpacing(10);
    auto *row = new QHBoxLayout;
    row->addStretch(1);
    for (int i = 0; i < buttons.size(); ++i) {
        auto *btn = new QPushButton(buttons.at(i));
        if (i == 0)
            btn->setDefault(true); // 回车 = 第一个按钮
        const int index = i;
        connect(btn, &QPushButton::clicked, &dlg, [&, index] {
            dlg.done(index + 1); // 0 留给 Esc/关闭
        });
        row->addWidget(btn);
    }
    lay->addLayout(row);
    dlg.setContentLayout(lay);
    return dlg.exec();
}

void FramelessDialog::message(QWidget *parent, const QString &title, const QString &text)
{
    runMessageBox(parent, title, text, { tr("确定") });
}

bool FramelessDialog::question(QWidget *parent, const QString &title, const QString &text,
                               const QString &acceptText, const QString &rejectText)
{
    return runMessageBox(parent, title, text,
                         { acceptText.isEmpty() ? tr("是") : acceptText,
                           rejectText.isEmpty() ? tr("否") : rejectText }) == 1;
}

QString FramelessDialog::getText(QWidget *parent, const QString &title, const QString &label,
                                 const QString &initial, bool *ok)
{
    FramelessDialog dlg(parent);
    dlg.setDialogTitle(title);
    auto *content = new QWidget;
    auto *lay = new QVBoxLayout(content);
    lay->setContentsMargins(20, 16, 20, 14);
    lay->addWidget(new QLabel(label));
    auto *edit = new QLineEdit(initial);
    edit->selectAll(); // 回车直接收下默认值，符合 QInputDialog 的习惯
    lay->addWidget(edit);
    lay->addSpacing(10);
    auto *row = new QHBoxLayout;
    row->addStretch(1);
    auto *okBtn = new QPushButton(tr("确定"));
    okBtn->setDefault(true);
    auto *cancelBtn = new QPushButton(tr("取消"));
    connect(okBtn, &QPushButton::clicked, &dlg, &QDialog::accept);
    connect(cancelBtn, &QPushButton::clicked, &dlg, &QDialog::reject);
    row->addWidget(okBtn);
    row->addWidget(cancelBtn);
    lay->addLayout(row);
    dlg.setContentLayout(lay);
    const bool accepted = dlg.exec() == QDialog::Accepted;
    if (ok)
        *ok = accepted;
    return edit->text();
}

QColor FramelessDialog::getColor(QWidget *parent, const QColor &initial, const QString &title)
{
    FramelessDialog dlg(parent);
    dlg.setDialogTitle(title);

    auto *content = new QWidget;
    auto *lay = new QVBoxLayout(content);
    lay->setContentsMargins(16, 12, 16, 12);
    lay->setSpacing(10);

    QColor current = initial.isValid() ? initial : QColor(0xE0, 0x6C, 0x75);

    // 标准色板：7 行 × 8 列（灰阶 + 红黄绿青蓝紫，每行由浅到深）
    static const char *kPalette[] = {
        "#FFFFFF", "#E0E0E0", "#BDBDBD", "#9E9E9E", "#757575", "#616161", "#424242", "#000000",
        "#FFEBEE", "#FFCDD2", "#EF9A9A", "#E57373", "#EF5350", "#F44336", "#D32F2F", "#B71C1C",
        "#FFFDE7", "#FFF9C4", "#FFF59D", "#FFEE58", "#FFEB3B", "#FDD835", "#FBC02D", "#F57F17",
        "#E8F5E9", "#C8E6C9", "#A5D6A7", "#81C784", "#66BB6A", "#4CAF50", "#388E3C", "#1B5E20",
        "#E0F7FA", "#B2EBF2", "#80DEEA", "#4DD0E1", "#26C6DA", "#00BCD4", "#0097A7", "#006064",
        "#E3F2FD", "#BBDEFB", "#90CAF9", "#64B5F6", "#42A5F5", "#2196F3", "#1976D2", "#0D47A1",
        "#F3E5F5", "#E1BEE7", "#CE93D8", "#BA68C8", "#AB47BC", "#9C27B0", "#7B1FA2", "#4A148C",
    };

    // 自定义 Hex 输入 + 实时预览（预览块上直接显示色值文字）
    auto *preview = new QPushButton;
    preview->setFixedHeight(26);
    preview->setFlat(true);
    auto *hexEdit = new QLineEdit;
    hexEdit->setFixedWidth(90);
    auto syncPreview = [&](const QColor &c) {
        const QString textColor =
            c.lightness() > 128 ? QStringLiteral("#1E1E1E") : QStringLiteral("#FFFFFF");
        preview->setStyleSheet(QStringLiteral(
                                   "QPushButton { background-color: %1; color: %2;"
                                   " border: 1px solid rgba(128,128,128,110); border-radius: 3px; }")
                                   .arg(c.name(), textColor));
        preview->setText(c.name());
    };
    syncPreview(current);
    hexEdit->setText(current.name());
    QObject::connect(hexEdit, &QLineEdit::textChanged, &dlg, [&](const QString &text) {
        const QColor c(text);
        if (!c.isValid())
            return;
        current = c;
        syncPreview(c);
    });

    auto *grid = new QGridLayout;
    grid->setSpacing(4);
    for (int i = 0; i < int(std::size(kPalette)); ++i) {
        const QColor c = QColor(QLatin1String(kPalette[i]));
        auto *btn = new QPushButton;
        btn->setFixedSize(28, 22);
        btn->setToolTip(c.name());
        btn->setStyleSheet(QStringLiteral(
                               "QPushButton { background-color: %1;"
                               " border: 1px solid rgba(128,128,128,110); border-radius: 2px; }"
                               "QPushButton:hover { border: 2px solid %2; }")
                               .arg(c.name(), s_darkHint ? QStringLiteral("#FFFFFF")
                                                         : QStringLiteral("#1E1E1E")));
        QObject::connect(btn, &QPushButton::clicked, &dlg, [&, c] {
            current = c;
            syncPreview(c);
            hexEdit->setText(c.name()); // 触发 textChanged，已是同一颜色
        });
        grid->addWidget(btn, i / 8, i % 8);
    }

    auto *customRow = new QHBoxLayout;
    customRow->addWidget(preview, 1);
    customRow->addWidget(new QLabel(QStringLiteral("Hex")));
    customRow->addWidget(hexEdit);

    auto *row = new QHBoxLayout;
    row->addStretch(1);
    auto *okBtn = new QPushButton(QObject::tr("确定"));
    okBtn->setDefault(true);
    auto *cancelBtn = new QPushButton(QObject::tr("取消"));
    QObject::connect(okBtn, &QPushButton::clicked, &dlg, &QDialog::accept);
    QObject::connect(cancelBtn, &QPushButton::clicked, &dlg, &QDialog::reject);
    row->addWidget(okBtn);
    row->addWidget(cancelBtn);

    lay->addLayout(grid);
    lay->addLayout(customRow);
    lay->addLayout(row);
    dlg.setContentLayout(lay);
    return dlg.exec() == QDialog::Accepted ? current : QColor();
}

bool FramelessDialog::eventFilter(QObject *watched, QEvent *event)
{
    // 拖动标题栏移动窗口（优先系统拖动，手动兜底），同 TitleBar 的做法
    if (watched == m_bar) {
        switch (event->type()) {
        case QEvent::MouseButtonPress: {
            auto *me = static_cast<QMouseEvent *>(event);
            if (me->button() == Qt::LeftButton) {
                m_dragging = true;
                m_dragOrigin = me->globalPosition().toPoint() - frameGeometry().topLeft();
            }
            break;
        }
        case QEvent::MouseMove: {
            auto *me = static_cast<QMouseEvent *>(event);
            if (m_dragging && (me->buttons() & Qt::LeftButton)) {
                if (windowHandle() && windowHandle()->startSystemMove()) {
                    m_dragging = false; // 系统接管，后续 move 事件不再到这
                    return true;
                }
                move(me->globalPosition().toPoint() - m_dragOrigin);
                return true;
            }
            break;
        }
        case QEvent::MouseButtonRelease:
            m_dragging = false;
            break;
        default:
            break;
        }
    }
    return QDialog::eventFilter(watched, event);
}
