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
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA  02110-1301  USA
 */

#include "updatechecker.h"

#include "browserapplication.h"
#include "networkaccessmanager.h"

#include <qcoreapplication.h>
#include <qfile.h>
#include <qjsonarray.h>
#include <qjsondocument.h>
#include <qjsonobject.h>
#include <qjsonparseerror.h>
#include <qnetworkaccessmanager.h>
#include <qnetworkreply.h>
#include <qnetworkrequest.h>
#include <qregularexpression.h>
#include <qtimer.h>

// AUDIT03: a release manifest is tens of KB of JSON — a server
// answering with megabytes is hostile or wedged, not informative.
// The parse-side cap fails fast; the read buffer bounds how much a
// slow-drip endpoint can grow the reply before the transfer timeout
// kills it.
static const qint64 kMaxReleaseJsonBytes = 1024 * 1024;
static const qint64 kReleaseReadBufferBytes = 8 * 1024 * 1024;

UpdateChecker::UpdateChecker(QObject *parent)
    : QObject(parent)
    , m_manager(nullptr)
    , m_reply(nullptr)
    , m_timer(new QTimer(this))
    , m_apiUrl(defaultApiUrl())
    , m_status(Status::Idle)
{
    m_timer->setSingleShot(true);
    // The transfer timeout below covers the socket stall; this is the
    // outer bound for the whole check so a wedged request can never
    // leave the dialog spinning forever.
    m_timer->setInterval(20000);
    connect(m_timer, &QTimer::timeout, this, &UpdateChecker::timeoutFired);
}

UpdateChecker::~UpdateChecker()
{
    cancel();
}

QUrl UpdateChecker::defaultApiUrl()
{
    return QUrl(QLatin1String(
        "https://api.github.com/repos/janos-szenfner/arora/releases/latest"));
}

QUrl UpdateChecker::releasesPageUrl()
{
    return QUrl(QLatin1String(
        "https://github.com/janos-szenfner/arora/releases"));
}

void UpdateChecker::setApiUrl(const QUrl &url)
{
    m_apiUrl = url;
}

void UpdateChecker::setNetworkAccessManager(QNetworkAccessManager *manager)
{
    m_manager = manager;
}

void UpdateChecker::check()
{
    if (m_reply)
        return;

    // UPD01: a tor process never phones home.  The Help-menu action is
    // already disabled there; this guard keeps every other caller
    // (tests, future wiring) from leaking a direct github.com request
    // out of the tor session.
    if (BrowserApplication::isTorMode()) {
        m_result = Result();
        m_result.status = Status::Error;
        m_result.errorString = tr("Update checks are disabled in Tor "
            "mode — contacting github.com would bypass the Tor proxy.");
        m_status = Status::Error;
        emit finished(m_result);
        return;
    }

    if (!m_manager)
        m_manager = NetworkAccessManager::instance();

    QNetworkRequest request(m_apiUrl);
    // GitHub's API 403s requests without a User-Agent.
    request.setHeader(QNetworkRequest::UserAgentHeader,
        QString::fromLatin1("Arora/%1").arg(
            QCoreApplication::applicationVersion()));
    request.setRawHeader("Accept", "application/vnd.github+json");
    request.setTransferTimeout(15000);

    m_status = Status::Checking;
    m_result = Result();
    m_result.status = Status::Checking;

    m_reply = m_manager->get(request);
    m_reply->setReadBufferSize(kReleaseReadBufferBytes);
    connect(m_reply, &QNetworkReply::finished,
            this, &UpdateChecker::replyFinished);
    m_timer->start();
}

void UpdateChecker::cancel()
{
    if (m_reply) {
        m_reply->disconnect(this);
        m_reply->abort();
        m_reply->deleteLater();
        m_reply = nullptr;
    }
    m_timer->stop();
    if (m_status == Status::Checking)
        m_status = Status::Idle;
}

void UpdateChecker::timeoutFired()
{
    if (m_reply) {
        m_reply->disconnect(this);
        m_reply->abort();
        m_reply->deleteLater();
        m_reply = nullptr;
    }
    m_result = Result();
    m_result.errorString = tr("The update server did not answer in time.");
    finish(Status::Error);
}

