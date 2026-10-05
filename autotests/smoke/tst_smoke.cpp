/*
 * Copyright 2026 Arora authors
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

#include <QtTest/QtTest>
#include <QtWebEngineWidgets/QWebEngineView>

// Toolchain gate: proves QtWebEngine actually runs headless on this
// host (QtWebEngineProcess, nss, GL stack) before any porting starts.
class tst_Smoke : public QObject
{
    Q_OBJECT

private slots:
    void webEngineBootsOffscreen();
};

void tst_Smoke::webEngineBootsOffscreen()
{
    QWebEngineView view;
    QSignalSpy spy(&view, &QWebEngineView::loadFinished);
    view.load(QUrl(QStringLiteral("about:blank")));
    QVERIFY(spy.wait(10000));
    QVERIFY(spy.takeFirst().first().toBool());
    QCOMPARE(view.url(), QUrl(QStringLiteral("about:blank")));
}

QTEST_MAIN(tst_Smoke)
#include "tst_smoke.moc"
