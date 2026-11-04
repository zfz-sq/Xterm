#include "filebrowserdialog.h"

#include "filebrowsermodel.h"
#include "filebrowserwidgets.h"
#include "icons.h"
#include "sshsession.h"
#include "transferdialog.h"
#include "transferqueue.h"

#include <QAction>
#include <QCheckBox>
#include <QClipboard>
#include <algorithm>
#include <functional>

#include <QEvent>
#include <QFocusEvent>
#include <QIcon>
#include <QKeyEvent>
#include <QScrollArea>
#include <QScrollBar>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QFileSystemModel>
#include <QFileSystemWatcher>
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QGuiApplication>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QLabel>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QListWidget>
#include <QLocale>
#include <QMenu>
#include <QPlainTextEdit>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QShortcut>
#include <QSortFilterProxyModel>
#include <QSizePolicy>
#include <QSplitter>
#include <QStandardPaths>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QTreeView>
#include <QUrl>
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

FileBrowserDialog::FileBrowserDialog(SshSession *session, QWidget *parent)
    : FramelessDialog(parent)
    , m_session(session)
    , m_target(session->target())
{
    setDialogTitle(tr("文件管理 - %1@%2").arg(
        m_target.user.isEmpty() ? QStringLiteral("root") : m_target.user, m_target.host));
    // 跟终端主窗口对齐，避免文件管理比终端宽一截。
    if (parent && parent->width() >= 640 && parent->height() >= 400)
        resize(parent->size());
    else
        resize(1040, 680);

    m_client = new SftpClient(m_session, this);
    // 传输队列网关：入队/批次跟踪/编辑回传都在里面，语义事件转到这里刷 UI
    m_queue = new TransferQueue(m_client, sessionKey(), this, this);
    connect(m_queue, &TransferQueue::folderBatchFinished, this, &FileBrowserDialog::refreshRemote);
    connect(m_queue, &TransferQueue::fetchFinished, this, &FileBrowserDialog::onFetchFinished);
    connect(m_queue, &TransferQueue::editUploadStarted, this, [this](const QString &fileName) {
        m_status->setText(tr("正在回传 %1").arg(fileName));
    });
    connect(m_queue, &TransferQueue::editUploadFinished, this,
            [this](bool ok, const QString &fileName, const QString &remote, const QString &error) {
        if (ok) {
            m_status->setText(tr("已回传 %1").arg(fileName));
            return;
        }
        FramelessDialog::message(this, tr("回传失败"), tr("%1\n%2").arg(remote, error));
    });
    connect(m_queue->dialog(), &TransferDialog::progressChanged,
            this, &FileBrowserDialog::onTransferProgressInfo);
    m_localProc = new QProcess(this);

    m_localUpload = makePaneButton(QStringLiteral("upload.png"), tr("上传"));
    m_localEdit = makePaneButton(QStringLiteral("edit.png"), tr("编辑"));
    m_localDelete = makePaneButton(QStringLiteral("trash.png"), tr("删除"));
    m_localRename = makePaneButton(QStringLiteral("rename.png"), tr("重命名"));
    m_localProps = makePaneButton(QStringLiteral("props.png"), tr("属性"));
    QToolButton *localNew = makeNewButton(false);

    m_remoteDownload = makePaneButton(QStringLiteral("download.png"), tr("下载"));
    m_remoteEdit = makePaneButton(QStringLiteral("edit.png"), tr("编辑"));
    m_remoteDelete = makePaneButton(QStringLiteral("trash.png"), tr("删除"));
    m_remoteRename = makePaneButton(QStringLiteral("rename.png"), tr("重命名"));
    m_remoteProps = makePaneButton(QStringLiteral("props.png"), tr("属性"));
    m_remoteNew = makeNewButton(true);

    connect(m_localUpload, &QToolButton::clicked, this, &FileBrowserDialog::uploadSelected);
    connect(m_localEdit, &QToolButton::clicked, this, &FileBrowserDialog::editLocal);
    connect(m_localDelete, &QToolButton::clicked, this, &FileBrowserDialog::deleteLocal);
    connect(m_localRename, &QToolButton::clicked, this, &FileBrowserDialog::renameLocal);
    connect(m_localProps, &QToolButton::clicked, this, &FileBrowserDialog::propsLocal);
    connect(m_remoteDownload, &QToolButton::clicked, this, &FileBrowserDialog::downloadSelected);
    connect(m_remoteEdit, &QToolButton::clicked, this, &FileBrowserDialog::editRemote);
    connect(m_remoteDelete, &QToolButton::clicked, this, &FileBrowserDialog::deleteRemote);
    connect(m_remoteRename, &QToolButton::clicked, this, &FileBrowserDialog::renameRemote);
    connect(m_remoteProps, &QToolButton::clicked, this, &FileBrowserDialog::propsRemote);

    auto *localBar = new QHBoxLayout;
    localBar->setContentsMargins(0, 0, 0, 0);
    localBar->setSpacing(4);
    for (QToolButton *button : { m_localUpload, m_localEdit, m_localDelete, m_localRename, m_localProps })
        localBar->addWidget(button);
    localBar->addWidget(vsep(this));
    localBar->addWidget(localNew);
    localBar->addStretch(1);

    auto *remoteBar = new QHBoxLayout;
    remoteBar->setContentsMargins(0, 0, 0, 0);
    remoteBar->setSpacing(4);
    for (QToolButton *button : { m_remoteDownload, m_remoteEdit, m_remoteDelete, m_remoteRename, m_remoteProps })
        remoteBar->addWidget(button);
    remoteBar->addWidget(vsep(this));
    remoteBar->addWidget(m_remoteNew);
    remoteBar->addStretch(1);

    m_fsModel = new QFileSystemModel(this);
    m_fsModel->setRootPath(QString());
    m_fsModel->setFilter(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden);
    // 排序代理：目录置顶 + 名称不区分大小写字母序；dynamicSortFilter 让
    // QFileSystemModel 异步填充出来的条目自动落位有序
    m_fsProxy = new LocalSortProxy(this);
    m_fsProxy->setSourceModel(m_fsModel);
    m_fsProxy->setDynamicSortFilter(true);
    m_fsProxy->sort(0, Qt::AscendingOrder);
    m_localView = new QTreeView(this);
    m_localView->setModel(m_fsProxy);
    // 与远程端对齐显示 名称/大小/修改时间：隐藏「类型」列（第 2 列，信息量低）
    m_localView->hideColumn(2);
    m_localView->setColumnWidth(1, 90);  // 大小
    m_localView->setColumnWidth(3, 150); // 修改时间（系统格式较长）
    // 大小列右对齐；列头不参与排序（顺序恒定：目录置顶 + 字母序）
    m_localView->setItemDelegateForColumn(1, new SizeColumnDelegate(m_localView));
    m_localView->header()->setSectionsClickable(false);
    m_localView->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_localView->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_localView->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_localView->header()->setStretchLastSection(false);
    m_localView->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    // 右键事件会被滚动区域收走，策略要设在视图上，坐标仍是视口坐标。
    m_localView->setContextMenuPolicy(Qt::CustomContextMenu);

    m_localPathBar = new PathBar(this);
    m_localPathBar->setEditIcon(themedIcon(QStringLiteral("edit.png")));
    auto *localUpBtn = new QToolButton(this);
    localUpBtn->setIcon(themedIcon(QStringLiteral("up.png")));
    localUpBtn->setToolTip(tr("上一级"));
    localUpBtn->setFocusPolicy(Qt::NoFocus);
    auto *localRefresh = new QToolButton(this);
    localRefresh->setIcon(themedIcon(QStringLiteral("refresh.png")));
    localRefresh->setToolTip(tr("刷新"));
    localRefresh->setFocusPolicy(Qt::NoFocus);

    auto *localRow = new QHBoxLayout;
    localRow->addWidget(localUpBtn);
    localRow->addWidget(localRefresh);
    localRow->addWidget(m_localPathBar, 1);

    auto *localPane = new QVBoxLayout;
    localPane->setContentsMargins(0, 0, 0, 0);
    localPane->addLayout(localBar);
    localPane->addLayout(localRow);
    localPane->addWidget(m_localView, 1);

    m_remoteTree = new QTreeWidget(this);
    m_remoteTree->setHeaderLabels({ tr("名称"), tr("大小"), tr("修改时间"), tr("权限") });
    m_remoteTree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_remoteTree->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_remoteTree->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_remoteTree->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_remoteTree->setRootIsDecorated(false);
    m_remoteTree->setAlternatingRowColors(true);
    m_remoteTree->setSortingEnabled(false);
    m_remoteTree->setContextMenuPolicy(Qt::CustomContextMenu);

    m_remotePathBar = new PathBar(this);
    m_remotePathBar->setEditIcon(themedIcon(QStringLiteral("edit.png")));
    auto *remoteUpBtn = new QToolButton(this);
    remoteUpBtn->setIcon(themedIcon(QStringLiteral("up.png")));
    remoteUpBtn->setToolTip(tr("上一级"));
    remoteUpBtn->setFocusPolicy(Qt::NoFocus);
    auto *remoteRefresh = new QToolButton(this);
    remoteRefresh->setIcon(themedIcon(QStringLiteral("refresh.png")));
    remoteRefresh->setToolTip(tr("刷新"));
    remoteRefresh->setFocusPolicy(Qt::NoFocus);

    auto *remoteRow = new QHBoxLayout;
    remoteRow->addWidget(remoteUpBtn);
    remoteRow->addWidget(remoteRefresh);
    remoteRow->addWidget(m_remotePathBar, 1);

    auto *remotePane = new QVBoxLayout;
    remotePane->setContentsMargins(0, 0, 0, 0);
    remotePane->addLayout(remoteBar);
    remotePane->addLayout(remoteRow);
    remotePane->addWidget(m_remoteTree, 1);

    auto *splitter = new QSplitter(this);
    auto *localWidget = new QWidget(this);
    localWidget->setLayout(localPane);
    auto *remoteWidget = new QWidget(this);
    remoteWidget->setLayout(remotePane);
    splitter->addWidget(localWidget);
    splitter->addWidget(remoteWidget);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 1);

    m_status = new QLabel(tr("正在打开主目录..."), this);
    // 底栏传输进度：文案 + 细进度条，详情/取消仍走「传输队列」窗口（不再自动弹出）
    m_transferText = new QLabel(this);
    m_transferBar = new QProgressBar(this);
    m_transferBar->setFixedWidth(140);
    m_transferBar->setTextVisible(false);
    m_transferBar->setRange(0, 100);
    m_transferBar->hide();
    auto *queueBtn = new QPushButton(tr("传输队列..."), this);
    queueBtn->setAutoDefault(false);
    queueBtn->setDefault(false);
    connect(queueBtn, &QPushButton::clicked, this, &FileBrowserDialog::showTransferQueue);
    auto *statusRow = new QHBoxLayout;
    statusRow->addWidget(m_status, 1);
    statusRow->addWidget(m_transferText);
    statusRow->addWidget(m_transferBar);
    statusRow->addWidget(queueBtn);

    auto *root = new QVBoxLayout;
    root->addWidget(splitter, 1);
    root->addLayout(statusRow);
    setContentLayout(root);

    restoreBrowserState();
    const QString savedLocal = m_localPath;
    m_localPath.clear();
    if (!savedLocal.isEmpty() && QDir(savedLocal).exists())
        setLocalPath(savedLocal);
    else
        setLocalPath(QDir::homePath());

    connect(m_client, &SftpClient::listed, this, &FileBrowserDialog::onListed);
    connect(m_client, &SftpClient::listedFailed, this, &FileBrowserDialog::onListedFailed);
    connect(m_client, &SftpClient::opFinished, this, &FileBrowserDialog::onOpFinished);
    // 共享会话断了，文件管理器没有独立生命，跟着收摊。
    // 关闭只销毁适配器并取消文件请求，不影响 shell 本身。
    connect(m_session, &SshSession::closed, this, &QDialog::close);
    connect(m_localView, &QTreeView::doubleClicked, this, &FileBrowserDialog::onLocalActivated);
    connect(m_remoteTree, &QTreeWidget::itemActivated, this, &FileBrowserDialog::onRemoteItemActivated);
    m_localPathBar->onJump = [this](const QString &path) { setLocalPath(path); };
    m_localPathBar->onSubmit = [this](const QString &) { onLocalPathChosen(); };
    m_remotePathBar->onJump = [this](const QString &path) { setRemotePath(path); };
    m_remotePathBar->onSubmit = [this](const QString &) { onRemotePathChosen(); };
    connect(localUpBtn, &QToolButton::clicked, this, &FileBrowserDialog::onLocalUp);
    connect(remoteUpBtn, &QToolButton::clicked, this, &FileBrowserDialog::onRemoteUp);
    connect(localRefresh, &QToolButton::clicked, this, &FileBrowserDialog::refreshLocal);
    connect(remoteRefresh, &QToolButton::clicked, this, &FileBrowserDialog::refreshRemote);
    connect(m_localView->selectionModel(), &QItemSelectionModel::selectionChanged,
            this, &FileBrowserDialog::updateActionStates);
    connect(m_remoteTree, &QTreeWidget::itemSelectionChanged, this, &FileBrowserDialog::updateActionStates);
    connect(m_localView, &QWidget::customContextMenuRequested,
            this, &FileBrowserDialog::onLocalContextMenu);
    connect(m_remoteTree, &QWidget::customContextMenuRequested,
            this, &FileBrowserDialog::onRemoteContextMenu);
    connect(m_localProc, &QProcess::finished, this, [this](int code, QProcess::ExitStatus) {
        const QString chunk = QString::fromLocal8Bit(m_localProc->readAllStandardOutput())
                              + QString::fromLocal8Bit(m_localProc->readAllStandardError());
        if (!chunk.trimmed().isEmpty())
            m_localOutput += QStringLiteral("> ") + m_localRunningCmd + QLatin1Char('\n') + chunk + QLatin1Char('\n');
        if (code != 0) {
            m_localFailed = true;
            if (chunk.trimmed().isEmpty()) {
                m_localOutput += QStringLiteral("> ") + m_localRunningCmd + QLatin1Char('\n')
                                 + tr("退出码 %1").arg(code) + QLatin1Char('\n');
            }
        }
        pumpLocalCommand();
    });
    connect(m_localProc, &QProcess::errorOccurred, this, [this](QProcess::ProcessError err) {
        if (err != QProcess::FailedToStart)
            return;
        m_localFailed = true;
        m_localOutput += QStringLiteral("> ") + m_localRunningCmd + QLatin1Char('\n')
                         + tr("无法启动命令") + QLatin1Char('\n');
        pumpLocalCommand();
    });

    auto *delLocal = new QShortcut(QKeySequence::Delete, m_localView);
    delLocal->setContext(Qt::WidgetWithChildrenShortcut);
    connect(delLocal, &QShortcut::activated, this, &FileBrowserDialog::deleteLocal);
    auto *delRemote = new QShortcut(QKeySequence::Delete, m_remoteTree);
    delRemote->setContext(Qt::WidgetWithChildrenShortcut);
    connect(delRemote, &QShortcut::activated, this, &FileBrowserDialog::deleteRemote);
    auto *f2Local = new QShortcut(QKeySequence(Qt::Key_F2), m_localView);
    f2Local->setContext(Qt::WidgetWithChildrenShortcut);
    connect(f2Local, &QShortcut::activated, this, &FileBrowserDialog::renameLocal);
    auto *f2Remote = new QShortcut(QKeySequence(Qt::Key_F2), m_remoteTree);
    f2Remote->setContext(Qt::WidgetWithChildrenShortcut);
    connect(f2Remote, &QShortcut::activated, this, &FileBrowserDialog::renameRemote);

    updateActionStates();
    // 会话已连接（MainWindow 只在 Connected 时放行），先取主目录再列目录。
    m_client->realPath(QStringLiteral("."));
}