void UpdateChecker::replyFinished()
{
    QNetworkReply *reply = m_reply;
    m_reply = nullptr;
    m_timer->stop();
    if (!reply)
        return;
    reply->deleteLater();

    const int httpStatus = reply->attribute(
        QNetworkRequest::HttpStatusCodeAttribute).toInt();

    if (reply->error() != QNetworkReply::NoError) {
        // A 404 on /releases/latest means the repository has no
        // published release yet — that is a real answer, not a
        // transport failure.
        if (httpStatus == 404) {
            finish(Status::NoReleases);
        } else {
            m_result = Result();
            m_result.errorString = httpStatus > 0
                ? tr("The update server answered with HTTP %1.")
                    .arg(httpStatus)
                : reply->errorString();
            finish(Status::Error);
        }
        return;
    }

    const QByteArray payload = reply->readAll();
    if (payload.size() > kMaxReleaseJsonBytes) {
        m_result = Result();
        m_result.errorString = tr("The update server returned an "
                                  "oversized response.");
        finish(Status::Error);
        return;
    }

    bool ok = false;
    QString error;
    m_result = parseReleaseJson(payload, &ok, &error);
    if (!ok) {
        m_result.errorString = error;
        finish(Status::Error);
        return;
    }

    // tag_name is the version source of truth ('name' is freeform
    // marketing text).  The comparison basis is the numeric core on
    // both sides — v-prefixes, prerelease markers and the app's own
    // "0.12 (Git: …)" decoration are stripped first.
    const QString remoteVersion = normalizedVersion(m_result.latestVersion);
    const QString localVersion =
        normalizedVersion(QCoreApplication::applicationVersion());
    if (remoteVersion.isEmpty()) {
        m_result.errorString = tr("The release has no recognizable "
                                  "version tag.");
        finish(Status::Error);
        return;
    }
    if (compareVersions(remoteVersion, localVersion) > 0)
        finish(Status::UpdateAvailable);
    else
        finish(Status::UpToDate);
}

void UpdateChecker::finish(Status status)
{
    m_status = status;
    m_result.status = status;
    emit finished(m_result);
}

UpdateChecker::Result UpdateChecker::parseReleaseJson(
        const QByteArray &payload, bool *ok, QString *error)
{
    Result result;
    QJsonParseError parseError;
    const QJsonDocument document =
        QJsonDocument::fromJson(payload, &parseError);
    if (parseError.error != QJsonParseError::NoError
            || !document.isObject()) {
        *ok = false;
        *error = tr("The update server returned an unreadable "
                    "response.");
        return result;
    }

    const QJsonObject release = document.object();
    result.latestVersion = release.value(QLatin1String("tag_name"))
        .toString();
    result.releaseTitle = release.value(QLatin1String("name"))
        .toString();
    result.releasePageUrl = QUrl(release.value(QLatin1String("html_url"))
        .toString());

    const QJsonArray assets = release.value(QLatin1String("assets"))
        .toArray();
    for (const QJsonValue &value : assets) {
        const QJsonObject asset = value.toObject();
        ReleaseAsset entry;
        entry.name = asset.value(QLatin1String("name")).toString();
        entry.downloadUrl = QUrl(asset.value(
            QLatin1String("browser_download_url")).toString());
        entry.size = asset.value(QLatin1String("size"))
            .toVariant().toLongLong();
        if (!entry.name.isEmpty() && entry.downloadUrl.isValid())
            result.assets.append(entry);
    }

    *ok = true;
    return result;
}

QString UpdateChecker::normalizedVersion(const QString &version)
{
    QString normalized = version.trimmed();
    if (normalized.startsWith(QLatin1Char('v'))
            || normalized.startsWith(QLatin1Char('V')))
        normalized = normalized.mid(1);
    // "0.12 (Git: abc123 4)" -> "0.12"
    const int decoration = normalized.indexOf(
        QRegularExpression(QLatin1String("[\\s(]")));
    if (decoration > 0)
        normalized = normalized.left(decoration);
    // "1.2.3-rc1+build" -> "1.2.3"
    const int suffix = normalized.indexOf(
        QRegularExpression(QLatin1String("[-+]")));
    if (suffix > 0)
        normalized = normalized.left(suffix);
    return normalized;
}

int UpdateChecker::compareVersions(const QString &a, const QString &b)
{
    const QString normalizedA = normalizedVersion(a);
    const QString normalizedB = normalizedVersion(b);

    const QStringList partsA = normalizedA.split(QLatin1Char('.'));
    const QStringList partsB = normalizedB.split(QLatin1Char('.'));
    QList<qint64> numbersA, numbersB;
    bool numeric = true;
    for (const QString &part : partsA) {
        if (part.isEmpty())
            continue;
        bool ok = false;
        numbersA.append(part.toLongLong(&ok));
        if (!ok) { numeric = false; break; }
    }
    if (numeric) {
        for (const QString &part : partsB) {
            if (part.isEmpty())
                continue;
            bool ok = false;
            numbersB.append(part.toLongLong(&ok));
            if (!ok) { numeric = false; break; }
        }
    }
    if (numeric) {
        const int count = qMax(numbersA.size(), numbersB.size());
        for (int i = 0; i < count; ++i) {
            const qint64 x = i < numbersA.size() ? numbersA.at(i) : 0;
            const qint64 y = i < numbersB.size() ? numbersB.at(i) : 0;
            if (x != y)
                return x < y ? -1 : 1;
        }
        return 0;
    }
    // A non-semver tag cannot be ordered numerically — a plain string
    // compare still answers "is this the same release", which keeps
    // the up-to-date verdict honest for equal tags.
    return QString::compare(normalizedA, normalizedB);
}

