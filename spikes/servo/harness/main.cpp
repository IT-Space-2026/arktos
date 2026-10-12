/* ENG03 spike harness: one Qt widget hosting a libservo WebView.
 *
 * Usage: servospike [--smoke SECONDS] <url>
 *   default: interactive window (works under xcb; offscreen renders
 *   into memory so it is also usable headless).
 *   --smoke: headless check — loads the url, pumps the loop until
 *   LoadStatus::Complete or the timeout, prints a PASS/FAIL line and
 *   exits. `servospike --smoke about:blank` is the --servo-smoke stand-in.
 */

#include "servoview.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QTimer>
#include <QUrl>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    QStringList args = app.arguments().mid(1);
    bool smoke = false;
    int timeoutMs = 30000;
    if (!args.isEmpty() && args.first() == QStringLiteral("--smoke")) {
        smoke = true;
        args.removeFirst();
        if (!args.isEmpty() && args.first().toInt() > 0) {
            timeoutMs = args.first().toInt() * 1000;
            args.removeFirst();
        }
    }
    const QUrl url = args.isEmpty() ? QUrl(QStringLiteral("about:blank"))
                                    : QUrl::fromUserInput(args.first());

    ServoView view;
    if (!view.isReady()) {
        qWarning("FAIL servo init");
        return 1;
    }
    view.resize(800, 600);
    view.show();
    view.load(url);

    if (!smoke)
        return app.exec();

    // The initial about:blank inside se_init fires its own Complete —
    // only accept completion once the requested URL itself navigated.
    const bool blankTarget =
        url.toString() == QStringLiteral("about:blank");
    bool navigated = blankTarget;
    bool complete = false;
    QObject::connect(&view, &ServoView::urlChanged,
                     [&] (const QUrl &u) { if (u == url) navigated = true; });
    QObject::connect(&view, &ServoView::loadComplete,
                     [&] { if (navigated) complete = true; });

    // Pump at ~60 Hz until Complete or timeout — wake() also queues
    // pumps, the timer just bounds the idle gaps.
    QElapsedTimer elapsed;
    elapsed.start();
    QTimer pump;
    pump.setInterval(16);
    QObject::connect(&pump, &QTimer::timeout, &view, &ServoView::pump);
    pump.start();

    while (!complete && elapsed.elapsed() < timeoutMs)
        app.processEvents(QEventLoop::WaitForMoreEvents, 40);

    const QImage frame = view.frame();
    qInfo("requests observed through interceptor hook: %lu", view.requestCount());
    qInfo("final url=%s title=%s frame=%dx%d",
          qPrintable(view.url().toString()), qPrintable(view.title()),
          frame.width(), frame.height());
    const QString log = view.requestLog();
    if (!log.isEmpty())
        qInfo("request log:\n%s", qPrintable(log));
    if (complete) {
        qInfo("PASS %s loaded", qPrintable(url.toString()));
        return 0;
    }
    qWarning("FAIL %s did not reach LoadStatus::Complete in %d ms",
             qPrintable(url.toString()), timeoutMs);
    return 1;
}
