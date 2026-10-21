#include "shellchannel.h"

#include <QThread>
#include <QtGlobal>

#include <limits>

ShellChannel::ShellChannel(LibsshApi &api, ssh_session session, int cols,
                           int rows)
    : m_api(api),
      m_ownerThread(QThread::currentThread()),
      m_session(session),
      m_cols(qMax(1, cols)),
      m_rows(qMax(1, rows))
{
    m_channel = m_api.newChannel(m_session);
}

ShellChannel::~ShellChannel()
{
    if (onOwnerThread()) {
        releaseChannel();
    }
}

ShellChannel::Result ShellChannel::advance(QList<QByteArray> *output,
                                           QString *error)
{
    if (!onOwnerThread()) {
        if (error != nullptr) {
            *error = QStringLiteral(
                "Shell channel called from non-owner thread");
        }
        return Result::Error;
    }
    if (m_closed || m_channel == nullptr) {
        if (!m_closed) {
            return fail(error, QStringLiteral("allocate shell channel"));
        }
        return Result::Closed;
    }
    if (m_step == Step::ClosingSendEof
        || m_step == Step::ClosingChannel) {
        return advanceClosing(error);
    }
    if (m_step != Step::Ready) {
        return advanceOpening(error);
    }
    return advanceReady(output, error);
}

bool ShellChannel::queueInput(const QByteArray &data)
{
    if (!onOwnerThread()) {
        return false;
    }
    if (data.isEmpty()) {
        return true;
    }
    const qsizetype queued = m_pendingInput.size() - m_writeOffset;
    if (data.size() > MaxPendingInput - queued) {
        return false;
    }
    if (m_writeOffset > 0) {
        m_pendingInput.remove(0, m_writeOffset);
        m_writeOffset = 0;
    }
    m_pendingInput.append(data);
    return true;
}

void ShellChannel::resize(int cols, int rows)
{
    if (!onOwnerThread()) {
        return;
    }
    m_pendingCols = qMax(1, cols);
    m_pendingRows = qMax(1, rows);
    m_resizePending = true;
}

void ShellChannel::close()
{
    if (!onOwnerThread() || m_closed
        || m_step == Step::ClosingSendEof
        || m_step == Step::ClosingChannel) {
        return;
    }
    if (m_channel == nullptr) {
        m_closed = true;
        m_step = Step::Closed;
        return;
    }
    m_step = Step::ClosingSendEof;
}

bool ShellChannel::isReady() const
{
    return onOwnerThread() && m_step == Step::Ready && !m_closed;
}

bool ShellChannel::isClosing() const
{
    return onOwnerThread()
        && (m_step == Step::ClosingSendEof
            || m_step == Step::ClosingChannel);
}

ShellChannel::Result ShellChannel::advanceOpening(QString *error)
{
    int result = SSH_ERROR;
    switch (m_step) {
    case Step::OpenSession:
        result = m_api.openChannelSession(m_channel);
        break;
    case Step::RequestPty:
        result = m_api.requestPtySize(
            m_channel, QByteArrayLiteral("xterm"), m_cols, m_rows);
        break;
    case Step::SetTerm:
        result = m_api.requestEnvironment(
            m_channel, QByteArrayLiteral("TERM"),
            QByteArrayLiteral("xterm"));
        break;
    case Step::SetColorTerm:
        result = m_api.requestEnvironment(
            m_channel, QByteArrayLiteral("COLORTERM"),
            QByteArrayLiteral("truecolor"));
        break;
    case Step::RequestShell:
        result = m_api.requestShell(m_channel);
        break;
    case Step::ClosingSendEof:
    case Step::ClosingChannel:
        return advanceClosing(error);
    case Step::Ready:
        return Result::Ready;
    case Step::Closed:
        return Result::Closed;
    }

    if (result == SSH_AGAIN) {
        return Result::Again;
    }
    if (result != SSH_OK) {
        return fail(error, QStringLiteral("open remote shell"));
    }

    switch (m_step) {
    case Step::OpenSession:
        m_step = Step::RequestPty;
        break;
    case Step::RequestPty:
        m_step = Step::SetTerm;
        break;
    case Step::SetTerm:
        m_step = Step::SetColorTerm;
        break;
    case Step::SetColorTerm:
        m_step = Step::RequestShell;
        break;
    case Step::RequestShell:
        m_step = Step::Ready;
        return Result::Ready;
    case Step::Ready:
    case Step::ClosingSendEof:
    case Step::ClosingChannel:
    case Step::Closed:
        break;
    }
    return Result::Progress;
}

