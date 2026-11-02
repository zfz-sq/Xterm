#ifndef FILEBROWSERWIDGETS_H
#define FILEBROWSERWIDGETS_H

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QEvent>
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QIcon>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPair>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QStringList>
#include <QTimer>
#include <QToolButton>
#include <QVector>
#include <QWidget>

#include <functional>

#include "filebrowsermodel.h"
#include "framelessdialog.h"

class QPlainTextEdit;

/*
 * FileBrowserWidgets: 文件管理器的自绘支撑控件与通用小面板，
 * 从 filebrowserdialog.cpp 拆出。均为纯组装（不新增信号槽，无 Q_OBJECT）。
 */

// ---- 远端权限编辑（chmod）：九宫格 + 特殊位 + 八进制输入联动 ----
class ChmodDialog : public FramelessDialog
{
public:
    ChmodDialog(const QString &titleText, const QString &extra, quint32 mode, QWidget *parent)
        : FramelessDialog(parent)
        , m_mode(mode & 07777)
    {
        setDialogTitle(tr("属性"));
        resize(380, 320);
        const quint32 bits[9] = { 0400, 0200, 0100, 0040, 0020, 0010, 0004, 0002, 0001 };
        auto *grid = new QGridLayout;
        grid->addWidget(new QLabel(tr("读取")), 0, 1, Qt::AlignCenter);
        grid->addWidget(new QLabel(tr("写入")), 0, 2, Qt::AlignCenter);
        grid->addWidget(new QLabel(tr("执行")), 0, 3, Qt::AlignCenter);
        const QStringList rows = { tr("所有者"), tr("用户组"), tr("其他人") };
        for (int r = 0; r < 3; ++r) {
            grid->addWidget(new QLabel(rows.at(r)), r + 1, 0);
            for (int c = 0; c < 3; ++c) {
                auto *box = new QCheckBox;
                const quint32 bit = bits[r * 3 + c];
                box->setChecked(m_mode & bit);
                m_bits.append({ bit, box });
                grid->addWidget(box, r + 1, c + 1, Qt::AlignCenter);
                connect(box, &QCheckBox::toggled, this, [this] { syncFromChecks(); });
            }
        }
        m_setuid = new QCheckBox(tr("setuid"));
        m_setgid = new QCheckBox(tr("setgid"));
        m_sticky = new QCheckBox(tr("sticky"));
        m_setuid->setChecked(m_mode & 04000);
        m_setgid->setChecked(m_mode & 02000);
        m_sticky->setChecked(m_mode & 01000);
        for (QCheckBox *box : { m_setuid, m_setgid, m_sticky })
            connect(box, &QCheckBox::toggled, this, [this] { syncFromChecks(); });

        m_octal = new QLineEdit;
        m_octal->setText(QStringLiteral("%1").arg(m_mode, 4, 8, QLatin1Char('0')));
        connect(m_octal, &QLineEdit::editingFinished, this, [this] { syncFromOctal(); });

        auto *root = new QVBoxLayout;
        root->addWidget(new QLabel(titleText));
        if (!extra.isEmpty()) {
            auto *detail = new QLabel(extra);
            detail->setWordWrap(true);
            root->addWidget(detail);
        }
        root->addSpacing(6);
        root->addLayout(grid);
        auto *special = new QHBoxLayout;
        special->addWidget(m_setuid);
        special->addWidget(m_setgid);
        special->addWidget(m_sticky);
        special->addStretch(1);
        root->addLayout(special);
        auto *oct = new QHBoxLayout;
        oct->addWidget(new QLabel(tr("八进制")));
        oct->addWidget(m_octal, 1);
        root->addLayout(oct);
        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
        connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        root->addWidget(buttons);
        setContentLayout(root);
    }

    quint32 mode() const { return m_mode & 07777; }

private:
    struct Bit
    {
        quint32 bit;
        QCheckBox *box;
    };

    void syncFromChecks()
    {
        if (m_guard)
            return;
        quint32 mode = 0;
        for (const Bit &bit : m_bits) {
            if (bit.box->isChecked())
                mode |= bit.bit;
        }
        if (m_setuid->isChecked())
            mode |= 04000;
        if (m_setgid->isChecked())
            mode |= 02000;
        if (m_sticky->isChecked())
            mode |= 01000;
        m_mode = mode;
        m_guard = true;
        m_octal->setText(QStringLiteral("%1").arg(m_mode, 4, 8, QLatin1Char('0')));
        m_guard = false;
    }

