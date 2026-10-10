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

#include <QVBoxLayout>

EngineTab::EngineTab(Engine::Backend *backend, Engine::Profile *profile,
                     QWidget *parent)
    : QWidget(parent)
    , m_backend(backend)
    , m_page(nullptr)
    , m_progress(0)
{
    QVBoxLayout *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    if (!backend)
        return;

    m_page = backend->createPage(profile, this);
    if (!m_page)
        return;

    if (QWidget *view = backend->createView(m_page, this))
        layout->addWidget(view);

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