QString UpdateChecker::hostOs()
{
#if defined(Q_OS_WIN)
    return QLatin1String("windows");
#elif defined(Q_OS_MACOS)
    return QLatin1String("macos");
#else
    return QLatin1String("linux");
#endif
}

QString UpdateChecker::hostOsReleaseIdLike()
{
#if defined(Q_OS_LINUX)
    QFile file(QLatin1String("/etc/os-release"));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return QString();
    QString id, like;
    const QByteArrayList lines = file.readAll().split('\n');
    for (const QByteArray &raw : lines) {
        const QByteArray line = raw.trimmed();
        QString value;
        if (line.startsWith("ID="))
            value = QString::fromUtf8(line.mid(3));
        else if (line.startsWith("ID_LIKE="))
            value = QString::fromUtf8(line.mid(8));
        else
            continue;
        value.remove(QLatin1Char('"'));
        if (line.startsWith("ID="))
            id = value;
        else
            like = value;
    }
    return (id + QLatin1Char(' ') + like).trimmed().toLower();
#else
    return QString();
#endif
}

UpdateChecker::PlatformPick UpdateChecker::platformAssetsFor(
        const QString &os, const QString &osReleaseIdLike,
        const QList<ReleaseAsset> &assets)
{
    PlatformPick pick;
    auto matching = [&assets](const QString &suffix,
                              bool preferInstaller) {
        QList<ReleaseAsset> matches, installerNamed;
        for (const ReleaseAsset &asset : assets) {
            const QString lower = asset.name.toLower();
            if (!lower.endsWith(suffix))
                continue;
            if (preferInstaller && lower.contains(QLatin1String("installer")))
                installerNamed.append(asset);
            else
                matches.append(asset);
        }
        // Exact "*Installer*.exe" ranks above a generic .exe.
        installerNamed.append(matches);
        return installerNamed;
    };

    if (os == QLatin1String("windows")) {
        // The installer always wins; a zip is never a candidate.
        pick.candidates = matching(QLatin1String(".exe"), true);
        return pick;
    }
    if (os == QLatin1String("macos")) {
        pick.candidates = matching(QLatin1String(".dmg"), false);
        return pick;
    }

    // Linux (and any other unix): the distro family picks the native
    // package, flatpak is the generic fallback.
    const QString idLike = osReleaseIdLike.toLower();
    const bool debianFamily = idLike.contains(QLatin1String("debian"))
        || idLike.contains(QLatin1String("ubuntu"))
        || idLike.contains(QLatin1String("mint"))
        || idLike.contains(QLatin1String("raspbian"));
    const bool rpmFamily = idLike.contains(QLatin1String("fedora"))
        || idLike.contains(QLatin1String("rhel"))
        || idLike.contains(QLatin1String("centos"))
        || idLike.contains(QLatin1String("opensuse"))
        || idLike.contains(QLatin1String("suse"))
        || idLike.contains(QLatin1String("mageia"));

    const QList<ReleaseAsset> debs = matching(QLatin1String(".deb"), false);
    const QList<ReleaseAsset> rpms = matching(QLatin1String(".rpm"), false);
    const QList<ReleaseAsset> flatpaks =
        matching(QLatin1String(".flatpak"), false);

    if (debianFamily && !rpmFamily) {
        pick.candidates = debs + flatpaks;
    } else if (rpmFamily && !debianFamily) {
        pick.candidates = rpms + flatpaks;
    } else {
        // Unknown or self-contradicting distro — offer every plausible
        // candidate instead of guessing; flatpak leads as the
        // distro-agnostic format.
        pick.candidates = flatpaks + debs + rpms;
        pick.ambiguous = pick.candidates.size() > 1;
    }
    return pick;
}

UpdateChecker::PlatformPick UpdateChecker::platformAssets(
        const QList<ReleaseAsset> &assets)
{
    return platformAssetsFor(hostOs(), hostOsReleaseIdLike(), assets);
}
