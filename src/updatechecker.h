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
#ifndef UPDATECHECKER_H
#define UPDATECHECKER_H

#include <qlist.h>
#include <qmetatype.h>
#include <qobject.h>
#include <qstring.h>
#include <qurl.h>

class QNetworkAccessManager;
class QNetworkReply;
class QTimer;

// UPD01: manual "Check for Updates" fetch against the GitHub releases
// API.  Runs on the application-side NetworkAccessManager (the web
// profile/Chromium stack never sees it), carries a hard timeout, and
// reports every failure honestly — nothing about it is periodic or
// automatic, so the zero-telemetry guarantee holds: the request fires
// only when the user picks Help > Check for Updates.
//
// In a tor process check() refuses outright — a clearnet API call
// would leak the real IP next to the anonymized session.
class UpdateChecker : public QObject
{
    Q_OBJECT

public:
    struct ReleaseAsset {
        QString name;
        QUrl downloadUrl;
        qint64 size = 0;
    };

    enum class Status {
        Idle,
        Checking,
        UpToDate,
        UpdateAvailable,
        NoReleases,     // 404 on /releases/latest — repo has none yet
        Error           // offline, API error, malformed payload
    };

    struct Result {
        Status status = Status::Idle;
        QString latestVersion;      // tag_name, normalized
        QString releaseTitle;       // release 'name' field
        QUrl releasePageUrl;        // html_url of the release
        QList<ReleaseAsset> assets;
        QString errorString;
    };

    // Preference-ordered candidates for one platform.  `ambiguous`
    // is set when host detection could not pick a single package
    // format — the dialog then offers the candidates explicitly
    // rather than guessing.
    struct PlatformPick {
        QList<ReleaseAsset> candidates;
        bool ambiguous = false;
    };

    explicit UpdateChecker(QObject *parent = nullptr);
    ~UpdateChecker() override;

    void check();
    void cancel();
    bool checking() const { return m_status == Status::Checking; }
    Result result() const { return m_result; }

    // Test/diagnostic seams — the default api url is the GitHub
    // releases endpoint, the default manager the app's own slim
    // NetworkAccessManager singleton.
    void setApiUrl(const QUrl &url);
    void setNetworkAccessManager(QNetworkAccessManager *manager);

    static QUrl defaultApiUrl();
    static QUrl releasesPageUrl();

    // "v1.2.3-beta+ci" / "0.12 (Git: abc123)" -> "1.2.3" / "0.12":
    // leading v/V, prerelease/build suffixes and the git-decoration
    // tail are stripped before numeric comparison.
    static QString normalizedVersion(const QString &version);
    // -1/0/1 numeric semver-ish compare; a side that does not parse
    // numerically falls back to a string compare of the normalized
    // form.
    static int compareVersions(const QString &a, const QString &b);

    // Host platform pick (osRelease = /etc/os-release ID + ID_LIKE,
    // lowercased).  Windows prefers *installer*.exe then any *.exe —
    // never a zip; macOS picks *.dmg; Linux picks *.deb on
    // Debian-family ids, *.rpm on rpm-family ids, *.flatpak as the
    // generic fallback, and reports ambiguous when the distro is
    // unrecognized so callers can offer the candidates.
    static PlatformPick platformAssetsFor(const QString &os,
            const QString &osReleaseIdLike,
            const QList<ReleaseAsset> &assets);
    static PlatformPick platformAssets(const QList<ReleaseAsset> &assets);
    // AUDIT04: the release payload is server input — only URLs on the
    // GitHub release surface (github.com / *.github.com /
    // *.githubusercontent.com over https) or on the configured api
    // url's own host (loopback tests, self-hosted GHE — never a
    // scheme downgrade below the api's own) are trusted as download
    // targets.  javascript:/file:/data:, credential-bearing and
    // off-host URLs are refused at parse time.
    static bool isTrustedDownloadUrl(const QUrl &url,
            const QUrl &apiUrl);
    // Separated for tests: reads /etc/os-release on Linux.
    static QString hostOs();
    static QString hostOsReleaseIdLike();

signals:
    void finished(const UpdateChecker::Result &result);

private slots:
    void replyFinished();
    void timeoutFired();

private:
    void finish(Status status);
    static Result parseReleaseJson(const QByteArray &payload,
                                   const QUrl &apiUrl,
                                   bool *ok, QString *error);

    QNetworkAccessManager *m_manager;
    QNetworkReply *m_reply;
    QTimer *m_timer;
    QUrl m_apiUrl;
    Status m_status;
    Result m_result;
};

Q_DECLARE_METATYPE(UpdateChecker::Result)

#endif // UPDATECHECKER_H
