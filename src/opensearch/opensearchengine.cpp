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

#include "opensearchengine.h"

#include "opensearchenginedelegate.h"

#if defined(ARORA_RUSTCORE)
#include "rustcore.h"

#include <qjsonobject.h>
#endif

#include <qbuffer.h>
#include <qcoreapplication.h>
#include <qjsonarray.h>
#include <qjsondocument.h>
#include <qlocale.h>
#include <qnetworkrequest.h>
#include <qnetworkreply.h>
#include <qregularexpression.h>
#include <qstringlist.h>
#include <qurlquery.h>

/*!
    \class OpenSearchEngine
    \brief A class representing a single search engine described in OpenSearch format

    OpenSearchEngine is a class that represents a single search engine based on
    the OpenSearch format.
    For more information about the format, see http://www.opensearch.org/.

    Instances of the class hold all the data associated with the corresponding search
    engines, such as name(), description() and also URL templates that are used
    to construct URLs, which can be used later to perform search queries. Search engine
    can also have an image, even an external one, in this case it will be downloaded
    automatically from the network.

    OpenSearchEngine instances can be constructed from scratch but also read from
    external sources and written back to them. OpenSearchReader and OpenSearchWriter
    are the classes provided for reading and writing OpenSearch descriptions.

    Default constructed engines need to be filled with the necessary information before
    they can be used to peform search requests. First of all, a search engine should have
    the metadata including the name and the description.
    However, the most important are URL templates, which are the construction of URLs
    but can also contain template parameters, that are replaced with corresponding values
    at the time of constructing URLs.

    There are two types of URL templates: search URL template and suggestions URL template.
    Search URL template is needed for constructing search URLs, which point directly to
    search results. Suggestions URL template is necessary to construct suggestion queries
    URLs, which are then used for requesting contextual suggestions, a popular service
    offered along with search results that provides search terms related to what has been
    supplied by the user.

    Both types of URLs are constructed by the class, by searchUrl() and suggestionsUrl()
    functions respectively. However, search requests are supposed to be performed outside
    the class, while suggestion queries can be executed using the requestSuggestions()
    method. The class will take care of peforming the network request and parsing the
    JSON response.

    Both the image request and suggestion queries need network access. The class can
    perform network requests on its own, though the client application needs to provide
    a network access manager, which then will to be used for network operations.
    Without that, both images delivered from remote locations and contextual suggestions
    will be disabled.

    \sa OpenSearchReader, OpenSearchWriter
*/

/*!
    Constructs an engine with a given \a parent.
*/
OpenSearchEngine::OpenSearchEngine(QObject *parent)
    : QObject(parent)
    , m_searchMethod(QLatin1String("get"))
    , m_suggestionsMethod(QLatin1String("get"))
    , m_imageSearchMethod(QLatin1String("get"))
    , m_networkAccessManager(nullptr)
    , m_suggestionsReply(nullptr)
    , m_delegate(nullptr)
{
    m_requestMethods.insert(QLatin1String("get"), QNetworkAccessManager::GetOperation);
    m_requestMethods.insert(QLatin1String("post"), QNetworkAccessManager::PostOperation);
}

QString OpenSearchEngine::parseTemplate(const QString &searchTerm, const QString &searchTemplate)
{
    QString language = QLocale().name();
    // Simple conversion to RFC 3066.
    language = language.replace(QLatin1Char('_'), QLatin1Char('-'));

    QString result = searchTemplate;
    result.replace(QLatin1String("{count}"), QLatin1String("20"));
    result.replace(QLatin1String("{startIndex}"), QLatin1String("0"));
    result.replace(QLatin1String("{startPage}"), QLatin1String("0"));
    result.replace(QLatin1String("{language}"), language);
    result.replace(QLatin1String("{inputEncoding}"), QLatin1String("UTF-8"));
    result.replace(QLatin1String("{outputEncoding}"), QLatin1String("UTF-8"));
    result.replace(QRegularExpression(QLatin1String("\\{([^\\}]*:|)source\\??\\}")), QCoreApplication::applicationName());
    result.replace(QLatin1String("{searchTerms}"), QLatin1String(QUrl::toPercentEncoding(searchTerm)));

    return result;
}

