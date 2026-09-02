#include "CompositorService.h"

#include <QMetaType>
#include <cmath>

namespace {
#ifdef Q_OS_ANDROID
// How late a finished frame may be and still be worth showing. A phone can miss the
// desktop 100 ms deadline on every single frame, and dropping them all leaves the
// playhead running over a frozen canvas, which is worse than a late picture. The
// adaptive-quality feedback below still measures against its own wall-clock budget,
// so a phone that is actually behind still scales down — this only widens the window
// for a frame that is late but not stale enough to be worse than nothing.
constexpr drift::TimeUs kMaxPreviewFrameStalenessUs = 1'000'000;
#else
constexpr drift::TimeUs kMaxPreviewFrameStalenessUs = 100'000;
#endif
constexpr double kAdaptiveScaleMin = 0.25;
constexpr double kAdaptiveScaleStepDown = 0.75;
constexpr double kAdaptiveScaleStepUp = 1.25;
constexpr int kLateBeforeScaleDown = 2;
constexpr int kOnTimeBeforeScaleUp = 6;
// Composites at the start of a run open and seek the decoders and are reliably
// slower than the ones after them. Charging those to the adaptive scale used to
// downscale the first seconds of every playback.
constexpr int kWarmupFrames = 3;
// Minimum useful realtime deadline.
//
// The old 40 ms floor prevented Auto quality from targeting 30/60 fps:
// a 30 fps frame has only ~33 ms and a 60 fps frame ~16 ms.
//
// Keep only a small safety floor for pathological timer values. The caller
// supplies the actual project-frame interval.
constexpr int kMinLateFrameBudgetMs = 8;
}

CompositorWorker::CompositorWorker(QObject *parent)
    : QObject(parent)
{
}

void CompositorWorker::composite(drift::TimeUs timeUs, FrameCompositor::RenderOptions options,
                                 std::shared_ptr<const drift::Project> snapshot)
{
    // Keep the shared tree alive for the whole frame; setProject only borrows.
    m_snapshot = std::move(snapshot);
    if (!m_snapshot) {
        // Still report completion: the service treats a request as in flight
        // until the worker answers, and the quality-mode play loop waits for it.
        emit frameReady(GpuFrameTexture{}, timeUs);
        return;
    }
    m_compositor.setProject(m_snapshot.get());

    emit frameReady(m_compositor.compositeToTextureAt(timeUs, options), timeUs);
}

CompositorService::CompositorService(QObject *parent)
    : QObject(parent)
    , m_worker(new CompositorWorker)
{
    qRegisterMetaType<drift::TimeUs>("drift::TimeUs");
    qRegisterMetaType<std::shared_ptr<const drift::Project>>("std::shared_ptr<const drift::Project>");
    qRegisterMetaType<FrameCompositor::RenderOptions>("FrameCompositor::RenderOptions");
    qRegisterMetaType<GpuFrameTexture>("GpuFrameTexture");
    m_worker->moveToThread(&m_thread);
    connect(m_worker, &CompositorWorker::frameReady, this, &CompositorService::onWorkerFrameReady,
            Qt::QueuedConnection);
    m_thread.start();
    m_debugWindow.start();
}

CompositorService::~CompositorService()
{
    m_thread.quit();
    m_thread.wait();
    delete m_worker;
    m_worker = nullptr;
}

void CompositorService::setProject(const drift::Project *project)
{
    m_project = project;
    invalidateSnapshot();
}

void CompositorService::invalidateSnapshot()
{
    ++m_liveGeneration;
    m_sharedSnapshot.reset();
}

void CompositorService::setDropLateFrames(bool drop)
{
    if (m_dropLateFrames == drop)
        return;
    m_dropLateFrames = drop;
    // Quality mode renders every frame at the requested scale; leaving a
    // downscale from a previous fast-mode run would defeat the point.
    resetAdaptiveState();
}

void CompositorService::setAdaptiveQuality(bool enabled)
{
    if (m_adaptiveQuality == enabled)
        return;
    m_adaptiveQuality = enabled;
    // Switching away hands the caller its requested scale back immediately;
    // switching on starts from full rather than from a stale measurement.
    resetAdaptiveState();
}

void CompositorService::setPlaybackActive(bool active)
{
    if (m_playbackActive == active)
        return;
    m_playbackActive = active;
    // The scale itself survives across runs — a machine that could not keep up a
    // moment ago still cannot, and relearning that on every play would drop the
    // preview a second or two into each one. Only the streaks restart.
    m_lateStreak = 0;
    m_onTimeStreak = 0;
    m_warmupFramesLeft = active ? kWarmupFrames : 0;
}

