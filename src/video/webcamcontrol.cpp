/*************************************************************************************
 *  Copyright (C) 2012 by Alejandro Fiestas Olivares <afiestaso@kde.org>             *
 *                                                                                   *
 *  This program is free software; you can redistribute it and/or                    *
 *  modify it under the terms of the GNU General Public License                      *
 *  as published by the Free Software Foundation; either version 2                   *
 *  of the License, or (at your option) any later version.                           *
 *                                                                                   *
 *  This program is distributed in the hope that it will be useful,                  *
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of                   *
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the                    *
 *  GNU General Public License for more details.                                     *
 *                                                                                   *
 *  You should have received a copy of the GNU General Public License                *
 *  along with this program; if not, write to the Free Software                      *
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA   *
 *************************************************************************************/


#include "webcamcontrol.h"
#include "kamosoSettings.h"
#include <devicemanager.h>
#include <kamosodirmodel.h>
#include <previewfetcher.h>
#include <whitewidgetmanager.h>
#include <video/videoelement.h>
#include <kamoso.h>
#include <KIO/CopyJob>
#include <KNotification>
#include <KLocalizedString>

#include <gst/gstcaps.h>
#include <gst/gstpad.h>
#include <gst/gststructure.h>
#include <gst/gstbuffer.h>
#include <gst/gstpipeline.h>
#include <gst/gstelementfactory.h>
#include <gst/gstparse.h>
#include <gst/gstbus.h>
#include <gst/gstmessage.h>
#include <gst/gst.h>

#include <QDir>
#include <QDebug>

#include <QtQml/QQmlEngine>
#include <QtQml/QQmlContext>
#include <QtQml/QQmlApplicationEngine>
#include <qqml.h>
#include <KJob>
#include <KLocalizedContext>

#include "gstpipelineplayer.h"

static QString debugMessage(GstMessage* msg)
{
    gchar *debug = nullptr;
    GError *e = nullptr;
    if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_WARNING) {
        gst_message_parse_warning(msg, &e, &debug);
    } else if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR) {
        gst_message_parse_error(msg, &e, &debug);
    }
    if (!debug)
        return {};

    if (e) {
        qWarning() << "error debugMessage:" << e->message;
        g_error_free (e);
    }
    const auto ret = QString::fromUtf8(debug);
    g_free(debug);
    return ret;
}

template <class T>
GstState pipelineCurrentState(const T &pipe)
{
    GstState currentState, pendingState;
    GstStateChangeReturn result = gst_element_get_state(GST_ELEMENT(pipe.data()), &currentState, &pendingState, GST_CLOCK_TIME_NONE );
    if(result == GST_STATE_CHANGE_FAILURE)
        qDebug() << "broken state";
    return currentState;
}

WebcamControl::WebcamControl()
{
    gst_init(NULL, NULL);
    // It's necessary to load the plugin before the qml loads
    GstElement *sink = gst_element_factory_make ("qmlglsink", NULL);
    gst_object_unref(sink);

    m_engine = new QQmlApplicationEngine(this);
    m_engine->rootContext()->setContextObject(new KLocalizedContext(m_engine));

    qmlRegisterUncreatableType<Device>("org.kde.kamoso", 3, 0, "Device", "You're not supposed to mess with this yo");
    qmlRegisterType<KamosoDirModel>("org.kde.kamoso", 3, 0, "DirModel");
    qmlRegisterType<PreviewFetcher>("org.kde.kamoso", 3, 0, "PreviewFetcher");
    qmlRegisterType<VideoElement>("VideoElement", 1, 0, "VideoElement");

    qmlRegisterUncreatableType<KJob>("org.kde.kamoso", 3, 0, "KJob", "you're not supposed to do that");

    m_engine->rootContext()->setContextProperty("config", Settings::self());
    m_engine->rootContext()->setContextProperty("whites", new WhiteWidgetManager(this));
    m_engine->rootContext()->setContextProperty("devicesModel", DeviceManager::self());
    m_engine->rootContext()->setContextProperty("webcam", new Kamoso(this));

    m_engine->load(QUrl("qrc:/qml/Main.qml"));

    connect(DeviceManager::self(), &DeviceManager::playingDeviceChanged, this, &WebcamControl::play);
    connect(DeviceManager::self(), &DeviceManager::noDevices, this, &WebcamControl::stop);
}