/*!
    \property OpenSearchEngine::name
    \brief the name of the engine

    \sa description()
*/
QString OpenSearchEngine::name() const
{
    return m_name;
}

void OpenSearchEngine::setName(QString name)
{
    m_name = std::move(name);
}

/*!
    \property OpenSearchEngine::description
    \brief the description of the engine

    \sa name()
*/
QString OpenSearchEngine::description() const
{
    return m_description;
}

void OpenSearchEngine::setDescription(QString description)
{
    m_description = std::move(description);
}

/*!
    \property OpenSearchEngine::searchUrlTemplate
    \brief the template of the search URL

    \sa searchUrl(), searchParameters(), suggestionsUrlTemplate()
*/
QString OpenSearchEngine::searchUrlTemplate() const
{
    return m_searchUrlTemplate;
}

void OpenSearchEngine::setSearchUrlTemplate(QString searchUrlTemplate)
{
    m_searchUrlTemplate = std::move(searchUrlTemplate);
}

/*!
    Constructs and returns a search URL with a given \a searchTerm.

    The URL template is processed according to the specification:
    http://www.opensearch.org/Specifications/OpenSearch/1.1#OpenSearch_URL_template_syntax

    A list of template parameters currently supported and what they are replaced with:
    \table
    \header \o parameter
            \o value
    \row    \o "{count}"
            \o "20"
    \row    \o "{startIndex}"
            \o "0"
    \row    \o "{startPage}"
            \o "0"
    \row    \o "{language}"
            \o "the default language code (RFC 3066)"
    \row    \o "{inputEncoding}"
            \o "UTF-8"
    \row    \o "{outputEncoding}"
            \o "UTF-8"
    \row    \o "{*:source}"
            \o "application name, QCoreApplication::applicationName()"
    \row    \o "{searchTerms}"
            \o "the string supplied by the user"
    \endtable

    \sa searchUrlTemplate(), searchParameters(), suggestionsUrl()
*/
QUrl OpenSearchEngine::searchUrl(const QString &searchTerm) const
{
    return buildUrl(searchTerm, m_searchUrlTemplate, m_searchMethod,
                    m_searchParameters);
}

/*!
    \property providesSuggestions
    \brief indicates whether the engine supports contextual suggestions
*/
bool OpenSearchEngine::providesSuggestions() const
{
    return !m_suggestionsUrlTemplate.isEmpty();
}

/*!
    \property OpenSearchEngine::suggestionsUrlTemplate
    \brief the template of the suggestions URL

    \sa suggestionsUrl(), suggestionsParameters(), searchUrlTemplate()
*/
QString OpenSearchEngine::suggestionsUrlTemplate() const
{
    return m_suggestionsUrlTemplate;
}

void OpenSearchEngine::setSuggestionsUrlTemplate(QString suggestionsUrlTemplate)
{
    m_suggestionsUrlTemplate = std::move(suggestionsUrlTemplate);
}

/*!
    Constructs a suggestions URL with a given \a searchTerm.

    The URL template is processed according to the specification:
    http://www.opensearch.org/Specifications/OpenSearch/1.1#OpenSearch_URL_template_syntax

    See searchUrl() for more information about processing template parameters.

    \sa suggestionsUrlTemplate(), suggestionsParameters(), searchUrl()
*/
QUrl OpenSearchEngine::suggestionsUrl(const QString &searchTerm) const
{
    return buildUrl(searchTerm, m_suggestionsUrlTemplate,
                    m_suggestionsMethod, m_suggestionsParameters);
}

