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


#ifndef WEBCAMCONTROL_H
#define WEBCAMCONTROL_H

#include <QObject>
#include <QSharedPointer>
#include <QUrl>

#include <gst/gstpipeline.h>
#include <gst/gstmessage.h>

//namespace QGst { namespace Quick { class VideoSurface; } }

class Device;
class QQmlApplicationEngine;
class WebcamControl : public QObject
{
    Q_OBJECT
    public:
        WebcamControl();
        virtual ~WebcamControl();

        void onBusMessage(GstMessage* msg);
        void setMirrored(bool m) {
            if (m != m_mirror) {
                m_mirror = m;
                updateSourceFilter();
                Q_EMIT mirroredChanged(m);
            }
        }

        bool mirrored() const { return m_mirror; }

    public Q_SLOTS:
        bool play();
        bool playDevice(Device* device);
        void stop();
        void takePhoto(const QUrl& url, bool emitTaken);
        void startRecording();
        QString stopRecording();

    private Q_SLOTS:
        void setExtraFilters(const QString &extraFilters);

    Q_SIGNALS:
        void photoTaken(const QString &photoUrl);
        void mirroredChanged(bool mirrored);

    private:
        void updateSourceFilter();
        void setVideoSettings();

        /**
         * @brief GstElement deleter
         *
         */
        static void gstElementDeleter(GstElement* element);
        static void gstPipelineDeleter(GstPipeline* element);

        void gstPipelinePlayer();

        QString m_extraFilters;
        QString m_tmpVideoPath;
        QString m_currentDevice;
        QSharedPointer<GstPipeline> m_pipeline;
        QSharedPointer<GstElement> m_cameraSource;
        bool m_emitTaken = true;
        bool m_mirror = true;
        QQmlApplicationEngine* m_engine;
};

#endif // WEBCAMCONTROL_H
