#include "filebrowserdialog.h"

#include "icons.h"
#include "transferdialog.h"

#include <QComboBox>
#include <QDateTime>
#include <QFileSystemModel>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSplitter>
#include <QToolButton>
#include <QTreeWidget>
#include <QTreeView>
#include <QVBoxLayout>

FileBrowserDialog::FileBrowserDialog(const SshTarget &target, QWidget *parent)
    : QDialog(parent)
    , m_target(target)
{
    setWindowTitle(tr("文件管理 - %1@%2").arg(
        target.user.isEmpty() ? QStringLiteral("root") : target.user, target.host));
    resize(1050, 620);

    m_client = new SftpClient();   // 无 parent：对象住在自己的工作线程里
    m_transfer = new TransferDialog(m_client, this);

    // --- 顶栏 ---
    m_uploadBtn = new QPushButton(themedIcon(QStringLiteral("up.png")), tr("上传"), this);
    m_downloadBtn = new QPushButton(themedIcon(QStringLiteral("down.png")), tr("下载"), this);
    QPushButton *refreshBtn = new QPushButton(themedIcon(QStringLiteral("refresh.png")), tr("刷新"), this);
    m_mkdirBtn = new QPushButton(themedIcon(QStringLiteral("add.png")), tr("新建目录"), this);
    m_renameBtn = new QPushButton(themedIcon(QStringLiteral("pencil.png")), tr("重命名"), this);
    m_deleteBtn = new QPushButton(themedIcon(QStringLiteral("trash.png")), tr("删除"), this);
    QPushButton *queueBtn = new QPushButton(tr("传输队列..."), this);
    for (QPushButton *b : { m_uploadBtn, m_downloadBtn, refreshBtn,
                            m_mkdirBtn, m_renameBtn, m_deleteBtn, queueBtn })
        b->setEnabled(false);
    queueBtn->setEnabled(true);

    connect(m_uploadBtn, &QPushButton::clicked, this, &FileBrowserDialog::onUploadClicked);
    connect(m_downloadBtn, &QPushButton::clicked, this, &FileBrowserDialog::onDownloadClicked);
    connect(refreshBtn, &QPushButton::clicked, this, &FileBrowserDialog::onRefreshClicked);
    connect(m_mkdirBtn, &QPushButton::clicked, this, &FileBrowserDialog::onMkdirClicked);
    connect(m_renameBtn, &QPushButton::clicked, this, &FileBrowserDialog::onRenameClicked);
    connect(m_deleteBtn, &QPushButton::clicked, this, &FileBrowserDialog::onDeleteClicked);
    connect(queueBtn, &QPushButton::clicked, this, &FileBrowserDialog::showTransferQueue);

    auto *topBar = new QHBoxLayout;
    topBar->addWidget(m_uploadBtn);
    topBar->addWidget(m_downloadBtn);
    topBar->addSpacing(12);
    topBar->addWidget(refreshBtn);
    topBar->addWidget(m_mkdirBtn);
    topBar->addWidget(m_renameBtn);
    topBar->addWidget(m_deleteBtn);
    topBar->addStretch(1);
    topBar->addWidget(queueBtn);

    // --- 本地栏 ---
    m_fsModel = new QFileSystemModel(this);
    m_fsModel->setRootPath(QString());
    m_fsModel->setFilter(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden);
    m_localView = new QTreeView(this);
    m_localView->setModel(m_fsModel);
    // 只留名称/大小/类型/修改时间，与远程栏观感一致
    for (int c = m_localView->header()->count() - 1; c >= 1; --c)
        m_localView->hideColumn(c);   // 默认只显示名称，想看详情可拖出
    m_localView->setRootIndex(m_fsModel->index(QDir::homePath()));
    m_localView->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_localView->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_localView->header()->setStretchLastSection(false);
    m_localView->header()->setSectionResizeMode(0, QHeaderView::Stretch);

    m_localPathBox = new QComboBox(this);
    m_localPathBox->setEditable(true);
    m_localPathBox->addItem(QDir::toNativeSeparators(QDir::homePath()));
    auto *localUpBtn = new QToolButton(this);
    localUpBtn->setIcon(themedIcon(QStringLiteral("up.png")));
    localUpBtn->setToolTip(tr("上一级"));

    auto *localRow = new QHBoxLayout;
    localRow->addWidget(localUpBtn);
    localRow->addWidget(m_localPathBox, 1);

    auto *localPane = new QVBoxLayout;
    localPane->addLayout(localRow);
    localPane->addWidget(m_localView, 1);

    // --- 远程栏 ---
    m_remoteTree = new QTreeWidget(this);
    m_remoteTree->setHeaderLabels({ tr("名称"), tr("大小"), tr("修改时间"), tr("权限") });
    m_remoteTree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_remoteTree->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_remoteTree->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_remoteTree->setRootIsDecorated(false);
    m_remoteTree->setAlternatingRowColors(true);
    m_remoteTree->setSortingEnabled(false);   // 排序已由服务端返回前做过（目录优先）

    m_remotePathBox = new QComboBox(this);
    m_remotePathBox->setEditable(true);
    auto *remoteUpBtn = new QToolButton(this);
    remoteUpBtn->setIcon(themedIcon(QStringLiteral("up.png")));
    remoteUpBtn->setToolTip(tr("上一级"));

    auto *remoteRow = new QHBoxLayout;
    remoteRow->addWidget(remoteUpBtn);
    remoteRow->addWidget(m_remotePathBox, 1);

    auto *remotePane = new QVBoxLayout;
    remotePane->addLayout(remoteRow);
    remotePane->addWidget(m_remoteTree, 1);

    // --- 双栏 + 状态栏 ---
    auto *splitter = new QSplitter(this);
    auto *localWidget = new QWidget(this);
    localWidget->setLayout(localPane);
    auto *remoteWidget = new QWidget(this);
    remoteWidget->setLayout(remotePane);
    splitter->addWidget(localWidget);
    splitter->addWidget(remoteWidget);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 1);

    m_status = new QLabel(tr("连接中..."), this);

    auto *root = new QVBoxLayout(this);
    root->addLayout(topBar);
    root->addWidget(splitter, 1);
    root->addWidget(m_status);

    setLocalPath(QDir::homePath());
    m_remotePath = QStringLiteral("/");

    // --- 信号 ---
    connect(m_client, &SftpClient::connected, this, &FileBrowserDialog::onConnected);
    connect(m_client, &SftpClient::connectFailed, this, &FileBrowserDialog::onConnectFailed);
    connect(m_client, &SftpClient::disconnected, this, &FileBrowserDialog::onSftpDisconnected);
    connect(m_client, &SftpClient::listed, this, &FileBrowserDialog::onListed);
    connect(m_client, &SftpClient::listedFailed, this, &FileBrowserDialog::onListedFailed);
    connect(m_client, &SftpClient::opFinished, this, &FileBrowserDialog::onOpFinished);

    connect(m_localView, &QTreeView::doubleClicked, this, &FileBrowserDialog::onLocalActivated);
    connect(m_remoteTree, &QTreeWidget::itemActivated, this, &FileBrowserDialog::onRemoteItemActivated);
    connect(m_localPathBox->lineEdit(), &QLineEdit::returnPressed, this, &FileBrowserDialog::onLocalPathChosen);
    connect(m_remotePathBox->lineEdit(), &QLineEdit::returnPressed, this, &FileBrowserDialog::onRemotePathChosen);
    connect(localUpBtn, &QToolButton::clicked, this, &FileBrowserDialog::onLocalUp);
    connect(remoteUpBtn, &QToolButton::clicked, this, &FileBrowserDialog::onRemoteUp);
    connect(m_localView->selectionModel(), &QItemSelectionModel::selectionChanged,
            this, &FileBrowserDialog::updateActionStates);
    connect(m_remoteTree, &QTreeWidget::itemSelectionChanged,
            this, &FileBrowserDialog::updateActionStates);

    updateActionStates();
    m_client->connectToHost(m_target);
}