/*!
    \property providesImageSearch
    \brief indicates whether the engine carries a dedicated image-search
    URL template

    \sa imageSearchUrl(), imageSearchUrlTemplate()
*/
bool OpenSearchEngine::providesImageSearch() const
{
    return !m_imageSearchUrlTemplate.isEmpty();
}

/*!
    \property imageSearchUrlTemplate
    \brief the template of the image-search URL

    A second results endpoint (marked purpose="image" in the
    OpenSearch description) used when the user wants image results
    rather than web results.

    \sa imageSearchUrl(), providesImageSearch()
*/
QString OpenSearchEngine::imageSearchUrlTemplate() const
{
    return m_imageSearchUrlTemplate;
}

void OpenSearchEngine::setImageSearchUrlTemplate(QString imageSearchUrlTemplate)
{
    m_imageSearchUrlTemplate = std::move(imageSearchUrlTemplate);
}

/*!
    Constructs an image-search URL with a given \a searchTerm.

    \sa imageSearchUrlTemplate(), searchUrl()
*/
QUrl OpenSearchEngine::imageSearchUrl(const QString &searchTerm) const
{
    return buildUrl(searchTerm, m_imageSearchUrlTemplate,
                    m_imageSearchMethod, m_imageSearchParameters);
}

QUrl OpenSearchEngine::buildUrl(const QString &searchTerm,
                                const QString &templ,
                                const QString &method,
                                const Parameters &parameters) const
{
    if (templ.isEmpty())
        return QUrl();

#if defined(ARORA_RUSTCORE)
    // OSE01: template expansion + query assembly run in Rust — the
    // port is byte-identical to the parseTemplate()/QUrlQuery
    // pipeline below (probed on Qt 6.12); the result is wrapped in
    // QUrl::fromEncoded exactly like before.
    QJsonObject spec;
    spec.insert(QLatin1String("template"), templ);
    spec.insert(QLatin1String("method"), method);
    QJsonArray params;
    for (const Parameter &parameter : parameters)
        params.append(QJsonArray{parameter.first, parameter.second});
    spec.insert(QLatin1String("params"), params);
    spec.insert(QLatin1String("term"), searchTerm);
    QString language = QLocale().name();
    // Simple conversion to RFC 3066, same as parseTemplate().
    language.replace(QLatin1Char('_'), QLatin1Char('-'));
    spec.insert(QLatin1String("language"), language);
    spec.insert(QLatin1String("source"),
                QCoreApplication::applicationName());
    const QByteArray json =
        QJsonDocument(spec).toJson(QJsonDocument::Compact);
    char *url = rc_ose_expand(
        reinterpret_cast<const uint8_t *>(json.constData()),
        size_t(json.size()));
    if (!url)
        return QUrl();
    const QUrl retVal = QUrl::fromEncoded(QByteArray::fromRawData(
        url, int(strlen(url))));
    rc_string_free(url);
    return retVal;
#else
    QUrl retVal = QUrl::fromEncoded(parseTemplate(searchTerm, templ).toUtf8());

    if (method != QLatin1String("post")) {
        QUrlQuery query(retVal);
        Parameters::const_iterator end = parameters.constEnd();
        Parameters::const_iterator i = parameters.constBegin();
        for (; i != end; ++i)
            query.addQueryItem(i->first, parseTemplate(searchTerm, i->second));
        retVal.setQuery(query);
    }

    return retVal;
#endif
}

/*!
    \property searchParameters
    \brief additional parameters that will be included in the search URL

    For more information see:
    http://www.opensearch.org/Specifications/OpenSearch/Extensions/Parameter/1.0
*/
OpenSearchEngine::Parameters OpenSearchEngine::searchParameters() const
{
    return m_searchParameters;
}

void OpenSearchEngine::setSearchParameters(const Parameters &searchParameters)
{
    m_searchParameters = searchParameters;
}

