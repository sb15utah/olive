/***

  Olive - Non-Linear Video Editor
  Copyright (C) 2019 Olive Team

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.

***/

#include "viewerglwidget.h"

#include <OpenImageIO/imagebuf.h>
#include <QFileInfo>
#include <QMessageBox>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QOpenGLTexture>

#include "common/define.h"
#include "render/backend/opengl/openglrenderfunctions.h"
#include "render/backend/opengl/openglshader.h"
#include "render/pixelformat.h"

#ifdef Q_OS_LINUX
bool ViewerGLWidget::nouveau_check_done_ = false;
#endif

ViewerGLWidget::ViewerGLWidget(QWidget *parent) :
  QOpenGLWidget(parent),
  color_manager_(nullptr),
  has_image_(false)
{
  setContextMenuPolicy(Qt::CustomContextMenu);
}

ViewerGLWidget::~ViewerGLWidget()
{
  ContextCleanup();
}

void ViewerGLWidget::ConnectColorManager(ColorManager *color_manager)
{
  if (color_manager_ != nullptr) {
    disconnect(color_manager_, SIGNAL(ConfigChanged()), this, SLOT(RefreshColorPipeline()));
  }

  color_manager_ = color_manager;

  if (color_manager_ != nullptr) {
    connect(color_manager_, SIGNAL(ConfigChanged()), this, SLOT(RefreshColorPipeline()));
  }

  RefreshColorPipeline();
}

void ViewerGLWidget::DisconnectColorManager()
{
  ConnectColorManager(nullptr);
}

void ViewerGLWidget::SetMatrix(const QMatrix4x4 &mat)
{
  matrix_ = mat;
  update();
}

void ViewerGLWidget::SetImage(const QString &fn)
{
  has_image_ = false;

  if (!fn.isEmpty()) {
    auto input = OIIO::ImageInput::open(fn.toStdString());

    if (input) {

      PixelFormat::Format image_format = PixelFormat::OIIOFormatToOliveFormat(input->spec().format,
                                                                              input->spec().nchannels == kRGBAChannels);

      // Ensure the following texture operations are done in our context (in case we're in a separate window for instance)
      makeCurrent();

      if (!texture_.IsCreated()
          || texture_.width() != input->spec().width
          || texture_.height() != input->spec().height
          || texture_.format() != image_format) {
        load_buffer_.destroy();
        texture_.Destroy();

        load_buffer_.set_width(input->spec().width);
        load_buffer_.set_height(input->spec().height);
        load_buffer_.set_format(image_format);
        load_buffer_.allocate();

        texture_.Create(context(), input->spec().width, input->spec().height, image_format);
      }

      input->read_image(input->spec().format, load_buffer_.data());
      input->close();

      texture_.Upload(load_buffer_.data());

      doneCurrent();

      has_image_ = true;

#if OIIO_VERSION < 10903
      OIIO::ImageInput::destroy(input);
#endif

    } else {
      qWarning() << "OIIO Error:" << OIIO::geterror().c_str();
    }
  }

  update();
}

void ViewerGLWidget::SetOCIODisplay(const QString &display)
{
  ocio_display_ = display;

  // Determine if the selected view is available in this display
  if (color_manager_
      && !color_manager_->ListAvailableViews(ocio_display_).contains(ocio_view_)) {
    // If not, set to the default view for this display
    ocio_view_ = color_manager_->GetDefaultView(ocio_display_);
  }

  SetupColorProcessor();
  update();
}

void ViewerGLWidget::SetOCIOView(const QString &view)
{
  ocio_view_ = view;
  SetupColorProcessor();
  update();
}

void ViewerGLWidget::SetOCIOLook(const QString &look)
{
  ocio_look_ = look;
  SetupColorProcessor();
  update();
}

ColorManager *ViewerGLWidget::color_manager() const
{
  return color_manager_;
}

const QString &ViewerGLWidget::ocio_display() const
{
  return ocio_display_;
}

const QString &ViewerGLWidget::ocio_view() const
{
  return ocio_view_;
}

const QString &ViewerGLWidget::ocio_look() const
{
  return ocio_look_;
}

