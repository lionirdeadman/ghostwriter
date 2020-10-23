/***********************************************************************
 *
 * Copyright (C) 2014-2020 wereturtle
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 ***********************************************************************/

#include <QVariant>
#include <QFile>
#include <QTextStream>
#include <QString>
#include <QApplication>
#include <QStack>
#include <QDir>
#include <QDesktopServices>
#include <QtConcurrentRun>
#include <QFuture>
#include <QWebChannel>

#include "exporter.h"
#include "htmlpreview.h"
#include "sandboxedwebpage.h"
#include "stringobserver.h"

namespace ghostwriter
{
class HtmlPreviewPrivate
{
    Q_DECLARE_PUBLIC(HtmlPreview)

public:
    HtmlPreviewPrivate(HtmlPreview *q_ptr)
        : q_ptr(q_ptr)
    {
        ;
    }

    ~HtmlPreviewPrivate()
    {
        ;
    }

    HtmlPreview *q_ptr;

    MarkdownDocument *document;
    bool updateInProgress;
    bool updateAgain;
    QString vanillaHtml;
    StringObserver livePreviewHtml;
    StringObserver styleSheet;
    QString baseUrl;
    QRegularExpression headingTagExp;
    Exporter *exporter;
    QString wrapperHtml;
    QFutureWatcher<QString> *futureWatcher;

    void onHtmlReady();
    void onLoadFinished(bool ok);

    /**
     * Sets the base directory path for determining resource
     * paths relative to the web page being previewed.
     * This method is called whenever the file path changes.
     */
    void updateBaseDir();
    /*
    * Sets the HTML contents to display, and creates a backup of the old
    * HTML for diffing to scroll to the first difference whenever
    * updatePreview() is called.
    */
    void setHtmlContent(const QString &html);