FileBrowserDialog::~FileBrowserDialog()
{
    delete m_client;   // 断开连接并停掉工作线程
}

// ---------------------------------------------------------------- SFTP 回调

void FileBrowserDialog::onConnected(const QString &homePath, const QString &fingerprint)
{
    m_connected = true;
    m_status->setText(tr("已连接（指纹 SHA256:%1...）").arg(fingerprint.left(16)));
    updateActionStates();
    setRemotePath(homePath);
}

void FileBrowserDialog::onConnectFailed(const QString &error)
{
    m_connected = false;
    m_status->setText(tr("连接失败"));
    QMessageBox::critical(this, tr("SFTP 连接失败"), error);
    for (QPushButton *b : { m_uploadBtn, m_downloadBtn, m_mkdirBtn, m_renameBtn, m_deleteBtn })
        b->setEnabled(false);
}

void FileBrowserDialog::onSftpDisconnected()
{
    m_connected = false;
    m_status->setText(tr("连接已断开"));
    for (QPushButton *b : { m_uploadBtn, m_downloadBtn, m_mkdirBtn, m_renameBtn, m_deleteBtn })
        b->setEnabled(false);
}

void FileBrowserDialog::onListed(const QString &path, const QVector<SftpEntry> &entries)
{
    if (path != m_remotePath)
        return;   // 已切到别的目录，旧结果作废
    m_remoteTree->clear();
    for (const SftpEntry &e : entries) {
        auto *item = new QTreeWidgetItem(m_remoteTree);
        item->setText(0, e.name);
        item->setIcon(0, e.isDir ? m_iconProvider.icon(QFileIconProvider::Folder)
                                 : m_iconProvider.icon(QFileInfo(e.name)));
        if (e.isDir) {
            item->setText(1, QStringLiteral("<DIR>"));
            item->setTextAlignment(1, Qt::AlignLeft | Qt::AlignVCenter);
        } else {
            QString sizeText;
            const double kb = double(e.size) / 1024.0;
            if (e.size < 1024 * 1024)
                sizeText = tr("%1 KB").arg(QString::number(kb, 'f', 1));
            else
                sizeText = tr("%1 MB").arg(QString::number(kb / 1024.0, 'f', 2));
            item->setText(1, sizeText);
            item->setTextAlignment(1, Qt::AlignRight | Qt::AlignVCenter);
        }
        item->setText(2, e.mtime > 0
                          ? QDateTime::fromSecsSinceEpoch(e.mtime).toString(QStringLiteral("yyyy-MM-dd hh:mm"))
                          : QString());
        item->setText(3, permString(e.permissions, e.isDir, e.isSymlink));
        item->setData(0, Qt::UserRole, e.isDir);
        item->setData(0, Qt::UserRole + 1, e.isSymlink);
        item->setData(0, Qt::UserRole + 2, quint64(e.size));
    }
    updateActionStates();
}

