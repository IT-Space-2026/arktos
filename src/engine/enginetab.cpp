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

#include "enginetab.h"

#include "engineinterface.h"
#include "pdfview.h"

#ifdef ARORA_SERVO
#include "servobackend.h"
#endif

#include <QLabel>
#include <QMetaObject>
#include <QStackedLayout>

EngineTab::EngineTab(Engine::Backend *backend, Engine::Profile *profile,
                     QWidget *parent)
    : QWidget(parent)
    , m_backend(backend)
    , m_profile(profile)
    , m_page(nullptr)
    , m_progress(0)
{
    // Stacked so PDF02's document view can cover the backend view —
    // same shape WebViewWithSearch uses for WebEngine tabs.
    m_layout = new QStackedLayout(this);
    m_layout->setContentsMargins(0, 0, 0, 0);

    if (!backend)
        return;

    m_page = backend->createPage(profile, this);
    if (!m_page)
        return;

    if (QWidget *view = backend->createView(m_page, this)) {
        m_layout->addWidget(view);
        wireViewRecovery(view);
    }
    wirePage();
}

// ENG28: a backend view that can die-and-recover (Servo's poisoned
// SeInstance) advertises a retry affordance through restartRequested
// — recreate the page pair in place when it fires.
void EngineTab::wireViewRecovery(QWidget *view)
{
#ifdef ARORA_SERVO
    if (auto *servoView = qobject_cast<ServoView *>(view)) {
        // Queued: restartRequested fires from the retry button's
        // clicked() emission — restartEngine deletes that button's
        // owning view, which must not happen mid-signal.
        connect(servoView, &ServoView::restartRequested,
                this, [this]() {
            QMetaObject::invokeMethod(this, [this]() {
                restartEngine();
            }, Qt::QueuedConnection);
        });
    }
#else
    Q_UNUSED(view);
#endif
}

void EngineTab::wirePage()
{
    connect(m_page, &Engine::Page::titleChanged,
            this, [this](const QString &title) {
        m_title = title;
        emit titleChanged(title);
    });
    connect(m_page, &Engine::Page::urlChanged,
            this, &EngineTab::urlChanged);
    connect(m_page, &Engine::Page::iconChanged,
            this, [this]() { emit iconChanged(); });
    connect(m_page, &Engine::Page::loadStarted,
            this, [this]() {
        m_progress = 0;
        emit loadStarted();
    });
    connect(m_page, &Engine::Page::loadProgress,
            this, [this](int progress) {
        m_progress = progress;
        emit loadProgress(progress);
    });
    connect(m_page, &Engine::Page::loadFinished,
            this, [this](bool ok) {
        m_progress = 100;
        emit loadFinished(ok);
    });
}

bool EngineTab::restartEngine()
{
    if (!m_backend || !m_profile)
        return false;
    const QUrl lastUrl = m_page ? m_page->url() : QUrl();

    // The dead objects go first — the engine's single-instance slot
    // only frees when the old view's SeInstance is destroyed, so the
    // recreate below only works after this teardown.
    if (QWidget *view = m_page ? m_page->view() : nullptr) {
        m_layout->removeWidget(view);
        delete view;
    }
    delete m_page;
    m_page = nullptr;
    m_progress = 0;

    m_page = m_backend->createPage(m_profile, this);
    if (!m_page) {
        // Honest failure — the engine could not restart in-process;
        // a browser restart is the only remaining recovery.
        if (!m_notice) {
            m_notice = new QLabel(this);
            m_notice->setWordWrap(true);
            m_notice->setContentsMargins(12, 12, 12, 12);
            m_notice->setAlignment(Qt::AlignTop | Qt::AlignLeft);
            m_layout->addWidget(m_notice);
        }
        m_notice->setText(
            tr("The %1 engine could not be restarted. Reload this "
               "tab in Chromium to keep browsing, or restart Arora "
               "to try %1 again.").arg(m_backend->displayName()));
        m_layout->setCurrentWidget(m_notice);
        emit pageRecreated();
        return false;
    }
    if (QWidget *view = m_backend->createView(m_page, this)) {
        m_layout->addWidget(view);
        m_layout->setCurrentWidget(view);
        wireViewRecovery(view);
    }
    wirePage();
    emit pageRecreated();
    if (lastUrl.isValid())
        m_page->load(lastUrl);
    return true;
}

QUrl EngineTab::url() const
{
    return m_page ? m_page->url() : QUrl();
}

QString EngineTab::title() const
{
    return m_title;
}

void EngineTab::load(const QUrl &url)
{
    if (m_page)
        m_page->load(url);
}

void EngineTab::stop()
{
    if (m_page)
        m_page->stop();
}

void EngineTab::reload()
{
    if (m_page)
        m_page->reload();
}

bool EngineTab::restartEngine()
{
    if (!m_backend || !m_profile)
        return false;
    const QUrl lastUrl = m_page ? m_page->url() : QUrl();

    // The dead objects go first — the engine's single-instance slot
    // only frees when the old view's SeInstance is destroyed, so the
    // recreate below only works after this teardown.
    if (QWidget *view = m_page ? m_page->view() : nullptr) {
        m_layout->removeWidget(view);
        delete view;
    }
    delete m_page;
    m_page = nullptr;
    m_progress = 0;

    m_page = m_backend->createPage(m_profile, this);
    if (!m_page) {
        // Honest failure — the engine could not restart in-process;
        // a browser restart is the only remaining recovery.
        if (!m_notice) {
            m_notice = new QLabel(this);
            m_notice->setWordWrap(true);
            m_notice->setContentsMargins(12, 12, 12, 12);
            m_notice->setAlignment(Qt::AlignTop | Qt::AlignLeft);
            m_layout->addWidget(m_notice);
        }
        m_notice->setText(
            tr("The %1 engine could not be restarted. Reload this "
               "tab in Chromium to keep browsing, or restart Arora "
               "to try %1 again.").arg(m_backend->displayName()));
        m_layout->setCurrentWidget(m_notice);
        emit pageRecreated();
        return false;
    }
    if (QWidget *view = m_backend->createView(m_page, this)) {
        m_layout->addWidget(view);
        m_layout->setCurrentWidget(view);
        wireViewRecovery(view);
    }
    wirePage();
    emit pageRecreated();
    if (lastUrl.isValid())
        m_page->load(lastUrl);
    return true;
}

void EngineTab::showPdfDocument(const QString &localPath,
                                const QUrl &sourceUrl)
{
    if (!m_pdfView) {
        m_pdfView = new PdfView(this);
        m_layout->addWidget(m_pdfView);
        connect(m_pdfView, &PdfView::reloadRequested, this,
                [this](const QUrl &url) { load(url); });
        connect(m_pdfView, &PdfView::titleChanged,
                this, &EngineTab::titleChanged);
    }
    m_pdfView->setDocument(localPath, sourceUrl);
    m_layout->setCurrentWidget(m_pdfView);
}