ShellChannel::Result
ShellChannel::advanceReady(QList<QByteArray> *output, QString *error)
{
    if (m_api.channelIsEof(m_channel)) {
        close();
        return advanceClosing(error);
    }

    bool progressed = false;
    bool sawAgain = false;
    for (int i = 0; i < KindCount; ++i) {
        const int kind = (m_ioCursor + i) % KindCount;
        Result result = Result::Again;
        switch (kind) {
        case Resize:
            if (!m_resizePending) {
                continue;
            }
            result = serviceResize(error);
            break;
        case Write:
            if (m_writeOffset >= m_pendingInput.size()) {
                continue;
            }
            result = serviceWrite(error);
            break;
        case Stdout:
            result = serviceRead(output, error, false);
            break;
        case Stderr:
            result = serviceRead(output, error, true);
            break;
        case KindCount:
            break;
        }
        if (result == Result::Error || result == Result::Closed) {
            return result;
        }
        if (result == Result::Progress) {
            progressed = true;
        } else if (result == Result::Again) {
            sawAgain = true;
        }
    }
    m_ioCursor = (m_ioCursor + 1) % KindCount;
    return progressed ? Result::Progress
                      : (sawAgain ? Result::Again : Result::Progress);
}

ShellChannel::Result ShellChannel::serviceResize(QString *error)
{
    const int result = m_api.changePtySize(
        m_channel, m_pendingCols, m_pendingRows);
    if (result == SSH_AGAIN) {
        return Result::Again;
    }
    if (result != SSH_OK) {
        return fail(error, QStringLiteral("resize remote PTY"));
    }
    m_cols = m_pendingCols;
    m_rows = m_pendingRows;
    m_resizePending = false;
    return Result::Progress;
}

ShellChannel::Result ShellChannel::serviceWrite(QString *error)
{
    const qsizetype remaining = m_pendingInput.size() - m_writeOffset;
    const int result = m_api.writeChannel(
        m_channel, m_pendingInput.constData() + m_writeOffset,
        quint32(qMin<quint64>(
            quint64(remaining),
            std::numeric_limits<quint32>::max())));
    if (result == SSH_AGAIN || result == 0) {
        return Result::Again;
    }
    if (result < 0) {
        return fail(error, QStringLiteral("write remote shell"));
    }
    m_writeOffset += result;
    if (m_writeOffset == m_pendingInput.size()) {
        m_pendingInput.clear();
        m_writeOffset = 0;
    }
    return Result::Progress;
}

ShellChannel::Result
ShellChannel::serviceRead(QList<QByteArray> *output, QString *error,
                          bool stderrStream)
{
    QByteArray bytes(ReadBufferSize, Qt::Uninitialized);
    const int result = m_api.readChannelNonblocking(
        m_channel, bytes.data(), quint32(bytes.size()), stderrStream);
    if (result == SSH_EOF) {
        close();
        return advanceClosing(error);
    }
    if (result == SSH_AGAIN) {
        return Result::Again;
    }
    if (result < 0) {
        return fail(error, QStringLiteral("read remote shell"));
    }
    if (result == 0) {
        return Result::Again;
    }
    bytes.resize(result);
    output->append(bytes);
    return Result::Progress;
}

ShellChannel::Result ShellChannel::advanceClosing(QString *error)
{
    if (m_channel == nullptr) {
        m_closed = true;
        m_step = Step::Closed;
        return Result::Closed;
    }

    if (m_step == Step::ClosingSendEof) {
        const int result = m_api.sendChannelEof(m_channel);
        if (result == SSH_AGAIN) {
            return Result::Again;
        }
        if (result != SSH_OK && result != SSH_EOF) {
            releaseChannel();
            return fail(error, QStringLiteral("send remote shell EOF"));
        }
        m_step = Step::ClosingChannel;
        return Result::Progress;
    }

    const int result = m_api.closeChannel(m_channel);
    if (result == SSH_AGAIN) {
        return Result::Again;
    }
    if (result != SSH_OK && result != SSH_EOF) {
        releaseChannel();
        return fail(error, QStringLiteral("close remote shell"));
    }
    releaseChannel();
    return Result::Closed;
}

ShellChannel::Result ShellChannel::fail(QString *error,
                                        const QString &operation)
{
    if (error != nullptr) {
        const QString detail = m_api.errorString(m_session);
        *error = detail.isEmpty()
            ? operation
            : QStringLiteral("%1: %2").arg(operation, detail);
    }
    return Result::Error;
}

void ShellChannel::releaseChannel()
{
    if (m_channel != nullptr) {
        m_api.freeChannel(m_channel);
        m_channel = nullptr;
    }
    m_pendingInput.clear();
    m_writeOffset = 0;
    m_closed = true;
    m_step = Step::Closed;
}

bool ShellChannel::onOwnerThread() const
{
    return QThread::currentThread() == m_ownerThread;
}