/*!
    \property suggestionsParameters
    \brief additional parameters that will be included in the suggestions URL

    For more information see:
    http://www.opensearch.org/Specifications/OpenSearch/Extensions/Parameter/1.0
*/
OpenSearchEngine::Parameters OpenSearchEngine::suggestionsParameters() const
{
    return m_suggestionsParameters;
}

void OpenSearchEngine::setSuggestionsParameters(const Parameters &suggestionsParameters)
{
    m_suggestionsParameters = suggestionsParameters;
}

/*!
    \property imageSearchParameters
    \brief additional parameters that will be included in the
    image-search URL
*/
OpenSearchEngine::Parameters OpenSearchEngine::imageSearchParameters() const
{
    return m_imageSearchParameters;
}

void OpenSearchEngine::setImageSearchParameters(const Parameters &imageSearchParameters)
{
    m_imageSearchParameters = imageSearchParameters;
}

/*!
    \property searchMethod
    \brief HTTP request method that will be used to perform search requests
*/
QString OpenSearchEngine::searchMethod() const
{
    return m_searchMethod;
}

void OpenSearchEngine::setSearchMethod(const QString &method)
{
    QString requestMethod = method.toLower();
    if (!m_requestMethods.contains(requestMethod))
        return;

    m_searchMethod = requestMethod;
}

/*!
    \property suggestionsMethod
    \brief HTTP request method that will be used to perform suggestions requests
*/
QString OpenSearchEngine::suggestionsMethod() const
{
    return m_suggestionsMethod;
}

void OpenSearchEngine::setSuggestionsMethod(const QString &method)
{
    QString requestMethod = method.toLower();
    if (!m_requestMethods.contains(requestMethod))
        return;

    m_suggestionsMethod = requestMethod;
}

/*!
    \property imageSearchMethod
    \brief HTTP request method used for image-search requests
*/
QString OpenSearchEngine::imageSearchMethod() const
{
    return m_imageSearchMethod;
}

void OpenSearchEngine::setImageSearchMethod(const QString &method)
{
    QString requestMethod = method.toLower();
    if (!m_requestMethods.contains(requestMethod))
        return;

    m_imageSearchMethod = requestMethod;
}

/*!
    \property imageUrl
    \brief the image URL of the engine

    When setting a new image URL, it won't be loaded immediately. The first request will be
    deferred until image() is called for the first time.

    \note To be able to request external images, you need to provide a network access manager,
          which will be used for network operations.

    \sa image(), networkAccessManager()
*/
QString OpenSearchEngine::imageUrl() const
{
    return m_imageUrl;
}

void OpenSearchEngine::setImageUrl(QString imageUrl)
{
    m_imageUrl = std::move(imageUrl);
}

void OpenSearchEngine::loadImage() const
{
    if (!m_networkAccessManager || m_imageUrl.isEmpty())
        return;

    QNetworkReply *reply = m_networkAccessManager->get(QNetworkRequest(QUrl::fromEncoded(m_imageUrl.toUtf8())));
    connect(reply, &QNetworkReply::finished, this, &OpenSearchEngine::imageObtained);
}

void OpenSearchEngine::imageObtained()
{
    QNetworkReply *reply = qobject_cast<QNetworkReply*>(sender());

    if (!reply)
        return;

    // Engine icons are small; drop oversized responses instead of
    // letting a hostile server pump unlimited data into the image
    // decoder.
    static const qint64 MaximumImageSize = 1024 * 1024;
    QByteArray response;
    if (reply->size() <= MaximumImageSize)
        response = reply->readAll();

    reply->close();
    reply->deleteLater();

    if (response.isEmpty())
        return;

    m_image.loadFromData(response);
    emit imageChanged();
}

/*!
    \property image
    \brief the image of the engine

    When no image URL has been set and an image will be set explicitly, a new data URL
    will be constructed, holding the image data encoded with Base64.

    \sa imageUrl()
*/
QImage OpenSearchEngine::image() const
{
    if (m_image.isNull())
        loadImage();
    return m_image;
}

