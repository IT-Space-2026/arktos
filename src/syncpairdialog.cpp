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

#include "syncpairdialog.h"

#include "qrcodedialog.h"
#include "rustcorebridge.h"

#include <qboxlayout.h>
#include <qclipboard.h>
#include <qfontdatabase.h>
#include <qguiapplication.h>
#include <qgroupbox.h>
#include <qjsonarray.h>
#include <qjsonvalue.h>
#include <qlabel.h>
#include <qlineedit.h>
#include <qplaintextedit.h>
#include <qpushbutton.h>
#include <qpixmap.h>
#include <qthread.h>
#include <qtimer.h>

// Runs mw calls off the GUI thread (transport_start's bootstrap and
// pair_join's connect can block for tens of seconds on Tor).
class SyncPairWorker : public QObject
{
    Q_OBJECT
public:
    using QObject::QObject;
public slots:
    void call(int kind, int op, QJsonObject payload)
    {
#ifdef ARKTOS_RUSTCORE
        RustCoreBridge *b = RustCoreBridge::instance();
        QJsonObject err;
        QJsonValue v = b
            ? b->mwCommand(AcMw::Sync, op, payload, &err)
            : QJsonValue();
        emit result(kind, v.isObject() ? v.toObject() : err);
#else
        // No-rust build: there is no mw host — answer every call
        // with an envelope error the dialog renders verbatim.
        Q_UNUSED(op);
        Q_UNUSED(payload);
        emit result(kind, QJsonObject{
            {QStringLiteral("code"), QStringLiteral("unavailable")},
            {QStringLiteral("status"), QStringLiteral("error")},
            {QStringLiteral("message"), QStringLiteral(
                "Sync is unavailable — the Rust core is not loaded.")}});
#endif
    }
signals:
    void result(int kind, const QJsonObject &doc);
};

enum CallKind { CallBegin = 1, CallJoin = 2, CallTransport = 3 };

SyncPairDialog::SyncPairDialog(bool inviter, const QString &profileId,
                               QWidget *parent)
    : QDialog(parent)
    , m_inviter(inviter)
    , m_profileId(profileId)
{
    setWindowTitle(inviter ? tr("Pair a New Device")
                           : tr("Join a Sync Group"));
    setModal(false);
    setAttribute(Qt::WA_DeleteOnClose);
    auto *lay = new QVBoxLayout(this);

    m_statusLabel = new QLabel(this);
    m_statusLabel->setWordWrap(true);

    if (inviter) {
        m_qrLabel = new QLabel(this);
        m_qrLabel->setAlignment(Qt::AlignCenter);
        m_qrLabel->setMinimumSize(240, 240);
        m_codeLabel = new QLineEdit(this);
        m_codeLabel->setReadOnly(true);
        m_codeLabel->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
        m_codeLabel->setAlignment(Qt::AlignCenter);
        m_codeLabel->setMinimumHeight(30);
        lay->addWidget(new QLabel(
            tr("Have the other device scan this code:"), this));
        lay->addWidget(m_qrLabel, 0, Qt::AlignCenter);
        auto *codeLbl = new QLabel(tr("…or type the pairing code:"), this);
        lay->addWidget(codeLbl);
        auto *codeRow = new QHBoxLayout;
        codeRow->addWidget(m_codeLabel, 1);
        m_copyButton = new QPushButton(tr("Copy"), this);
        codeRow->addWidget(m_copyButton);
        lay->addLayout(codeRow);
        connect(m_copyButton, &QPushButton::clicked, this, [this] {
            QGuiApplication::clipboard()->setText(m_codeLabel->text());
            m_copyButton->setText(tr("Copied"));
            QTimer::singleShot(1500, this, [this] {
                m_copyButton->setText(tr("Copy"));
            });
        });
        mwCall(CallTransport, AcMw::SyncTransportStart,
               QJsonObject{{QStringLiteral("backend"), QStringLiteral("tor")}});
    } else {
        m_payloadEdit = new QPlainTextEdit(this);
        m_payloadEdit->setPlaceholderText(
            tr("Paste the QR payload or the grouped pairing code"));
        m_startButton = new QPushButton(tr("Start Pairing"), this);
        lay->addWidget(m_payloadEdit);
        lay->addWidget(m_startButton);
        connect(m_startButton, &QPushButton::clicked,
                this, &SyncPairDialog::onJoinStarted);
        mwCall(CallTransport, AcMw::SyncTransportStart,
               QJsonObject{{QStringLiteral("backend"), QStringLiteral("tor")}});
    }

    auto *fpBox = new QGroupBox(tr("Confirm the fingerprints match"), this);
    auto *fpLay = new QVBoxLayout(fpBox);
    m_wordsLabel = new QLabel(this);
    m_wordsLabel->setWordWrap(true);
    m_wordsLabel->setStyleSheet(QStringLiteral("font-weight:600"));
    m_emojiLabel = new QLabel(this);
    m_emojiLabel->setStyleSheet(QStringLiteral("font-size:22px"));
    fpLay->addWidget(m_wordsLabel);
    fpLay->addWidget(m_emojiLabel);
    lay->addWidget(fpBox);
    fpBox->setVisible(false);
    m_wordsLabel->setProperty("fpbox", QVariant::fromValue((void *)fpBox));

    lay->addWidget(m_statusLabel);

    auto *buttons = new QHBoxLayout;
    m_confirmButton = new QPushButton(tr("They Match"), this);
    m_rejectButton = new QPushButton(tr("They Don't Match"), this);
    m_cancelButton = new QPushButton(tr("Cancel"), this);
    buttons->addWidget(m_confirmButton);
    buttons->addWidget(m_rejectButton);
    buttons->addStretch();
    buttons->addWidget(m_cancelButton);
    lay->addLayout(buttons);
    m_confirmButton->setVisible(false);
    m_rejectButton->setVisible(false);
    connect(m_confirmButton, &QPushButton::clicked,
            this, &SyncPairDialog::onConfirmed);
    connect(m_rejectButton, &QPushButton::clicked,
            this, &SyncPairDialog::onRejected);
    connect(m_cancelButton, &QPushButton::clicked,
            this, &SyncPairDialog::onCancel);

    m_thread = new QThread(this);
    auto *worker = new SyncPairWorker;
    worker->moveToThread(m_thread);
    connect(m_thread, &QThread::finished, worker, &QObject::deleteLater);
    connect(this, &SyncPairDialog::destroyed,
            m_thread, &QThread::quit);
    m_thread->start();
    m_workerHolder = worker;
    connect(worker, &SyncPairWorker::result,
            this, &SyncPairDialog::onWorkerResult);

    m_timer = new QTimer(this);
    m_timer->setInterval(800);
    connect(m_timer, &QTimer::timeout, this, &SyncPairDialog::poll);
    m_timer->start();
    resize(420, 0);
}