    QString exportToHtml(const QString &text, Exporter *exporter) const;
};

HtmlPreview::HtmlPreview
(
    MarkdownDocument *document,
    Exporter *exporter,
    QWidget *parent
) : QWebEngineView(parent),
    d_ptr(new HtmlPreviewPrivate(this))
{
    d_func()->document = document;
    d_func()->updateInProgress = false;
    d_func()->updateAgain = false;
    d_func()->exporter = exporter;

    d_func()->vanillaHtml = "";
    d_func()->baseUrl = "";
    d_func()->livePreviewHtml.setText("");
    d_func()->styleSheet.setText("");

    this->setPage(new SandboxedWebPage(this));
    this->settings()->setDefaultTextEncoding("utf-8");
    this->settings()->setAttribute(QWebEngineSettings::LocalContentCanAccessFileUrls, true);
    this->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    this->page()->action(QWebEnginePage::Reload)->setVisible(false);
    this->page()->action(QWebEnginePage::ReloadAndBypassCache)->setVisible(false);
    this->page()->action(QWebEnginePage::OpenLinkInThisWindow)->setVisible(false);
    this->page()->action(QWebEnginePage::OpenLinkInNewWindow)->setVisible(false);
    this->page()->action(QWebEnginePage::ViewSource)->setVisible(false);
    this->page()->action(QWebEnginePage::SavePage)->setVisible(false);
    QWebEngineProfile::defaultProfile()->setHttpCacheType(QWebEngineProfile::NoCache);
    QWebEngineProfile::defaultProfile()->clearHttpCache();
    QWebEngineProfile::defaultProfile()->clearAllVisitedLinks();

    this->connect
    (
        this,
        &QWebEngineView::loadFinished,
    [this](bool ok) {
        d_func()->onLoadFinished(ok);
    }
    );

    d_func()->headingTagExp.setPattern("^[Hh][1-6]$");

    d_func()->futureWatcher = new QFutureWatcher<QString>(this);
    this->connect
    (
        d_func()->futureWatcher,
        &QFutureWatcher<QString>::finished,
        [this]() {
            d_func()->onHtmlReady();
        }
    );

    this->connect
    (
        document,
        &MarkdownDocument::filePathChanged,
        [this]() {
            d_func()->updateBaseDir();
        }
    );

    // Set zoom factor for Chromium browser to account for system DPI settings,
    // since Chromium assumes 96 DPI as a fixed resolution.
    //
    qreal horizontalDpi = QGuiApplication::primaryScreen()->logicalDotsPerInchX();
    this->setZoomFactor((horizontalDpi / 96.0));

    QWebChannel *channel = new QWebChannel(this);
    channel->registerObject(QStringLiteral("stylesheet"), &d_func()->styleSheet);
    channel->registerObject(QStringLiteral("livepreviewcontent"), &d_func()->livePreviewHtml);
    this->page()->setWebChannel(channel);
    d_func()->wrapperHtml =
        "<!doctype html>"
        "<html lang=\"en\">"
        "<meta charset=\"utf-8\">"
        "<head>"
        "    <script>"
        "         MathJax = {"
        "            tex: {"
        "                inlineMath: [['$', '$']]"
        "             }"
        "         };"
        "    </script>"
        "    <script type=\"text/javascript\" id=\"MathJax-script\" src=\"https://cdn.jsdelivr.net/npm/mathjax@3/es5/tex-mml-chtml.js\"></script>"
        "    <style id='ghostwriter_css' type='text/css' media='all'></style>"
        "    <script src=\"qrc:/qtwebchannel/qwebchannel.js\"></script>"
        "</head>"
        "<body>"
        "    <div id=\"livepreviewplaceholder\"></div>"
        "    <script src=\"qrc:/resources/gw.js\"></script>"
        "    <script>"
        "        new QWebChannel(qt.webChannelTransport,"
        "            function(channel) {"
        "                var styleSheet = channel.objects.stylesheet;"
        "                loadStyleSheet(styleSheet.text);"
        "                styleSheet.textChanged.connect(loadStyleSheet);"
        ""
        "                var content = channel.objects.livepreviewcontent;"
        "                updateText(content.text);"
        "                content.textChanged.connect(updateText);"
        "            }"
        "        );"
        "    </script>"
        "</body>"
        "</html>";

    // Set the base URL and load the preview using wrapperHtml above.
    d_func()->updateBaseDir();
}

HtmlPreview::~HtmlPreview()
{
    // Wait for thread to finish if in the middle of updating the preview.
    d_func()->futureWatcher->waitForFinished();
}

void HtmlPreview::contextMenuEvent(QContextMenuEvent *event)
{
    QMenu *menu = page()->createStandardContextMenu();
    menu->popup(event->globalPos());
}

void HtmlPreview::updatePreview()
{
    if (d_func()->updateInProgress) {
        d_func()->updateAgain = true;
        return;
    }

    if (this->isVisible()) {
        // Some markdown processors don't handle empty text very well
        // and will err.  Thus, only pass in text from the document
        // into the markdown processor if the text isn't empty or null.
        //
        if (d_func()->document->isEmpty()) {
            d_func()->setHtmlContent("");
        } else if (nullptr != d_func()->exporter) {
            QString text = d_func()->document->toPlainText();

            if (!text.isNull() && !text.isEmpty()) {
                d_func()->updateInProgress = true;
                QFuture<QString> future =
                    QtConcurrent::run
                    (
                        d_func(),
                        &HtmlPreviewPrivate::exportToHtml,
                        d_func()->document->toPlainText(),
                        d_func()->exporter
                    );
                d_func()->futureWatcher->setFuture(future);
            }
        }
    }
}

void HtmlPreview::navigateToHeading(int headingSequenceNumber)
{
    this->page()->runJavaScript
    (
        QString
        (
            "document.getElementById('livepreviewhnbr%1').scrollIntoView()"
        ).arg(headingSequenceNumber)
    );
}

void HtmlPreview::setHtmlExporter(Exporter *exporter)
{
    d_func()->exporter = exporter;
    d_func()->setHtmlContent("");
    updatePreview();
}

void HtmlPreview::setStyleSheet(const QString &css)
{
    d_func()->styleSheet.setText(css);
}

void HtmlPreviewPrivate::onHtmlReady()
{
    QString html = futureWatcher->result();

    // Find where the change occurred since last time, and slip an
    // anchor in the location so that we can scroll there.
    //
    QString anchoredHtml = "";

    QTextStream newHtmlDoc((QString *) &html, QIODevice::ReadOnly);
    QTextStream oldHtmlDoc((QString *) & (this->vanillaHtml), QIODevice::ReadOnly);
    QTextStream anchoredHtmlDoc(&anchoredHtml, QIODevice::WriteOnly);

    bool differenceFound = false;
    QString oldLine = oldHtmlDoc.readLine();
    QString newLine = newHtmlDoc.readLine();

    while (!oldLine.isNull() && !newLine.isNull() && !differenceFound) {
        if (oldLine != newLine) {
            // Found the difference, so insert an anchor point at the
            // beginning of the line.
            //
            differenceFound = true;
            anchoredHtmlDoc << "<a id=\"livepreviewmodifypoint\"></a>";
        } else {
            anchoredHtmlDoc << newLine << "\n";
            oldLine = oldHtmlDoc.readLine();
            newLine = newHtmlDoc.readLine();
        }
    }

    // If lines were removed at the end of the new document,
    // ensure anchor point is inserted.
    //
    if (!differenceFound && !oldLine.isNull() && newLine.isNull()) {
        differenceFound = true;
        anchoredHtmlDoc << "<a id=\"livepreviewmodifypoint\"></a>";
    }

    // Put any remaining new HTML data into the
    // anchored HTML string.
    //
    while (!newLine.isNull()) {
        if (!differenceFound) {
            anchoredHtmlDoc << "<a id=\"livepreviewmodifypoint\"></a>";
        }

        differenceFound = true;
        anchoredHtmlDoc << newLine << "\n";
        newLine = newHtmlDoc.readLine();
    }

    if (differenceFound) {
        setHtmlContent(anchoredHtml);
        this->vanillaHtml = html;
    }

    updateInProgress = false;

    if (updateAgain) {
        updateAgain = false;
        q_func()->updatePreview();
    }

}

void HtmlPreviewPrivate::onLoadFinished(bool ok)
{
    if (ok) {
        q_func()->page()->runJavaScript("document.documentElement.contentEditable = false;");
    }
}

void HtmlPreviewPrivate::updateBaseDir()
{
    if (!document->filePath().isNull() && !document->filePath().isEmpty()) {
        // Note that a forward slash ("/") is appended to the path to
        // ensure it works.  If the slash isn't there, then it won't
        // recognize the base URL for some reason.
        //
        baseUrl =
            QUrl::fromLocalFile(QFileInfo(document->filePath()).dir().absolutePath()
                                + "/").toString();
    } else {
        this->baseUrl = "";
    }

    q_func()->setHtml(wrapperHtml, baseUrl);
    q_func()->updatePreview();
}

QSize HtmlPreview::sizeHint() const
{
    return QSize(500, 600);
}

void HtmlPreview::closeEvent(QCloseEvent *event)
{
    Q_UNUSED(event);

    d_func()->setHtmlContent("");
    d_func()->vanillaHtml = "";
    d_func()->livePreviewHtml.setText("");
}

void HtmlPreviewPrivate::setHtmlContent(const QString &html)
{
    static int count = 0;
    count++;
    this->vanillaHtml = html;
    this->livePreviewHtml.setText(html);
}

QString HtmlPreviewPrivate::exportToHtml
(
    const QString &text,
    Exporter *exporter
) const
{
    QString html;

    // Enable smart typography for preview, if available for the exporter.
    bool smartTypographyEnabled = exporter->smartTypographyEnabled();
    exporter->setSmartTypographyEnabled(true);

    // Export to HTML.
    exporter->exportToHtml(text, html);

    // Put smart typography setting back to the way it was before
    // so that the last setting used during document export is remembered.
    //
    exporter->setSmartTypographyEnabled(smartTypographyEnabled);

    return html;
}
} // namespace ghostwriter