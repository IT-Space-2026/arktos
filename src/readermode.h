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

#ifndef READERMODE_H
#define READERMODE_H

#include <qobject.h>
#include <qpointer.h>
#include <qvariant.h>

#include <functional>

class QEvent;
class ReaderBridge;
class WebView;
class QWidget;

// READ01: per-view controller for Reader Mode — a clutter-free article
// view rendered by the vendored Mozilla Readability.js + reader.js
// driver (src/data/, injected via runJavaScript).
//
// RDR01: under the Rust core the extraction runs in rustcore
// (rc_readability_probe/extract over the page's serialized DOM) and
// the article renders into a dedicated overlay web view with
// Qt-widget chrome — no page-side script eval at all, so the feature
// works identically on a JS-one-way engine and on channel-less tor
// pages (preferences persist via QSettings directly).  Without the
// crate this stays the JS shadow-DOM overlay described below.
//
// The JS reader overlay is Shadow DOM on top of the live document, so
// exiting restores the page exactly (scroll, form state, JS world) —
// there is no reload.  The in-overlay chrome (font size, light/dark,
// exit) talks back through the "aroraReader" QWebChannel bridge when
// the page has one; tor pages (no channel) still get the overlay, the
// settings just don't persist and C++ is only told about JS-side exits
// via the next probe.  The Rust overlay preserves the same "page is
// never touched" contract — it is a sibling widget on top of the view.
//
// JavascriptEnabled off (JSCTL rules, Safer/Safest tiers): runJavaScript
// never executes while the attribute is off, so enter()/probe() lift it
// for the injection window only — parse-time-blocked page scripts do
// not retro-run, so the page stays scriptless.  The Rust path's
// serialized-DOM grab (toHtml) rides the same injection channel and
// gets the same lift treatment.
class ReaderMode : public QObject
{
    Q_OBJECT
    // The channel-facing half of the pair forwards JS calls here.
    friend class ReaderBridge;

public:
    explicit ReaderMode(WebView *view);

    bool isActive() const { return m_active; }
    // Whether the current page looks article-like (the location-bar
    // reader icon's visibility).  Always true while active.
    bool isAvailable() const { return m_available || m_active; }

    static int fontSize();
    static QString theme();

public slots:
    void enter();
    void exit();
    void toggle();

signals:
    void activeChanged(bool active);
    void availableChanged(bool available);
    // One-line user-visible message (e.g. "not an article") for the
    // status bar.
    void message(const QString &message);

private slots:
    void onLoadStarted();
    void onLoadFinished(bool ok);
    // ReaderBridge relays JS-side exits and in-overlay preference
    // changes through these.
    void scriptExited();
    void scriptPreference(const QString &key, const QVariant &value);

private:
    void setActive(bool active);
    void setAvailable(bool available);
    void probe();
    // Runs program (the Readability+driver bundle plus a trailing
    // call) in the page's main world, working around WebEngine's
    // refusal to runJavaScript while JavascriptEnabled is off.
    void runDriver(const QString &call,
                   const std::function<void(const QVariant &)> &callback
                       = std::function<void(const QVariant &)>());
    QString scriptBundle() const;
    bool eventFilter(QObject *watched, QEvent *event) override;

#ifdef ARORA_RUSTCORE
    // The Rust path: collect the page's serialized DOM (toHtml with
    // the same JS-lift workaround as runDriver), extract in the core,
    // render the returned fragment into the overlay view.
    void collectHtml(const std::function<void(const QString &)> &callback);
    void showOverlay(const QVariantMap &article);
    void closeOverlay();
    QWidget *m_overlay = nullptr;
#endif

    WebView *m_view;
    ReaderBridge *m_bridge;
    bool m_active;
    bool m_available;
    bool m_busy;
};

// Channel object registered as "aroraReader" on the page's existing
// QWebChannel (WebPage::init).  Any page can reach it (SEC08), so it
// exposes nothing but cosmetic state: an exit notification and two
// clamped preference writes.
class ReaderBridge : public QObject
{
    Q_OBJECT

public:
    explicit ReaderBridge(ReaderMode *readerMode)
        : QObject(readerMode)
        , m_readerMode(readerMode)
    {
    }

public slots:
    void notifyExited();
    void setPreference(const QString &key, const QVariant &value);

private:
    QPointer<ReaderMode> m_readerMode;
};

#endif // READERMODE_H