void OpenSearchEngine::setImage(const QImage &image)
{
    if (m_imageUrl.isEmpty()) {
        QBuffer imageBuffer;
        imageBuffer.open(QBuffer::ReadWrite);
        if (image.save(&imageBuffer, "PNG")) {
            m_imageUrl = QString(QLatin1String("data:image/png;base64,%1"))
                         .arg(QLatin1String(imageBuffer.buffer().toBase64()));
        }
    }

    m_image = image;
    emit imageChanged();
}

/*!
    \property valid
    \brief indicates whether the engine is valid i.e. the description was properly formed and included all necessary information
*/
bool OpenSearchEngine::isValid() const
{
    return (!m_name.isEmpty() && !m_searchUrlTemplate.isEmpty());
}

bool OpenSearchEngine::operator==(const OpenSearchEngine &other) const
{
    return (m_name == other.m_name
            && m_description == other.m_description
            && m_imageUrl == other.m_imageUrl
            && m_searchUrlTemplate == other.m_searchUrlTemplate
            && m_suggestionsUrlTemplate == other.m_suggestionsUrlTemplate
            && m_imageSearchUrlTemplate == other.m_imageSearchUrlTemplate
            && m_searchParameters == other.m_searchParameters
            && m_suggestionsParameters == other.m_suggestionsParameters
            && m_imageSearchParameters == other.m_imageSearchParameters);
}

bool OpenSearchEngine::operator<(const OpenSearchEngine &other) const
{
    return (m_name < other.m_name);
}

/*!
    Requests contextual suggestions on the search engine, for a given \a searchTerm.

    If succeeded, suggestions() signal will be emitted once the suggestions are received.

    \note To be able to request suggestions, you need to provide a network access manager,
          which will be used for network operations.

    \sa requestSearchResults()
*/
void OpenSearchEngine::requestSuggestions(const QString &searchTerm)
{
    if (searchTerm.isEmpty() || !providesSuggestions())
        return;

    Q_ASSERT(m_networkAccessManager);

    if (!m_networkAccessManager)
        return;

    if (m_suggestionsReply) {
        m_suggestionsReply->disconnect(this);
        m_suggestionsReply->abort();
        m_suggestionsReply->deleteLater();
        m_suggestionsReply = nullptr;
    }

    Q_ASSERT(m_requestMethods.contains(m_suggestionsMethod));
    if (m_suggestionsMethod == QLatin1String("get")) {
        m_suggestionsReply = m_networkAccessManager->get(QNetworkRequest(suggestionsUrl(searchTerm)));
    } else {
        QStringList parameters;
        Parameters::const_iterator end = m_suggestionsParameters.constEnd();
        Parameters::const_iterator i = m_suggestionsParameters.constBegin();
        for (; i != end; ++i)
            parameters.append(i->first + QLatin1String("=") + i->second);

        QByteArray data = parameters.join(QLatin1String("&")).toUtf8();
        m_suggestionsReply = m_networkAccessManager->post(QNetworkRequest(suggestionsUrl(searchTerm)), data);
    }

    connect(m_suggestionsReply, &QNetworkReply::finished, this, &OpenSearchEngine::suggestionsObtained);
}

/*!
    Requests search results on the search engine, for a given \a searchTerm.

    The default implementation does nothing, to supply your own you need to create your own
    OpenSearchEngineDelegate subclass and supply it to the engine. Then the function will call
    the performSearchRequest() method of the delegate, which can then handle the request
    in a custom way.

    \sa requestSuggestions(), delegate()
*/
void OpenSearchEngine::requestSearchResults(const QString &searchTerm)
{
    if (!m_delegate || searchTerm.isEmpty())
        return;

    Q_ASSERT(m_requestMethods.contains(m_searchMethod));

    QNetworkRequest request(QUrl(searchUrl(searchTerm)));
    QByteArray data;
    QNetworkAccessManager::Operation operation = m_requestMethods.value(m_searchMethod);

    if (operation == QNetworkAccessManager::PostOperation) {
        QStringList parameters;
        Parameters::const_iterator end = m_searchParameters.constEnd();
        Parameters::const_iterator i = m_searchParameters.constBegin();
        for (; i != end; ++i)
            parameters.append(i->first + QLatin1String("=") + i->second);

        data = parameters.join(QLatin1String("&")).toUtf8();
    }

    m_delegate->performSearchRequest(request, operation, data);
}

