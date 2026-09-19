/*
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "AudioFlinger"

#include "DolbyDapController.h"

#include <cutils/properties.h>
#include <hardware/audio_effect.h>
#include <utils/Log.h>
#include <utils/Timers.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <cmath>
#include <limits>
#include <utility>

namespace android {
namespace {

constexpr effect_uuid_t kDapTypeUuid = {
        0x46d279d9, 0x9be7, 0x453d, 0x9d7c,
        {0xef, 0x93, 0x7f, 0x67, 0x55, 0x87}};

constexpr int32_t kParamSkipHardBypass = 0x13;
constexpr int32_t kParamIoHandle = 0x14;

constexpr char kDolbySupportProperty[] = "ro.vendor.audio.dolby.dax.support";
// This is the effect-binary contract selected by the product, not the audio HAL
// version. DAX version strings alone do not establish private command support.
constexpr char kDolbyControlProperty[] = "ro.vendor.audio.dolby.dap.control";

// Playback-driven recovery, not a vendor timing constant. No sleeps or retry
// worker on the audio thread, and no unbounded command storm on a failed HAL.
constexpr std::array<int64_t, 5> kAttachmentRetryNs = {
        250'000'000, 500'000'000, 1'000'000'000, 2'000'000'000, 4'000'000'000};

}  // namespace

DolbyDapController& DolbyDapController::getInstance() {
    static DolbyDapController instance;
    return instance;
}

bool DolbyDapController::isDapEffect(const sp<IAfEffectModule>& effect) {
    return effect != nullptr
            && effect->sessionId() == AUDIO_SESSION_OUTPUT_MIX
            && std::memcmp(
                    &effect->desc().type, &kDapTypeUuid, sizeof(effect_uuid_t)) == 0;
}

bool DolbyDapController::isSupported() {
    if (!property_get_bool(kDolbySupportProperty, false)) return false;
    char profile[PROPERTY_VALUE_MAX] = {};
    property_get(kDolbyControlProperty, profile, "none");
    return std::strcmp(profile, "qdsp") == 0 || std::strcmp(profile, "legacy") == 0;
}

bool DolbyDapController::usesQdspControlPath() const {
    char profile[PROPERTY_VALUE_MAX] = {};
    property_get(kDolbyControlProperty, profile, "none");
    return std::strcmp(profile, "qdsp") == 0;
}

DolbyDapController::EffectSnapshot DolbyDapController::currentEffect() const {
    EffectSnapshot snapshot;
    {
        std::lock_guard lock(mMutex);
        snapshot.effect = mEffect;
    }
    if (snapshot.effect != nullptr) {
        snapshot.callback = snapshot.effect->getCallback();
        if (snapshot.callback != nullptr) {
            snapshot.chain = snapshot.callback->chain().promote();
        }
    }
    return snapshot;
}

bool DolbyDapController::isCurrentEffect_l(const EffectSnapshot& snapshot) const {
    // A callback can still refer to the source chain after removeEffect(false).
    // Membership under the chain lock, not just a live callback, proves that
    // configure/release/migration cannot overlap this command transaction.
    if (snapshot.effect->getCallback() != snapshot.callback
            || !snapshot.chain->containsEffect_l(snapshot.effect)) {
        return false;
    }
    std::lock_guard lock(mMutex);
    return mEffect == snapshot.effect;
}

void DolbyDapController::resetAttachmentState_l(audio_io_handle_t io) {
    mEffectIo = io;
    mSyncedCallback.clear();
    mTargetCallback.clear();
    mAttachmentFailures = 0;
    mNextAttachmentAttemptNs = 0;
}

void DolbyDapController::effectCreated(
        const sp<IAfEffectModule>& effect,
        audio_io_handle_t io,
        IAfThreadBase::type_t threadType,
        const DeviceTypeSet& devices) {
    if (!isSupported() || !isDapEffect(effect)
            || effect->sessionId() != AUDIO_SESSION_OUTPUT_MIX) {
        return;
    }

    // Do not drop the previous effect's last strong reference under mMutex.
    sp<IAfEffectModule> previous;
    {
        std::lock_guard lock(mMutex);
        previous = std::move(mEffect);
        mEffect = effect;
        resetAttachmentState_l(AUDIO_IO_HANDLE_NONE);
    }

    updateOffload(io, threadType, devices);
    ALOGI("%s: captured output-mix DAP on io %d, offloadable %d",
            __func__, io, effect->isOffloadable());
}

void DolbyDapController::effectReleased(
        const sp<IAfEffectModule>& effect) {
    if (!isDapEffect(effect)
            || effect->sessionId() != AUDIO_SESSION_OUTPUT_MIX) {
        return;
    }

    std::lock_guard lock(mMutex);
    if (mEffect == effect) {
        mEffect.clear();
        resetAttachmentState_l(AUDIO_IO_HANDLE_NONE);
        ALOGI("%s: released output-mix DAP", __func__);
    }
}

void DolbyDapController::updateOffload(
        audio_io_handle_t io,
        IAfThreadBase::type_t threadType,
        const DeviceTypeSet& devices) {
    if (!isSupported() || io == AUDIO_IO_HANDLE_NONE) return;

    const auto snapshot = currentEffect();
    if (snapshot.chain == nullptr) return;
    audio_utils::lock_guard chainLock(snapshot.chain->mutex());
    if (!isCurrentEffect_l(snapshot) || snapshot.callback->io() != io) return;

    const bool requiresSoftwareDap =
            devices.find(AUDIO_DEVICE_OUT_REMOTE_SUBMIX) != devices.end();
    const bool offloaded =
            threadType == IAfThreadBase::OFFLOAD || !requiresSoftwareDap;
    const bool enabled = snapshot.effect->isEnabled();
    {
        std::lock_guard lock(mMutex);
        if (mEffect != snapshot.effect) return;
        // A fresh attachment/route event invalidates both gains and command
        // acknowledgements, including reattachment to the same numeric I/O.
        resetAttachmentState_l(io);
        mTargetCallback = snapshot.callback;
        mTargetOffloaded = offloaded;
        mLastEnabled = enabled;
    }
    (void)syncAttachment_l(snapshot);
}

void DolbyDapController::invalidateOutput(audio_io_handle_t io) {
    if (!isSupported() || io == AUDIO_IO_HANDLE_NONE) return;
    const auto snapshot = currentEffect();
    if (snapshot.chain == nullptr) return;
    audio_utils::lock_guard chainLock(snapshot.chain->mutex());
    if (!isCurrentEffect_l(snapshot) || snapshot.callback->io() != io) return;
    std::lock_guard lock(mMutex);
    if (mEffect == snapshot.effect) resetAttachmentState_l(AUDIO_IO_HANDLE_NONE);
}

bool DolbyDapController::syncAttachment_l(const EffectSnapshot& snapshot) {
    audio_io_handle_t io;
    bool offloaded;
    {
        std::lock_guard lock(mMutex);
        if (mEffect != snapshot.effect || mTargetCallback != snapshot.callback) return false;
        if (mSyncedCallback == snapshot.callback) return true;
        if (mAttachmentFailures > kAttachmentRetryNs.size()
                || systemTime(SYSTEM_TIME_MONOTONIC) < mNextAttachmentAttemptNs) return false;
        io = mEffectIo;
        offloaded = mTargetOffloaded;
    }
    if (io == AUDIO_IO_HANDLE_NONE || snapshot.callback->io() != io) return false;

    // The shipped Qualcomm DAP rejects private 0x14 and 0x15. Its standard
    // EFFECT_CMD_OFFLOAD carries the I/O handle, including non-offload outputs.
    // Only the explicitly selected legacy contract uses a separate I/O write.
    status_t status = usesQdspControlPath()
            ? NO_ERROR : setParam(snapshot.effect, kParamIoHandle, io);
    if (status == NO_ERROR && snapshot.effect->isOffloadable()) {
        status = snapshot.effect->setOffloaded_l(offloaded, io);
    }

    std::lock_guard lock(mMutex);
    if (mEffect != snapshot.effect || mTargetCallback != snapshot.callback || mEffectIo != io) {
        return false;
    }
    if (status == NO_ERROR) {
        // Acknowledge only the applicable contract's successful transaction.
        // Unsupported private commands are never synthesized as successful HAL
        // replies. No processing gate is changed by this owner.
        mSyncedCallback = snapshot.callback;
        mAttachmentFailures = 0;
        mNextAttachmentAttemptNs = 0;
        return true;
    }
    ++mAttachmentFailures;
    if (mAttachmentFailures <= kAttachmentRetryNs.size()) {
        mNextAttachmentAttemptNs = systemTime(SYSTEM_TIME_MONOTONIC)
                + kAttachmentRetryNs[mAttachmentFailures - 1];
    }
    ALOGW("%s: DAP io %d offloaded %d sync failed %d, attempt %u%s",
            __func__, io, offloaded, status, mAttachmentFailures,
            mAttachmentFailures > kAttachmentRetryNs.size() ? " (waiting for a new event)" : "");
    return false;
}

status_t DolbyDapController::skipHardBypass() {
    if (!isSupported()) {
        return NO_INIT;
    }
    const auto snapshot = currentEffect();
    if (snapshot.chain == nullptr) {
        return NO_INIT;
    }
    audio_utils::lock_guard chainLock(snapshot.chain->mutex());
    if (!isCurrentEffect_l(snapshot)) {
        return NO_INIT;
    }
    if (!snapshot.effect->isEnabled() || !snapshot.effect->isOffloadable()) return NO_INIT;
    return setParam(snapshot.effect, kParamSkipHardBypass, 1);
}

status_t DolbyDapController::setParam(
        const sp<IAfEffectModule>& effect,
        int32_t paramId,
        int32_t value) const {
    return setParameters(effect, paramId, {value});
}

status_t DolbyDapController::setParameters(
        const sp<IAfEffectModule>& effect,
        int32_t paramId,
        const std::vector<int32_t>& values) const {
    if (effect == nullptr) {
        return NO_INIT;
    }
    if (values.empty()) {
        return BAD_VALUE;
    }

    const size_t parameterSize = sizeof(int32_t);
    const size_t valueSize = sizeof(int32_t) * values.size();
    std::vector<uint8_t> request(
            sizeof(effect_param_t) + parameterSize + valueSize);
    auto* parameter = reinterpret_cast<effect_param_t*>(request.data());
    parameter->psize = parameterSize;
    parameter->vsize = valueSize;

    std::memcpy(parameter->data, &paramId, parameterSize);
    std::memcpy(
            parameter->data + parameterSize, values.data(), valueSize);

    std::vector<uint8_t> response;
    status_t status = effect->command(
            EFFECT_CMD_SET_PARAM, request, sizeof(status_t), &response);
    if (status != NO_ERROR) {
        return status;
    }
    if (response.size() != sizeof(status_t)) {
        return BAD_VALUE;
    }

    std::memcpy(&status, response.data(), sizeof(status));
    return status;
}

}  // namespace android
