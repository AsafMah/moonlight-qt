#include "overlaybitmaplayers.h"

#include <QMutexLocker>

#include <cmath>

namespace Overlay {

namespace {
bool validBounds(const QRectF& bounds)
{
    return std::isfinite(bounds.x()) && std::isfinite(bounds.y()) &&
           std::isfinite(bounds.width()) && std::isfinite(bounds.height()) && bounds.width() > 0 &&
           bounds.height() > 0;
}
} // namespace

bool BitmapLayerStore::publish(BitmapLayer layer, const QImage& image, const QRectF& bounds)
{
    if (image.isNull() || !validBounds(bounds)) {
        return false;
    }

    // Convert outside the lock so the render thread's pull stays short. The
    // stored QImage is implicitly shared: a publisher that keeps painting into
    // its own copy detaches instead of changing what a renderer is uploading.
    auto content = std::make_shared<BitmapLayerImage>();
    content->image = image.convertToFormat(QImage::Format_ARGB32);
    content->image.setDevicePixelRatio(1);
    content->bounds = bounds;

    QMutexLocker lock(&m_Mutex);
    auto& slot = m_Layers[size_t(layer)];
    slot.content = std::move(content);
    slot.generation = ++m_Generation;
    return true;
}

void BitmapLayerStore::clear(BitmapLayer layer)
{
    QMutexLocker lock(&m_Mutex);
    auto& slot = m_Layers[size_t(layer)];
    if (slot.content) {
        slot.content.reset();
        slot.generation = ++m_Generation;
    }
}

void BitmapLayerStore::clearAll()
{
    for (int i = 0; i < BitmapLayerCount; i++) {
        clear(BitmapLayer(i));
    }
}

BitmapLayerSnapshot BitmapLayerStore::snapshot(BitmapLayer layer) const
{
    QMutexLocker lock(&m_Mutex);
    return m_Layers[size_t(layer)];
}

quint64 BitmapLayerStore::generation() const
{
    return m_Generation.load();
}

void BitmapLayerStore::setRendererSupport(bool hasRenderer, bool supportsBitmapLayers)
{
    if (!hasRenderer) {
        return;
    }

    const BitmapLayerSupport support =
        supportsBitmapLayers ? BitmapLayerSupport::Supported : BitmapLayerSupport::Unsupported;
    std::function<void(BitmapLayerSupport)> callback;
    {
        QMutexLocker lock(&m_Mutex);
        if (m_Support == support) {
            return;
        }
        m_Support = support;
        callback = m_SupportChanged;
    }
    if (callback) {
        callback(support);
    }
}

BitmapLayerSupport BitmapLayerStore::support() const
{
    QMutexLocker lock(&m_Mutex);
    return m_Support;
}

void BitmapLayerStore::setSupportChangedCallback(std::function<void(BitmapLayerSupport)> callback)
{
    QMutexLocker lock(&m_Mutex);
    m_SupportChanged = std::move(callback);
}

} // namespace Overlay