void SyncPairDialog::mwCall(int kind, int op, const QJsonObject &payload)
{
    QMetaObject::invokeMethod(
        m_workerHolder, "call", Qt::QueuedConnection,
        Q_ARG(int, kind), Q_ARG(int, op), Q_ARG(QJsonObject, payload));
}

void SyncPairDialog::invite(const QString &profileId, QWidget *parent)
{
    auto *d = new SyncPairDialog(true, profileId, parent);
    d->show();
    // The transport starts on the worker; begin once it is up —
    // the pair_poll loop retries until pair_begin lands.
    QMetaObject::invokeMethod(d, [d]() {
        QJsonObject payload{{QStringLiteral("profile"), d->m_profileId}};
        d->mwCall(CallBegin, AcMw::SyncPairBegin, payload);
    }, Qt::QueuedConnection);
}

void SyncPairDialog::join(const QString &profileId, QWidget *parent)
{
    auto *d = new SyncPairDialog(false, profileId, parent);
    d->show();
}

void SyncPairDialog::onJoinStarted()
{
    const QString payloadText = m_payloadEdit->toPlainText().trimmed();
    if (payloadText.isEmpty())
        return;
    m_startButton->setEnabled(false);
    m_statusLabel->setText(tr("Connecting to the inviter…"));
    mwCall(CallJoin, AcMw::SyncPairJoin,
           QJsonObject{{QStringLiteral("payload"), payloadText}});
}

void SyncPairDialog::poll()
{
    if (!m_pairId)
        return;
    mwCall(0, AcMw::SyncPairPoll,
           QJsonObject{{QStringLiteral("pair_id"),
                        QString::number(m_pairId)}});
}