void FileBrowserDialog::onListedFailed(const QString &path, const QString &error)
{
    m_status->setText(tr("列目录失败：%1").arg(error));
    QMessageBox::warning(this, tr("列目录失败"),
                         tr("路径 %1\n%2").arg(path, error));
}

void FileBrowserDialog::onOpFinished(const QString &op, const QString &arg, bool ok, const QString &error)
{
    if (!ok) {
        QMessageBox::warning(this, tr("操作失败"),
                             tr("%1 %2\n%3").arg(op, arg, error));
        return;
    }
    if (op == QLatin1String("realpath")) {
        // setRemotePath 遇到非规范路径先发 realpath，结果回来在这里落地
        setRemotePath(arg);
    } else {
        refreshRemote();
    }
}

// ---------------------------------------------------------------- 顶栏动作

void FileBrowserDialog::onUploadClicked()
{
    const QModelIndexList rows = m_localView->selectionModel()->selectedRows(0);
    if (rows.isEmpty())
        return;
    // 收集文件（目录上传第一版不做）
    struct Pending { QString local; QString remote; qint64 size; };
    QVector<Pending> pendings;
    QString skippedDirs;
    for (const QModelIndex &idx : rows) {
        const QString path = m_fsModel->filePath(idx);
        if (m_fsModel->isDir(idx)) {
            skippedDirs += path + QStringLiteral("\n");
            continue;
        }
        Pending p;
        p.local = path;
        p.remote = remoteJoin(m_remotePath, QFileInfo(path).fileName());
        p.size = QFileInfo(path).size();
        pendings.append(p);
    }
    if (!skippedDirs.isEmpty())
        QMessageBox::information(this, tr("不支持目录上传"),
                                 tr("以下是目录，已跳过（暂不支持递归上传）：\n%1").arg(skippedDirs));
    for (const Pending &p : pendings) {
        // 单文件时目标已存在则确认覆盖；多文件静默覆盖
        if (pendings.size() == 1) {
            const QString checkPath = p.remote;
            for (int i = 0; i < m_remoteTree->topLevelItemCount(); ++i) {
                const QTreeWidgetItem *it = m_remoteTree->topLevelItem(i);
                if (remoteJoin(m_remotePath, it->text(0)) == checkPath
                    && !it->data(0, Qt::UserRole).toBool()) {
                    const auto ans = QMessageBox::question(
                        this, tr("覆盖确认"),
                        tr("远程已存在 %1，覆盖？").arg(checkPath));
                    if (ans != QMessageBox::Yes)
                        return;
                    break;
                }
            }
        }
        m_transfer->enqueue(true, p.local, p.remote, p.size);
    }
}