FileBrowserDialog::~FileBrowserDialog()
{
    persistBrowserState();
}

QToolButton *FileBrowserDialog::makePaneButton(const QString &iconName, const QString &text)
{
    auto *button = new QToolButton(this);
    button->setObjectName(QStringLiteral("paneTool"));
    button->setIcon(themedIcon(iconName));
    button->setIconSize(QSize(16, 16));
    button->setText(text);
    button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    button->setFocusPolicy(Qt::NoFocus);
    button->setAutoRaise(false);
    button->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);
    return button;
}

QToolButton *FileBrowserDialog::makeNewButton(bool remote)
{
    QToolButton *button = makePaneButton(QStringLiteral("new.png"), tr("新建"));
    auto *menu = new QMenu(button);
    if (remote) {
        menu->addAction(tr("新建目录"), this, &FileBrowserDialog::newRemoteDir);
        menu->addAction(tr("新建文件"), this, &FileBrowserDialog::newRemoteFile);
    } else {
        menu->addAction(tr("新建目录"), this, &FileBrowserDialog::newLocalDir);
        menu->addAction(tr("新建文件"), this, &FileBrowserDialog::newLocalFile);
    }
    button->setMenu(menu);
    button->setPopupMode(QToolButton::InstantPopup);
    return button;
}