void OpenSearchEngine::suggestionsObtained()
{
    // Suggestions replies are small JSON arrays; cap what we parse.
    static const qint64 MaximumSuggestionsSize = 256 * 1024;
    QByteArray response = m_suggestionsReply->size() <= MaximumSuggestionsSize
        ? m_suggestionsReply->readAll().trimmed() : QByteArray();

    m_suggestionsReply->close();
    m_suggestionsReply->deleteLater();
    m_suggestionsReply = nullptr;

    if (response.isEmpty())
        return;

#if defined(ARORA_RUSTCORE)
    // SEC19: remote JSON is parsed by memory-safe Rust; the reply
    // schema ([term, [s1, ...]]) is enforced there and a corrupt
    // reply simply yields no suggestions, like the old early returns.
    RcBuffer out{};
    const RcStatus parsed = rc_suggest_parse(
        reinterpret_cast<const uint8_t *>(response.constData()),
        size_t(response.size()), &out);
    if (parsed != RC_OK)
        return;
    const QByteArray json(reinterpret_cast<const char *>(out.data),
                          qsizetype(out.len));
    rc_buffer_free(out);

    QStringList suggestionsList;
    const QJsonArray suggestionsArray =
        QJsonDocument::fromJson(json).array();
    for (const QJsonValue &value : suggestionsArray)
        suggestionsList.append(value.toString());

    emit suggestions(suggestionsList);
#else
    // The suggestions response is a JSON array: ["term", ["sug1", ...]].
    const QJsonDocument document = QJsonDocument::fromJson(response);
    if (!document.isArray())
        return;

    const QJsonArray parts = document.array();
    if (parts.size() < 2 || !parts.at(1).isArray())
        return;

    QStringList suggestionsList;
    const QJsonArray suggestionsArray = parts.at(1).toArray();
    for (const QJsonValue &value : suggestionsArray)
        suggestionsList.append(value.toString());

    emit suggestions(suggestionsList);
#endif // ARORA_RUSTCORE
}

/*!
    \property networkAccessManager
    \brief the network access manager that is used to perform network requests

    It is required for network operations: loading external images and requesting
    contextual suggestions.
*/
QNetworkAccessManager *OpenSearchEngine::networkAccessManager() const
{
    return m_networkAccessManager;
}

void OpenSearchEngine::setNetworkAccessManager(QNetworkAccessManager *networkAccessManager)
{
    m_networkAccessManager = networkAccessManager;
}

/*!
    \property delegate
    \brief the delegate that is used to perform specific tasks.

    It can be currently supplied to provide a custom behaviour ofthe requetSearchResults() method.
    The default implementation does nothing.
*/
OpenSearchEngineDelegate *OpenSearchEngine::delegate() const
{
    return m_delegate;
}

void OpenSearchEngine::setDelegate(OpenSearchEngineDelegate *delegate)
{
    m_delegate = delegate;
}

/*!
    \fn void OpenSearchEngine::imageChanged()

    This signal is emitted whenever the image of the engine changes.

    \sa image(), imageUrl()
*/

/*!
    \fn void OpenSearchEngine::suggestions(const QStringList &suggestions)

    This signal is emitted whenever new contextual suggestions have been provided
    by the search engine. To request suggestions, use requestSuggestions().
    The suggestion set is specified by \a suggestions.

    \sa requestSuggestions()
*/

