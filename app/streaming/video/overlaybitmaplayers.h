#pragma once

#include <QImage>
#include <QMutex>
#include <QRectF>

#include <array>
#include <atomic>
#include <functional>
#include <memory>

namespace Overlay {

// Overlay surfaces that a capable renderer can draw into the stream's
// swapchain instead of showing them as separate top-level windows. A window
// over the stream forces desktop composition; drawing into the swapchain keeps
// independent flip. Renderers draw the layers back to front in this order,
// after the video and the legacy text overlays.
enum class BitmapLayer {
    StatsPanel,
    StatsTooltip,
    Toast,
    MenuButton,
};
constexpr int BitmapLayerCount = 4;

struct BitmapLayerImage {
    // QImage::Format_ARGB32 (straight alpha) in device pixels
    QImage image;
    // Placement in the stream window's client area, where (0, 0)-(1, 1) covers
    // all of it. Content may extend past the edges; renderers clip it.
    QRectF bounds;
};

struct BitmapLayerSnapshot {
    // Null when the layer has nothing to draw
    std::shared_ptr<const BitmapLayerImage> content;
    // Changes on every publish or clear of this layer
    quint64 generation = 0;
};

enum class BitmapLayerSupport {
    // No renderer has registered yet
    Unknown,
    Supported,
    Unsupported,
};

// Thread-safe store of the latest image per layer. The UI thread publishes
// immutable images; the render thread polls generation() once per frame and
// pulls changed layers, so a renderer created later (reconnect, resize
// recreation) catches up without any replay.
class BitmapLayerStore
{
public:
    // Returns false, leaving the previous content, for a null image or an
    // empty or non-finite rect
    bool publish(BitmapLayer layer, const QImage& image, const QRectF& bounds);
    void clear(BitmapLayer layer);
    void clearAll();

    BitmapLayerSnapshot snapshot(BitmapLayer layer) const;
    // Changes whenever any layer changes; cheap enough to poll every frame
    quint64 generation() const;

    // Called whenever the overlay renderer changes. A gap without a renderer
    // keeps the last answer so native windows don't flash up during a
    // reconnect or a renderer recreation.
    void setRendererSupport(bool hasRenderer, bool supportsBitmapLayers);
    BitmapLayerSupport support() const;
    // Runs on the thread that changed the renderer, outside the store's lock
    void setSupportChangedCallback(std::function<void(BitmapLayerSupport)> callback);

private:
    mutable QMutex m_Mutex;
    std::array<BitmapLayerSnapshot, BitmapLayerCount> m_Layers;
    std::atomic<quint64> m_Generation { 0 };
    BitmapLayerSupport m_Support = BitmapLayerSupport::Unknown;
    std::function<void(BitmapLayerSupport)> m_SupportChanged;
};

} // namespace Overlay