QString FileBrowserDialog::sessionKey() const
{
    const int port = m_target.port > 0 ? m_target.port : 22;
    const QString user = m_target.user.isEmpty() ? QStringLiteral("root") : m_target.user;
    return user + QLatin1Char('@') + m_target.host + QLatin1Char(':') + QString::number(port);
}

void FileBrowserDialog::restoreBrowserState()
{
    const BrowserState state = loadBrowserState(sessionKey());
    m_localPath = state.local;
    m_savedRemote = state.remote;
    m_persistedRemote = state.remote;
    m_commands = state.commands;
}

void FileBrowserDialog::persistBrowserState() const
{
    BrowserState state;
    state.local = m_localPath;
    state.remote = m_persistedRemote;
    state.commands = m_commands;
    saveBrowserState(state);
}

void FileBrowserDialog::onListed(const QString &path, const QVector<SftpEntry> &entries)
{
    if (path != m_remotePath)
        return;
    const bool restoring = m_remoteRestorePending;
    m_remoteRestorePending = false;
    m_persistedRemote = path;
    if (restoring)
        m_status->setText(tr("已连接，已恢复上次目录"));
    m_remoteTree->clear();
    for (const SftpEntry &entry : entries) {
        auto *item = new QTreeWidgetItem(m_remoteTree);
        item->setText(0, entry.name);
        item->setIcon(0, entry.isDir ? m_iconProvider.icon(QFileIconProvider::Folder)
                                     : m_iconProvider.icon(QFileInfo(entry.name)));
        if (entry.isDir) {
            item->setText(1, QStringLiteral("<DIR>"));
            item->setTextAlignment(1, Qt::AlignLeft | Qt::AlignVCenter);
        } else {
            item->setText(1, humanSize(entry.size));
            item->setTextAlignment(1, Qt::AlignRight | Qt::AlignVCenter);
        }
        item->setText(2, entry.mtime > 0
                          ? QDateTime::fromSecsSinceEpoch(entry.mtime).toString(QStringLiteral("yyyy-MM-dd hh:mm"))
                          : QString());
        item->setText(3, permString(entry.permissions, entry.isDir, entry.isSymlink));
        item->setData(0, kRoleDir, entry.isDir);
        item->setData(0, kRoleLink, entry.isSymlink);
        item->setData(0, kRoleSize, quint64(entry.size));
        item->setData(0, kRolePerm, entry.permissions);
        item->setData(0, kRoleMtime, entry.mtime);
    }
    persistBrowserState();
    updateActionStates();
}

void FileBrowserDialog::onListedFailed(const QString &path, const QString &error)
{
    if (m_remoteRestorePending) {
        m_remoteRestorePending = false;
        m_status->setText(tr("上次的远程目录不可用，已回到主目录"));
        if (!m_homePath.isEmpty() && path != m_homePath)
            setRemotePath(m_homePath);
        return;
    }
    m_status->setText(tr("列目录失败：%1").arg(error));
    FramelessDialog::message(this, tr("列目录失败"), tr("路径 %1\n%2").arg(path, error));
}

