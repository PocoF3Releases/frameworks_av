/*
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "IAfEffect.h"
#include "IAfThread.h"

#include <media/AudioContainers.h>

#include <map>
#include <mutex>
#include <optional>
#include <vector>

namespace android {

/**
 * Synchronizes the global Dolby DAP effect with AudioFlinger state.
 *
 * Product-selected effect contracts distinguish the preserved QDSP binary
 * (standard I/O/offload command and scalar pregain) from the newer legacy
 * reference (private I/O, two-word pregain and track attributes). A DAX version
 * or HAL version does not imply support for either private protocol.
 * Optional sound-type bypass is not app VoIP policy and is not enabled here.
 */
class DolbyDapController final {
public:
    static DolbyDapController& getInstance();

    // Only the output-mix DAP belongs to this controller. Per-session DAP
    // movement must not issue skip-hard-bypass on an unrelated global effect.
    static bool isSupported();
    static bool isDapEffect(const sp<IAfEffectModule>& effect);

    void effectCreated(
            const sp<IAfEffectModule>& effect,
            audio_io_handle_t io,
            IAfThreadBase::type_t threadType,
            const DeviceTypeSet& devices) EXCLUDES_EffectChain_Mutex;
    void effectReleased(const sp<IAfEffectModule>& effect)
            REQUIRES(audio_utils::EffectChain_Mutex);

    void updateOffload(
            audio_io_handle_t io,
            IAfThreadBase::type_t threadType,
            const DeviceTypeSet& devices) EXCLUDES_EffectChain_Mutex;

    // A failed/released patch has no confirmed route. Do not retain its I/O ACK.
    void invalidateOutput(audio_io_handle_t io) EXCLUDES_EffectChain_Mutex;

    void updatePregain(
            IAfThreadBase::type_t threadType,
            audio_io_handle_t io,
            audio_output_flags_t flags,
            uint32_t maxVolume) EXCLUDES_EffectChain_Mutex;

    struct ActiveTrackState {
        uint32_t flags = 0;
        bool hasTracks = false;
    };

    // Caller owns the output thread lock. No live track objects cross into the
    // controller, which may need to lock a DIFFERENT output's effect chain.
    template <typename Tracks>
    static ActiveTrackState summarizeTracks(const Tracks& tracks) {
        ActiveTrackState state;
        for (const auto& track : tracks) {
            if (track != nullptr && !track->isFastTrack() && track->isExternalTrack()) {
                state.flags |= static_cast<uint32_t>(track->attributes().flags);
                state.hasTracks = true;
            }
        }
        return state;
    }
    void updateAudioTracks(audio_io_handle_t io, ActiveTrackState state)
            EXCLUDES_EffectChain_Mutex;

    // Saturating U8.24 conversion. Invalid channels contribute silence rather
    // than causing undefined float-to-integer conversion in the control path.
    static uint32_t volumeToU8_24(float volume);

    status_t skipHardBypass() EXCLUDES_EffectChain_Mutex;

private:
    DolbyDapController() = default;

    bool usesQdspControlPath() const;
    bool usesLegacyDax36ControlPath() const;
    struct EffectSnapshot {
        sp<IAfEffectModule> effect;
        sp<EffectCallbackInterface> callback;
        sp<IAfEffectChain> chain;
    };

    EffectSnapshot currentEffect() const;
    bool isCurrentEffect_l(const EffectSnapshot& snapshot) const
            REQUIRES(audio_utils::EffectChain_Mutex);
    bool syncAttachment_l(const EffectSnapshot& snapshot)
            REQUIRES(audio_utils::EffectChain_Mutex) EXCLUDES_EffectBase_Mutex;
    bool observeEnabled_l(const EffectSnapshot& snapshot)
            REQUIRES(audio_utils::EffectChain_Mutex);
    // Bookkeeping only; caller holds mMutex, never an effect command.
    void resetAttachmentState_l(audio_io_handle_t io);

    status_t setParam(
            const sp<IAfEffectModule>& effect,
            int32_t paramId,
            int32_t value) const
            REQUIRES(audio_utils::EffectChain_Mutex) EXCLUDES_EffectBase_Mutex;
    status_t setParameters(
            const sp<IAfEffectModule>& effect,
            int32_t paramId,
            const std::vector<int32_t>& values) const
            REQUIRES(audio_utils::EffectChain_Mutex) EXCLUDES_EffectBase_Mutex;

    // Never acquire a chain/effect lock or call the HAL while holding mMutex.
    // Commands follow ThreadBase (if held) -> EffectChain -> EffectBase.
    mutable std::mutex mMutex;
    sp<IAfEffectModule> mEffect;

    sp<EffectCallbackInterface> mSyncedCallback;
    sp<EffectCallbackInterface> mTargetCallback;
    bool mTargetOffloaded = false;
    bool mLastEnabled = false;
    unsigned mAttachmentFailures = 0;
    int64_t mNextAttachmentAttemptNs = 0;
    audio_io_handle_t mEffectIo = AUDIO_IO_HANDLE_NONE;
    uint64_t mGeneration = 0;
    std::map<audio_io_handle_t, uint32_t> mOutputVolumes;

    std::map<audio_io_handle_t, uint32_t> mOutputAudioFlags;
    std::optional<uint32_t> mDesiredAudioFlags;
    std::optional<uint32_t> mLastAudioFlags;
    uint64_t mAudioFlagsGeneration = 0;
    unsigned mAudioFlagsFailures = 0;
    int64_t mNextAudioFlagsAttemptNs = 0;

    struct PregainRetryState {
        std::optional<uint32_t> desired;
        unsigned failures = 0;
        int64_t nextAttemptNs = 0;
        status_t lastStatus = NO_ERROR;
    };
    PregainRetryState& pregainRetryState_l(bool scalar, audio_output_flags_t flag);
    void resetPregainRetries_l();
    PregainRetryState mScalarPregainRetry;
    PregainRetryState mDeepBufferPregainRetry;
    PregainRetryState mDirectPregainRetry;
    PregainRetryState mOffloadPregainRetry;

    uint32_t mLastScalarPregain = 0;
    uint32_t mLastDeepBufferPregain = 0;
    uint32_t mLastDirectPregain = 0;
    uint32_t mLastOffloadPregain = 0;
};

}  // namespace android