#if defined(ARORA_RUSTCORE)
// OSE01: rc_opensearch_parse field map <-> engine marshaling.  The
// schema is the one rustcore.h documents for rc_ose_get: name,
// description, imageUrl plus a search/suggestions/image slot of
// {template, method, params:[[k,v],...]}.
static OpenSearchEngine::Parameters parametersOfJson(
    const QJsonObject &slot)
{
    OpenSearchEngine::Parameters parameters;
    const QJsonArray pairs = slot.value(QLatin1String("params")).toArray();
    for (const QJsonValue &pair : pairs) {
        const QJsonArray kv = pair.toArray();
        if (kv.size() == 2)
            parameters.append(OpenSearchEngine::Parameter(
                kv.at(0).toString(), kv.at(1).toString()));
    }
    return parameters;
}

void openSearchEngineApplyJson(OpenSearchEngine *engine,
                               const QJsonObject &root)
{
    if (!engine)
        return;

    engine->setName(root.value(QLatin1String("name")).toString());
    engine->setDescription(
        root.value(QLatin1String("description")).toString());
    engine->setImageUrl(root.value(QLatin1String("imageUrl")).toString());

    const QJsonObject search = root.value(QLatin1String("search")).toObject();
    if (!search.isEmpty()) {
        engine->setSearchUrlTemplate(
            search.value(QLatin1String("template")).toString());
        engine->setSearchParameters(parametersOfJson(search));
        engine->setSearchMethod(
            search.value(QLatin1String("method")).toString());
    }
    const QJsonObject suggestions =
        root.value(QLatin1String("suggestions")).toObject();
    if (!suggestions.isEmpty()) {
        engine->setSuggestionsUrlTemplate(
            suggestions.value(QLatin1String("template")).toString());
        engine->setSuggestionsParameters(parametersOfJson(suggestions));
        engine->setSuggestionsMethod(
            suggestions.value(QLatin1String("method")).toString());
    }
    const QJsonObject image = root.value(QLatin1String("image")).toObject();
    if (!image.isEmpty()) {
        engine->setImageSearchUrlTemplate(
            image.value(QLatin1String("template")).toString());
        engine->setImageSearchParameters(parametersOfJson(image));
        engine->setImageSearchMethod(
            image.value(QLatin1String("method")).toString());
    }
}

QJsonObject openSearchEngineToJson(const OpenSearchEngine *engine)
{
    QJsonObject root;
    if (!engine)
        return root;

    const auto slotOf = [](const QString &templ, const QString &method,
                           const OpenSearchEngine::Parameters &parameters) {
        QJsonObject slot;
        slot.insert(QLatin1String("template"), templ);
        slot.insert(QLatin1String("method"), method);
        QJsonArray pairs;
        for (const OpenSearchEngine::Parameter &parameter : parameters)
            pairs.append(QJsonArray{parameter.first, parameter.second});
        slot.insert(QLatin1String("params"), pairs);
        return slot;
    };

    root.insert(QLatin1String("name"), engine->name());
    root.insert(QLatin1String("description"), engine->description());
    root.insert(QLatin1String("imageUrl"), engine->imageUrl());
    if (!engine->searchUrlTemplate().isEmpty()) {
        root.insert(QLatin1String("search"),
                    slotOf(engine->searchUrlTemplate(),
                           engine->searchMethod(),
                           engine->searchParameters()));
    }
    if (!engine->suggestionsUrlTemplate().isEmpty()) {
        root.insert(QLatin1String("suggestions"),
                    slotOf(engine->suggestionsUrlTemplate(),
                           engine->suggestionsMethod(),
                           engine->suggestionsParameters()));
    }
    if (!engine->imageSearchUrlTemplate().isEmpty()) {
        root.insert(QLatin1String("image"),
                    slotOf(engine->imageSearchUrlTemplate(),
                           engine->imageSearchMethod(),
                           engine->imageSearchParameters()));
    }
    return root;
}
#endif // ARORA_RUSTCORE