void FileBrowserDialog::onOpFinished(const QString &op, const QString &arg, bool ok, const QString &error)
{
    // 文件夹上传的建目录结果由传输队列行展示（远程已存在等），这里静默放行
    if (op == QLatin1String("mkdir") && m_queue->consumeFolderMkdir(arg))
        return;
    if (op == QLatin1String("exec")) {
        handleExecFinished(ok, arg, error);
        return;
    }
    if (op == QLatin1String("realpath")) {
        if (!ok) {
            if (m_homePath.isEmpty())
                m_status->setText(tr("读取主目录失败"));
            FramelessDialog::message(this, tr("操作失败"), tr("%1\n%2").arg(arg, error));
            return;
        }
        if (m_homePath.isEmpty()) {
            // 开窗后的第一次 realpath：定位主目录，等同旧的“已连接”。
            m_homePath = arg.isEmpty() ? QStringLiteral("/") : arg;
            m_connected = true;
            updateActionStates();
            if (!m_savedRemote.isEmpty()) {
                m_remoteRestorePending = true;
                m_status->setText(tr("已连接，正在打开上次目录"));
                setRemotePath(m_savedRemote);
            } else {
                m_status->setText(tr("已连接"));
                setRemotePath(m_homePath);
            }
            return;
        }
        setRemotePath(arg);
        return;
    }
    if (!ok) {
        FramelessDialog::message(this, tr("操作失败"),
                             tr("%1 %2\n%3").arg(op, arg, error.isEmpty() ? tr("失败") : error));
        return;
    }
    refreshRemote();
}

void FileBrowserDialog::onFetchFinished(const PendingFetch &job, bool ok, const QString &error)
{
    if (!ok) {
        FramelessDialog::message(this, tr(job.edit ? "编辑失败" : "打开失败"), error);
        return;
    }
    if (job.edit)
        m_queue->watchEditFile(job.local, job.remote);
    QDesktopServices::openUrl(QUrl::fromLocalFile(job.local));
}

void FileBrowserDialog::onTransferProgressInfo(const QString &text, int percent, bool active)
{
    m_transferText->setText(text);
    m_transferBar->setVisible(active);
    m_transferBar->setValue(percent);
}

void FileBrowserDialog::showTransferQueue()
{
    m_queue->dialog()->show();
    m_queue->dialog()->raise();
    m_queue->dialog()->activateWindow();
}

void FileBrowserDialog::onLocalActivated(const QModelIndex &index)
{
    if (!index.isValid())
        return;
    const QModelIndex src = mapToSource(index);
    if (m_fsModel->isDir(src))
        setLocalPath(m_fsModel->filePath(src));
    else
        QDesktopServices::openUrl(QUrl::fromLocalFile(m_fsModel->filePath(src)));
}

void FileBrowserDialog::onLocalPathChosen()
{
    const QString text = m_localPathBar->submittedText().trimmed();
    if (text.isEmpty() || text == tr("此电脑")) {
        setLocalPath(QString());
        return;
    }
    QString path = QDir::fromNativeSeparators(text);
    if (path.size() == 2 && path.at(1) == QLatin1Char(':'))
        path += QLatin1Char('/');
    const QFileInfo info(path);
    if (!info.exists() || !info.isDir()) {
        m_status->setText(tr("目录不存在：%1").arg(text));
        syncLocalBar();
        return;
    }
    setLocalPath(path);
}

void FileBrowserDialog::onLocalUp()
{
    if (m_localPath.isEmpty())
        return;
    QDir dir(m_localPath);
    if (!dir.cdUp()) {
        setLocalPath(QString());
        return;
    }
    setLocalPath(dir.absolutePath());
}

void FileBrowserDialog::onRemoteItemActivated(QTreeWidgetItem *item, int column)
{
    Q_UNUSED(column);
    if (!item)
        return;
    if (item->data(0, kRoleDir).toBool())
        setRemotePath(remoteJoin(m_remotePath, item->text(0)));
    else
        downloadSelected();
}

void FileBrowserDialog::onRemotePathChosen()
{
    const QString text = m_remotePathBar->submittedText().trimmed();
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
    const QModelIndexList localRows = m_localView->selectionModel()->selectedRows(0);
    int localFiles = 0;
    for (const QModelIndex &idx : localRows) {
        if (!m_fsModel->isDir(mapToSource(idx)))
            ++localFiles;
    }
    const QVector<RemoteItem> remote = selectedRemote();
    int remoteFiles = 0;
    for (const RemoteItem &item : remote) {
        if (!item.isDir)
            ++remoteFiles;
    }
    m_localUpload->setEnabled(m_connected && !localRows.isEmpty());
    m_localEdit->setEnabled(localRows.size() == 1 && localFiles == 1);
    m_localDelete->setEnabled(!localRows.isEmpty());
    m_localRename->setEnabled(localRows.size() == 1);
    m_localProps->setEnabled(localRows.size() == 1);
    m_remoteDownload->setEnabled(m_connected && !remote.isEmpty());
    m_remoteEdit->setEnabled(m_connected && remote.size() == 1 && remoteFiles == 1);
    m_remoteDelete->setEnabled(m_connected && !remote.isEmpty());
    m_remoteRename->setEnabled(m_connected && remote.size() == 1);
    m_remoteProps->setEnabled(m_connected && !remote.isEmpty());
    m_remoteNew->setEnabled(m_connected);
}

