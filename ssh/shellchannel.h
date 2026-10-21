#pragma once

#include "libsshapi.h"

#include <QByteArray>
#include <QList>
#include <QString>

class ShellChannel final
{
public:
    enum class Result {
        Progress,
        Again,
        Ready,
        Closed,
        Error
    };

    ShellChannel(LibsshApi &api, ssh_session session, int cols, int rows);
    ~ShellChannel();

    ShellChannel(const ShellChannel &) = delete;
    ShellChannel &operator=(const ShellChannel &) = delete;

    Result advance(QList<QByteArray> *output, QString *error);
    bool queueInput(const QByteArray &data);
    void resize(int cols, int rows);
    void close();
    bool isReady() const;

private:
    enum class Step {
        OpenSession,
        RequestPty,
        SetTerm,
        SetColorTerm,
        RequestShell,
        Ready,
        Closed
    };

    Result advanceOpening(QString *error);
    Result advanceReady(QList<QByteArray> *output, QString *error);
    Result fail(QString *error, const QString &operation);

    static constexpr qsizetype MaxPendingInput = 1024 * 1024;
    static constexpr int IoBudgetPerTick = 8;
    static constexpr int ReadBufferSize = 16 * 1024;

    LibsshApi &m_api;
    ssh_session m_session = nullptr;
    ssh_channel m_channel = nullptr;
    Step m_step = Step::OpenSession;
    QByteArray m_pendingInput;
    qsizetype m_writeOffset = 0;
    int m_cols = 80;
    int m_rows = 24;
    int m_pendingCols = 0;
    int m_pendingRows = 0;
    bool m_resizePending = false;
    bool m_readStderrNext = false;
    bool m_closed = false;
};
