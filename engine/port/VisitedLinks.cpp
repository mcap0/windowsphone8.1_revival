#include "config.h"
#include "VisitedLinks.h"

#include "Engine.h"
#include <WebCore/SharedStringHash.h>
#include <wtf/FileHandle.h>
#include <wtf/FileSystem.h>
#include <wtf/HashSet.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/Vector.h>

namespace Lumia {
using namespace WebCore;

namespace {

// The newest this many: about 80 KB on disk, as many visits as a phone makes in months.
constexpr size_t maximumLinks = 20000;

// WebCore's link hashes, oldest first, and the same set for lookups. 0 is a valid hash.
class VisitedLinks final : public VisitedLinkStore {
public:
    bool isLinkVisited(Page&, SharedStringHash hash, const URL&, const AtomString&) final
    {
        load();
        return m_hashes.contains(hash);
    }

    void addVisitedLink(Page&, SharedStringHash hash) final
    {
        load();
        if (!m_hashes.add(hash).isNewEntry)
            return;
        m_order.append(hash);
        if (m_order.size() > maximumLinks) {
            // Drop the oldest tenth at once rather than one per visit.
            size_t drop = maximumLinks / 10;
            for (size_t i = 0; i < drop; ++i)
                m_hashes.remove(m_order[i]);
            m_order.removeAt(0, drop);
        }
        m_dirty = true;
        invalidateStylesForLink(hash);
    }

    void save()
    {
        if (!m_dirty)
            return;
        auto path = filePath();
        if (path.isEmpty())
            return;
        auto file = FileSystem::openFile(path, FileSystem::FileOpenMode::Truncate);
        if (!file)
            return;
        file.write(asBytes(m_order.span()));
        m_dirty = false;
    }

    void clear()
    {
        load();
        m_hashes.clear();
        m_order.clear();
        m_dirty = true;
        save();
        invalidateStylesForAllLinks();
    }

private:
    static String filePath()
    {
        auto* dataDirectory = config().data_directory;
        return dataDirectory ? FileSystem::pathByAppendingComponent(String(dataDirectory), "visited.dat"_s) : String();
    }

    void load()
    {
        if (std::exchange(m_loaded, true))
            return;
        auto path = filePath();
        auto file = path.isEmpty() ? FileSystem::FileHandle { } : FileSystem::openFile(path, FileSystem::FileOpenMode::Read);
        auto size = file ? file.size() : std::nullopt;
        if (!size || !*size || *size % sizeof(SharedStringHash))
            return;
        Vector<SharedStringHash> hashes(static_cast<size_t>(*size / sizeof(SharedStringHash)));
        if (file.read(asMutableByteSpan(hashes.mutableSpan())) != *size)
            return;
        for (auto hash : hashes) {
            if (m_hashes.add(hash).isNewEntry)
                m_order.append(hash);
        }
    }

    HashSet<SharedStringHash, IntHash<SharedStringHash>, WTF::UnsignedWithZeroKeyHashTraits<SharedStringHash>> m_hashes;
    Vector<SharedStringHash> m_order;
    bool m_loaded { false };
    bool m_dirty { false };
};

VisitedLinks& store()
{
    static NeverDestroyed<Ref<VisitedLinks>> links = adoptRef(*new VisitedLinks);
    return links.get();
}

} // namespace

VisitedLinkStore& visitedLinks()
{
    return store();
}

void saveVisitedLinks()
{
    store().save();
}

void clearVisitedLinks()
{
    store().clear();
}

} // namespace Lumia
