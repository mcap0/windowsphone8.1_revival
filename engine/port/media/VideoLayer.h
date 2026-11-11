// A decoded picture as the contents of the video element's compositing layer (PLAN §75): the three planes are
// uploaded into GL textures on the compositor's thread when the layer is painted, and drawn by the texture mapper's
// YUV shader. The textures belong to the player and are written again for each picture.

#pragma once

#include "MediaDecoder.h"
#include <WebCore/CoordinatedPlatformLayerBuffer.h>
#include <array>
#include <wtf/ThreadSafeRefCounted.h>

namespace Lumia {

class VideoTextures final : public ThreadSafeRefCounted<VideoTextures> {
public:
    static Ref<VideoTextures> create() { return adoptRef(*new VideoTextures); }
    // Any thread: the textures are deleted at the next paint of any video (the compositor's thread and context).
    ~VideoTextures();

    // Compositor thread, its context current.
    const std::array<unsigned, 3>& upload(const VideoPicture&);

private:
    VideoTextures() = default;

    std::array<unsigned, 3> m_textures { };
    int m_width { 0 }, m_height { 0 };
    RefPtr<const VideoPicture> m_uploaded;
};

class VideoLayerBuffer final : public WebCore::CoordinatedPlatformLayerBuffer {
public:
    static std::unique_ptr<VideoLayerBuffer> create(Ref<VideoTextures>&&, Ref<VideoPicture>&&);
    VideoLayerBuffer(Ref<VideoTextures>&&, Ref<VideoPicture>&&);
    ~VideoLayerBuffer();

private:
    void paintToTextureMapper(WebCore::TextureMapper&, const WebCore::FloatRect&, const WebCore::TransformationMatrix&, float opacity) final;

    const Ref<VideoTextures> m_textures;
    const Ref<VideoPicture> m_picture;
};

} // namespace Lumia