void FileBrowserDialog::onDownloadClicked()
{
    const QList<QTreeWidgetItem *> items = m_remoteTree->selectedItems();
    if (items.isEmpty())
        return;
    QString skippedDirs;
    for (QTreeWidgetItem *it : items) {
        if (it->data(0, Qt::UserRole).toBool()) {
            skippedDirs += it->text(0) + QStringLiteral("\n");
            continue;
        }
        const QString remote = remoteJoin(m_remotePath, it->text(0));
        const QString local = localJoin(m_localPath, it->text(0));
        if (items.size() == 1 && QFileInfo::exists(local)) {
            const auto ans = QMessageBox::question(
                this, tr("覆盖确认"), tr("本地已存在 %1，覆盖？").arg(local));
            if (ans != QMessageBox::Yes)
                return;
        }
        m_transfer->enqueue(false, local, remote,
                            it->data(0, Qt::UserRole + 2).toLongLong());
    }
    if (!skippedDirs.isEmpty())
        QMessageBox::information(this, tr("不支持目录下载"),
                                 tr("以下是目录，已跳过（暂不支持递归下载）：\n%1").arg(skippedDirs));
}

void FileBrowserDialog::onRefreshClicked()
{
    m_fsModel->setRootPath(m_localPath);
    refreshRemote();
}

void FileBrowserDialog::onMkdirClicked()
{
    bool ok = false;
    const QString name = QInputDialog::getText(this, tr("新建目录"),
                                               tr("目录名:"), QLineEdit::Normal, QString(), &ok);
    if (!ok || name.trimmed().isEmpty())
        return;
    m_client->makeDir(remoteJoin(m_remotePath, name.trimmed()));
}

void FileBrowserDialog::onRenameClicked()
{
    const QList<QTreeWidgetItem *> items = m_remoteTree->selectedItems();
    if (items.size() != 1)
        return;
    QTreeWidgetItem *it = items.first();
    bool ok = false;
    const QString newName = QInputDialog::getText(this, tr("重命名"),
                                                  tr("新名字:"), QLineEdit::Normal, it->text(0), &ok);
    if (!ok || newName.trimmed().isEmpty() || newName == it->text(0))
        return;
    m_client->renameEntry(remoteJoin(m_remotePath, it->text(0)),
                          remoteJoin(m_remotePath, newName.trimmed()));
}

void FileBrowserDialog::onDeleteClicked()
{
    // 焦点在远程栏则删远程，否则删本地
    if (m_remoteTree->hasFocus()) {
        const QList<QTreeWidgetItem *> items = m_remoteTree->selectedItems();
        if (items.isEmpty())
            return;
        const auto ans = QMessageBox::question(
            this, tr("删除确认"),
            tr("删除远程的 %n 个条目？", "", int(items.size())));
        if (ans != QMessageBox::Yes)
            return;
        for (QTreeWidgetItem *it : items)
            m_client->removeEntry(remoteJoin(m_remotePath, it->text(0)),
                                  it->data(0, Qt::UserRole).toBool());
        return;
    }
    const QModelIndexList rows = m_localView->selectionModel()->selectedRows(0);
    if (rows.isEmpty())
        return;
    const auto ans = QMessageBox::question(
        this, tr("删除确认"), tr("删除本地的 %n 个条目？", "", int(rows.size())));
    if (ans != QMessageBox::Yes)
        return;
    for (const QModelIndex &idx : rows) {
        const QString path = m_fsModel->filePath(idx);
        if (m_fsModel->isDir(idx))
            QDir(path).rmdir(path);   // 只删空目录，和远程行为一致
        else
            QFile::remove(path);
    }
}

void FileBrowserDialog::showTransferQueue()
{
    m_transfer->show();
    m_transfer->raise();
    m_transfer->activateWindow();
}

// ---------------------------------------------------------------- 导航

void FileBrowserDialog::onLocalActivated(const QModelIndex &index)
{
    if (m_fsModel->isDir(index))
        setLocalPath(m_fsModel->filePath(index));
}

void FileBrowserDialog::onLocalPathChosen()
{
    const QString text = QDir::fromNativeSeparators(m_localPathBox->currentText().trimmed());
    if (!text.isEmpty())
        setLocalPath(text);
}

void FileBrowserDialog::onLocalUp()
{
    QDir d(m_localPath);
    if (d.cdUp())
        setLocalPath(d.absolutePath());
}

