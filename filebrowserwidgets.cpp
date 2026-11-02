#include "filebrowserwidgets.h"

#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#endif

QFrame *vsep(QWidget *parent)
{
    auto *line = new QFrame(parent);
    line->setFrameShape(QFrame::VLine);
    line->setFrameShadow(QFrame::Sunken);
    return line;
}

void showCommandOutput(QWidget *parent, const QString &title, const QString &text)
{
    auto *dlg = new FramelessDialog(parent);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->setDialogTitle(title);
    dlg->resize(640, 380);
    auto *edit = new QPlainTextEdit(dlg);
    edit->setReadOnly(true);
    edit->setPlainText(text);
    auto *closeBtn = new QPushButton(QObject::tr("关闭"), dlg);
    QObject::connect(closeBtn, &QPushButton::clicked, dlg, &QDialog::close);
    auto *lay = new QVBoxLayout;
    lay->addWidget(edit, 1);
    auto *row = new QHBoxLayout;
    row->addStretch(1);
    row->addWidget(closeBtn);
    lay->addLayout(row);
    dlg->setContentLayout(lay);
    dlg->show();
}

LocalPropsDialog::LocalPropsDialog(const QString &path, QWidget *parent)
    : FramelessDialog(parent)
    , m_path(path)
{
    const QFileInfo info(path);
    setDialogTitle(tr("属性"));
    auto *form = new QFormLayout;
    form->addRow(tr("名称"), new QLabel(info.fileName()));
    form->addRow(tr("位置"), new QLabel(QDir::toNativeSeparators(info.absolutePath())));
    form->addRow(tr("类型"), new QLabel(info.isDir() ? tr("目录") : tr("文件")));
    if (!info.isDir())
        form->addRow(tr("大小"), new QLabel(humanSize(quint64(info.size()))));
    form->addRow(tr("修改时间"),
                 new QLabel(info.lastModified().toString(QStringLiteral("yyyy-MM-dd hh:mm:ss"))));
    m_readonly = new QCheckBox(tr("只读"));
    m_readonly->setChecked(!(info.permissions() & QFile::WriteUser));
#ifdef Q_OS_WIN
    const QString native = QDir::toNativeSeparators(path);
    const DWORD attr = GetFileAttributesW(reinterpret_cast<LPCWSTR>(native.utf16()));
    m_hidden = new QCheckBox(tr("隐藏"));
    m_hidden->setChecked(attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_HIDDEN));
#endif
    auto *root = new QVBoxLayout;
    root->addLayout(form);
    root->addWidget(m_readonly);
#ifdef Q_OS_WIN
    root->addWidget(m_hidden);
#endif
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    root->addWidget(buttons);
    setContentLayout(root);
    resize(420, 220);
}

void LocalPropsDialog::accept()
{
    apply();
    QDialog::accept();
}

void LocalPropsDialog::apply()
{
    QFile::Permissions perm = QFile::permissions(m_path);
    if (m_readonly->isChecked())
        perm &= ~(QFile::WriteUser | QFile::WriteGroup | QFile::WriteOther);
    else
        perm |= QFile::WriteUser;
    QFile::setPermissions(m_path, perm);
#ifdef Q_OS_WIN
    const QString native = QDir::toNativeSeparators(m_path);
    DWORD attr = GetFileAttributesW(reinterpret_cast<LPCWSTR>(native.utf16()));
    if (m_hidden && attr != INVALID_FILE_ATTRIBUTES) {
        if (m_hidden->isChecked())
            attr |= FILE_ATTRIBUTE_HIDDEN;
        else
            attr &= ~DWORD(FILE_ATTRIBUTE_HIDDEN);
        SetFileAttributesW(reinterpret_cast<LPCWSTR>(native.utf16()), attr);
    }
#endif
}


#ifdef Q_OS_WIN
bool showWindowsShellMenu(const QStringList &paths, WId hwnd)
{
    if (paths.isEmpty())
        return false;
    QVector<PIDLIST_ABSOLUTE> pidls;
    for (const QString &path : paths) {
        PIDLIST_ABSOLUTE pidl = nullptr;
        if (SUCCEEDED(SHParseDisplayName(reinterpret_cast<PCWSTR>(path.utf16()), nullptr, &pidl, 0, nullptr))
            && pidl) {
            pidls.append(pidl);
        }
    }
    if (pidls.isEmpty())
        return false;

    IShellFolder *folder = nullptr;
    LPCITEMIDLIST child = nullptr;
    HRESULT hr = SHBindToParent(pidls.first(), IID_IShellFolder, reinterpret_cast<void **>(&folder), &child);
    if (FAILED(hr) || !folder) {
        for (auto *pidl : pidls)
            CoTaskMemFree(pidl);
        return false;
    }

    QVector<LPCITEMIDLIST> children;
    children.reserve(pidls.size());
    for (auto *pidl : pidls)
        children.append(ILFindLastID(pidl));

    IContextMenu *menu = nullptr;
    hr = folder->GetUIObjectOf(reinterpret_cast<HWND>(hwnd), UINT(children.size()),
                               const_cast<LPCITEMIDLIST *>(children.constData()),
                               IID_IContextMenu, nullptr, reinterpret_cast<void **>(&menu));
    if (FAILED(hr) || !menu) {
        folder->Release();
        for (auto *pidl : pidls)
            CoTaskMemFree(pidl);
        return false;
    }

    HMENU hmenu = CreatePopupMenu();
    if (!hmenu) {
        menu->Release();
        folder->Release();
        for (auto *pidl : pidls)
            CoTaskMemFree(pidl);
        return false;
    }
    menu->QueryContextMenu(hmenu, 0, 1, 0x7FFF, CMF_NORMAL | CMF_EXPLORE);
    POINT pt;
    GetCursorPos(&pt);
    const int cmd = int(TrackPopupMenuEx(hmenu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
                                          pt.x, pt.y, reinterpret_cast<HWND>(hwnd), nullptr));
    if (cmd > 0) {
        CMINVOKECOMMANDINFOEX info{};
        info.cbSize = sizeof(info);
        info.fMask = CMIC_MASK_UNICODE;
        info.hwnd = reinterpret_cast<HWND>(hwnd);
        info.lpVerb = MAKEINTRESOURCEA(cmd - 1);
        info.lpVerbW = MAKEINTRESOURCEW(cmd - 1);
        info.nShow = SW_SHOWNORMAL;
        menu->InvokeCommand(reinterpret_cast<LPCMINVOKECOMMANDINFO>(&info));
    }
    DestroyMenu(hmenu);
    menu->Release();
    folder->Release();
    for (auto *pidl : pidls)
        CoTaskMemFree(pidl);
    return true;
}
#endif