void FileBrowserDialog::onLocalContextMenu(const QPoint &pos)
{
    const QModelIndex idx = m_localView->indexAt(pos);
    if (idx.isValid() && !m_localView->selectionModel()->isSelected(idx)) {
        m_localView->selectionModel()->select(idx, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
        m_localView->setCurrentIndex(idx);
    }
    m_localView->setFocus();
    m_shellMenuPos = m_localView->viewport()->mapToGlobal(pos);
    const QStringList paths = selectedLocalPaths();
    const int n = paths.size();
    QMenu menu(this);
    menu.addAction(themedIcon(QStringLiteral("upload.png")), tr("上传"), this, &FileBrowserDialog::uploadSelected)
        ->setEnabled(m_connected && n > 0);
    menu.addAction(themedIcon(QStringLiteral("trash.png")), tr("删除"), this, &FileBrowserDialog::deleteLocal)
        ->setEnabled(n > 0);
    menu.addAction(themedIcon(QStringLiteral("rename.png")), tr("重命名"), this, &FileBrowserDialog::renameLocal)
        ->setEnabled(n == 1);
    menu.addAction(themedIcon(QStringLiteral("open.png")), tr("在文件资源管理器中显示"), this, &FileBrowserDialog::revealLocal)
        ->setEnabled(n > 0 || !m_localPath.isEmpty());
    menu.addSeparator();
    fillCommandMenu(menu.addMenu(themedIcon(QStringLiteral("command.png")), tr("文件自定义命令(&C)")), false);
    QMenu *names = menu.addMenu(themedIcon(QStringLiteral("name.png")), tr("文件名(&F)"));
    names->addAction(tr("复制文件名"), this, [this] { copyLocalNames(false); })->setEnabled(n > 0);
    names->addAction(tr("复制完整路径"), this, [this] { copyLocalNames(true); })->setEnabled(n > 0);
    menu.addAction(themedIcon(QStringLiteral("props.png")), tr("属性(&P)"), this, &FileBrowserDialog::propsLocal)
        ->setEnabled(n == 1);
#ifdef Q_OS_WIN
    menu.addAction(tr("系统菜单"), this, &FileBrowserDialog::showSystemMenu)->setEnabled(n > 0);
#endif
    menu.addSeparator();
    menu.addAction(themedIcon(QStringLiteral("new.png")), tr("新建目录"), this, &FileBrowserDialog::newLocalDir);
    menu.addAction(tr("新建文件"), this, &FileBrowserDialog::newLocalFile);
    menu.exec(m_shellMenuPos);
}

void FileBrowserDialog::onRemoteContextMenu(const QPoint &pos)
{
    m_remoteTree->setFocus();
    QTreeWidgetItem *item = m_remoteTree->itemAt(pos);
    if (item && !item->isSelected()) {
        m_remoteTree->clearSelection();
        item->setSelected(true);
        m_remoteTree->setCurrentItem(item);
    }
    const QVector<RemoteItem> items = selectedRemote();
    const int n = items.size();
    QMenu menu(this);
    menu.addAction(themedIcon(QStringLiteral("download.png")), tr("下载"), this, &FileBrowserDialog::downloadSelected)
        ->setEnabled(m_connected && n > 0);
    menu.addSeparator();
    menu.addAction(themedIcon(QStringLiteral("trash.png")), tr("删除"), this, &FileBrowserDialog::deleteRemote)
        ->setEnabled(m_connected && n > 0);
    menu.addAction(themedIcon(QStringLiteral("rename.png")), tr("重命名"), this, &FileBrowserDialog::renameRemote)
        ->setEnabled(m_connected && n == 1);
    menu.addSeparator();
    fillCommandMenu(menu.addMenu(themedIcon(QStringLiteral("command.png")), tr("文件自定义命令(&C)")), true);
    QMenu *names = menu.addMenu(themedIcon(QStringLiteral("name.png")), tr("文件名(&F)"));
    names->addAction(tr("复制文件名"), this, [this] { copyRemoteNames(false, false); })->setEnabled(n > 0);
    names->addAction(tr("复制完整路径"), this, [this] { copyRemoteNames(true, false); })->setEnabled(n > 0);
    names->addAction(tr("复制 SFTP 地址"), this, [this] { copyRemoteNames(true, true); })->setEnabled(n > 0);
    menu.addAction(themedIcon(QStringLiteral("props.png")), tr("属性(&P)"), this, &FileBrowserDialog::propsRemote)
        ->setEnabled(m_connected && n > 0);
    menu.addSeparator();
    menu.addAction(themedIcon(QStringLiteral("new.png")), tr("新建目录"), this, &FileBrowserDialog::newRemoteDir)
        ->setEnabled(m_connected);
    menu.addAction(tr("新建文件"), this, &FileBrowserDialog::newRemoteFile)->setEnabled(m_connected);
    menu.exec(m_remoteTree->viewport()->mapToGlobal(pos));
}

void FileBrowserDialog::fillCommandMenu(QMenu *menu, bool remoteSide)
{
    bool any = false;
    for (int i = 0; i < m_commands.size(); ++i) {
        if (m_commands.at(i).remote != remoteSide)
            continue;
        any = true;
        const int index = i;
        menu->addAction(m_commands.at(i).name, this, [this, index] { runCustomCommand(index); });
    }
    if (!any)
        menu->addAction(tr("（无）"))->setEnabled(false);
    menu->addSeparator();
    menu->addAction(tr("管理自定义命令..."), this, &FileBrowserDialog::manageCommands);
}

void FileBrowserDialog::uploadSelected()
{
    const QModelIndexList rows = m_localView->selectionModel()->selectedRows(0);
    if (rows.isEmpty() || !m_connected)
        return;
    QStringList localPaths;
    localPaths.reserve(rows.size());
    for (const QModelIndex &idx : rows)
        localPaths << m_fsModel->filePath(mapToSource(idx));
    // 任务收集（递归展开、链接跳过、同名目录冲突）是纯数据活，交给模型层
    const UploadBatch batch = collectUploadTasks(
        localPaths, m_remotePath, [this](const QString &name) { return remoteNameExists(name); });

    if (!batch.conflicts.isEmpty()
        && !FramelessDialog::question(
               this, tr("合并确认"),
               tr("远程已存在同名目录：\n%1\n\n合并上传？其中同名文件将被覆盖。")
                   .arg(batch.conflicts.join(QLatin1Char('\n')))))
        return;
    if (rows.size() == 1 && batch.files.size() == 1 && batch.dirs.isEmpty()
        && remoteNameExists(QFileInfo(batch.files.first().local).fileName())
        && !FramelessDialog::question(
               this, tr("覆盖确认"), tr("远程已存在 %1，覆盖？").arg(batch.files.first().remote)))
        return;
    if (batch.skippedLinks > 0)
        m_status->setText(tr("已跳过 %1 个链接").arg(batch.skippedLinks));

    // 建目录任务在前、文件在后入队；队列串行执行，保证目录先于其中的文件就位。
    // 批次 id 全部记下，收尾时（folderBatchFinished）刷新一次远程列表。
    for (const UploadDirTask &task : batch.dirs) {
        m_queue->noteFolderMkdir(task.remote);
        m_queue->trackFolderJob(m_queue->uploadDir(task.local, task.remote));
    }
    for (const UploadTask &task : batch.files)
        m_queue->trackFolderJob(m_queue->upload(task.local, task.remote, task.size));
}

void FileBrowserDialog::downloadSelected()
{
    const QVector<RemoteItem> items = selectedRemote();
    if (items.isEmpty() || !m_connected)
        return;
    QString skippedDirs;
    for (const RemoteItem &item : items) {
        if (item.isDir) {
            skippedDirs += item.name + QLatin1Char('\n');
            continue;
        }
        const QString remote = remoteJoin(m_remotePath, item.name);
        const QString local = localJoin(m_localPath, item.name);
        if (items.size() == 1 && QFileInfo::exists(local)) {
            if (!FramelessDialog::question(
                    this, tr("覆盖确认"), tr("本地已存在 %1，覆盖？").arg(local)))
                return;
        }
        m_queue->download(local, remote, qint64(item.size));
    }
    if (!skippedDirs.isEmpty()) {
        FramelessDialog::message(this, tr("不支持目录下载"),
                                 tr("以下是目录，已跳过（暂不支持递归下载）：\n%1").arg(skippedDirs));
    }
}

void FileBrowserDialog::deleteLocal()
{
    const QStringList paths = selectedLocalPaths();
    if (paths.isEmpty())
        return;
    if (!FramelessDialog::question(
            this, tr("删除确认"), tr("删除本地的 %n 个条目？", "", int(paths.size()))))
        return;
    QStringList failed;
    for (const QString &path : paths) {
        const QFileInfo info(path);
        bool ok = false;
        if (info.isDir())
            ok = QDir(path).rmdir(path);
        else
            ok = QFile::remove(path);
        if (!ok)
            failed << path;
    }
    if (!failed.isEmpty()) {
        FramelessDialog::message(this, tr("删除失败"),
                             tr("以下条目没能删除（目录需要是空的）：\n%1").arg(failed.join(QLatin1Char('\n'))));
    }
}

void FileBrowserDialog::deleteRemote()
{
    const QVector<RemoteItem> items = selectedRemote();
    if (items.isEmpty() || !m_connected)
        return;
    if (!FramelessDialog::question(
            this, tr("删除确认"), tr("删除远程的 %n 个条目？", "", int(items.size()))))
        return;
    for (const RemoteItem &item : items)
        m_client->removeEntry(remoteJoin(m_remotePath, item.name), item.isDir);
}

void FileBrowserDialog::renameLocal()
{
    const QStringList paths = selectedLocalPaths();
    if (paths.size() != 1)
        return;
    const QFileInfo info(paths.first());
    QString name;
    if (!askName(tr("重命名"), info.fileName(), &name) || name == info.fileName())
        return;
    const QString dest = info.dir().filePath(name);
    if (QFileInfo::exists(dest)) {
        FramelessDialog::message(this, tr("重命名"), tr("已存在 %1").arg(dest));
        return;
    }
    if (!QFile::rename(info.absoluteFilePath(), dest))
        FramelessDialog::message(this, tr("重命名"), tr("无法重命名 %1").arg(info.fileName()));
}

void FileBrowserDialog::renameRemote()
{
    const QVector<RemoteItem> items = selectedRemote();
    if (items.size() != 1 || !m_connected)
        return;
    QString name;
    if (!askName(tr("重命名"), items.first().name, &name) || name == items.first().name)
        return;
    m_client->renameEntry(remoteJoin(m_remotePath, items.first().name), remoteJoin(m_remotePath, name));
}

void FileBrowserDialog::editLocal()
{
    const QStringList paths = selectedLocalPaths();
    if (paths.size() != 1 || QFileInfo(paths.first()).isDir())
        return;
    const QString path = paths.first();
#ifdef Q_OS_WIN
    const QString native = QDir::toNativeSeparators(path);
    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_INVOKEIDLIST;
    info.lpVerb = L"edit";
    info.lpFile = reinterpret_cast<LPCWSTR>(native.utf16());
    info.nShow = SW_SHOWNORMAL;
    if (ShellExecuteExW(&info))
        return;
#endif
    QDesktopServices::openUrl(QUrl::fromLocalFile(path));
}

void FileBrowserDialog::editRemote()
{
    const QVector<RemoteItem> items = selectedRemote();
    if (!m_connected || items.size() != 1 || items.first().isDir)
        return;
    m_queue->fetch(true, remoteJoin(m_remotePath, items.first().name), qint64(items.first().size));
}

// 「在文件资源管理器中显示」：定位并高亮第一个选中项（explorer /select）；
// 无选中时打开当前目录。非 Windows / 调用失败时后退为打开所在目录
void FileBrowserDialog::revealLocal()
{
    const QStringList paths = selectedLocalPaths();
    if (paths.isEmpty()) {
        if (!m_localPath.isEmpty())
            QDesktopServices::openUrl(QUrl::fromLocalFile(m_localPath));
        return;
    }
#ifdef Q_OS_WIN
    const QString arg = QStringLiteral("/select,") + QDir::toNativeSeparators(paths.first());
    if (ShellExecuteW(nullptr, L"open", L"explorer.exe",
                      reinterpret_cast<LPCWSTR>(arg.utf16()), nullptr, SW_SHOWNORMAL) > HINSTANCE(32))
        return;
#endif
    QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(paths.first()).absolutePath()));
}