    void syncFromOctal()
    {
        if (m_guard)
            return;
        bool ok = false;
        const uint value = m_octal->text().trimmed().toUInt(&ok, 8);
        if (!ok)
            return;
        m_mode = quint32(value) & 07777;
        m_guard = true;
        for (const Bit &bit : m_bits)
            bit.box->setChecked(m_mode & bit.bit);
        m_setuid->setChecked(m_mode & 04000);
        m_setgid->setChecked(m_mode & 02000);
        m_sticky->setChecked(m_mode & 01000);
        m_guard = false;
    }

    QVector<Bit> m_bits;
    QCheckBox *m_setuid = nullptr;
    QCheckBox *m_setgid = nullptr;
    QCheckBox *m_sticky = nullptr;
    QLineEdit *m_octal = nullptr;
    quint32 m_mode = 0;
    bool m_guard = false;
};

// ---- 本地文件属性：只读/隐藏（Windows）----
class LocalPropsDialog : public FramelessDialog
{
public:
    explicit LocalPropsDialog(const QString &path, QWidget *parent);
    void accept() override;

private:
    void apply();

    QString m_path;
    QCheckBox *m_readonly = nullptr;
    QCheckBox *m_hidden = nullptr;
};


// ---- 文件自定义命令管理（增删改列表）----
class CommandManagerDialog : public FramelessDialog
{
public:
    explicit CommandManagerDialog(const QList<FileCommand> &initial, QWidget *parent)
        : FramelessDialog(parent)
        , m_commands(initial)
    {
        setDialogTitle(tr("文件自定义命令"));
        resize(620, 380);
        m_list = new QListWidget(this);
        for (const FileCommand &command : m_commands)
            m_list->addItem(command.name);
        m_name = new QLineEdit(this);
        m_command = new QLineEdit(this);
        m_command->setPlaceholderText(QStringLiteral("gzip -k !"));
        m_remote = new QCheckBox(tr("在远程执行"), this);
        auto *addBtn = new QPushButton(tr("添加"), this);
        auto *delBtn = new QPushButton(tr("删除"), this);
        connect(addBtn, &QPushButton::clicked, this, [this] { addCommand(); });
        connect(delBtn, &QPushButton::clicked, this, [this] { removeCommand(); });
        connect(m_list, &QListWidget::currentRowChanged, this, [this](int row) { loadRow(row); });
        connect(m_name, &QLineEdit::textEdited, this, [this] { storeRow(); });
        connect(m_command, &QLineEdit::textEdited, this, [this] { storeRow(); });
        connect(m_remote, &QCheckBox::toggled, this, [this](bool) { storeRow(); });

        auto *fields = new QFormLayout;
        fields->addRow(tr("名称"), m_name);
        fields->addRow(tr("命令"), m_command);
        fields->addRow(QString(), m_remote);
        auto *help = new QLabel(tr("占位符：! 当前文件，!& 全部选中，!/ 当前目录。路径会自动加引号。\n"
                                   "含 ! 时对每个选中文件各执行一次。远程命令在服务器 shell 执行，"
                                   "本地命令通过 cmd /c 执行。"),
                                this);
        help->setWordWrap(true);
        auto *sideBtns = new QHBoxLayout;
        sideBtns->addWidget(addBtn);
        sideBtns->addWidget(delBtn);
        sideBtns->addStretch(1);
        auto *left = new QVBoxLayout;
        left->addWidget(m_list, 1);
        left->addLayout(sideBtns);
        auto *right = new QVBoxLayout;
        right->addLayout(fields);
        right->addWidget(help);
        right->addStretch(1);
        auto *columns = new QHBoxLayout;
        columns->addLayout(left, 1);
        columns->addLayout(right, 2);
        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
        connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        auto *root = new QVBoxLayout;
        root->addLayout(columns, 1);
        root->addWidget(buttons);
        setContentLayout(root);
        if (m_list->count() > 0)
            m_list->setCurrentRow(0);
        else
            setFieldsEnabled(false);
    }