void FileBrowserDialog::onRemoteItemActivated(QTreeWidgetItem *item, int column)
{
    Q_UNUSED(column);
    if (item->data(0, Qt::UserRole).toBool())
        setRemotePath(remoteJoin(m_remotePath, item->text(0)));
    else
        onDownloadClicked();   // 双击远程文件 = 下载
}

void FileBrowserDialog::onRemotePathChosen()
{
    const QString text = m_remotePathBox->currentText().trimmed();
    if (!text.isEmpty())
        setRemotePath(text);
}

void FileBrowserDialog::onRemoteUp()
{
    if (m_remotePath == QLatin1String("/"))
        return;
    const int idx = m_remotePath.lastIndexOf(QLatin1Char('/'));
    setRemotePath(idx <= 0 ? QStringLiteral("/") : m_remotePath.left(idx));
}

void FileBrowserDialog::updateActionStates()
{
    const bool hasLocalSel = !m_localView->selectionModel()->selectedRows(0).isEmpty();
    const bool hasRemoteSel = !m_remoteTree->selectedItems().isEmpty();
    m_uploadBtn->setEnabled(m_connected && hasLocalSel);
    m_downloadBtn->setEnabled(m_connected && hasRemoteSel);
    m_mkdirBtn->setEnabled(m_connected);
    m_renameBtn->setEnabled(m_connected && m_remoteTree->selectedItems().size() == 1);
    m_deleteBtn->setEnabled(m_connected && (hasLocalSel || hasRemoteSel));
}

// ---------------------------------------------------------------- 内部工具

void FileBrowserDialog::setLocalPath(const QString &path)
{
    const QDir d(path);
    if (!d.exists() || !d.isReadable())
        return;
    m_localPath = d.absolutePath();
    m_localView->setRootIndex(m_fsModel->index(m_localPath));
    const QString native = QDir::toNativeSeparators(m_localPath);
    const int existing = m_localPathBox->findText(native);
    if (existing >= 0)
        m_localPathBox->setCurrentIndex(existing);
    else
        m_localPathBox->insertItem(0, native);
    m_localPathBox->lineEdit()->setText(native);
}

void FileBrowserDialog::setRemotePath(const QString &path)
{
    // 让服务器把 .. / 相对路径规范成绝对路径再列目录
    QString p = path;
    if (p.isEmpty())
        p = QStringLiteral("/");
    if (!p.startsWith(QLatin1Char('/')) || p.contains(QLatin1String(".."))) {
        m_remotePath = p;   // 先占位，realpath 回来再刷新列表
        m_remotePathBox->lineEdit()->setText(p);
        m_client->realPath(p);
        return;
    }
    m_remotePath = p;
    const int existing = m_remotePathBox->findText(p);
    if (existing >= 0)
        m_remotePathBox->setCurrentIndex(existing);
    else
        m_remotePathBox->insertItem(0, p);
    m_remotePathBox->lineEdit()->setText(p);
    refreshRemote();
}

void FileBrowserDialog::refreshRemote()
{
    if (m_connected)
        m_client->listDir(m_remotePath);
}

QString FileBrowserDialog::remoteJoin(const QString &base, const QString &name) const
{
    if (base.endsWith(QLatin1Char('/')))
        return base + name;
    return base + QLatin1Char('/') + name;
}

QString FileBrowserDialog::localJoin(const QString &base, const QString &name)
{
    return QDir(base).filePath(name);
}

QString FileBrowserDialog::permString(quint32 perm, bool isDir, bool isLink)
{
    QString s;
    s += isLink ? QChar::fromLatin1('l') : (isDir ? QChar::fromLatin1('d') : QChar::fromLatin1('-'));
    const quint32 masks[3] = { 0400, 0200, 0100 };
    const char sym[3] = { 'r', 'w', 'x' };
    for (int group = 0; group < 3; ++group) {
        for (int bit = 0; bit < 3; ++bit)
            s += QChar::fromLatin1((perm & (masks[bit] >> (group * 3))) ? sym[bit] : '-');
    }
    return s;
}

FileBrowserDialog::RemoteRowInfo FileBrowserDialog::remoteSelected(int *count) const
{
    RemoteRowInfo info;
    info.valid = false;
    if (count)
        *count = m_remoteTree->selectedItems().size();
    const QList<QTreeWidgetItem *> items = m_remoteTree->selectedItems();
    if (items.size() == 1) {
        info.valid = true;
        info.isDir = items.first()->data(0, Qt::UserRole).toBool();
        info.name = items.first()->text(0);
    }
    return info;
}
