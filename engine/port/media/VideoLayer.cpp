#include "config.h"
#include "VideoLayer.h"

#include <WebCore/TextureMapper.h>
#include <WebCore/TextureMapperGLHeaders.h>
#include <wtf/Lock.h>
#include <wtf/NeverDestroyed.h>

namespace Lumia {

using namespace WebCore;

namespace {

Lock orphansLock;
Vector<unsigned>& orphans() WTF_REQUIRES_LOCK(orphansLock)
{
    static NeverDestroyed<Vector<unsigned>> list;
    return list;
}

void deleteOrphans()
{
    Vector<unsigned> list;
    {
        Locker locker { orphansLock };
        list = std::exchange(orphans(), { });
    }
    if (!list.isEmpty())
        glDeleteTextures(list.size(), list.span().data());
}

} // namespace

VideoTextures::~VideoTextures()
{
    Locker locker { orphansLock };
    for (unsigned texture : m_textures) {
        if (texture)
            orphans().append(texture);
    }
}

const std::array<unsigned, 3>& VideoTextures::upload(const VideoPicture& picture)
{
    deleteOrphans();
    // A context made again (the app came back from the background) has none of the old textures.
    bool fresh = !m_textures[0] || !glIsTexture(m_textures[0]) || m_width != picture.width || m_height != picture.height;
    if (!fresh && m_uploaded == &picture)
        return m_textures;
    if (fresh) {
        if (m_textures[0] && glIsTexture(m_textures[0]))
            glDeleteTextures(3, m_textures.data());
        glGenTextures(3, m_textures.data());
        m_width = picture.width;
        m_height = picture.height;
    }
    const uint8_t* planes[3] = { picture.y(), picture.u(), picture.v() };
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    for (int i = 0; i < 3; ++i) {
        int width = i ? picture.chromaWidth() : picture.width;
        int height = i ? picture.chromaHeight() : picture.height;
        glBindTexture(GL_TEXTURE_2D, m_textures[i]);
        if (fresh) {
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, width, height, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, planes[i]);
        } else
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_LUMINANCE, GL_UNSIGNED_BYTE, planes[i]);
    }
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glBindTexture(GL_TEXTURE_2D, 0);
    m_uploaded = &picture;
    return m_textures;
}

std::unique_ptr<VideoLayerBuffer> VideoLayerBuffer::create(Ref<VideoTextures>&& textures, Ref<VideoPicture>&& picture)
{
    return makeUnique<VideoLayerBuffer>(WTF::move(textures), WTF::move(picture));
}

VideoLayerBuffer::VideoLayerBuffer(Ref<VideoTextures>&& textures, Ref<VideoPicture>&& picture)
    : CoordinatedPlatformLayerBuffer(Type::WP81Video, IntSize(picture->width, picture->height), { }, nullptr)
    , m_textures(WTF::move(textures))
    , m_picture(WTF::move(picture))
{
}

VideoLayerBuffer::~VideoLayerBuffer() = default;

void VideoLayerBuffer::paintToTextureMapper(TextureMapper& textureMapper, const FloatRect& targetRect, const TransformationMatrix& modelViewMatrix, float opacity)
{
    // Video range to full range RGB (the same matrices as CoordinatedPlatformLayerBufferYUV).
    static constexpr std::array<GLfloat, 16> bt601 {
        1.164383561643836,  0.0,                1.596026785714286, -0.874202217873451,
        1.164383561643836, -0.391762290094914, -0.812967647237771,  0.531667823499146,
        1.164383561643836,  2.017232142857143,  0.0,               -1.085630789302022,
        0.0,                0.0,                0.0,                1.0,
    };
    static constexpr std::array<GLfloat, 16> bt709 {
        1.164383561643836,  0.0,                1.792741071428571, -0.972945075016308,
        1.164383561643836, -0.213248614273730, -0.532909328559444,  0.301482665475862,
        1.164383561643836,  2.112401785714286,  0.0,               -1.133402217873451,
        0.0,                0.0,                0.0,                1.0,
    };
    auto& textures = m_textures->upload(m_picture.get());
    textureMapper.drawTexturePlanarYUV(std::array<GLuint, 3> { textures[0], textures[1], textures[2] }, m_picture->bt709 ? bt709 : bt601,
        m_flags, targetRect, modelViewMatrix, opacity, std::nullopt, TextureMapper::TransferFunction::Bt709);
}

} // namespace Lumia
