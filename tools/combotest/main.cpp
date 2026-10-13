// baudCombo 弹出层卡顿基准 v2。
//
// 指标：
//   sync-open : showPopup() 调用本身的同步耗时（布局/尺寸计算/容器 show）
//   open->ui  : showPopup() 到弹出层真正画出第一帧（viewport 的 Paint/Expose）
//   step      : 弹出状态下换一格选中项（等价滚轮/方向键一步）的耗时
// 场景:
//   1 bare no-QSS      裸可编辑下拉，无样式 —— 基线
//   2 plain QSS        应用真实深色 QSS —— 单看 QSS 开销
//   3 baud QSS         再加 baudCombo 完整配置（校验器/tune）
//   4 baud QSS wired   再接上等价 MainWindow 的信号链（含防抖定时器重启）
#include <QApplication>
#include <QComboBox>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEvent>
#include <QFile>
#include <QIntValidator>
#include <QLabel>
#include <QListView>
#include <QTimer>
#include <QVBoxLayout>
#include <QVector>
#include <algorithm>
#include <cstdio>

namespace {

QElapsedTimer g_clock;
qint64 g_openedAtNs = -1; // 本轮 showPopup() 起始；<0 = 不在测量窗口
qint64 g_uiNs = -1;       // 第一帧（Paint/Expose whichever first）到达时刻
QObject *g_probeA = nullptr; // 弹出容器
QObject *g_probeB = nullptr; // 视口（列表真正绘制的地方）

class UiProbe : public QObject
{
public:
    using QObject::QObject;
    bool eventFilter(QObject *obj, QEvent *ev) override
    {
        if (g_openedAtNs < 0 || g_uiNs >= 0)
            return false;
        if (obj != g_probeA && obj != g_probeB)
            return false;
        if (ev->type() == QEvent::Paint || ev->type() == QEvent::Expose)
            g_uiNs = g_clock.nsecsElapsed();
        return false;
    }
};

void pump(qint64 ms)
{
    const qint64 end = g_clock.elapsed() + ms;
    while (g_clock.elapsed() < end)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 2);
}

void fillBaud(QComboBox *c)
{
    const char *items[] = {"1200", "2400", "4800", "9600", "19200", "38400",
                           "57600", "115200", "230400", "460800", "921600",
                           "1000000", "1500000", "2000000"};
    for (const char *s : items)
        c->addItem(QString::fromLatin1(s));
    c->setCurrentText(QStringLiteral("115200"));
}

// 复刻 MainWindow::tuneEditableCombo
void tuneLikeApp(QComboBox *c)
{
    c->setCompleter(nullptr);
    c->setMaxVisibleItems(20);
    if (auto *list = qobject_cast<QListView *>(c->view()))
        list->setUniformItemSizes(true);
    c->setValidator(new QIntValidator(1, 4000000, c));
}

struct Result
{
    QVector<qint64> syncNs; // showPopup() 同步耗时
    QVector<qint64> uiNs;   // showPopup() → 第一帧 Paint/Expose
    QVector<qint64> stepNs; // 换一格
};

Result runScenario(QComboBox *combo, int rounds)
{
    Result r;
    combo->showPopup(); // 首次创建容器 + 风格计算，不计入样本
    pump(80);
    combo->hidePopup();
    pump(40);

    QObject *container = combo->view()->window();
    QObject *vp = combo->view()->viewport();
    auto *probe = new UiProbe(combo);
    container->installEventFilter(probe);
    vp->installEventFilter(probe);
    g_probeA = container;
    g_probeB = vp;

    for (int i = 0; i < rounds; ++i) {
        g_uiNs = -1;
        const qint64 t0 = g_clock.nsecsElapsed();
        g_openedAtNs = t0;
        combo->showPopup();
        r.syncNs.append(g_clock.nsecsElapsed() - t0);
        pump(250); // 等第一帧（等不到记 -1）
        r.uiNs.append(g_uiNs < 0 ? -1 : g_uiNs - t0);
        g_openedAtNs = -1; // 换项阶段不再计帧

        for (int k = 1; k <= 10; ++k) {
            const qint64 s0 = g_clock.nsecsElapsed();
            combo->setCurrentIndex((i * 5 + k) % combo->count());
            r.stepNs.append(g_clock.nsecsElapsed() - s0);
            pump(4);
        }
        combo->hidePopup();
        pump(40);
    }
    g_probeA = g_probeB = nullptr;
    return r;
}

void report(const char *name, const Result &r)
{
    auto stats = [](QVector<qint64> v, double &mn, double &md, double &mx) {
        v.erase(std::remove_if(v.begin(), v.end(),
                               [](qint64 x) { return x < 0; }),
                v.end());
        if (v.isEmpty()) {
            mn = md = mx = -1;
            return;
        }
        std::sort(v.begin(), v.end());
        mn = v.first() / 1e6;
        md = v.at(v.size() / 2) / 1e6;
        mx = v.last() / 1e6;
    };
    double a1, a2, a3, b1, b2, b3, c1, c2, c3;
    stats(r.syncNs, a1, a2, a3);
    stats(r.uiNs, b1, b2, b3);
    stats(r.stepNs, c1, c2, c3);
    printf("%-18s sync-open ms: %7.2f/%7.2f/%7.2f"
           "  open->ui ms: %7.2f/%7.2f/%7.2f"
           "  step ms: %6.3f/%6.3f\n",
           name, a1, a2, a3, b1, b2, b3, c2, c3);
    fflush(stdout);
}

} // namespace

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    g_clock.start();

    const QString qssPath = argc > 1
        ? QString::fromLocal8Bit(argv[1])
        : QStringLiteral("../../resources/style-dark.qss");

    QWidget host;
    host.setWindowTitle(QStringLiteral("combotest"));
    auto *lay = new QVBoxLayout(&host);
    lay->setContentsMargins(12, 12, 12, 12);
    lay->setSpacing(10);

    auto *cBare = new QComboBox(&host);
    cBare->setEditable(true);
    fillBaud(cBare);
    lay->addWidget(cBare);

    auto *cBaud = new QComboBox(&host);
    cBaud->setEditable(true);
    fillBaud(cBaud);
    tuneLikeApp(cBaud);
    lay->addWidget(cBaud);

    auto *cWired = new QComboBox(&host);
    cWired->setEditable(true);
    fillBaud(cWired);
    tuneLikeApp(cWired);
    QTimer saveTimer; // 复刻 MainWindow 的防抖定时器
    saveTimer.setSingleShot(true);
    QObject::connect(cWired, &QComboBox::currentTextChanged, &saveTimer, [&] {
        // 等价 currentProfileFromUi()+upsert()：10 条命令的字符串列表拷贝
        QStringList texts;
        for (int i = 0; i < 10; ++i)
            texts << QStringLiteral("sudo cmd%1 --flag value").arg(i);
        const QStringList copy = texts;
        Q_UNUSED(copy.size());
        saveTimer.start(400);
    });
    lay->addWidget(cWired);

    lay->addWidget(new QLabel(QStringLiteral("measuring..."), &host), 1);
    host.show();
    pump(300);

    report("1 bare no-QSS", runScenario(cBare, 12));

    QFile f(qssPath);
    if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        qApp->setStyleSheet(QString::fromUtf8(f.readAll()));
        pump(200);
        report("2 plain QSS", runScenario(cBare, 12));
        report("3 baud QSS", runScenario(cBaud, 12));
        report("4 baud QSS wired", runScenario(cWired, 12));
    } else {
        printf("QSS not found: %s\n", qPrintable(qssPath));
    }
    printf("done\n");
    return 0;
}