void ViewerGLWidget::mousePressEvent(QMouseEvent *event)
{
  QOpenGLWidget::mousePressEvent(event);

  emit DragStarted();
}

void ViewerGLWidget::SetOCIOParameters(const QString &display, const QString &view, const QString &look)
{
  ocio_display_ = display;
  ocio_view_ = view;
  ocio_look_ = look;
  SetupColorProcessor();
  update();
}

void ViewerGLWidget::initializeGL()
{
  SetupColorProcessor();

  connect(context(), SIGNAL(aboutToBeDestroyed()), this, SLOT(ContextCleanup()), Qt::DirectConnection);

#ifdef Q_OS_LINUX
  if (!nouveau_check_done_) {
    const char* vendor = reinterpret_cast<const char*>(context()->functions()->glGetString(GL_VENDOR));

    if (!strcmp(vendor, "nouveau")) {
      // Working with Qt widgets in this function segfaults, so we queue the messagebox for later
      QMetaObject::invokeMethod(this,
                                "ShowNouveauWarning",
                                Qt::QueuedConnection);
    }

    nouveau_check_done_ = true;
  }
#endif
}

void ViewerGLWidget::paintGL()
{
  // Get functions attached to this context (they will already be initialized)
  QOpenGLFunctions* f = context()->functions();

  // Clear background to empty
  f->glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
  f->glClear(GL_COLOR_BUFFER_BIT);

  // We only draw if we have a pipeline
  if (!has_image_ || !color_service_ || !texture_.IsCreated()) {
    return;
  }

  // Bind retrieved texture
  f->glBindTexture(GL_TEXTURE_2D, texture_.texture());

  // Blit using the color service
  color_service_->ProcessOpenGL(true, matrix_);

  // Release retrieved texture
  f->glBindTexture(GL_TEXTURE_2D, 0);
}

void ViewerGLWidget::RefreshColorPipeline()
{
  if (!color_manager_) {
    color_service_ = nullptr;
    return;
  }

  QStringList displays = color_manager_->ListAvailableDisplays();
  if (!displays.contains(ocio_display_)) {
    ocio_display_ = color_manager_->GetDefaultDisplay();
  }

  QStringList views = color_manager_->ListAvailableViews(ocio_display_);
  if (!views.contains(ocio_view_)) {
    ocio_view_ = color_manager_->GetDefaultView(ocio_display_);
  }

  QStringList looks = color_manager_->ListAvailableLooks();
  if (!looks.contains(ocio_look_)) {
    ocio_look_.clear();
  }

  SetupColorProcessor();
  update();
}

#ifdef Q_OS_LINUX
void ViewerGLWidget::ShowNouveauWarning()
{
  QMessageBox::warning(this,
                       tr("Driver Warning"),
                       tr("Olive has detected your system is using the Nouveau graphics driver.\n\nThis driver is "
                          "known to have stability and performance issues with Olive. It is highly recommended "
                          "you install the proprietary NVIDIA driver before continuing to use Olive."),
                       QMessageBox::Ok);
}
#endif

void ViewerGLWidget::SetupColorProcessor()
{
  if (!context()) {
    return;
  }

  color_service_ = nullptr;

  if (color_manager_) {
    // (Re)create color processor

    try {
      ColorProcessorPtr new_service = ColorProcessor::Create(color_manager_->GetConfig(),
                                                             OCIO::ROLE_SCENE_LINEAR,
                                                             ocio_display_,
                                                             ocio_view_,
                                                             ocio_look_);

      color_service_ = OpenGLColorProcessor::Create(color_manager_->GetConfig(),
                                                          OCIO::ROLE_SCENE_LINEAR,
                                                          ocio_display_,
                                                          ocio_view_,
                                                          ocio_look_);

      color_service_->Enable(context(), true);

    } catch (OCIO::Exception& e) {
      QMessageBox::critical(this,
                            tr("OpenColorIO Error"),
                            tr("Failed to set color configuration: %1").arg(e.what()),
                            QMessageBox::Ok);
    }

  } else {
    color_service_ = nullptr;
  }
}

void ViewerGLWidget::ContextCleanup()
{
  makeCurrent();

  color_service_ = nullptr;
  texture_.Destroy();

  doneCurrent();
}