    QList<FileCommand> result() const { return m_commands; }

private:
    void setFieldsEnabled(bool on)
    {
        m_name->setEnabled(on);
        m_command->setEnabled(on);
        m_remote->setEnabled(on);
    }

    void loadRow(int row)
    {
        m_guard = true;
        m_row = row;
        if (row < 0 || row >= m_commands.size()) {
            m_name->clear();
            m_command->clear();
            m_remote->setChecked(true);
            setFieldsEnabled(false);
            m_guard = false;
            return;
        }
        setFieldsEnabled(true);
        const FileCommand &command = m_commands.at(row);
        m_name->setText(command.name);
        m_command->setText(command.command);
        m_remote->setChecked(command.remote);
        m_guard = false;
    }

    void storeRow()
    {
        if (m_guard || m_row < 0 || m_row >= m_commands.size())
            return;
        FileCommand &command = m_commands[m_row];
        command.name = m_name->text();
        command.command = m_command->text();
        command.remote = m_remote->isChecked();
        if (QListWidgetItem *item = m_list->item(m_row))
            item->setText(command.name.isEmpty() ? tr("(未命名)") : command.name);
    }

    void addCommand()
    {
        m_commands.append(FileCommand{ tr("新命令"), QString(), true });
        m_list->addItem(tr("新命令"));
        m_list->setCurrentRow(m_list->count() - 1);
    }

    void removeCommand()
    {
        const int row = m_list->currentRow();
        if (row < 0)
            return;
        m_guard = true;
        m_commands.removeAt(row);
        delete m_list->takeItem(row);
        m_guard = false;
        loadRow(m_list->currentRow());
    }

    QList<FileCommand> m_commands;
    QListWidget *m_list = nullptr;
    QLineEdit *m_name = nullptr;
    QLineEdit *m_command = nullptr;
    QCheckBox *m_remote = nullptr;
    int m_row = -1;
    bool m_guard = false;
};


// ---- 路径栏：每一段目录名可点，跳到那一级。右侧按钮或双击空白处改成手动输入。----
class PathBar : public QFrame
{
public:
    explicit PathBar(QWidget *parent = nullptr)
        : QFrame(parent)
    {
        setObjectName(QStringLiteral("pathBar"));
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        setFixedHeight(28);

        m_scroll = new QScrollArea(this);
        m_scroll->setFrameShape(QFrame::NoFrame);
        m_scroll->setWidgetResizable(false);
        m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        m_scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        m_scroll->viewport()->setAutoFillBackground(false);

        m_host = new QWidget;
        m_host->setObjectName(QStringLiteral("pathCrumbs"));
        m_host->setAutoFillBackground(false);
        m_layout = new QHBoxLayout(m_host);
        m_layout->setContentsMargins(4, 0, 4, 0);
        m_layout->setSpacing(0);
        m_scroll->setWidget(m_host);
        m_host->installEventFilter(this);
        m_scroll->viewport()->installEventFilter(this);

        m_edit = new QLineEdit(this);
        m_edit->setObjectName(QStringLiteral("pathEdit"));
        m_edit->hide();
        m_edit->installEventFilter(this);

        m_editBtn = new QToolButton(this);
        m_editBtn->setObjectName(QStringLiteral("pathEditBtn"));
        m_editBtn->setToolTip(tr("输入路径"));
        m_editBtn->setFocusPolicy(Qt::NoFocus);
        m_editBtn->setAutoRaise(true);
        connect(m_editBtn, &QToolButton::clicked, this, [this] { showEdit(); });

        auto *row = new QHBoxLayout(this);
        row->setContentsMargins(0, 0, 2, 0);
        row->setSpacing(0);
        row->addWidget(m_scroll, 1);
        row->addWidget(m_edit, 1);
        row->addWidget(m_editBtn);
    }

    void setEditIcon(const QIcon &icon)
    {
        m_editBtn->setIcon(icon);
        m_editBtn->setIconSize(QSize(14, 14));
        m_editBtn->setText(QString());
    }

    void setSegments(const QVector<QPair<QString, QString>> &segments, const QString &editText,
                     const QString &separator)
    {
        m_segments = segments;
        m_editText = editText;
        m_separator = separator;
        rebuild();
        showCrumbs();
    }

    QString submittedText() const { return m_submitted; }

