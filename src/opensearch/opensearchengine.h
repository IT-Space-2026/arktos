/*
 * Copyright 2009 Jakub Wieczorek <faw217@gmail.com>
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

#ifndef OPENSEARCHENGINE_H
#define OPENSEARCHENGINE_H

#include <qpair.h>
#include <qimage.h>
#include <qmap.h>
#include <qnetworkaccessmanager.h>
#include <qstring.h>
#include <qurl.h>

class QNetworkReply;
class QJsonObject;

class OpenSearchEngineDelegate;
class OpenSearchEngine : public QObject
{
    Q_OBJECT

signals:
    void imageChanged();
    void suggestions(const QStringList &suggestions);

public:
    typedef QPair<QString, QString> Parameter;
    typedef QList<Parameter> Parameters;

    Q_PROPERTY(QString name READ name WRITE setName)
    Q_PROPERTY(QString description READ description WRITE setDescription)
    Q_PROPERTY(QString searchUrlTemplate READ searchUrlTemplate WRITE setSearchUrlTemplate)
    Q_PROPERTY(Parameters searchParameters READ searchParameters WRITE setSearchParameters)
    Q_PROPERTY(QString searchMethod READ searchMethod WRITE setSearchMethod)
    Q_PROPERTY(QString suggestionsUrlTemplate READ suggestionsUrlTemplate WRITE setSuggestionsUrlTemplate)
    Q_PROPERTY(Parameters suggestionsParameters READ suggestionsParameters WRITE setSuggestionsParameters)
    Q_PROPERTY(QString suggestionsMethod READ suggestionsMethod WRITE setSuggestionsMethod)
    Q_PROPERTY(bool providesSuggestions READ providesSuggestions)
    // SRCH04: engines may carry a second results template for image
    // searches (<Url type="text/html" purpose="image" ...>).
    Q_PROPERTY(QString imageSearchUrlTemplate READ imageSearchUrlTemplate WRITE setImageSearchUrlTemplate)
    Q_PROPERTY(Parameters imageSearchParameters READ imageSearchParameters WRITE setImageSearchParameters)
    Q_PROPERTY(QString imageSearchMethod READ imageSearchMethod WRITE setImageSearchMethod)
    Q_PROPERTY(bool providesImageSearch READ providesImageSearch)
    Q_PROPERTY(QString imageUrl READ imageUrl WRITE setImageUrl)
    Q_PROPERTY(bool valid READ isValid)
    Q_PROPERTY(QNetworkAccessManager *networkAccessManager READ networkAccessManager WRITE setNetworkAccessManager)

    OpenSearchEngine(QObject *parent = nullptr);

    QString name() const;
    void setName(QString name);

    QString description() const;
    void setDescription(QString description);

    QString searchUrlTemplate() const;
    void setSearchUrlTemplate(QString searchUrl);
    QUrl searchUrl(const QString &searchTerm) const;

    bool providesSuggestions() const;

    QString suggestionsUrlTemplate() const;
    void setSuggestionsUrlTemplate(QString suggestionsUrl);
    QUrl suggestionsUrl(const QString &searchTerm) const;

    bool providesImageSearch() const;

    QString imageSearchUrlTemplate() const;
    void setImageSearchUrlTemplate(QString imageSearchUrl);
    QUrl imageSearchUrl(const QString &searchTerm) const;

    Parameters searchParameters() const;
    void setSearchParameters(const Parameters &searchParameters);

    Parameters suggestionsParameters() const;
    void setSuggestionsParameters(const Parameters &suggestionsParameters);

    Parameters imageSearchParameters() const;
    void setImageSearchParameters(const Parameters &imageSearchParameters);

    QString searchMethod() const;
    void setSearchMethod(const QString &method);

    QString suggestionsMethod() const;
    void setSuggestionsMethod(const QString &method);

    QString imageSearchMethod() const;
    void setImageSearchMethod(const QString &method);

    QString imageUrl() const;
    void setImageUrl(QString url);

    QImage image() const;
    void setImage(const QImage &image);

    bool isValid() const;

    QNetworkAccessManager *networkAccessManager() const;
    void setNetworkAccessManager(QNetworkAccessManager *networkAccessManager);

    OpenSearchEngineDelegate *delegate() const;
    void setDelegate(OpenSearchEngineDelegate *delegate);

    bool operator==(const OpenSearchEngine &other) const;
    bool operator<(const OpenSearchEngine &other) const;

public slots:
    void requestSuggestions(const QString &searchTerm);
    void requestSearchResults(const QString &searchTerm);

protected:
    static QString parseTemplate(const QString &searchTerm, const QString &searchTemplate);
    QUrl buildUrl(const QString &searchTerm, const QString &templ,
                  const QString &method, const Parameters &parameters) const;
    void loadImage() const;

private slots:
    void imageObtained();
    void suggestionsObtained();

private:
    QString m_name;
    QString m_description;

    QString m_imageUrl;
    QImage m_image;

    QString m_searchUrlTemplate;
    QString m_suggestionsUrlTemplate;
    QString m_imageSearchUrlTemplate;
    Parameters m_searchParameters;
    Parameters m_suggestionsParameters;
    Parameters m_imageSearchParameters;
    QString m_searchMethod;
    QString m_suggestionsMethod;
    QString m_imageSearchMethod;

    QMap<QString, QNetworkAccessManager::Operation> m_requestMethods;

    QNetworkAccessManager *m_networkAccessManager;
    QNetworkReply *m_suggestionsReply;

    OpenSearchEngineDelegate *m_delegate;
};

#if defined(ARORA_RUSTCORE)
// OSE01: the rc_opensearch_parse field map <-> engine marshaling the
// reader's Rust path and the manager's registry hydration share
// (rustcore builds only — the field names are documented in
// rustcore.h next to rc_ose_get).
void openSearchEngineApplyJson(OpenSearchEngine *engine,
                               const QJsonObject &fields);
QJsonObject openSearchEngineToJson(const OpenSearchEngine *engine);
#endif

#endif // OPENSEARCHENGINE_H