void FileBrowserDialog::fetchRemoteFile(const QString &remotePath)
{
    // 与「编辑」同一通道（edit=false：下载后自动打开，不监视回传）。
    // size 未知传 -1，进度条按字节流累计展示。
    if (!m_connected)
        return;
    m_queue->fetch(false, remotePath, -1);
}

void FileBrowserDialog::propsLocal()
{
    const QStringList paths = selectedLocalPaths();
    if (paths.size() != 1)
        return;
    LocalPropsDialog dlg(paths.first(), this);
    dlg.exec();
}

void FileBrowserDialog::propsRemote()
{
    const QVector<RemoteItem> items = selectedRemote();
    if (items.isEmpty() || !m_connected)
        return;
    QString title;
    QString extra;
    if (items.size() == 1) {
        const RemoteItem &item = items.first();
        title = item.name;
        const QString when = item.mtime > 0
                                 ? QDateTime::fromSecsSinceEpoch(item.mtime).toString(QStringLiteral("yyyy-MM-dd hh:mm"))
                                 : QStringLiteral("-");
        extra = tr("路径：%1\n类型：%2\n大小：%3\n修改时间：%4\n当前：%5")
                    .arg(remoteJoin(m_remotePath, item.name),
                         item.isLink ? tr("符号链接") : (item.isDir ? tr("目录") : tr("文件")),
                         item.isDir ? QStringLiteral("-") : humanSize(item.size),
                         when,
                         permString(item.permissions, item.isDir, item.isLink));
    } else {
        title = tr("%1 等 %n 个条目", "", int(items.size())).arg(items.first().name);
        extra = tr("下面的权限会写到全部所选条目。");
    }
    ChmodDialog dlg(title, extra, items.first().permissions, this);
    if (dlg.exec() != QDialog::Accepted)
        return;
    const quint32 mode = dlg.mode();
    for (const RemoteItem &item : items)
        m_client->setPermissions(remoteJoin(m_remotePath, item.name), mode);
}