void SyncPairDialog::onWorkerResult(int kind, const QJsonObject &doc)
{
    if (doc.contains(QStringLiteral("code"))
        && doc.value(QStringLiteral("code")).isString()
        && doc.contains(QStringLiteral("status"))) {
        // An envelope error: {"code","status","message"}.
        m_statusLabel->setText(
            doc.value(QStringLiteral("message")).toString());
        return;
    }
    if (kind == CallBegin) {
        if (doc.value(QStringLiteral("pair_id")).isDouble()) {
            m_pairId = quint64(doc.value(QStringLiteral("pair_id"))
                                   .toDouble());
            const QString qr = doc.value(QStringLiteral("qr")).toString();
            const QImage img = QrCodeDialog::imageForText(qr);
            if (!img.isNull())
                m_qrLabel->setPixmap(QPixmap::fromImage(img));
            else
                m_qrLabel->setText(tr("(payload too large for QR — use the code)"));
            m_codeLabel->setText(doc.value(QStringLiteral("code")).toString());
            m_statusLabel->setText(tr("Waiting for the other device…"));
        }
        return;
    }
    if (kind == CallJoin) {
        if (doc.value(QStringLiteral("pair_id")).isDouble()) {
            m_pairId = quint64(doc.value(QStringLiteral("pair_id"))
                                   .toDouble());
            m_statusLabel->setText(tr("Handshake in progress…"));
        }
        return;
    }
    applyStatus(doc);
}

void SyncPairDialog::applyStatus(const QJsonObject &doc)
{
    const QString state = doc.value(QStringLiteral("state")).toString();
    if (state == QLatin1String("gone"))
        return;
    if (doc.contains(QStringLiteral("fingerprint"))) {
        const QJsonObject fp = doc.value(QStringLiteral("fingerprint")).toObject();
        QStringList words, emoji;
        for (const QJsonValue &w : fp.value(QStringLiteral("words")).toArray())
            words << w.toString();
        for (const QJsonValue &e : fp.value(QStringLiteral("emoji")).toArray())
            emoji << e.toString();
        m_wordsLabel->setText(words.join(QLatin1String("  ")));
        m_emojiLabel->setText(emoji.join(QLatin1String(" ")));
        QWidget *fpBox = (QWidget *)m_wordsLabel->property("fpbox").value<void *>();
        if (fpBox)
            fpBox->setVisible(true);
    }
    if (state == QLatin1String("fingerprint")
        || state == QLatin1String("committing")
        || state == QLatin1String("handshaking")) {
        m_confirmButton->setVisible(state != QLatin1String("handshaking"));
        m_rejectButton->setVisible(state != QLatin1String("handshaking"));
        if (state == QLatin1String("fingerprint"))
            m_statusLabel->setText(
                tr("Compare these with the other device — they must be identical."));
        else if (state == QLatin1String("committing"))
            m_statusLabel->setText(tr("Finishing…"));
        else
            m_statusLabel->setText(tr("Handshake in progress…"));
    } else if (state == QLatin1String("done")) {
        m_statusLabel->setText(tr("Paired — this device is in the sync group."));
        m_confirmButton->setVisible(false);
        m_rejectButton->setVisible(false);
        m_cancelButton->setText(tr("Close"));
        if (m_timer)
            m_timer->stop();
    } else if (state == QLatin1String("failed")
               || state == QLatin1String("cancelled")) {
        m_statusLabel->setText(tr("Pairing failed: %1")
            .arg(doc.value(QStringLiteral("error")).toString()));
        m_confirmButton->setVisible(false);
        m_rejectButton->setVisible(false);
        m_cancelButton->setText(tr("Close"));
        if (m_timer)
            m_timer->stop();
    }
}

void SyncPairDialog::onConfirmed()
{
    mwCall(0, AcMw::SyncPairConfirm,
           QJsonObject{{QStringLiteral("pair_id"), QString::number(m_pairId)},
                       {QStringLiteral("ok"), true}});
}

void SyncPairDialog::onRejected()
{
    mwCall(0, AcMw::SyncPairConfirm,
           QJsonObject{{QStringLiteral("pair_id"), QString::number(m_pairId)},
                       {QStringLiteral("ok"), false}});
    m_cancelButton->setText(tr("Close"));
}

void SyncPairDialog::onCancel()
{
    if (m_pairId
        && !(m_statusLabel->text().startsWith(tr("Paired"))
             || m_statusLabel->text().startsWith(tr("Pairing failed"))))
        mwCall(0, AcMw::SyncPairCancel,
               QJsonObject{{QStringLiteral("pair_id"), QString::number(m_pairId)}});
    close();
}

#include "syncpairdialog.moc"