WebcamControl::~WebcamControl()
{
    DeviceManager::self()->save();
    Settings::self()->save();
}

void WebcamControl::stop()
{
    if(m_pipeline) {
        gst_element_set_state(GST_ELEMENT(m_pipeline.data()), GST_STATE_NULL);
        m_pipeline.reset(nullptr);
    }
}

bool WebcamControl::play()
{
    auto dev = DeviceManager::self()->playingDevice();
    return !dev || playDevice(dev);
}

static gboolean webcamWatch(GstBus */*bus*/, GstMessage *message, gpointer user_data)
{
    WebcamControl* wc = static_cast<WebcamControl*>(user_data);
    wc->onBusMessage(message);
    return G_SOURCE_CONTINUE;
}

void WebcamControl::gstElementDeleter(GstElement* element)
{
    gst_element_set_state(GST_ELEMENT(element), GST_STATE_NULL);
    gst_object_unref(GST_OBJECT(element));
}

void WebcamControl::gstPipelineDeleter(GstPipeline* element)
{
    gst_element_set_state(GST_ELEMENT(element), GST_STATE_NULL);
    gst_object_unref(GST_OBJECT(element));
}

bool WebcamControl::playDevice(Device *device)
{
    Q_ASSERT(device);

    //If we already have a pipeline for this device, just set it to video mode
    if (!m_pipeline.isNull() && m_currentDevice == device->udi()) {
        g_object_set(m_pipeline.data(), "mode", 2, nullptr);
        return true;
    }

    if (!m_pipeline.isNull()) {
        gst_element_set_state(GST_ELEMENT(m_pipeline.data()), GST_STATE_NULL);
    }

    if (m_cameraSource.isNull()) {
        m_cameraSource.reset(gst_element_factory_make("wrappercamerabinsrc", "video_balance"), gstElementDeleter);
        // Another option here is to return true, therefore continuing with launching, but
        // in that case the application is mostly useless.
        if (m_cameraSource.isNull()) {
            qWarning() << "The webcam controller was unable to find or load wrappercamerabinsrc plugin;"
                       << "please make sure all required gstreamer plugins are installed.";
            return false;
        }

        auto source = gst_element_factory_make("v4l2src", "v4l2src");
        g_object_set(m_cameraSource.data(), "video-source", source, nullptr);
    }

    // If the webcam uid changes, we are going to change the video-source component from camerabin
    if (m_currentDevice != device->udi()) {
        GstElement* source;
        g_object_get(m_cameraSource.data(), "video-source", &source, nullptr);
        g_object_set(source, "device", device->path().toUtf8().constData(), nullptr);
    }

    if (m_pipeline.isNull()) {
        // Get qml item that will display our video
        QObject* rootObject = m_engine->rootObjects().first();
        QObject* webcamVideoOutput = rootObject->findChild<QObject*>("webcamVideoOutput");

        // Create camerabin to get webcam stream
        m_pipeline.reset(GST_PIPELINE(gst_element_factory_make("camerabin", "camerabin")), gstPipelineDeleter);
        gst_bus_add_watch(gst_pipeline_get_bus(m_pipeline.data()), &webcamWatch, this);
        g_object_set(m_pipeline.data(), "camera-source", m_cameraSource.data(), nullptr);

        // Convert video from webcam and set the caps
        GstElement* videoconvert = gst_element_factory_make("videoconvert", nullptr);
        GstElement*capsfilter = gst_element_factory_make("capsfilter", nullptr);
        GstCaps* caps = gst_caps_from_string("video/x-raw, format=RGBA, framerate=30/1");
        // Add glupload and do the connection with the qmlglsink for the qml widget
        GstElement* glupload = gst_element_factory_make("glupload", nullptr);
        GstElement* sink = gst_element_factory_make("qmlglsink", nullptr);

        // Connect and set all elements
        g_object_set(capsfilter, "caps", caps, nullptr);
        gst_bin_add_many(GST_BIN(m_pipeline.data()), videoconvert, capsfilter, glupload, sink, nullptr);
        gst_element_link_many(videoconvert, capsfilter, glupload, sink, nullptr);
        g_object_set(m_pipeline.data(), "viewfinder-sink", videoconvert, nullptr);
        g_object_set(sink, "widget", webcamVideoOutput, nullptr);
    }

    setVideoSettings();

    auto window = static_cast<QQuickWindow*>(m_engine->rootObjects().first());
    connect(window, &QQuickWindow::beforeSynchronizing, this, &WebcamControl::gstPipelinePlayer);

    m_currentDevice = device->udi();
    return true;
}