void FileBrowserDialog::newLocalDir()
{
    QString name;
    if (!askName(tr("新建目录"), QString(), &name))
        return;
    if (!QDir(m_localPath).mkdir(name))
        FramelessDialog::message(this, tr("新建目录"), tr("无法创建 %1").arg(name));
}

void FileBrowserDialog::newLocalFile()
{
    QString name;
    if (!askName(tr("新建文件"), QString(), &name))
        return;
    const QString path = localJoin(m_localPath, name);
    if (QFileInfo::exists(path)) {
        FramelessDialog::message(this, tr("新建文件"), tr("已存在 %1").arg(name));
        return;
    }
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        FramelessDialog::message(this, tr("新建文件"), tr("无法创建 %1").arg(name));
}

void FileBrowserDialog::newRemoteDir()
{
    if (!m_connected)
        return;
    QString name;
    if (!askName(tr("新建目录"), QString(), &name))
        return;
    m_client->makeDir(remoteJoin(m_remotePath, name));
}

void FileBrowserDialog::newRemoteFile()
{
    if (!m_connected)
        return;
    QString name;
    if (!askName(tr("新建文件"), QString(), &name))
        return;
    m_client->createFile(remoteJoin(m_remotePath, name));
}

void FileBrowserDialog::refreshLocal()
{
    if (!m_localPath.isEmpty())
        m_fsModel->setRootPath(m_localPath);
}

void FileBrowserDialog::refreshRemote()
{
    if (m_connected && !m_remotePath.isEmpty())
        m_client->listDir(m_remotePath);
}

void FileBrowserDialog::manageCommands()
{
    CommandManagerDialog dlg(m_commands, this);
    if (dlg.exec() != QDialog::Accepted)
        return;
    m_commands = dlg.result();
    persistBrowserState();
}

void FileBrowserDialog::runCustomCommand(int index)
{
    if (index < 0 || index >= m_commands.size())
        return;
    const FileCommand command = m_commands.at(index);
    QStringList files;
    QString dir;
    if (command.remote) {
        if (!m_connected) {
            FramelessDialog::message(this, tr("自定义命令"), tr("SFTP 未连接"));
            return;
        }
        dir = m_remotePath;
        for (const RemoteItem &item : selectedRemote())
            files << remoteJoin(m_remotePath, item.name);
    } else {
        dir = m_localPath;
        files = selectedLocalPaths();
    }
    if (commandNeedsSelection(command.command) && files.isEmpty()) {
        FramelessDialog::message(this, tr("自定义命令"), tr("请先选择文件"));
        return;
    }
    const auto quote = command.remote ? shellQuote : cmdQuote;
    const auto expand = [&](const QString &one) {
        QStringList quoted;
        quoted.reserve(files.size());
        for (const QString &file : files)
            quoted << quote(file);
        QString text = command.command;
        text.replace(QLatin1String("!&"), quoted.join(QLatin1Char(' ')));
        text.replace(QLatin1String("!/"), quote(dir));
        text.replace(QLatin1String("!"), one.isEmpty() ? QString() : quote(one));
        return text;
    };
    if (command.remote)
        beginCommandBatch();
    if (commandUsesEachFile(command.command)) {
        for (const QString &file : files) {
            const QString line = expand(file);
            if (command.remote)
                queueRemoteCommand(line);
            else
                enqueueLocalCommand(line);
        }
    } else {
        const QString line = expand(QString());
        if (command.remote)
            queueRemoteCommand(line);
        else
            enqueueLocalCommand(line);
    }
    if (command.remote)
        endCommandBatch();
}

void FileBrowserDialog::showSystemMenu()
{
#ifdef Q_OS_WIN
    const QStringList paths = selectedLocalPaths();
    QStringList native;
    native.reserve(paths.size());
    for (const QString &path : paths)
        native << QDir::toNativeSeparators(path);
    if (!showWindowsShellMenu(native, winId()))
        FramelessDialog::message(this, tr("系统菜单"), tr("无法打开系统右键菜单"));
#else
    Q_UNUSED(m_shellMenuPos);
    FramelessDialog::message(this, tr("系统菜单"), tr("系统菜单仅在 Windows 上可用"));
#endif
}

void FileBrowserDialog::copyLocalNames(bool fullPath)
{
    const QStringList paths = selectedLocalPaths();
    QStringList lines;
    for (const QString &path : paths)
        lines << (fullPath ? QDir::toNativeSeparators(path) : QFileInfo(path).fileName());
    QGuiApplication::clipboard()->setText(lines.join(QLatin1Char('\n')));
    m_status->setText(tr("已复制 %n 条到剪贴板", "", int(lines.size())));
}

void FileBrowserDialog::copyRemoteNames(bool fullPath, bool asUrl)
{
    const QString user = m_target.user.isEmpty() ? QStringLiteral("root") : m_target.user;
    const int port = m_target.port > 0 ? m_target.port : 22;
    QStringList lines;
    for (const RemoteItem &item : selectedRemote()) {
        const QString path = remoteJoin(m_remotePath, item.name);
        if (asUrl) {
            QUrl url;
            url.setScheme(QStringLiteral("sftp"));
            url.setUserName(user);
            url.setHost(m_target.host);
            url.setPort(port);
            url.setPath(path);
            lines << url.toString();
        } else if (fullPath) {
            lines << path;
        } else {
            lines << item.name;
        }
    }
    QGuiApplication::clipboard()->setText(lines.join(QLatin1Char('\n')));
    m_status->setText(tr("已复制 %n 条到剪贴板", "", int(lines.size())));
}

void FileBrowserDialog::beginCommandBatch()
{
    m_execBatching = true;
    m_execLeft = 0;
    m_execFailed = false;
    m_execRemote = false;
    m_execLog.clear();
}

void FileBrowserDialog::queueRemoteCommand(const QString &command)
{
    ++m_execLeft;
    m_execRemote = true;
    m_client->execCommand(command);
}

void FileBrowserDialog::endCommandBatch()
{
    m_execBatching = false;
    if (m_execLeft <= 0)
        flushExecUi();
}

void FileBrowserDialog::flushExecUi()
{
    if (m_execRemote && m_connected)
        refreshRemote();
    if (!m_execLog.trimmed().isEmpty())
        showCommandOutput(this, m_execFailed ? tr("命令失败") : tr("命令输出"), m_execLog);
    else if (m_execFailed)
        FramelessDialog::message(this, tr("命令失败"), tr("远程命令执行失败"));
    else
        m_status->setText(tr("命令已完成"));
    m_execLog.clear();
    m_execFailed = false;
    m_execRemote = false;
    m_execLeft = 0;
}

