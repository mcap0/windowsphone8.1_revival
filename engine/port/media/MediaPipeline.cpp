#include "config.h"
#include "MediaPipeline.h"

#include "Engine.h"
#include <WebCore/CoordinatedPlatformLayerBufferProxy.h>
#include <windows.h>
#include <objbase.h>
#include <wtf/MainThread.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringBuilder.h>

namespace Lumia {

using namespace WebCore;

namespace {

constexpr long long audioQueueUs = 400000;       // sound decoded ahead while playing
constexpr long long audioPrerollUs = 120000;     // and before playback starts
constexpr size_t pictureQueue = 4;               // pictures decoded ahead
constexpr long long audioSlackUs = 20000;        // a gap or overlap in the sound's times smaller than this is left alone

long long framesToUs(uint64_t frames, int rate)
{
    return rate > 0 ? static_cast<long long>(frames * 1000000 / rate) : 0;
}

} // namespace

Ref<MediaPipeline> MediaPipeline::create(Source&& source, Ref<CoordinatedPlatformLayerBufferProxy>&& proxy, Function<void(Event)>&& notify)
{
    return adoptRef(*new MediaPipeline(WTF::move(source), WTF::move(proxy), WTF::move(notify)));
}

MediaPipeline::MediaPipeline(Source&& source, Ref<CoordinatedPlatformLayerBufferProxy>&& proxy, Function<void(Event)>&& notify)
    : m_data(WTF::move(source.data))
    , m_fetcher(WTF::move(source.fetcher))
    , m_url(source.url.isolatedCopy())
    , m_queue(WTF::move(source.queue))
    , m_queueTracks(std::move(source.tracks))
    , m_proxy(WTF::move(proxy))
    , m_textures(VideoTextures::create())
    , m_notify(WTF::move(notify))
{
}

MediaPipeline::~MediaPipeline() = default;

void MediaPipeline::start()
{
    ASSERT(isMainThread());
    // A read that waits for the network: a clock that is not the sound's stops with it.
    if (m_data)
        m_data->onWaiting = [this](bool waiting) { setStalled(waiting, true); };
    if (m_fetcher)
        m_fetcher->onWaiting = [this](bool waiting) { setStalled(waiting, true); };
    if (m_queue) {
        m_queue->setWake([this] {
            Locker locker { m_lock };
            m_condition.notifyAll();
        });
    }
    m_thread = Thread::create("Media"_s, [this, protectedThis = Ref { *this }] {
        run();
    });
}

void MediaPipeline::shutdown()
{
    ASSERT(isMainThread());
    {
        Locker locker { m_lock };
        m_stop = true;
        m_condition.notifyAll();
    }
    m_audio.stop();
    if (m_data)
        m_data->stop();
    if (m_fetcher)
        m_fetcher->stop();
    if (m_queue)
        m_queue->setWake(nullptr);
    if (auto thread = std::exchange(m_thread, nullptr))
        thread->waitForCompletion();
    m_notify = nullptr;
}

void MediaPipeline::setPlaying(bool playing)
{
    Locker locker { m_lock };
    if (m_playing == playing)
        return;
    m_playing = playing;
    updateRunning();
    m_condition.notifyAll();
}

void MediaPipeline::seek(long long timeUs)
{
    Locker locker { m_lock };
    m_seekTargetUs = std::max<long long>(timeUs, 0);
    m_shownSeekUs = *m_seekTargetUs;
    updateRunning();
    m_condition.notifyAll();
}

void MediaPipeline::suspendForSeek(long long timeUs)
{
    Locker locker { m_lock };
    m_shownSeekUs = std::max<long long>(timeUs, 0);
    updateRunning();
}

void MediaPipeline::setStalled(bool stalled, bool tell)
{
    {
        Locker locker { m_lock };
        if (m_stop || m_stalled == stalled)
            return;
        m_stalled = stalled;
        updateRunning();
    }
    if (tell)
        notify(stalled ? Event::Waiting : Event::Resumed);
}

bool MediaPipeline::sourceFailed() const
{
    return (m_data && m_data->failed()) || (m_fetcher && !m_fetcher->failure().isNull());
}

String MediaPipeline::sourceFailure() const
{
    if (m_data && m_data->failed())
        return m_data->failure();
    return m_fetcher ? m_fetcher->failure() : String();
}

void MediaPipeline::setRate(double rate)
{
    rate = std::clamp(rate, 0.25, 4.0);
    Locker locker { m_lock };
    if (m_wallRunning) {
        auto now = MonotonicTime::now();
        m_wallBaseUs += static_cast<long long>((now - m_wallStart).microseconds() * m_rate);
        m_wallStart = now;
    }
    m_rate = rate;
    m_audio.setRate(rate);
    m_condition.notifyAll();
}

void MediaPipeline::setVolume(float volume)
{
    m_audio.setVolume(volume);
}

void MediaPipeline::setVisible(bool visible)
{
    Locker locker { m_lock };
    if (m_visible == visible)
        return;
    m_visible = visible;
    updateRunning();
    m_condition.notifyAll();
}

long long MediaPipeline::audioClockUs() const
{
    uint64_t played = std::min(m_audio.playedFrames(), m_audio.submittedFrames());
    return m_audioBaseUs + framesToUs(played, m_audio.rate());
}

long long MediaPipeline::clockUs() const
{
    if (m_shownSeekUs >= 0)
        return m_shownSeekUs;
    return positionUs();
}

long long MediaPipeline::positionUs() const
{
    if (m_ended)
        return m_endUs;
    if (m_audioMaster)
        return audioClockUs();
    if (m_wallRunning)
        return m_wallBaseUs + static_cast<long long>((MonotonicTime::now() - m_wallStart).microseconds() * m_rate);
    return m_wallBaseUs;
}

// The sound and the clock run when the element plays, the page shows, and the pictures and sound of the current
// position are ready.
void MediaPipeline::updateRunning()
{
    bool run = m_playing && m_visible && m_prerolled && !m_ended && m_shownSeekUs < 0 && !m_stop;
    if (run)
        m_audio.start();
    else
        m_audio.stop();
    bool wall = run && !m_stalled && !m_audioMaster;
    if (wall && !m_wallRunning) {
        m_wallStart = MonotonicTime::now();
        m_wallRunning = true;
    } else if (!wall && m_wallRunning) {
        m_wallBaseUs += static_cast<long long>((MonotonicTime::now() - m_wallStart).microseconds() * m_rate);
        m_wallRunning = false;
    }
}

long long MediaPipeline::currentTimeUs() const
{
    Locker locker { m_lock };
    long long now = clockUs();
    if (m_durationUs > 0 && now > m_durationUs)
        now = m_durationUs;
    return std::max<long long>(now, 0);
}

long long MediaPipeline::durationUs() const
{
    Locker locker { m_lock };
    return m_durationUs;
}

bool MediaPipeline::hasAudio() const
{
    Locker locker { m_lock };
    return m_hasAudio;
}

bool MediaPipeline::hasVideo() const
{
    Locker locker { m_lock };
    return m_hasVideo;
}

IntSize MediaPipeline::naturalSize() const
{
    Locker locker { m_lock };
    return m_size;
}

RefPtr<VideoPicture> MediaPipeline::currentPicture() const
{
    Locker locker { m_lock };
    return m_picture;
}

bool MediaPipeline::ended() const
{
    Locker locker { m_lock };
    return m_ended;
}

String MediaPipeline::failure() const
{
    Locker locker { m_lock };
    return m_failure.isolatedCopy();
}

bool MediaPipeline::failedInNetwork() const
{
    Locker locker { m_lock };
    return m_failedInNetwork;
}

void MediaPipeline::notify(Event event)
{
    callOnMainThread([protectedThis = Ref { *this }, event] {
        if (protectedThis->m_notify)
            protectedThis->m_notify(event);
    });
}

void MediaPipeline::fail(const String& why, bool network)
{
    log("media: %s", why.utf8().data());
    {
        Locker locker { m_lock };
        if (m_stop)
            return;
        m_failure = why.isolatedCopy();
        m_failedInNetwork = network;
        m_ended = false;
        m_prerolled = false;
        updateRunning();
    }
    notify(Event::Failed);
}

void MediaPipeline::run()
{
    HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (open())
        loop();
    report(true);
    m_audio.close();
    m_pictures.clear();
    m_audioDecoder = nullptr;
    m_videoDecoder = nullptr;
    m_demuxer = nullptr;
    if (SUCCEEDED(com))
        CoUninitialize();
}

bool MediaPipeline::open()
{
    std::string error;
    if (m_queue)
        m_demuxer = m_queue->createDemuxer(std::move(m_queueTracks));
    else
        m_demuxer = Media::Demuxer::open(*m_data, error, m_fetcher.get(), std::string(m_url.utf8().data()));
    if (!m_demuxer) {
        bool network = sourceFailed();
        fail(network ? makeString("caricamento fallito: "_s, sourceFailure()) : String::fromUTF8(error.c_str()), network);
        return false;
    }
    m_live = m_demuxer->live();
    StringBuilder description;
    IntSize size;
    auto& tracks = m_demuxer->tracks();
    for (size_t i = 0; i < tracks.size(); ++i) {
        auto& track = tracks[i];
        auto& decoder = track.video ? m_videoDecoder : m_audioDecoder;
        if (decoder)
            continue;
        String why;
        decoder = MediaDecoder::create(track, why);
        if (!decoder) {
            log("media: traccia %s scartata: %s", track.name.c_str(), why.utf8().data());
            continue;
        }
        if (track.video) {
            m_videoTrack = i;
            m_videoConfig = track.config;
            size = IntSize(track.width, track.height);
            description.append(", video "_s, String::fromUTF8(track.name.c_str()), ' ', track.width, 'x', track.height, " ("_s, decoder->description(), ')');
        } else {
            m_audioTrack = i;
            description.append(", audio "_s, String::fromUTF8(track.name.c_str()), ' ', track.sampleRate, " Hz "_s, track.channels, " canali ("_s, decoder->description(), ')');
        }
    }
    if (!m_audioDecoder && !m_videoDecoder) {
        fail(error.empty() ? "nessuna traccia con un decoder"_s : String::fromUTF8(error.c_str()), false);
        return false;
    }
    log("media: %s, %.1f s%s%s%s", m_demuxer->container(), m_demuxer->durationUs() / 1e6, description.toString().utf8().data(),
        error.empty() ? "" : "; ", error.c_str());
    {
        Locker locker { m_lock };
        m_durationUs = m_demuxer->durationUs();
        m_hasAudio = !!m_audioDecoder;
        m_hasVideo = !!m_videoDecoder;
        m_size = size;
    }
    m_lastReport = MonotonicTime::now();
    notify(Event::Metadata);
    return true;
}

void MediaPipeline::loop()
{
    for (;;) {
        std::optional<long long> seekTarget;
        {
            Locker locker { m_lock };
            if (m_stop || !m_failure.isNull())
                return;
            seekTarget = std::exchange(m_seekTargetUs, std::nullopt);
        }
        if (seekTarget) {
            performSeek(*seekTarget);
            continue;
        }

        bool worked = feedAudio();
        worked |= feedVideo();
        m_bufferedEndUs = m_demuxer->bufferedEndUs();
        if (m_queue)
            updateStarving();

        bool prerolled, playing, ended;
        {
            Locker locker { m_lock };
            if (m_stop || !m_failure.isNull())
                return;
            prerolled = m_prerolled;
            playing = m_playing && m_visible;
            ended = m_ended;
        }
        Seconds wait = 20_ms;
        if (!prerolled) {
            bool videoReady = !m_videoDecoder || m_videoEnded || (m_pictures.size() >= 2 && m_pictures[1]->ptsUs > m_startUs);
            bool audioReady = !m_audioDecoder || m_audioEnded
                || (m_audio.isOpen() && framesToUs(m_audio.submittedFrames(), m_audio.rate()) >= audioPrerollUs);
            if (videoReady && audioReady)
                finishPreroll();
            else if (!worked)
                wait = 5_ms;
            if (videoReady && audioReady)
                continue;
        } else {
            wait = present();
            checkEnd();
            if (!playing || ended)
                wait = Seconds::infinity();
            else if (MonotonicTime::now() - m_lastReport > 10_s)
                report(false);
        }
        if (worked)
            continue;
        Locker locker { m_lock };
        if (m_stop || m_seekTargetUs)
            continue;
        m_condition.waitFor(m_lock, wait);
    }
}

// Media Source: the page has not brought the samples of the position yet. A clock that is not the sound's waits for
// them (the sound's own stops when its output runs dry).
void MediaPipeline::updateStarving()
{
    bool video = m_videoDecoder && !m_videoEnded && m_pictures.isEmpty() && m_demuxer->pending(m_videoTrack);
    bool audio = m_audioDecoder && !m_audioEnded && m_demuxer->pending(m_audioTrack)
        && (!m_audio.isOpen() || m_audio.playedFrames() >= m_audio.submittedFrames());
    setStalled(video || audio, false);
}

void MediaPipeline::performSeek(long long targetUs)
{
    if (m_demuxer->live()) {
        // No going back in a stream that is not kept: the position stays.
        Locker locker { m_lock };
        targetUs = positionUs();
        m_shownSeekUs = targetUs;
    }
    m_audio.close();
    m_pictures.clear();
    if (m_audioDecoder)
        m_audioDecoder->flush();
    if (m_videoDecoder)
        m_videoDecoder->flush();
    {
        Locker locker { m_lock };
        if (m_durationUs > 0 && targetUs > m_durationUs)
            targetUs = m_durationUs;
    }
    m_demuxer->seek(targetUs);
    m_audioEnded = m_videoEnded = false;
    m_audioFailures = m_videoFailures = 0;
    m_startUs = m_lastPictureUs = targetUs;
    m_seeking = true;
    Locker locker { m_lock };
    m_ended = false;
    m_prerolled = false;
    m_audioMaster = false;
    m_wallRunning = false;
    m_wallBaseUs = m_audioBaseUs = targetUs;
    updateRunning();
}

void MediaPipeline::finishPreroll()
{
    if (!m_pictures.isEmpty())
        show(takeNextPicture());
    bool newerSeek = false;
    {
        Locker locker { m_lock };
        m_prerolled = true;
        newerSeek = !!m_seekTargetUs;
        if (!newerSeek)
            m_shownSeekUs = -1;
        updateRunning();
    }
    if (m_seeking && !newerSeek) {
        m_seeking = false;
        notify(Event::SeekDone);
    }
    if (!m_reportedReady) {
        m_reportedReady = true;
        notify(Event::Ready);
    }
}

bool MediaPipeline::feedAudio()
{
    if (!m_audioDecoder)
        return false;
    if (m_audioEnded) {
        // Media Source: samples after the end (the page went on appending).
        if (!m_queue || m_queue->finished(false) || m_demuxer->pending(m_audioTrack))
            return false;
        m_audioEnded = false;
        m_audioDecoder->flush();
    }
    if (m_audio.isOpen()) {
        bool prerolled;
        {
            Locker locker { m_lock };
            prerolled = m_prerolled;
        }
        uint64_t submitted = m_audio.submittedFrames();
        uint64_t played = std::min(m_audio.playedFrames(), submitted);
        if (framesToUs(submitted - played, m_audio.rate()) >= (prerolled ? audioQueueUs : audioPrerollUs) || m_audio.queuedBuffers() >= 48)
            return false;
    }
    if (m_demuxer->pending(m_audioTrack))
        return false;
    Media::Sample sample;
    if (!m_demuxer->read(m_audioTrack, sample)) {
        if (m_demuxer->pending(m_audioTrack))
            return false;
        if (m_demuxer->failed() && sourceFailed()) {
            fail(makeString("caricamento interrotto: "_s, sourceFailure()), true);
            return false;
        }
        m_audioDecoder->drain();
        submitAudio();
        m_audioEnded = true;
        return true;
    }
    if (m_audioDecoder->decode({ sample.data.data(), sample.data.size() }, sample.ptsUs, sample.durationUs))
        m_audioFailures = 0;
    else if (++m_audioFailures > 50) {
        log("media: il decoder audio rifiuta tutto: traccia chiusa");
        m_audioEnded = true;
    }
    submitAudio();
    return true;
}

void MediaPipeline::submitAudio()
{
    auto& decoded = m_audioDecoder->audio();
    for (auto& audio : decoded) {
        if (!audio.rate || !audio.channels)
            continue;
        if (!m_audio.isOpen() || m_audio.rate() != audio.rate || m_audio.channels() != audio.channels) {
            // The first sound, or a format that changed on the way (the decoder found the stream's real rate): a
            // new voice, whose first frame is the time the old one had reached.
            long long base;
            {
                Locker locker { m_lock };
                base = m_audio.isOpen() ? m_audioBaseUs + framesToUs(m_audio.submittedFrames(), m_audio.rate()) : m_startUs;
            }
            String error;
            if (!m_audio.open(audio.rate, audio.channels, error)) {
                log("media: senza audio: %s", error.utf8().data());
                m_audioEnded = true;
                break;
            }
            log("media: audio in uscita %d Hz, %d canali", audio.rate, audio.channels);
            Locker locker { m_lock };
            m_audioBaseUs = base;
            m_audioMaster = true;
            updateRunning();
        }
        long long expectedUs;
        {
            Locker locker { m_lock };
            expectedUs = m_audioBaseUs + framesToUs(m_audio.submittedFrames(), audio.rate);
        }
        long long deltaUs = audio.ptsUs - expectedUs;
        size_t frames = audio.samples.size() / audio.channels;
        if (deltaUs > audioSlackUs) {
            // Later than the sound given so far ends: silence in between (a second at most; the rest moves the clock).
            long long padUs = std::min<long long>(deltaUs, 1000000);
            if (deltaUs > padUs) {
                Locker locker { m_lock };
                m_audioBaseUs += deltaUs - padUs;
            }
            Vector<int16_t> silence(FillWith { }, static_cast<size_t>(padUs * audio.rate / 1000000) * audio.channels, static_cast<int16_t>(0));
            m_audio.submit(WTF::move(silence));
        } else if (deltaUs < -audioSlackUs) {
            // Earlier: the part before the position (the start of the frame a seek landed in, an encoder's priming).
            size_t trim = static_cast<size_t>(-deltaUs * audio.rate / 1000000);
            if (trim >= frames)
                continue;
            audio.samples.removeAt(0, trim * audio.channels);
        }
        m_audio.submit(WTF::move(audio.samples));
    }
    decoded.clear();
}

bool MediaPipeline::feedVideo()
{
    if (!m_videoDecoder || m_pictures.size() >= pictureQueue)
        return false;
    if (m_videoEnded) {
        if (!m_queue || m_queue->finished(true) || m_demuxer->pending(m_videoTrack))
            return false;
        m_videoEnded = false;
        m_videoDecoder->flush();
    }
    if (m_demuxer->pending(m_videoTrack))
        return false;
    Media::Sample sample;
    if (!m_demuxer->read(m_videoTrack, sample)) {
        if (m_demuxer->pending(m_videoTrack))
            return false;
        if (m_demuxer->failed() && sourceFailed()) {
            fail(makeString("caricamento interrotto: "_s, sourceFailure()), true);
            return false;
        }
        m_videoDecoder->drain();
        collectPictures();
        m_videoEnded = true;
        return true;
    }
    // The parameter sets before every key frame: the decoder starts, or starts again after a seek, from any of them.
    if (sample.key && !m_videoConfig.empty() && !m_demuxer->configInBand())
        sample.data.insert(sample.data.begin(), m_videoConfig.begin(), m_videoConfig.end());
    auto before = MonotonicTime::now();
    bool decoded = m_videoDecoder->decode({ sample.data.data(), sample.data.size() }, sample.ptsUs, sample.durationUs);
    double ms = (MonotonicTime::now() - before).milliseconds();
    m_decodeMs += ms;
    m_decodeMaxMs = std::max(m_decodeMaxMs, ms);
    ++m_decodeCount;
    if (decoded)
        m_videoFailures = 0;
    else if (++m_videoFailures > 50) {
        log("media: il decoder video rifiuta tutto: traccia chiusa");
        m_videoEnded = true;
    }
    collectPictures();
    return true;
}

void MediaPipeline::collectPictures()
{
    auto& pictures = m_videoDecoder->pictures();
    for (auto& picture : pictures) {
        ++m_decodedFrames;
        m_lastPictureUs = picture->ptsUs;
        m_pictures.append(WTF::move(picture));
    }
    pictures.clear();
    bool prerolled;
    {
        Locker locker { m_lock };
        prerolled = m_prerolled;
    }
    // On the way to the position (from the key frame before it): only the picture that holds it is kept.
    if (!prerolled) {
        while (m_pictures.size() >= 2 && m_pictures[1]->ptsUs <= m_startUs)
            m_pictures.removeAt(0);
    }
}

Ref<VideoPicture> MediaPipeline::takeNextPicture()
{
    Ref picture = WTF::move(m_pictures[0]);
    m_pictures.removeAt(0);
    return picture;
}

void MediaPipeline::show(Ref<VideoPicture>&& picture)
{
    bool sizeChanged = false;
    {
        Locker locker { m_lock };
        IntSize size(picture->width, picture->height);
        if (m_size != size) {
            m_size = size;
            sizeChanged = true;
        }
        m_picture = picture.ptr();
    }
    m_proxy->setDisplayBuffer(VideoLayerBuffer::create(m_textures.copyRef(), WTF::move(picture)));
    if (sizeChanged)
        notify(Event::SizeChanged);
}

Seconds MediaPipeline::present()
{
    long long nowUs;
    double rate;
    bool playing;
    {
        Locker locker { m_lock };
        nowUs = clockUs();
        rate = m_rate;
        playing = m_playing && m_visible && !m_ended;
    }
    while (m_pictures.size() >= 2 && m_pictures[1]->ptsUs <= nowUs) {
        m_pictures.removeAt(0);
        ++m_droppedFrames;
    }
    if (!m_pictures.isEmpty() && m_pictures.first()->ptsUs <= nowUs + 3000)
        show(takeNextPicture());
    if (m_pictures.isEmpty()) {
        if (playing)
            catchUp(nowUs);
        return 10_ms;
    }
    double untilUs = static_cast<double>(m_pictures.first()->ptsUs - nowUs) / rate;
    return Seconds::fromMicroseconds(std::clamp(untilUs, 1000.0, 50000.0));
}

// Decoding that cannot keep the clock's pace: over a second behind, the pictures up to the next key frame are given
// up, sound and clock go on.
void MediaPipeline::catchUp(long long nowUs)
{
    if (!m_videoDecoder || m_videoEnded || nowUs - m_lastPictureUs < 1000000)
        return;
    log("media: video indietro di %lld ms: salto al prossimo fotogramma chiave", (nowUs - m_lastPictureUs) / 1000);
    m_videoDecoder->flush();
    Media::Sample sample;
    for (;;) {
        {
            Locker locker { m_lock };
            if (m_stop || m_seekTargetUs)
                return;
        }
        if (m_demuxer->pending(m_videoTrack))
            return;
        if (!m_demuxer->read(m_videoTrack, sample)) {
            m_videoEnded = !m_demuxer->pending(m_videoTrack);
            return;
        }
        if (sample.key && sample.ptsUs >= nowUs)
            break;
        ++m_droppedFrames;
    }
    if (!m_videoConfig.empty() && !m_demuxer->configInBand())
        sample.data.insert(sample.data.begin(), m_videoConfig.begin(), m_videoConfig.end());
    m_videoDecoder->decode({ sample.data.data(), sample.data.size() }, sample.ptsUs, sample.durationUs);
    m_lastPictureUs = sample.ptsUs;
    collectPictures();
}

void MediaPipeline::checkEnd()
{
    uint64_t submitted = m_audio.submittedFrames();
    bool audioDone = !m_audioDecoder || (m_audioEnded && (!m_audio.isOpen() || m_audio.playedFrames() >= submitted));
    bool videoDone = !m_videoDecoder || (m_videoEnded && m_pictures.isEmpty());
    bool durationChanged = false;
    // Media Source: the duration is the page's, and it may have changed.
    long long queueDurationUs = m_queue ? m_queue->durationUs() : 0;
    {
        Locker locker { m_lock };
        if (!m_prerolled || m_ended || m_shownSeekUs >= 0)
            return;
        if (m_audioMaster && audioDone && !videoDone) {
            // The sound is over before the pictures: the clock goes on without it.
            m_wallBaseUs = audioClockUs();
            m_audioMaster = false;
            updateRunning();
        }
        if (!audioDone || !videoDone)
            return;
        long long nowUs = clockUs();
        if (queueDurationUs > 0)
            m_durationUs = queueDurationUs;
        if (m_durationUs <= 0) {
            m_durationUs = std::max<long long>(nowUs, 1);
            durationChanged = true;
        }
        m_endUs = m_durationUs;
        m_ended = true;
        updateRunning();
    }
    if (durationChanged)
        notify(Event::DurationChanged);
    notify(Event::Ended);
    report(true);
}

void MediaPipeline::report(bool final)
{
    m_lastReport = MonotonicTime::now();
    if (!m_decodeCount)
        return;
    log("media: %s%u fotogrammi decodificati, %u saltati, decodifica %.1f ms in media (massimo %.1f)", final ? "fine, " : "",
        m_decodedFrames.load(), m_droppedFrames.load(), m_decodeMs / m_decodeCount, m_decodeMaxMs);
    m_decodeMs = m_decodeMaxMs = 0;
    m_decodeCount = 0;
}

} // namespace Lumia