    std::function<void(const QString &)> onJump;
    std::function<void(const QString &)> onSubmit;

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if ((watched == m_host || watched == m_scroll->viewport())
            && event->type() == QEvent::MouseButtonDblClick) {
            showEdit();
            return true;
        }
        if (watched == m_edit && event->type() == QEvent::KeyPress) {
            const auto *key = static_cast<const QKeyEvent *>(event);
            if (key->key() == Qt::Key_Escape) {
                showCrumbs();
                return true;
            }
            // QLineEdit 会把回车交给对话框的默认按钮。这里吃掉，避免打开传输队列。
            if (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) {
                commitEdit();
                return true;
            }
        }
        if (event->type() == QEvent::FocusOut && m_editing) {
            // 焦点去了别的窗口（比如编辑器）时不要立刻收起输入框，
            // 等回到本控件再切回面包屑。
            QTimer::singleShot(0, this, [this] {
                if (m_editing && !m_edit->hasFocus())
                    showCrumbs();
            });
        }
        return QFrame::eventFilter(watched, event);
    }
    void showEdit()
    {
        m_editing = true;
        m_scroll->hide();
        m_edit->setText(m_editText);
        m_edit->show();
        m_edit->setFocus();
        m_edit->selectAll();
    }

    void showCrumbs()
    {
        m_editing = false;
        m_edit->hide();
        m_scroll->show();
        fitCrumbs();
    }

    void commitEdit()
    {
        m_submitted = m_edit->text();
        showCrumbs();
        if (onSubmit)
            onSubmit(m_submitted);
    }

    void rebuild()
    {
        while (QLayoutItem *item = m_layout->takeAt(0)) {
            delete item->widget();
            delete item;
        }
        for (int i = 0; i < m_segments.size(); ++i) {
            if (i > 0) {
                const QString previous = m_segments.at(i - 1).first;
                QString sepText = m_separator;
                if (previous == QStringLiteral("/"))
                    sepText.clear();
                else if (m_segments.at(i - 1).second.isEmpty())
                    sepText = QStringLiteral(" ");
                if (!sepText.isEmpty()) {
                    auto *sep = new QLabel(sepText, m_host);
                    sep->setObjectName(QStringLiteral("pathSep"));
                    m_layout->addWidget(sep);
                }
            }
            auto *button = new QToolButton(m_host);
            button->setObjectName(QStringLiteral("pathCrumb"));
            button->setText(m_segments.at(i).first);
            button->setToolTip(m_segments.at(i).second.isEmpty()
                                   ? m_segments.at(i).first
                                   : QDir::toNativeSeparators(m_segments.at(i).second));
            button->setAutoRaise(true);
            button->setFocusPolicy(Qt::NoFocus);
            button->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Preferred);
            const QString target = m_segments.at(i).second;
            connect(button, &QToolButton::clicked, this, [this, target] {
                if (onJump)
                    onJump(target);
            });
            m_layout->addWidget(button);
        }
        m_layout->addStretch(1);
        fitCrumbs();
    }

    void fitCrumbs()
    {
        m_host->adjustSize();
        const int width = qMax(m_host->sizeHint().width(), m_scroll->viewport()->width());
        const int height = qMax(m_scroll->viewport()->height(), 24);
        m_host->resize(width, height);
        QTimer::singleShot(0, m_scroll, [this] {
            m_scroll->horizontalScrollBar()->setValue(m_scroll->horizontalScrollBar()->maximum());
        });
    }

    QScrollArea *m_scroll = nullptr;
    QWidget *m_host = nullptr;
    QHBoxLayout *m_layout = nullptr;
    QLineEdit *m_edit = nullptr;
    QToolButton *m_editBtn = nullptr;
    QVector<QPair<QString, QString>> m_segments;
    QString m_separator = QStringLiteral("/");
    QString m_editText;
    QString m_submitted;
    bool m_editing = false;
};

// ---- 通用小件 ----
QFrame *vsep(QWidget *parent);
// 命令输出弹窗（无边框、只读、自动删除）
void showCommandOutput(QWidget *parent, const QString &title, const QString &text);
#ifdef Q_OS_WIN
// Windows 资源管理器原生右键菜单；hwnd 为调用窗口
bool showWindowsShellMenu(const QStringList &paths, WId hwnd);
#endif

#endif // FILEBROWSERWIDGETS_H