void WebcamControl::gstPipelinePlayer()
{
    auto window = static_cast<QQuickWindow*>(m_engine->rootObjects().first());
    disconnect(window, &QQuickWindow::beforeSynchronizing, 0, 0);
    gst_element_set_state(GST_ELEMENT(m_pipeline.data()), GST_STATE_PLAYING);
}

void WebcamControl::onBusMessage(GstMessage* message)
{
    switch (GST_MESSAGE_TYPE (message)) {
    case GST_MESSAGE_EOS: //End of stream. We reached the end of the file.
        qWarning() << __PRETTY_FUNCTION__ << "EOS!";
        stop();
        break;
    case GST_MESSAGE_ERROR: {//Some error occurred.
        static int error = 0;
        qWarning() << __PRETTY_FUNCTION__ << "Error:" << debugMessage(message);
        stop();
        if (error < 3) {
            play();
            //++error;
        }
    }   break;
    case GST_MESSAGE_ELEMENT:
        if (strcmp (GST_MESSAGE_SRC_NAME (message), "camerabin") == 0) {
            auto structure = gst_message_get_structure (message);
            if (gst_structure_get_name (structure) == QByteArray("image-done")) {
                const gchar *filename = gst_structure_get_string(structure, "filename");
                if (m_emitTaken)
                    Q_EMIT photoTaken(QString::fromUtf8(filename));
            }
        } else {
            qDebug() << "skipping message..." << GST_MESSAGE_SRC_NAME (message);
        }
    default:
        /*
        qDebug() << msg->type();
        qDebug() << msg->typeName();
        qDebug() << msg->internalStructure()->name();
        */
        break;
    }
}
void WebcamControl::takePhoto(const QUrl &url, bool emitTaken)
{
    if (m_pipeline.isNull()) {
        qWarning() << "Couldn't take photo, no pipeline";
        return;
    }
    m_emitTaken = emitTaken;

    g_object_set(m_pipeline.data(), "mode", 1, nullptr);
    const QString path = url.isLocalFile() ? url.toLocalFile() : QStandardPaths::writableLocation(QStandardPaths::TempLocation)+"/kamoso_photo.jpg";
    g_object_set(m_pipeline.data(), "location", path.toUtf8().constData(), nullptr);

    g_signal_emit_by_name(m_pipeline.data(), "start-capture", nullptr);

    if (!url.isLocalFile()) {
        KIO::copy(QUrl::fromLocalFile(path), url);
    }

    if (emitTaken) {
        KNotification::event(QStringLiteral("photoTaken"), i18n("Photo taken"), i18n("Saved in %1", url.toDisplayString(QUrl::PreferLocalFile)));
    }
    g_object_set(m_pipeline.data(), "mode", 2, nullptr);
}

void WebcamControl::startRecording()
{
    QString date = QDateTime::currentDateTime().toString("ddmmyyyy_hhmmss");
    m_tmpVideoPath = QDir::tempPath() + QStringLiteral("/kamoso_%1.mkv").arg(date);

    g_object_set(m_pipeline.data(), "mode", 2, nullptr);
    g_object_set(m_pipeline.data(), "location", m_tmpVideoPath.toUtf8().constData(), nullptr);

    g_signal_emit_by_name(m_pipeline.data(), "start-capture", 0);
}

QString WebcamControl::stopRecording()
{
    g_signal_emit_by_name(m_pipeline.data(), "stop-capture", 0);
    return m_tmpVideoPath;
}

void WebcamControl::setExtraFilters(const QString& extraFilters)
{
    if (extraFilters != m_extraFilters) {
        m_extraFilters = extraFilters;
        updateSourceFilter();
    }
}

void WebcamControl::updateSourceFilter()
{

}

void WebcamControl::setVideoSettings()
{
    Device *device = DeviceManager::self()->playingDevice();
    connect(device, &Device::filtersChanged, this, &WebcamControl::setExtraFilters);

    m_extraFilters = device->filters();
    updateSourceFilter();
}
