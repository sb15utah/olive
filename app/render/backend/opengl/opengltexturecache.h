#ifndef OPENGLTEXTURECACHE_H
#define OPENGLTEXTURECACHE_H

#include <QMutex>

#include "openglframebuffer.h"
#include "opengltexture.h"
#include "render/videoparams.h"

class OpenGLTextureCache
{
public:
  class Reference {
  public:
    Reference(OpenGLTextureCache* parent, OpenGLTexturePtr texture);
    ~Reference();

    DISABLE_COPY_MOVE(Reference)

    OpenGLTexturePtr texture();

    void ParentKilled();

  private:
    OpenGLTextureCache* parent_;

    OpenGLTexturePtr texture_;
  };

  using ReferencePtr = std::shared_ptr<Reference>;

  OpenGLTextureCache() = default;

  ~OpenGLTextureCache();

  DISABLE_COPY_MOVE(OpenGLTextureCache)

  ReferencePtr Get(QOpenGLContext *ctx, const VideoRenderingParams& params, const void *data = nullptr);

private:
  void Relinquish(Reference* ref);

  QMutex lock_;

  QList<OpenGLTexturePtr> available_textures_;

  QList<Reference*> existing_references_;

};

Q_DECLARE_METATYPE(OpenGLTextureCache::ReferencePtr)

#endif // OPENGLTEXTURECACHE_H
