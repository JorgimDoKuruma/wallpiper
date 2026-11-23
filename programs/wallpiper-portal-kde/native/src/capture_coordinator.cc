/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Ethan Alexander
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "capture_coordinator.h"

#include "capture_item.h"
#include "capture_socket.h"
#include "ctl_listener.h"
#include "x11_output_lookup.h"

#include <QCursor>
#include <QDebug>
#include <QGuiApplication>
#include <QScreen>
#include <QTimer>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <memory>
#include <mutex>

#include <unistd.h>

namespace WallpiperKde {

namespace {
constexpr int kCursorSampleIntervalMs = 8;
constexpr int kPendingBufRetryIntervalMs = 250;

/* must match loader/vk-layer-hook/config.h */
constexpr uint32_t kCaptureSlotCount = 3;

/*
 * a buffered tolerance, as fractional scaling mathmatically floats,
 * we could differ by a pixel on either end.
 */
constexpr int32_t kGeometryTolerancePx = 2;

/*
 * A bound channel that has not delivered a frame for this long may hand its
 * screen over to a newer channel. Wallpaper Engine sometimes creates the
 * swapchain for the next wallpaper before (or instead of) destroying the old
 * one; the new swapchain then lands on a fresh wire channel while the screen
 * stays bound to the silent old one, freezing the desktop on its last frame.
 */
constexpr auto kStaleChannelTimeout = std::chrono::milliseconds(1500);
constexpr auto kRebindAttemptInterval = std::chrono::milliseconds(250);
constexpr auto kMirrorEvalInterval = std::chrono::milliseconds(500);
constexpr auto kMirrorAfter = std::chrono::milliseconds(4000);

bool nearlyEqual(int64_t a, int64_t b) {
  return std::abs(a - b) <= kGeometryTolerancePx;
}

/*
 * X11 only has one global scale factor, which translates over to KWins Xwayland
 * policy. Needed to match synthetic monitor dimensions.
 */
double xwaylandGlobalScale() {
  QScreen *primary = QGuiApplication::primaryScreen();
  double scale = primary ? primary->devicePixelRatio() : 1.0;
  return scale > 0.0 ? scale : 1.0;
}

WallpiperProtocol::MonitorGeometry
predictedXWaylandRect(const WallpiperProtocol::MonitorGeometry &real,
                      double globalScale) {
  WallpiperProtocol::MonitorGeometry predicted;
  predicted.x = static_cast<int32_t>(std::lround(real.x * globalScale));
  predicted.y = static_cast<int32_t>(std::lround(real.y * globalScale));
  predicted.width =
      static_cast<uint32_t>(std::lround(real.logicalWidth * globalScale));
  predicted.height =
      static_cast<uint32_t>(std::lround(real.logicalHeight * globalScale));
  return predicted;
}
} // namespace

CaptureCoordinator *CaptureCoordinator::instance() {
  static CaptureCoordinator *coordinator = new CaptureCoordinator();
  return coordinator;
}

CaptureCoordinator::CaptureCoordinator(QObject *parent)
    : QObject(parent), m_captureSocket(new CaptureSocket(this)),
      m_ctlListener(new CtlListener(QStringLiteral("kde"), this)),
      m_cursorTimer(new QTimer(this)), m_pendingRetryTimer(new QTimer(this)) {
  connect(m_captureSocket, &CaptureSocket::bufReceived, this,
          [this](quint32 slot, quint32 width, quint32 height, quint32 format,
                 quint32 stride, quint64 modifier, bool hasGeometry,
                 qint32 geomX, qint32 geomY, int fd, int syncFd) {
            handleBuf(slot, width, height, format, stride, modifier,
                      hasGeometry, geomX, geomY, fd, syncFd);
          });
  connect(m_captureSocket, &CaptureSocket::frameReceived, this,
          [this](quint32 slot, int syncFd) {
            uint32_t channel = slot / kCaptureSlotCount;
            m_channelLastFrame[channel] = std::chrono::steady_clock::now();
            WallpaperCaptureItem *item = channelItem(channel);
            if (!item && rebindStaleItem(channel)) {
              item = channelItem(channel);
            }
            evaluateMirrors();
            for (const auto &[mirror, source] : m_mirrors) {
              if (source == channel && mirror != item) {
                mirror->stageFrame(slot, syncFd >= 0 ? ::dup(syncFd) : -1);
              }
            }
            if (item) {
              item->stageFrame(slot, syncFd);
            } else {
              if (syncFd >= 0) {
                ::close(syncFd);
              }
              if (m_loggedDroppedChannels.insert(channel).second) {
                qWarning() << "[coordinator] dropping frames for channel"
                           << channel
                           << "- output not bound yet (further drops on this "
                              "channel won't be logged)";
              }
            }
          });
  connect(m_captureSocket, &CaptureSocket::shmReceived, this,
          [this](quint32 width, quint32 height, quint32 stride, int fd) {
            if (m_primaryItem) {
              m_primaryItem->stageShm(width, height, stride, fd);
            } else {
              ::close(fd);
            }
          });

  m_ctlListener->setCursorPosProvider(
      [this]() -> std::optional<std::pair<int32_t, int32_t>> {
        return std::make_pair(m_cursorX.load(), m_cursorY.load());
      });
  m_ctlListener->setGeometryProvider(
      [this]() -> std::optional<WallpiperProtocol::MonitorGeometry> {
        std::optional<WallpiperProtocol::MonitorGeometry> result;
        QMetaObject::invokeMethod(
            this, [this, &result]() { result = geometryFromActiveItem(); },
            Qt::BlockingQueuedConnection);
        return result;
      });
  m_ctlListener->setRenderNodeProvider(
      [this]() -> std::optional<std::pair<uint32_t, uint32_t>> {
        std::optional<std::pair<uint32_t, uint32_t>> result;
        QMetaObject::invokeMethod(
            this,
            [this, &result]() {
              if (!m_primaryItem) {
                return;
              }
              uint32_t major = 0, minor = 0;
              if (m_primaryItem->queryRenderNode(&major, &minor)) {
                result = std::make_pair(major, minor);
              }
            },
            Qt::BlockingQueuedConnection);
        return result;
      });
  m_ctlListener->setDetachHandler([this]() {
    QMetaObject::invokeMethod(
        this, [this]() { handleDetach(); }, Qt::BlockingQueuedConnection);
  });
  m_ctlListener->setDebugHandler([this](bool enabled) {
    QMetaObject::invokeMethod(
        this, [this, enabled]() { handleSetDebug(enabled); },
        Qt::BlockingQueuedConnection);
  });
  m_ctlListener->setCaptureHandler(
      [this](uint32_t channel, const QString &path, QString &err) -> bool {
        return captureChannel(channel, path, err);
      });

  m_pendingRetryTimer->setInterval(kPendingBufRetryIntervalMs);
  connect(m_pendingRetryTimer, &QTimer::timeout, this,
          [this]() { retryPendingBufs(); });

  m_cursorTimer->setInterval(kCursorSampleIntervalMs);
  connect(m_cursorTimer, &QTimer::timeout, this, [this]() {
    QPoint pos = QCursor::pos();
    m_cursorX = pos.x();
    m_cursorY = pos.y();
  });
}

CaptureCoordinator::~CaptureCoordinator() { teardownSockets(); }

void CaptureCoordinator::registerItem(WallpaperCaptureItem *item) {
  if (!item ||
      std::find(m_items.begin(), m_items.end(), item) != m_items.end()) {
    return;
  }
  m_items.push_back(item);
  ensureSocketsBound();
  reevaluateActiveItem();
  retryPendingBufs();
}

void CaptureCoordinator::unregisterItem(WallpaperCaptureItem *item) {
  auto it = std::find(m_items.begin(), m_items.end(), item);
  if (it == m_items.end()) {
    return;
  }
  m_items.erase(it);
  for (auto channelIt = m_channelItems.begin();
       channelIt != m_channelItems.end();) {
    if (channelIt->second == item) {
      channelIt = m_channelItems.erase(channelIt);
    } else {
      ++channelIt;
    }
  }
  if (m_primaryItem == item) {
    m_primaryItem = nullptr;
  }
  m_mirrors.erase(item);
  m_orphanSince.erase(item);
  reevaluateActiveItem();
  if (m_items.empty()) {
    teardownSockets();
  }
}

void CaptureCoordinator::reevaluateActiveItem() {
  WallpaperCaptureItem *candidate = nullptr;
  QScreen *primary = QGuiApplication::primaryScreen();
  for (auto *item : m_items) {
    if (item->window() && item->window()->screen() == primary) {
      candidate = item;
      break;
    }
  }
  if (!candidate && !m_items.empty()) {
    candidate = m_items.front();
  }
  if (candidate == m_primaryItem) {
    return;
  }
  m_primaryItem = candidate;
  if (m_primaryItem) {
    qInfo() << "[coordinator] primary wallpaper item is now on screen"
            << (m_primaryItem->window() && m_primaryItem->window()->screen()
                    ? m_primaryItem->window()->screen()->name()
                    : QStringLiteral("<none>"));
  }
}

WallpaperCaptureItem *CaptureCoordinator::channelItem(uint32_t channel) const {
  auto it = m_channelItems.find(channel);
  return it == m_channelItems.end() ? nullptr : it->second;
}

WallpaperCaptureItem *
CaptureCoordinator::claimItemByOutputName(uint32_t channel,
                                          const QString &name) {
  for (auto *item : m_items) {
    bool alreadyClaimed = false;
    for (const auto &[boundChannel, boundItem] : m_channelItems) {
      if (boundItem == item) {
        alreadyClaimed = true;
        break;
      }
    }
    if (alreadyClaimed) {
      continue;
    }
    QScreen *screen = item->window() ? item->window()->screen() : nullptr;
    if (screen && screen->name() == name) {
      m_channelItems[channel] = item;
      qInfo() << "[coordinator] channel" << channel
              << "matched by XRandR output name" << name;
      return item;
    }
  }
  return nullptr;
}

WallpaperCaptureItem *CaptureCoordinator::claimItemForSize(uint32_t channel,
                                                           quint32 width,
                                                           quint32 height) {
  if (auto name = x11OutputForSize(width, height)) {
    if (WallpaperCaptureItem *item = claimItemByOutputName(channel, *name)) {
      return item;
    }
  }

  double globalScale = xwaylandGlobalScale();
  for (auto *item : m_items) {
    bool alreadyClaimed = false;
    for (const auto &[boundChannel, boundItem] : m_channelItems) {
      if (boundItem == item) {
        alreadyClaimed = true;
        break;
      }
    }
    if (alreadyClaimed) {
      continue;
    }
    std::optional<WallpiperProtocol::MonitorGeometry> geometry =
        item->currentGeometry();
    if (!geometry) {
      continue;
    }
    WallpiperProtocol::MonitorGeometry predicted =
        predictedXWaylandRect(*geometry, globalScale);
    if (nearlyEqual(predicted.width, width) &&
        nearlyEqual(predicted.height, height)) {
      m_channelItems[channel] = item;
      QScreen *screen = item->window() ? item->window()->screen() : nullptr;
      qInfo() << "[coordinator] channel" << channel << "bound to screen"
              << (screen ? screen->name() : QStringLiteral("<unknown>"))
              << "for stream" << width << "x" << height << "(predicted"
              << predicted.width << "x" << predicted.height
              << "at XWayland scale" << globalScale << ")";
      return item;
    }
  }
  return nullptr;
}

WallpaperCaptureItem *
CaptureCoordinator::claimItemForPosition(uint32_t channel, qint32 x, qint32 y) {
  if (auto name = x11OutputForPosition(x, y)) {
    if (WallpaperCaptureItem *item = claimItemByOutputName(channel, *name)) {
      return item;
    }
  }

  double globalScale = xwaylandGlobalScale();
  for (auto *item : m_items) {
    bool alreadyClaimed = false;
    for (const auto &[boundChannel, boundItem] : m_channelItems) {
      if (boundItem == item) {
        alreadyClaimed = true;
        break;
      }
    }
    if (alreadyClaimed) {
      continue;
    }
    std::optional<WallpiperProtocol::MonitorGeometry> geometry =
        item->currentGeometry();
    if (!geometry) {
      continue;
    }
    WallpiperProtocol::MonitorGeometry predicted =
        predictedXWaylandRect(*geometry, globalScale);
    if (nearlyEqual(predicted.x, x) && nearlyEqual(predicted.y, y)) {
      m_channelItems[channel] = item;
      QScreen *screen = item->window() ? item->window()->screen() : nullptr;
      qInfo() << "[coordinator] channel" << channel << "bound to screen"
              << (screen ? screen->name() : QStringLiteral("<unknown>"))
              << "at position" << x << "," << y << "(predicted" << predicted.x
              << "," << predicted.y << "at XWayland scale" << globalScale
              << ")";
      return item;
    }
  }
  return nullptr;
}

WallpaperCaptureItem *
CaptureCoordinator::claimItem(uint32_t channel, quint32 width, quint32 height,
                              bool hasGeometry, qint32 geomX, qint32 geomY) {
  WallpaperCaptureItem *item = channelItem(channel);
  if (!item && hasGeometry) {
    item = claimItemForPosition(channel, geomX, geomY);
  }
  if (!item) {
    item = claimItemForSize(channel, width, height);
  }
  return item;
}

void CaptureCoordinator::cacheBuf(quint32 slot, quint32 width, quint32 height,
                                  quint32 format, quint32 stride,
                                  quint64 modifier, bool hasGeometry,
                                  qint32 geomX, qint32 geomY, int fd) {
  auto existing = m_slotCache.find(slot);
  if (existing != m_slotCache.end() && existing->second.fd >= 0) {
    ::close(existing->second.fd);
  }
  m_slotCache[slot] = CachedBuf{width,       height, format, stride, modifier,
                                hasGeometry, geomX,  geomY,  ::dup(fd)};
}

const CaptureCoordinator::CachedBuf *
CaptureCoordinator::cachedInfo(uint32_t channel) const {
  for (const auto &[slot, cached] : m_slotCache) {
    if (slot / kCaptureSlotCount == channel) {
      return &cached;
    }
  }
  return nullptr;
}

WallpaperCaptureItem *
CaptureCoordinator::itemForOutput(const QString &name) const {
  for (auto *item : m_items) {
    QScreen *screen = item->window() ? item->window()->screen() : nullptr;
    if (screen && screen->name() == name) {
      return item;
    }
  }
  return nullptr;
}

/*
 * The screen a stream of the given size/position is meant for, regardless of
 * current bindings. Wire channel numbers are recycled by the capture layer, so
 * a channel that used to carry the 1920x1080 monitor may later carry the
 * 1366x768 one; bindings must follow the stream, not the channel number.
 */
WallpaperCaptureItem *
CaptureCoordinator::preferredItemFor(quint32 width, quint32 height,
                                     bool hasGeometry, qint32 geomX,
                                     qint32 geomY) const {
  std::optional<QString> output;
  if (hasGeometry) {
    output = x11OutputForPosition(geomX, geomY);
  }
  if (!output) {
    output = x11OutputForSize(width, height);
  }
  return output ? itemForOutput(*output) : nullptr;
}

bool CaptureCoordinator::channelIsStale(
    uint32_t channel, std::chrono::steady_clock::time_point now) const {
  auto last = m_channelLastFrame.find(channel);
  return last == m_channelLastFrame.end() ||
         now - last->second > kStaleChannelTimeout;
}

// Bind `channel` to `item` (dropping any other binding either of them had) and
// re-stage the channel's cached buffers of the given size on the item.
void CaptureCoordinator::bindChannel(uint32_t channel,
                                     WallpaperCaptureItem *item,
                                     std::optional<quint32> skipSlot,
                                     quint32 width, quint32 height) {
  for (auto it = m_channelItems.begin(); it != m_channelItems.end();) {
    if (it->second == item || it->first == channel) {
      it = m_channelItems.erase(it);
    } else {
      ++it;
    }
  }
  m_channelItems[channel] = item;
  m_loggedDroppedChannels.erase(channel);

  for (const auto &[slot, cached] : m_slotCache) {
    if (slot / kCaptureSlotCount != channel || cached.fd < 0 ||
        (skipSlot && slot == *skipSlot) || cached.width != width ||
        cached.height != height) {
      continue;
    }
    int fd = ::dup(cached.fd);
    if (fd >= 0) {
      item->stageBuf(slot, cached.width, cached.height, cached.format,
                     cached.stride, cached.modifier, fd, -1);
    }
  }
}

static QString screenNameOf(WallpaperCaptureItem *item) {
  QScreen *screen = item && item->window() ? item->window()->screen() : nullptr;
  return screen ? screen->name() : QStringLiteral("<unknown>");
}

void CaptureCoordinator::handleBuf(quint32 slot, quint32 width, quint32 height,
                                   quint32 format, quint32 stride,
                                   quint64 modifier, bool hasGeometry,
                                   qint32 geomX, qint32 geomY, int fd,
                                   int syncFd) {
  uint32_t channel = slot / kCaptureSlotCount;
  cacheBuf(slot, width, height, format, stride, modifier, hasGeometry, geomX,
           geomY, fd);
  m_channelSince[channel] = std::chrono::steady_clock::now();
  for (const auto &[mirror, source] : m_mirrors) {
    if (source == channel) {
      int mfd = ::dup(fd);
      if (mfd >= 0) {
        mirror->stageBuf(slot, width, height, format, stride, modifier, mfd, -1);
      }
    }
  }

  const auto now = std::chrono::steady_clock::now();
  WallpaperCaptureItem *current = channelItem(channel);
  WallpaperCaptureItem *preferred =
      preferredItemFor(width, height, hasGeometry, geomX, geomY);

  if (preferred && preferred != current) {
    std::optional<uint32_t> holder;
    for (const auto &[boundChannel, boundItem] : m_channelItems) {
      if (boundItem == preferred && boundChannel != channel) {
        holder = boundChannel;
      }
    }
    bool takeOver = !holder || channelIsStale(*holder, now);
    if (!takeOver) {
      // The holder is live; only displace it if it does not belong there.
      const CachedBuf *info = cachedInfo(*holder);
      takeOver = !info || preferredItemFor(info->width, info->height,
                                           info->hasGeometry, info->geomX,
                                           info->geomY) != preferred;
    }
    if (takeOver) {
      qInfo() << "[coordinator] channel" << channel << "(" << width << "x"
              << height << ") belongs on" << screenNameOf(preferred)
              << (holder ? QStringLiteral("- taking it from channel %1")
                               .arg(*holder)
                         : QStringLiteral(""));
      bindChannel(channel, preferred, slot, width, height);
      preferred->stageBuf(slot, width, height, format, stride, modifier, fd,
                          syncFd);

      // Give the displaced channel the screen it actually belongs on, if free.
      if (holder) {
        if (const CachedBuf *info = cachedInfo(*holder)) {
          WallpaperCaptureItem *target =
              preferredItemFor(info->width, info->height, info->hasGeometry,
                               info->geomX, info->geomY);
          if (target && !channelItemFor(target)) {
            qInfo() << "[coordinator] moving displaced channel" << *holder
                    << "to" << screenNameOf(target);
            bindChannel(*holder, target, std::nullopt, info->width,
                        info->height);
          }
        }
      }
      return;
    }
  }

  WallpaperCaptureItem *item =
      claimItem(channel, width, height, hasGeometry, geomX, geomY);
  if (item) {
    item->stageBuf(slot, width, height, format, stride, modifier, fd, syncFd);
    return;
  }

  auto it = m_pendingBufs.find(slot);
  if (it != m_pendingBufs.end()) {
    ::close(it->second.fd);
    if (it->second.syncFd >= 0) {
      ::close(it->second.syncFd);
    }
  }
  m_pendingBufs[slot] =
      PendingBuf{width,       height, format, stride, modifier,
                 hasGeometry, geomX,  geomY,  fd,     syncFd};
  qInfo() << "[coordinator] no wallpaper item available yet for new channel"
          << channel << "(" << width << "x" << height << "), queued for retry";
}

std::optional<uint32_t>
CaptureCoordinator::channelItemFor(WallpaperCaptureItem *item) const {
  for (const auto &[channel, bound] : m_channelItems) {
    if (bound == item) {
      return channel;
    }
  }
  return std::nullopt;
}

/*
 * A channel with no screen is delivering frames. If the screen its stream
 * belongs on is held by a channel that went silent (Wallpaper Engine created
 * the next wallpaper's swapchain before tearing down the old one), move the
 * screen over and re-stage the cached buffers.
 */
bool CaptureCoordinator::rebindStaleItem(uint32_t channel) {
  const auto now = std::chrono::steady_clock::now();
  auto attempt = m_lastRebindAttempt.find(channel);
  if (attempt != m_lastRebindAttempt.end() &&
      now - attempt->second < kRebindAttemptInterval) {
    return false;
  }
  m_lastRebindAttempt[channel] = now;

  const CachedBuf *info = cachedInfo(channel);
  if (!info) {
    return false;
  }
  WallpaperCaptureItem *target = preferredItemFor(
      info->width, info->height, info->hasGeometry, info->geomX, info->geomY);
  const bool matchesAScreen = target != nullptr;
  if (!target && m_channelItems.size() == 1) {
    // Unknown size (e.g. odd scaling); only take over when unambiguous.
    target = m_channelItems.begin()->second;
  }
  if (!target) {
    return false;
  }

  std::optional<uint32_t> holder = channelItemFor(target);
  if (holder && *holder == channel) {
    return false;
  }
  if (holder && !channelIsStale(*holder, now)) {
    /*
     * Its screen is taken by another live stream of the same kind. In "clone"
     * mode Wallpaper Engine renders every monitor at the primary's size, so
     * both streams look like they belong on the primary. If this has gone on
     * for a while (i.e. it is not just the overlap of a wallpaper switch),
     * adopt a screen that has no live stream instead of leaving it frozen.
     */
    auto since = m_channelSince.find(channel);
    // Only real wallpaper streams (sized like a monitor) may adopt a screen;
    // never e.g. Wallpaper Engine's own UI/preview swapchains.
    if (!matchesAScreen || since == m_channelSince.end() ||
        now - since->second < 2 * kStaleChannelTimeout) {
      return false;
    }
    WallpaperCaptureItem *orphan = nullptr;
    for (auto *item : m_items) {
      std::optional<uint32_t> owner = channelItemFor(item);
      if (!owner) {
        orphan = item;
        break;
      }
      if (!orphan && channelIsStale(*owner, now)) {
        orphan = item;
      }
    }
    if (!orphan) {
      return false;
    }
    holder = channelItemFor(orphan);
    target = orphan;
  }

  qInfo() << "[coordinator] channel"
          << (holder ? QString::number(*holder) : QStringLiteral("<none>"))
          << "went silent; moving screen" << screenNameOf(target)
          << "to channel" << channel;
  bindChannel(channel, target, std::nullopt, info->width, info->height);

  for (auto p = m_pendingBufs.begin(); p != m_pendingBufs.end();) {
    if (p->first / kCaptureSlotCount != channel) {
      ++p;
      continue;
    }
    ::close(p->second.fd);
    if (p->second.syncFd >= 0) {
      ::close(p->second.syncFd);
    }
    p = m_pendingBufs.erase(p);
  }
  return true;
}

/*
 * "Clone" layout: Wallpaper Engine renders a single swapchain (for the primary
 * monitor) and on Windows mirrors it to the other monitors by itself. Here a
 * screen in that situation would stay frozen on whatever it showed last, so
 * when a screen has had no live stream for a while and exactly one wallpaper
 * stream is live, show that stream on it as well. With separate wallpapers per
 * display there are several live streams and nothing is mirrored.
 */
void CaptureCoordinator::evaluateMirrors() {
  const auto now = std::chrono::steady_clock::now();
  if (now - m_lastMirrorEval < kMirrorEvalInterval) {
    return;
  }
  m_lastMirrorEval = now;

  std::vector<uint32_t> live;
  for (const auto &[channel, item] : m_channelItems) {
    if (channelIsStale(channel, now)) {
      continue;
    }
    const CachedBuf *info = cachedInfo(channel);
    if (info && preferredItemFor(info->width, info->height, info->hasGeometry,
                                 info->geomX, info->geomY)) {
      live.push_back(channel);
    }
  }

  for (auto *item : m_items) {
    std::optional<uint32_t> owner = channelItemFor(item);
    const bool hasLive = owner && !channelIsStale(*owner, now);
    auto mirror = m_mirrors.find(item);

    if (hasLive) {
      m_orphanSince.erase(item);
      if (mirror != m_mirrors.end()) {
        qInfo() << "[coordinator] screen" << screenNameOf(item)
                << "has its own stream again; stopping mirror";
        m_mirrors.erase(mirror);
      }
      continue;
    }

    if (mirror != m_mirrors.end()) {
      const bool sourceOk = live.size() == 1 && live.front() == mirror->second;
      if (sourceOk) {
        continue;
      }
      m_mirrors.erase(mirror);
    }

    auto since = m_orphanSince.emplace(item, now).first->second;
    if (live.size() != 1 || now - since < kMirrorAfter) {
      continue;
    }
    const uint32_t source = live.front();
    if (channelItem(source) == item) {
      continue;
    }
    qInfo() << "[coordinator] screen" << screenNameOf(item)
            << "has no stream of its own; mirroring channel" << source
            << "(clone layout)";
    m_mirrors[item] = source;
    for (const auto &[slot, cached] : m_slotCache) {
      if (slot / kCaptureSlotCount != source || cached.fd < 0) {
        continue;
      }
      int fd = ::dup(cached.fd);
      if (fd >= 0) {
        item->stageBuf(slot, cached.width, cached.height, cached.format,
                       cached.stride, cached.modifier, fd, -1);
      }
    }
  }
}

void CaptureCoordinator::retryPendingBufs() {
  for (auto it = m_pendingBufs.begin(); it != m_pendingBufs.end();) {
    quint32 slot = it->first;
    const PendingBuf &pending = it->second;
    uint32_t channel = slot / kCaptureSlotCount;
    WallpaperCaptureItem *item =
        claimItem(channel, pending.width, pending.height, pending.hasGeometry,
                  pending.geomX, pending.geomY);
    if (!item) {
      ++it;
      continue;
    }
    item->stageBuf(slot, pending.width, pending.height, pending.format,
                   pending.stride, pending.modifier, pending.fd,
                   pending.syncFd);
    it = m_pendingBufs.erase(it);
  }
}

void CaptureCoordinator::ensureSocketsBound() {
  m_captureSocket->start();
  m_ctlListener->start();
  if (!m_cursorTimer->isActive()) {
    m_cursorTimer->start();
  }
  if (!m_pendingRetryTimer->isActive()) {
    m_pendingRetryTimer->start();
  }
}

void CaptureCoordinator::teardownSockets() {
  m_captureSocket->stop();
  m_ctlListener->stop();
  m_cursorTimer->stop();
  m_pendingRetryTimer->stop();
  for (auto &[slot, pending] : m_pendingBufs) {
    ::close(pending.fd);
    if (pending.syncFd >= 0) {
      ::close(pending.syncFd);
    }
  }
  m_pendingBufs.clear();
  for (auto &[slot, cached] : m_slotCache) {
    if (cached.fd >= 0) {
      ::close(cached.fd);
    }
  }
  m_slotCache.clear();
  m_loggedDroppedChannels.clear();
}

std::optional<WallpiperProtocol::MonitorGeometry>
CaptureCoordinator::geometryFromActiveItem() const {
  if (!m_primaryItem) {
    return std::nullopt;
  }
  return m_primaryItem->currentGeometry();
}

void CaptureCoordinator::handleDetach() {
  for (auto *item : m_items) {
    item->requestDetach();
  }
  qInfo() << "[ctl] detached, released all buffers";
}

bool CaptureCoordinator::captureChannel(uint32_t channel, const QString &path,
                                        QString &err) {
  struct BeginCaptureResult {
    std::mutex mutex;
    std::condition_variable cv;
    bool done = false;
    std::shared_ptr<CaptureCompletion> completion;
  };
  auto begin = std::make_shared<BeginCaptureResult>();

  QMetaObject::invokeMethod(
      this,
      [this, channel, path, begin]() {
        std::shared_ptr<CaptureCompletion> completion;
        if (WallpaperCaptureItem *item = channelItem(channel)) {
          completion = item->beginCapture(path);
        }
        std::lock_guard<std::mutex> lock(begin->mutex);
        begin->completion = std::move(completion);
        begin->done = true;
        begin->cv.notify_all();
      },
      Qt::QueuedConnection);

  std::shared_ptr<CaptureCompletion> completion;
  {
    std::unique_lock<std::mutex> lock(begin->mutex);
    if (!begin->cv.wait_for(lock, std::chrono::seconds(8),
                            [&] { return begin->done; })) {
      err = QStringLiteral("timed out waiting for capture to start");
      return false;
    }
    completion = begin->completion;
  }

  if (!completion) {
    err = QStringLiteral("no wallpaper item bound to channel %1").arg(channel);
    return false;
  }

  std::unique_lock<std::mutex> lock(completion->mutex);
  bool signaled = completion->cv.wait_for(lock, std::chrono::seconds(8),
                                          [&] { return completion->done; });
  if (!signaled) {
    err = QStringLiteral("timed out waiting for capture");
    return false;
  }
  err = completion->err;
  return completion->ok;
}

void CaptureCoordinator::handleSetDebug(bool enabled) {
  for (auto *item : m_items) {
    item->setDebugEnabled(enabled);
  }
  qInfo() << "[ctl] debug overlay ->" << enabled;
}

} // namespace WallpiperKde
