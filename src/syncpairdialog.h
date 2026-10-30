/*
 * Copyright 2026 Arktos Browser - IT-Space
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#ifndef SYNCPAIRDIALOG_H
#define SYNCPAIRDIALOG_H

#include <qdialog.h>
#include <qjsonobject.h>

class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QThread;

// SYNC03: the sync pairing ceremony UI.  Invite mode renders the
// one-time QR payload + grouped manual code; join mode takes the
// pasted payload/code.  Both then run the Noise handshake under a
// poll timer and show the 6-word/6-emoji fingerprint for the
// out-of-band confirm on BOTH ends — the MITM check.  mw ops that
// can block (transport_start, pair_join's connect) run on a worker
// thread; everything else is a quick envelope round trip.
class SyncPairDialog : public QDialog
{
    Q_OBJECT

public:
    // Show the invite side ("pair THIS device with another").
    static void invite(const QString &profileId, QWidget *parent);
    // Show the join side ("this device joins another's group").
    static void join(const QString &profileId, QWidget *parent);

private slots:
    void poll();
    void onConfirmed();
    void onRejected();
    void onCancel();
    void onJoinStarted();
    void onWorkerResult(int kind, const QJsonObject &doc);

private:
    explicit SyncPairDialog(bool inviter, const QString &profileId,
                          QWidget *parent);

    void mwCall(int kind, int op, const QJsonObject &payload);
    void applyStatus(const QJsonObject &doc);

    bool m_inviter;
    QString m_profileId;
    quint64 m_pairId = 0;
    bool m_pollInFlight = false;
    QLabel *m_qrLabel;
    QLineEdit *m_codeLabel = nullptr;
    QPushButton *m_copyButton = nullptr;
    QPlainTextEdit *m_payloadEdit;
    QPushButton *m_startButton;
    QLabel *m_statusLabel;
    QLabel *m_wordsLabel;
    QLabel *m_emojiLabel;
    QPushButton *m_confirmButton;
    QPushButton *m_rejectButton;
    QPushButton *m_cancelButton;
    QTimer *m_timer;
    QThread *m_thread;
    QObject *m_workerHolder = nullptr;
};

#endif // SYNCPAIRDIALOG_H