void CompositorService::setLateFrameBudgetMs(int ms)
{
    m_lateFrameBudgetMs = qMax(kMinLateFrameBudgetMs, ms);
}

double CompositorService::adaptiveScaleFactor() const
{
    return m_adaptiveScale;
}

void CompositorService::resetAdaptiveState()
{
    m_adaptiveScale = 1.0;
    m_lateStreak = 0;
    m_onTimeStreak = 0;
    m_warmupFramesLeft = 0;
}

FrameCompositor::RenderOptions CompositorService::effectiveOptions(
    FrameCompositor::RenderOptions options) const
{
    if (!m_adaptiveQuality)
        return options;
    options.previewScale = qBound(kMinPreviewScale, options.previewScale * m_adaptiveScale, 1.0);
    return options;
}

void CompositorService::noteFrameLate(bool late)
{
    if (m_warmupFramesLeft > 0) {
        --m_warmupFramesLeft;
        return;
    }

    if (late) {
        m_onTimeStreak = 0;
        ++m_lateStreak;
        if (m_lateStreak >= kLateBeforeScaleDown && m_adaptiveScale > kAdaptiveScaleMin + 1e-6) {
            m_adaptiveScale = qMax(kAdaptiveScaleMin, m_adaptiveScale * kAdaptiveScaleStepDown);
            m_lateStreak = 0;
        }
        return;
    }

    m_lateStreak = 0;
    ++m_onTimeStreak;
    if (m_onTimeStreak >= kOnTimeBeforeScaleUp && m_adaptiveScale < 1.0 - 1e-6) {
        m_adaptiveScale = qMin(1.0, m_adaptiveScale * kAdaptiveScaleStepUp);
        m_onTimeStreak = 0;
    }
}

void CompositorService::dispatch(drift::TimeUs timeUs, const FrameCompositor::RenderOptions &options)
{
    if (!m_project)
        return;

    if (!m_sharedSnapshot || m_snapshotGeneration != m_liveGeneration) {
        // One uniquely-owned snapshot per generation; subsequent ticks only bump
        // the shared_ptr. Plain Project copy would keep sharing QMap/QList with
        // the live tree — unsafe once the GUI mutates while the worker reads.
        m_sharedSnapshot = std::make_shared<drift::Project>(m_project->detachedCopy());
        m_snapshotGeneration = m_liveGeneration;
    }

    m_renderElapsed.start();
    QMetaObject::invokeMethod(m_worker, "composite", Qt::QueuedConnection,
                              Q_ARG(drift::TimeUs, timeUs),
                              Q_ARG(FrameCompositor::RenderOptions, options),
                              Q_ARG(std::shared_ptr<const drift::Project>, m_sharedSnapshot));
}

void CompositorService::requestComposite(drift::TimeUs timeUs, FrameCompositor::RenderOptions options)
{
    options.previewScale = qBound(kMinPreviewScale, options.previewScale, 1.0);

    // The pending scale is published as whole percent, and the catch-up dispatch in
    // onWorkerFrameReady reads it back as percent/100. Dispatching the unrounded value here
    // would leave the two permanently unequal (0.16667 vs 0.17), so every finished frame would
    // re-dispatch at a slightly different canvas size — the preview would flip between, say,
    // 120x213 and 122x218 on every frame, rebuilding the presentation ring's FBOs each time and
    // handing the scene graph freshly allocated (black) textures. Round-trip through the same
    // integer here so both paths ask for one size. What is stored and compared is the requested
    // scale; the adaptive multiplier is a discrete ratchet applied at dispatch, so both paths
    // still derive the same size from it until the ratchet deliberately moves.
    const int previewScalePercent =
        qBound(kMinPreviewScalePercent, static_cast<int>(std::lround(options.previewScale * 100.0)), 100);
    options.previewScale = previewScalePercent / 100.0;

    m_pendingTimeUs.store(timeUs, std::memory_order_release);
    m_pendingPreviewScalePercent.store(previewScalePercent, std::memory_order_release);
    m_pendingMaxTimeEchoHistoryFrames.store(options.maxTimeEchoHistoryFrames, std::memory_order_release);
    m_pendingReadAheadUs.store(options.readAheadUs, std::memory_order_release);
    if (m_requestPending.exchange(true, std::memory_order_acq_rel))
        return;

    m_lastDispatchedTimeUs = timeUs;
    m_lastDispatchedOptions = options;
    dispatch(timeUs, effectiveOptions(options));
}

