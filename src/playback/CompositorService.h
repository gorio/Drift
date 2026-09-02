#pragma once

#include "engine/FrameCompositor.h"
#include "engine/GpuCompositor.h"

#include <QElapsedTimer>
#include <QObject>
#include <QThread>

#include <atomic>
#include <memory>

class CompositorWorker : public QObject
{
    Q_OBJECT

public:
    explicit CompositorWorker(QObject *parent = nullptr);

public slots:
    // Shared immutable snapshot taken on the GUI thread. The worker must never
    // hold a pointer into the live project: compositing runs concurrently with
    // editing, and reading a QMap/QList while the GUI thread rebalances it is a
    // use-after-free. Use Project::detachedCopy() so Qt COW payloads are unique,
    // then share that snapshot via shared_ptr across queued invokes.
    void composite(drift::TimeUs timeUs, FrameCompositor::RenderOptions options,
                   std::shared_ptr<const drift::Project> snapshot);

signals:
    void frameReady(const GpuFrameTexture &frame, drift::TimeUs timeUs);

private:
    FrameCompositor m_compositor;
    std::shared_ptr<const drift::Project> m_snapshot;
};

// Background compositor thread. Frames are delivered to the GUI as live GL
// textures out of the runtime's presentation ring — never read back to the CPU
// and never re-uploaded. The ring is what keeps the scene graph from sampling a
// target that is being drawn into, so there is no frame buffering here.
class CompositorService : public QObject
{
    Q_OBJECT

public:
    explicit CompositorService(QObject *parent = nullptr);
    ~CompositorService() override;

    void setProject(const drift::Project *project);
    void requestComposite(drift::TimeUs timeUs,
                          FrameCompositor::RenderOptions options = FrameCompositor::RenderOptions{});

    // Fast playback (the default) drops frames that finish after the playhead has
    // moved on. Turn this off to present every rendered frame, however long it takes.
    void setDropLateFrames(bool drop);

    // Automatic preview quality. Off — the default — every frame renders at exactly
    // the previewScale the caller asked for (Full/Half/Quarter stay put). On, the
    // service walks that scale down while composites overrun their frame budget
    // and back up once they stop, which only ever happens during fast-mode playback.
    void setAdaptiveQuality(bool enabled);

    // Realtime playback running. Adaptation measures playback frames only — paused
    // and scrubbed frames have no deadline to miss — and ignores the first few
    // frames of each run, which pay for opening and seeking the decoders.
    void setPlaybackActive(bool active);

    // Wall-clock budget for one composite before it counts against the adaptive
    // scale. Callers derive it from the display cadence so it tracks project fps
    // and playback rate rather than assuming 30 fps at 1x.
    void setLateFrameBudgetMs(int ms);

    // Multiplier applied on top of the caller's previewScale while adaptive
    // quality is on (1.0 = full requested quality).
    double adaptiveScaleFactor() const;

    // Used by realtime transport priming. compositeFinished is emitted after
    // any catch-up request has already been dispatched, so this tells the
    // caller whether another composite is still in flight.
    bool isBusy() const
    {
        return m_requestPending.load(std::memory_order_acquire);
    }

signals:
    // Carries the exact timeline timestamp represented by the GPU texture.
    // PlaybackEngine uses it to keep one frame ready ahead of presentation
    // without displaying future video before the audio-master clock reaches it.
    void frameReady(const GpuFrameTexture &frame, drift::TimeUs timeUs);
    // One per completed request, whether or not a frame was produced or shown.
    void compositeFinished();

private slots:
    void onWorkerFrameReady(const GpuFrameTexture &frame, drift::TimeUs timeUs);

private:
    void dispatch(drift::TimeUs timeUs, const FrameCompositor::RenderOptions &options);
    void noteFrameLate(bool late);
    void resetAdaptiveState();
    FrameCompositor::RenderOptions effectiveOptions(FrameCompositor::RenderOptions options) const;

    const drift::Project *m_project = nullptr;
    // Reused across ticks until the live project pointer changes or the GUI
    // asks for a fresh snapshot after edits (generation bump via invalidateSnapshot).
    std::shared_ptr<const drift::Project> m_sharedSnapshot;
    int m_snapshotGeneration = 0;
    int m_liveGeneration = 0;

    std::atomic<bool> m_requestPending{false};
    std::atomic<drift::TimeUs> m_pendingTimeUs{0};
    std::atomic<int> m_pendingPreviewScalePercent{100};
    std::atomic<int> m_pendingMaxTimeEchoHistoryFrames{-1};
    std::atomic<drift::TimeUs> m_pendingReadAheadUs{0};
    drift::TimeUs m_lastDispatchedTimeUs = -1;
    // The scale the caller asked for, before any adaptive multiplier — that is
    // applied at dispatch, so a change takes effect on the very next frame.
    FrameCompositor::RenderOptions m_lastDispatchedOptions;
    // Wall time the in-flight composite has been running, which is what "late"
    // means. The old measure was how far the playhead had moved, in timeline
    // microseconds: at 4x that is four times larger for the same real delay, so
    // fast rates were judged late no matter how quickly frames actually rendered.
    QElapsedTimer m_renderElapsed;

    // Preview performance diagnostics. These counters are intentionally
    // aggregated and printed roughly once per second so diagnostic logging
    // does not itself disturb realtime playback.
    QElapsedTimer m_debugWindow;
    qint64 m_debugRenderTotalMs = 0;
    qint64 m_debugRenderMaxMs = 0;
    int m_debugCompletedFrames = 0;
    int m_debugPresentedFrames = 0;
    int m_debugDroppedFrames = 0;

    // Adaptive quality: consecutive on-time frames recover; late frames drop scale.
    int m_lateStreak = 0;
    int m_onTimeStreak = 0;
    int m_warmupFramesLeft = 0;
    int m_lateFrameBudgetMs = 100;
    double m_adaptiveScale = 1.0;
    bool m_adaptiveQuality = false;
    bool m_playbackActive = false;
    bool m_dropLateFrames = true;

    QThread m_thread;
    CompositorWorker *m_worker = nullptr;

public:
    // Call when the live project has been mutated so the next dispatch recopies.
    void invalidateSnapshot();
};

Q_DECLARE_METATYPE(GpuFrameTexture)
Q_DECLARE_METATYPE(std::shared_ptr<const drift::Project>)