void FileBrowserDialog::handleExecFinished(bool ok, const QString &command, const QString &detail)
{
    if (!ok)
        m_execFailed = true;
    if (!detail.trimmed().isEmpty()) {
        m_execLog += QStringLiteral("$ ") + command + QLatin1Char('\n') + detail.trimmed()
                     + QStringLiteral("\n\n");
    } else if (!ok) {
        m_execLog += QStringLiteral("$ ") + command + QStringLiteral("\n失败\n\n");
    }
    if (m_execLeft > 0)
        --m_execLeft;
    if (m_execBatching || m_execLeft > 0)
        return;
    flushExecUi();
}

void FileBrowserDialog::enqueueLocalCommand(const QString &command)
{
    const bool idle = m_localCmdQueue.isEmpty() && m_localProc->state() == QProcess::NotRunning;
    if (idle) {
        m_localOutput.clear();
        m_localFailed = false;
    }
    m_localCmdQueue.append(command);
    pumpLocalCommand();
}

void FileBrowserDialog::pumpLocalCommand()
{
    if (m_localProc->state() != QProcess::NotRunning)
        return;
    if (m_localCmdQueue.isEmpty()) {
        if (!m_localOutput.trimmed().isEmpty())
            showCommandOutput(this, m_localFailed ? tr("命令失败") : tr("命令输出"), m_localOutput);
        else if (m_localFailed)
            FramelessDialog::message(this, tr("命令失败"), tr("本地命令执行失败"));
        else
            m_status->setText(tr("命令已完成"));
        m_localOutput.clear();
        m_localFailed = false;
        return;
    }
    m_localRunningCmd = m_localCmdQueue.takeFirst();
    m_localProc->setWorkingDirectory(m_localPath);
    m_localProc->start(QStringLiteral("cmd.exe"), { QStringLiteral("/c"), m_localRunningCmd });
}

bool FileBrowserDialog::askName(const QString &title, const QString &initial, QString *out)
{
    bool ok = false;
    const QString name = FramelessDialog::getText(this, title, tr("名称:"), initial, &ok).trimmed();
    if (!ok)
        return false;
    if (!validFileName(name)) {
        FramelessDialog::message(this, title, tr("名称无效"));
        return false;
    }
    *out = name;
    return true;
}

bool FileBrowserDialog::remoteNameExists(const QString &name) const
{
    const int count = m_remoteTree->topLevelItemCount();
    for (int i = 0; i < count; ++i) {
        if (m_remoteTree->topLevelItem(i)->text(0) == name)
            return true;
    }
    return false;
}

void FileBrowserDialog::setLocalPath(const QString &path)
{
    if (path.isEmpty()) {
        m_localPath.clear();
        QString anchor = QStringLiteral("C:/");
        const QFileInfoList drives = QDir::drives();
        if (!drives.isEmpty())
            anchor = drives.first().absoluteFilePath();
        const QModelIndex drive = m_fsModel->index(anchor);
        m_localView->setRootIndex(m_fsProxy->mapFromSource(
            drive.isValid() ? drive.parent() : QModelIndex()));
        syncLocalBar();
        return;
    }
    const QDir dir(path);
    if (!dir.exists() || !dir.isReadable())
        return;
    m_localPath = dir.absolutePath();
    m_localView->setRootIndex(m_fsProxy->mapFromSource(m_fsModel->index(m_localPath)));
    syncLocalBar();
    persistBrowserState();
}

void FileBrowserDialog::syncLocalBar()
{
    QVector<QPair<QString, QString>> segments;
    segments.append(qMakePair(tr("此电脑"), QString()));
    QString editText;
    if (!m_localPath.isEmpty()) {
        const QString native = QDir::toNativeSeparators(QDir::cleanPath(m_localPath));
        editText = native;
        if (native.startsWith(QLatin1String("\\\\"))) {
            segments.append(qMakePair(native, m_localPath));
        } else {
            const QStringList parts = native.split(QLatin1Char('\\'), Qt::SkipEmptyParts);
            QString acc;
            for (int i = 0; i < parts.size(); ++i) {
                if (i == 0 && parts.at(i).endsWith(QLatin1Char(':')))
                    acc = parts.at(i) + QLatin1Char('/');
                else if (acc.isEmpty())
                    acc = parts.at(i);
                else
                    acc = QDir::cleanPath(acc + QLatin1Char('/') + parts.at(i));
                segments.append(qMakePair(parts.at(i), QDir::fromNativeSeparators(QDir::cleanPath(acc))));
            }
        }
    }
    m_localPathBar->setSegments(segments, editText, QStringLiteral("\\"));
}

void FileBrowserDialog::syncRemoteBar()
{
    const QString path = m_remotePath.isEmpty() ? QStringLiteral("/") : m_remotePath;
    QVector<QPair<QString, QString>> segments;
    segments.append(qMakePair(QStringLiteral("/"), QStringLiteral("/")));
    QString acc;
    const QStringList parts = path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    for (const QString &part : parts) {
        acc += QLatin1Char('/') + part;
        segments.append(qMakePair(part, acc));
    }
    m_remotePathBar->setSegments(segments, path, QStringLiteral("/"));
}

void FileBrowserDialog::setRemotePath(const QString &path)
{
    QString normalized = path;
    if (normalized.isEmpty())
        normalized = QStringLiteral("/");
    if (!normalized.startsWith(QLatin1Char('/')) || normalized.contains(QLatin1String(".."))) {
        m_remotePath = normalized;
        syncRemoteBar();
        m_client->realPath(normalized);
        return;
    }
    m_remotePath = normalized;
    syncRemoteBar();
    refreshRemote();
}

QStringList FileBrowserDialog::selectedLocalPaths() const
{
    QStringList paths;
    const QModelIndexList rows = m_localView->selectionModel()->selectedRows(0);
    paths.reserve(rows.size());
    for (const QModelIndex &idx : rows)
        paths << m_fsModel->filePath(mapToSource(idx));
    return paths;
}

// 视图（经排序代理）索引 → QFileSystemModel 源索引；文件路径与目录判断都以源模型为准
QModelIndex FileBrowserDialog::mapToSource(const QModelIndex &viewIndex) const
{
    return m_fsProxy ? m_fsProxy->mapToSource(viewIndex) : viewIndex;
}

QVector<RemoteItem> FileBrowserDialog::selectedRemote() const
{
    QVector<RemoteItem> items;
    const QList<QTreeWidgetItem *> selected = m_remoteTree->selectedItems();
    items.reserve(selected.size());
    for (QTreeWidgetItem *item : selected) {
        RemoteItem row;
        row.name = item->text(0);
        row.isDir = item->data(0, kRoleDir).toBool();
        row.isLink = item->data(0, kRoleLink).toBool();
        row.size = item->data(0, kRoleSize).toULongLong();
        row.permissions = item->data(0, kRolePerm).toUInt();
        row.mtime = item->data(0, kRoleMtime).toLongLong();
        items.append(row);
    }
    return items;
}
