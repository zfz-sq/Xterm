#include "shellchannel.h"

#include <QtGlobal>

#include <limits>

ShellChannel::ShellChannel(LibsshApi &api, ssh_session session, int cols,
                           int rows)
    : m_api(api),
      m_session(session),
      m_cols(qMax(1, cols)),
      m_rows(qMax(1, rows))
{
    m_channel = m_api.newChannel(m_session);
}

ShellChannel::~ShellChannel()
{
    close();
}

ShellChannel::Result ShellChannel::advance(QList<QByteArray> *output,
                                           QString *error)
{
    if (m_closed || m_channel == nullptr) {
        if (!m_closed) {
            return fail(error, QStringLiteral("allocate shell channel"));
        }
        return Result::Closed;
    }
    if (m_step != Step::Ready) {
        return advanceOpening(error);
    }
    return advanceReady(output, error);
}

bool ShellChannel::queueInput(const QByteArray &data)
{
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
    m_pendingCols = qMax(1, cols);
    m_pendingRows = qMax(1, rows);
    m_resizePending = true;
}

void ShellChannel::close()
{
    if (m_closed) {
        return;
    }
    m_closed = true;
    m_step = Step::Closed;
    if (m_channel == nullptr) {
        return;
    }
    m_api.sendChannelEof(m_channel);
    m_api.closeChannel(m_channel);
    m_api.freeChannel(m_channel);
    m_channel = nullptr;
    m_pendingInput.clear();
    m_writeOffset = 0;
}

bool ShellChannel::isReady() const
{
    return m_step == Step::Ready && !m_closed;
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
    case Step::Closed:
        break;
    }
    return Result::Progress;
}

ShellChannel::Result
ShellChannel::advanceReady(QList<QByteArray> *output, QString *error)
{
    for (int operation = 0; operation < IoBudgetPerTick; ++operation) {
        if (m_api.channelIsEof(m_channel)) {
            return Result::Closed;
        }

        if (m_resizePending) {
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
            continue;
        }

        if (m_writeOffset < m_pendingInput.size()) {
            const qsizetype remaining =
                m_pendingInput.size() - m_writeOffset;
            const int result = m_api.writeChannel(
                m_channel, m_pendingInput.constData() + m_writeOffset,
                quint32(qMin<quint64>(
                    quint64(remaining),
                    std::numeric_limits<quint32>::max())));
            if (result == SSH_AGAIN) {
                return Result::Again;
            }
            if (result < 0) {
                return fail(error, QStringLiteral("write remote shell"));
            }
            if (result == 0) {
                return Result::Again;
            }
            m_writeOffset += result;
            if (m_writeOffset == m_pendingInput.size()) {
                m_pendingInput.clear();
                m_writeOffset = 0;
            }
            continue;
        }

        QByteArray bytes(ReadBufferSize, Qt::Uninitialized);
        const bool stderrStream = m_readStderrNext;
        m_readStderrNext = !m_readStderrNext;
        const int result = m_api.readChannelNonblocking(
            m_channel, bytes.data(), quint32(bytes.size()), stderrStream);
        if (result == SSH_AGAIN) {
            return Result::Again;
        }
        if (result < 0) {
            return fail(error, QStringLiteral("read remote shell"));
        }
        if (result == 0) {
            continue;
        }
        bytes.resize(result);
        output->append(bytes);
    }
    return Result::Progress;
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
