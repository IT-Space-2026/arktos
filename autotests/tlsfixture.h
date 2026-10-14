/*
 * Copyright 2026 The Arora Authors
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
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA  02110-1301  USA
 */

#ifndef TLSFIXTURE_H
#define TLSFIXTURE_H

// TRUST14: the loopback TLS peer fixture shared by tst_tlsverify and
// tst_sitepanel — an `openssl s_server` process holding one of the
// scratch-CA leaves in src/rustcore/data/testcerts.  A real OpenSSL
// server is the right peer for the second-opinion verifier; an
// in-process Qt fixture would deadlock, since the synchronous
// rc_tls_check FFI call blocks the event loop the fixture would live
// on.

#include <qcoreapplication.h>
#include <qfile.h>
#include <qprocess.h>
#include <qtcpserver.h>
#include <qtcpsocket.h>
#include <qtest.h>

namespace TlsFixtureData {

inline QString certDir()
{
    return QCoreApplication::applicationDirPath()
        + QLatin1String("/../../src/rustcore/data/testcerts");
}

inline QByteArray certFile(const QString &name)
{
    QFile file(certDir() + QLatin1Char('/') + name);
    if (!file.open(QIODevice::ReadOnly))
        return QByteArray();
    return file.readAll();
}

} // namespace TlsFixtureData

class TlsFixture
{
public:
    ~TlsFixture()
    {
        m_proc.kill();
        m_proc.waitForFinished(2000);
    }

    // base is the fixture name ("valid", "expired", ...) — cert/key
    // PEMs sit in src/rustcore/data/testcerts.
    bool start(const QString &base)
    {
        const QString cert =
            TlsFixtureData::certDir() + QLatin1Char('/') + base
            + QLatin1String(".pem");
        const QString key = TlsFixtureData::certDir() + QLatin1Char('/')
            + base + QLatin1String("-key.pem");
        if (!QFile::exists(cert) || !QFile::exists(key))
            return false;
        quint16 candidate = freePort();
        m_proc.setProgram(QStringLiteral("openssl"));
        m_proc.setArguments({
            QStringLiteral("s_server"),
            QStringLiteral("-accept"),
            QLatin1String("127.0.0.1:") + QString::number(candidate),
            QStringLiteral("-cert"), cert,
            QStringLiteral("-key"), key,
            QStringLiteral("-alpn"), QStringLiteral("http/1.1"),
            QStringLiteral("-quiet"),
        });
        m_proc.start();
        if (!m_proc.waitForStarted(5000))
            return false;
        // Wait for the listener: a bare TCP connect drops the attempt
        // but s_server keeps accepting.
        for (int i = 0; i < 50; ++i) {
            QTcpSocket probe;
            probe.connectToHost(QHostAddress::LocalHost, candidate);
            if (probe.waitForConnected(200)) {
                probe.disconnectFromHost();
                m_port = candidate;
                return true;
            }
            QTest::qWait(50);
        }
        return false;
    }

    quint16 port() const { return m_port; }

private:
    static quint16 freePort()
    {
        QTcpServer server;
        server.listen(QHostAddress::LocalHost);
        return server.serverPort();
    }

    QProcess m_proc;
    quint16 m_port = 0;
};

#endif // TLSFIXTURE_H