void CompositorService::onWorkerFrameReady(const GpuFrameTexture &frame, drift::TimeUs timeUs)
{
    const qint64 renderMs = m_renderElapsed.isValid() ? m_renderElapsed.elapsed() : 0;

    ++m_debugCompletedFrames;
    m_debugRenderTotalMs += renderMs;
    m_debugRenderMaxMs = qMax(m_debugRenderMaxMs, renderMs);
    const drift::TimeUs latest = m_pendingTimeUs.load(std::memory_order_acquire);
    FrameCompositor::RenderOptions latestOptions;
    latestOptions.previewScale =
        static_cast<double>(m_pendingPreviewScalePercent.load(std::memory_order_acquire)) / 100.0;
    latestOptions.maxTimeEchoHistoryFrames =
        m_pendingMaxTimeEchoHistoryFrames.load(std::memory_order_acquire);
    latestOptions.readAheadUs = m_pendingReadAheadUs.load(std::memory_order_acquire);

    // Quality mode shows every frame it renders, however far behind the request
    // it finished; only fast mode discards frames the playhead has run past.
    const bool stale = m_dropLateFrames && latest > timeUs
        && latest - timeUs > kMaxPreviewFrameStalenessUs;
    // Whether the frame is worth showing and whether it was rendered fast enough
    // are different questions. Adaptation asks the second one, in wall time, and
    // only about frames with a deadline: a paused or scrubbed frame is never late.
    if (m_adaptiveQuality && m_dropLateFrames && m_playbackActive)
        noteFrameLate(renderMs > m_lateFrameBudgetMs);
    if (!stale && frame.isValid()) {
        ++m_debugPresentedFrames;
        emit frameReady(frame, timeUs);
    } else if (stale) {
        ++m_debugDroppedFrames;
    }

    if (m_debugWindow.isValid() && m_debugWindow.elapsed() >= 1000) {
        const double seconds = m_debugWindow.elapsed() / 1000.0;

        const double presentedFps =
            seconds > 0.0 ? m_debugPresentedFrames / seconds : 0.0;

        const double completedFps =
            seconds > 0.0 ? m_debugCompletedFrames / seconds : 0.0;

        const double avgRenderMs =
            m_debugCompletedFrames > 0
                ? static_cast<double>(m_debugRenderTotalMs)
                    / m_debugCompletedFrames
                : 0.0;

        qWarning().noquote()
            << QStringLiteral(
                   "[PREVIEW-PERF] "
                   "presented=%1fps "
                   "completed=%2fps "
                   "avgRender=%3ms "
                   "maxRender=%4ms "
                   "dropped=%5 "
                   "budget=%6ms "
                   "scale=%7%% "
                   "adaptive=%8 "
                   "pending=%9")
                   .arg(presentedFps, 0, 'f', 1)
                   .arg(completedFps, 0, 'f', 1)
                   .arg(avgRenderMs, 0, 'f', 1)
                   .arg(m_debugRenderMaxMs)
                   .arg(m_debugDroppedFrames)
                   .arg(m_lateFrameBudgetMs)
                   .arg(static_cast<int>(m_adaptiveScale * 100.0))
                   .arg(m_adaptiveQuality ? QStringLiteral("yes")
                                         : QStringLiteral("no"))
                   .arg(m_requestPending.load(std::memory_order_acquire)
                            ? QStringLiteral("yes")
                            : QStringLiteral("no"));

        m_debugWindow.restart();
        m_debugRenderTotalMs = 0;
        m_debugRenderMaxMs = 0;
        m_debugCompletedFrames = 0;
        m_debugPresentedFrames = 0;
        m_debugDroppedFrames = 0;
    }

    m_requestPending.store(false, std::memory_order_release);

    if (latest != m_lastDispatchedTimeUs
        || latestOptions.previewScale != m_lastDispatchedOptions.previewScale
        || latestOptions.maxTimeEchoHistoryFrames != m_lastDispatchedOptions.maxTimeEchoHistoryFrames
        || latestOptions.readAheadUs != m_lastDispatchedOptions.readAheadUs) {
        m_lastDispatchedTimeUs = latest;
        m_lastDispatchedOptions = latestOptions;
        if (!m_requestPending.exchange(true, std::memory_order_acq_rel))
            dispatch(latest, effectiveOptions(latestOptions));
    }

    // Last: a listener may start the next composite from here, and that request
    // must not be overwritten by the catch-up dispatch above.
    emit compositeFinished();
}
