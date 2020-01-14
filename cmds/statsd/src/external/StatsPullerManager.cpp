/*
 * Copyright (C) 2017 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define DEBUG false
#include "Log.h"

#include "StatsPullerManager.h"

#include <android/os/IPullAtomCallback.h>
#include <android/os/IStatsCompanionService.h>
#include <android/os/IStatsPullerCallback.h>
#include <cutils/log.h>
#include <math.h>
#include <stdint.h>

#include <algorithm>
#include <iostream>

#include "../StatsService.h"
#include "../logd/LogEvent.h"
#include "../stats_log_util.h"
#include "../statscompanion_util.h"
#include "CarStatsPuller.h"
#include "GpuStatsPuller.h"
#include "PowerStatsPuller.h"
#include "ResourceHealthManagerPuller.h"
#include "StatsCallbackPuller.h"
#include "StatsCallbackPullerDeprecated.h"
#include "StatsCompanionServicePuller.h"
#include "SubsystemSleepStatePuller.h"
#include "TrainInfoPuller.h"
#include "statslog.h"

using std::make_shared;
using std::map;
using std::shared_ptr;
using std::string;
using std::vector;
using std::list;

namespace android {
namespace os {
namespace statsd {

// Values smaller than this may require to update the alarm.
const int64_t NO_ALARM_UPDATE = INT64_MAX;

std::map<PullerKey, PullAtomInfo> StatsPullerManager::kAllPullAtomInfo = {

        // kernel_wakelock
        {{.atomTag = android::util::KERNEL_WAKELOCK},
         {.puller = new StatsCompanionServicePuller(android::util::KERNEL_WAKELOCK)}},

        // subsystem_sleep_state
        {{.atomTag = android::util::SUBSYSTEM_SLEEP_STATE},
         {.puller = new SubsystemSleepStatePuller()}},

        // on_device_power_measurement
        {{.atomTag = android::util::ON_DEVICE_POWER_MEASUREMENT},
         {.puller = new PowerStatsPuller()}},

        // cpu_time_per_uid_freq
        // the throttling is 3sec, handled in
        // frameworks/base/core/java/com/android/internal/os/KernelCpuProcReader
        {{.atomTag = android::util::CPU_TIME_PER_UID_FREQ},
         {.additiveFields = {4},
          .puller = new StatsCompanionServicePuller(android::util::CPU_TIME_PER_UID_FREQ)}},

        // cpu_active_time
        // the throttling is 3sec, handled in
        // frameworks/base/core/java/com/android/internal/os/KernelCpuProcReader
        {{.atomTag = android::util::CPU_ACTIVE_TIME},
         {.additiveFields = {2},
          .puller = new StatsCompanionServicePuller(android::util::CPU_ACTIVE_TIME)}},

        // cpu_cluster_time
        // the throttling is 3sec, handled in
        // frameworks/base/core/java/com/android/internal/os/KernelCpuProcReader
        {{.atomTag = android::util::CPU_CLUSTER_TIME},
         {.additiveFields = {3},
          .puller = new StatsCompanionServicePuller(android::util::CPU_CLUSTER_TIME)}},

        // wifi_activity_energy_info
        {{.atomTag = android::util::WIFI_ACTIVITY_INFO},
         {.puller = new StatsCompanionServicePuller(android::util::WIFI_ACTIVITY_INFO)}},

        // modem_activity_info
        {{.atomTag = android::util::MODEM_ACTIVITY_INFO},
         {.puller = new StatsCompanionServicePuller(android::util::MODEM_ACTIVITY_INFO)}},

        // system_elapsed_realtime
        {{.atomTag = android::util::SYSTEM_ELAPSED_REALTIME},
         {.coolDownNs = NS_PER_SEC,
          .puller = new StatsCompanionServicePuller(android::util::SYSTEM_ELAPSED_REALTIME),
          .pullTimeoutNs = NS_PER_SEC / 2,
         }},

        // system_uptime
        {{.atomTag = android::util::SYSTEM_UPTIME},
         {.puller = new StatsCompanionServicePuller(android::util::SYSTEM_UPTIME)}},

        // remaining_battery_capacity
        {{.atomTag = android::util::REMAINING_BATTERY_CAPACITY},
         {.puller = new ResourceHealthManagerPuller(android::util::REMAINING_BATTERY_CAPACITY)}},

        // full_battery_capacity
        {{.atomTag = android::util::FULL_BATTERY_CAPACITY},
         {.puller = new ResourceHealthManagerPuller(android::util::FULL_BATTERY_CAPACITY)}},

        // battery_voltage
        {{.atomTag = android::util::BATTERY_VOLTAGE},
         {.puller = new ResourceHealthManagerPuller(android::util::BATTERY_VOLTAGE)}},

        // battery_level
        {{.atomTag = android::util::BATTERY_LEVEL},
         {.puller = new ResourceHealthManagerPuller(android::util::BATTERY_LEVEL)}},

        // battery_cycle_count
        {{.atomTag = android::util::BATTERY_CYCLE_COUNT},
         {.puller = new ResourceHealthManagerPuller(android::util::BATTERY_CYCLE_COUNT)}},

        // process_memory_state
        {{.atomTag = android::util::PROCESS_MEMORY_STATE},
         {.additiveFields = {4, 5, 6, 7, 8},
          .puller = new StatsCompanionServicePuller(android::util::PROCESS_MEMORY_STATE)}},

        // process_memory_high_water_mark
        {{.atomTag = android::util::PROCESS_MEMORY_HIGH_WATER_MARK},
         {.puller =
                  new StatsCompanionServicePuller(android::util::PROCESS_MEMORY_HIGH_WATER_MARK)}},

        // process_memory_snapshot
        {{.atomTag = android::util::PROCESS_MEMORY_SNAPSHOT},
         {.puller = new StatsCompanionServicePuller(android::util::PROCESS_MEMORY_SNAPSHOT)}},

        // system_ion_heap_size
        {{.atomTag = android::util::SYSTEM_ION_HEAP_SIZE},
         {.puller = new StatsCompanionServicePuller(android::util::SYSTEM_ION_HEAP_SIZE)}},

        // process_system_ion_heap_size
        {{.atomTag = android::util::PROCESS_SYSTEM_ION_HEAP_SIZE},
         {.puller = new StatsCompanionServicePuller(android::util::PROCESS_SYSTEM_ION_HEAP_SIZE)}},

        // temperature
        {{.atomTag = android::util::TEMPERATURE},
         {.puller = new StatsCompanionServicePuller(android::util::TEMPERATURE)}},

        // cooling_device
        {{.atomTag = android::util::COOLING_DEVICE},
         {.puller = new StatsCompanionServicePuller(android::util::COOLING_DEVICE)}},

        // binder_calls
        {{.atomTag = android::util::BINDER_CALLS},
         {.additiveFields = {4, 5, 6, 8, 12},
          .puller = new StatsCompanionServicePuller(android::util::BINDER_CALLS)}},

        // binder_calls_exceptions
        {{.atomTag = android::util::BINDER_CALLS_EXCEPTIONS},
         {.puller = new StatsCompanionServicePuller(android::util::BINDER_CALLS_EXCEPTIONS)}},

        // looper_stats
        {{.atomTag = android::util::LOOPER_STATS},
         {.additiveFields = {5, 6, 7, 8, 9},
          .puller = new StatsCompanionServicePuller(android::util::LOOPER_STATS)}},

        // Disk Stats
        {{.atomTag = android::util::DISK_STATS},
         {.puller = new StatsCompanionServicePuller(android::util::DISK_STATS)}},

        // Directory usage
        {{.atomTag = android::util::DIRECTORY_USAGE},
         {.puller = new StatsCompanionServicePuller(android::util::DIRECTORY_USAGE)}},

        // Size of app's code, data, and cache
        {{.atomTag = android::util::APP_SIZE},
         {.puller = new StatsCompanionServicePuller(android::util::APP_SIZE)}},

        // Size of specific categories of files. Eg. Music.
        {{.atomTag = android::util::CATEGORY_SIZE},
         {.puller = new StatsCompanionServicePuller(android::util::CATEGORY_SIZE)}},

        // Number of fingerprints enrolled for each user.
        {{.atomTag = android::util::NUM_FINGERPRINTS_ENROLLED},
         {.puller = new StatsCompanionServicePuller(android::util::NUM_FINGERPRINTS_ENROLLED)}},

        // Number of faces enrolled for each user.
        {{.atomTag = android::util::NUM_FACES_ENROLLED},
         {.puller = new StatsCompanionServicePuller(android::util::NUM_FACES_ENROLLED)}},

        // ProcStats.
        {{.atomTag = android::util::PROC_STATS},
         {.puller = new StatsCompanionServicePuller(android::util::PROC_STATS)}},

        // ProcStatsPkgProc.
        {{.atomTag = android::util::PROC_STATS_PKG_PROC},
         {.puller = new StatsCompanionServicePuller(android::util::PROC_STATS_PKG_PROC)}},

        // Disk I/O stats per uid.
        {{.atomTag = android::util::DISK_IO},
         {.additiveFields = {2, 3, 4, 5, 6, 7, 8, 9, 10, 11},
          .coolDownNs = 3 * NS_PER_SEC,
          .puller = new StatsCompanionServicePuller(android::util::DISK_IO)}},

        // PowerProfile constants for power model calculations.
        {{.atomTag = android::util::POWER_PROFILE},
         {.puller = new StatsCompanionServicePuller(android::util::POWER_PROFILE)}},

        // Process cpu stats. Min cool-down is 5 sec, inline with what AcitivityManagerService uses.
        {{.atomTag = android::util::PROCESS_CPU_TIME},
         {.coolDownNs = 5 * NS_PER_SEC /* min cool-down in seconds*/,
          .puller = new StatsCompanionServicePuller(android::util::PROCESS_CPU_TIME)}},
        {{.atomTag = android::util::CPU_TIME_PER_THREAD_FREQ},
         {.additiveFields = {7, 9, 11, 13, 15, 17, 19, 21},
          .puller = new StatsCompanionServicePuller(android::util::CPU_TIME_PER_THREAD_FREQ)}},

        // DeviceCalculatedPowerUse.
        {{.atomTag = android::util::DEVICE_CALCULATED_POWER_USE},
         {.puller = new StatsCompanionServicePuller(android::util::DEVICE_CALCULATED_POWER_USE)}},

        // DeviceCalculatedPowerBlameUid.
        {{.atomTag = android::util::DEVICE_CALCULATED_POWER_BLAME_UID},
         {.puller = new StatsCompanionServicePuller(
                  android::util::DEVICE_CALCULATED_POWER_BLAME_UID)}},

        // DeviceCalculatedPowerBlameOther.
        {{.atomTag = android::util::DEVICE_CALCULATED_POWER_BLAME_OTHER},
         {.puller = new StatsCompanionServicePuller(
                  android::util::DEVICE_CALCULATED_POWER_BLAME_OTHER)}},

        // DebugElapsedClock.
        {{.atomTag = android::util::DEBUG_ELAPSED_CLOCK},
         {.additiveFields = {1, 2, 3, 4},
          .puller = new StatsCompanionServicePuller(android::util::DEBUG_ELAPSED_CLOCK)}},

        // DebugFailingElapsedClock.
        {{.atomTag = android::util::DEBUG_FAILING_ELAPSED_CLOCK},
         {.additiveFields = {1, 2, 3, 4},
          .puller = new StatsCompanionServicePuller(android::util::DEBUG_FAILING_ELAPSED_CLOCK)}},

        // BuildInformation.
        {{.atomTag = android::util::BUILD_INFORMATION},
         {.puller = new StatsCompanionServicePuller(android::util::BUILD_INFORMATION)}},

        // RoleHolder.
        {{.atomTag = android::util::ROLE_HOLDER},
         {.puller = new StatsCompanionServicePuller(android::util::ROLE_HOLDER)}},

        // PermissionState.
        {{.atomTag = android::util::DANGEROUS_PERMISSION_STATE},
         {.puller = new StatsCompanionServicePuller(android::util::DANGEROUS_PERMISSION_STATE)}},

        // TrainInfo.
        {{.atomTag = android::util::TRAIN_INFO}, {.puller = new TrainInfoPuller()}},

        // TimeZoneDataInfo.
        {{.atomTag = android::util::TIME_ZONE_DATA_INFO},
         {.puller = new StatsCompanionServicePuller(android::util::TIME_ZONE_DATA_INFO)}},

        // ExternalStorageInfo
        {{.atomTag = android::util::EXTERNAL_STORAGE_INFO},
         {.puller = new StatsCompanionServicePuller(android::util::EXTERNAL_STORAGE_INFO)}},

        // GpuStatsGlobalInfo
        {{.atomTag = android::util::GPU_STATS_GLOBAL_INFO},
         {.puller = new GpuStatsPuller(android::util::GPU_STATS_GLOBAL_INFO)}},

        // GpuStatsAppInfo
        {{.atomTag = android::util::GPU_STATS_APP_INFO},
         {.puller = new GpuStatsPuller(android::util::GPU_STATS_APP_INFO)}},

        // AppsOnExternalStorageInfo
        {{.atomTag = android::util::APPS_ON_EXTERNAL_STORAGE_INFO},
         {.puller = new StatsCompanionServicePuller(android::util::APPS_ON_EXTERNAL_STORAGE_INFO)}},

        // Face Settings
        {{.atomTag = android::util::FACE_SETTINGS},
         {.puller = new StatsCompanionServicePuller(android::util::FACE_SETTINGS)}},

        // App ops
        {{.atomTag = android::util::APP_OPS},
         {.puller = new StatsCompanionServicePuller(android::util::APP_OPS)}},

        // VmsClientStats
        {{.atomTag = android::util::VMS_CLIENT_STATS},
         {.additiveFields = {5, 6, 7, 8, 9, 10},
          .puller = new CarStatsPuller(android::util::VMS_CLIENT_STATS)}},

        // NotiifcationRemoteViews.
        {{.atomTag = android::util::NOTIFICATION_REMOTE_VIEWS},
         {.puller = new StatsCompanionServicePuller(android::util::NOTIFICATION_REMOTE_VIEWS)}},

        // PermissionStateSampled.
        {{.atomTag = android::util::DANGEROUS_PERMISSION_STATE_SAMPLED},
         {.puller =
               new StatsCompanionServicePuller(android::util::DANGEROUS_PERMISSION_STATE_SAMPLED)}},
};

StatsPullerManager::StatsPullerManager() : mNextPullTimeNs(NO_ALARM_UPDATE) {
}

bool StatsPullerManager::Pull(int tagId, vector<shared_ptr<LogEvent>>* data) {
    AutoMutex _l(mLock);
    return PullLocked(tagId, data);
}

bool StatsPullerManager::PullLocked(int tagId, vector<shared_ptr<LogEvent>>* data) {
    VLOG("Initiating pulling %d", tagId);

    if (kAllPullAtomInfo.find({.atomTag = tagId}) != kAllPullAtomInfo.end()) {
        bool ret = kAllPullAtomInfo.find({.atomTag = tagId})->second.puller->Pull(data);
        VLOG("pulled %d items", (int)data->size());
        if (!ret) {
            StatsdStats::getInstance().notePullFailed(tagId);
        }
        return ret;
    } else {
        VLOG("Unknown tagId %d", tagId);
        return false;  // Return early since we don't know what to pull.
    }
}

bool StatsPullerManager::PullerForMatcherExists(int tagId) const {
    // Vendor pulled atoms might be registered after we parse the config.
    return isVendorPulledAtom(tagId) ||
           kAllPullAtomInfo.find({.atomTag = tagId}) != kAllPullAtomInfo.end();
}

void StatsPullerManager::updateAlarmLocked() {
    if (mNextPullTimeNs == NO_ALARM_UPDATE) {
        VLOG("No need to set alarms. Skipping");
        return;
    }

    sp<IStatsCompanionService> statsCompanionServiceCopy = mStatsCompanionService;
    if (statsCompanionServiceCopy != nullptr) {
        statsCompanionServiceCopy->setPullingAlarm(mNextPullTimeNs / 1000000);
    } else {
        VLOG("StatsCompanionService not available. Alarm not set.");
    }
    return;
}

void StatsPullerManager::SetStatsCompanionService(
        sp<IStatsCompanionService> statsCompanionService) {
    AutoMutex _l(mLock);
    sp<IStatsCompanionService> tmpForLock = mStatsCompanionService;
    mStatsCompanionService = statsCompanionService;
    for (const auto& pulledAtom : kAllPullAtomInfo) {
        pulledAtom.second.puller->SetStatsCompanionService(statsCompanionService);
    }
    if (mStatsCompanionService != nullptr) {
        updateAlarmLocked();
    }
}

void StatsPullerManager::RegisterReceiver(int tagId, wp<PullDataReceiver> receiver,
                                              int64_t nextPullTimeNs, int64_t intervalNs) {
    AutoMutex _l(mLock);
    auto& receivers = mReceivers[tagId];
    for (auto it = receivers.begin(); it != receivers.end(); it++) {
        if (it->receiver == receiver) {
            VLOG("Receiver already registered of %d", (int)receivers.size());
            return;
        }
    }
    ReceiverInfo receiverInfo;
    receiverInfo.receiver = receiver;

    // Round it to the nearest minutes. This is the limit of alarm manager.
    // In practice, we should always have larger buckets.
    int64_t roundedIntervalNs = intervalNs / NS_PER_SEC / 60 * NS_PER_SEC * 60;
    // Scheduled pulling should be at least 1 min apart.
    // This can be lower in cts tests, in which case we round it to 1 min.
    if (roundedIntervalNs < 60 * (int64_t)NS_PER_SEC) {
        roundedIntervalNs = 60 * (int64_t)NS_PER_SEC;
    }

    receiverInfo.intervalNs = roundedIntervalNs;
    receiverInfo.nextPullTimeNs = nextPullTimeNs;
    receivers.push_back(receiverInfo);

    // There is only one alarm for all pulled events. So only set it to the smallest denom.
    if (nextPullTimeNs < mNextPullTimeNs) {
        VLOG("Updating next pull time %lld", (long long)mNextPullTimeNs);
        mNextPullTimeNs = nextPullTimeNs;
        updateAlarmLocked();
    }
    VLOG("Puller for tagId %d registered of %d", tagId, (int)receivers.size());
}

void StatsPullerManager::UnRegisterReceiver(int tagId, wp<PullDataReceiver> receiver) {
    AutoMutex _l(mLock);
    if (mReceivers.find(tagId) == mReceivers.end()) {
        VLOG("Unknown pull code or no receivers: %d", tagId);
        return;
    }
    auto& receivers = mReceivers.find(tagId)->second;
    for (auto it = receivers.begin(); it != receivers.end(); it++) {
        if (receiver == it->receiver) {
            receivers.erase(it);
            VLOG("Puller for tagId %d unregistered of %d", tagId, (int)receivers.size());
            return;
        }
    }
}

void StatsPullerManager::OnAlarmFired(int64_t elapsedTimeNs) {
    AutoMutex _l(mLock);
    int64_t wallClockNs = getWallClockNs();

    int64_t minNextPullTimeNs = NO_ALARM_UPDATE;

    vector<pair<int, vector<ReceiverInfo*>>> needToPull =
            vector<pair<int, vector<ReceiverInfo*>>>();
    for (auto& pair : mReceivers) {
        vector<ReceiverInfo*> receivers = vector<ReceiverInfo*>();
        if (pair.second.size() != 0) {
            for (ReceiverInfo& receiverInfo : pair.second) {
                if (receiverInfo.nextPullTimeNs <= elapsedTimeNs) {
                    receivers.push_back(&receiverInfo);
                } else {
                    if (receiverInfo.nextPullTimeNs < minNextPullTimeNs) {
                        minNextPullTimeNs = receiverInfo.nextPullTimeNs;
                    }
                }
            }
            if (receivers.size() > 0) {
                needToPull.push_back(make_pair(pair.first, receivers));
            }
        }
    }

    for (const auto& pullInfo : needToPull) {
        vector<shared_ptr<LogEvent>> data;
        bool pullSuccess = PullLocked(pullInfo.first, &data);
        if (pullSuccess) {
            StatsdStats::getInstance().notePullDelay(
                    pullInfo.first, getElapsedRealtimeNs() - elapsedTimeNs);
        } else {
            VLOG("pull failed at %lld, will try again later", (long long)elapsedTimeNs);
        }

        // Convention is to mark pull atom timestamp at request time.
        // If we pull at t0, puller starts at t1, finishes at t2, and send back
        // at t3, we mark t0 as its timestamp, which should correspond to its
        // triggering event, such as condition change at t0.
        // Here the triggering event is alarm fired from AlarmManager.
        // In ValueMetricProducer and GaugeMetricProducer we do same thing
        // when pull on condition change, etc.
        for (auto& event : data) {
            event->setElapsedTimestampNs(elapsedTimeNs);
            event->setLogdWallClockTimestampNs(wallClockNs);
        }

        for (const auto& receiverInfo : pullInfo.second) {
            sp<PullDataReceiver> receiverPtr = receiverInfo->receiver.promote();
            if (receiverPtr != nullptr) {
                receiverPtr->onDataPulled(data, pullSuccess, elapsedTimeNs);
                // We may have just come out of a coma, compute next pull time.
                int numBucketsAhead =
                        (elapsedTimeNs - receiverInfo->nextPullTimeNs) / receiverInfo->intervalNs;
                receiverInfo->nextPullTimeNs += (numBucketsAhead + 1) * receiverInfo->intervalNs;
                if (receiverInfo->nextPullTimeNs < minNextPullTimeNs) {
                    minNextPullTimeNs = receiverInfo->nextPullTimeNs;
                }
            } else {
                VLOG("receiver already gone.");
            }
        }
    }

    VLOG("mNextPullTimeNs: %lld updated to %lld", (long long)mNextPullTimeNs,
         (long long)minNextPullTimeNs);
    mNextPullTimeNs = minNextPullTimeNs;
    updateAlarmLocked();
}

int StatsPullerManager::ForceClearPullerCache() {
    int totalCleared = 0;
    for (const auto& pulledAtom : kAllPullAtomInfo) {
        totalCleared += pulledAtom.second.puller->ForceClearCache();
    }
    return totalCleared;
}

int StatsPullerManager::ClearPullerCacheIfNecessary(int64_t timestampNs) {
    int totalCleared = 0;
    for (const auto& pulledAtom : kAllPullAtomInfo) {
        totalCleared += pulledAtom.second.puller->ClearCacheIfNecessary(timestampNs);
    }
    return totalCleared;
}

// Deprecated, remove after puller API is complete.
void StatsPullerManager::RegisterPullerCallback(int32_t atomTag,
        const sp<IStatsPullerCallback>& callback) {
    AutoMutex _l(mLock);
    // Platform pullers cannot be changed.
    if (!isVendorPulledAtom(atomTag)) {
        VLOG("RegisterPullerCallback: atom tag %d is not vendor pulled", atomTag);
        return;
    }
    VLOG("RegisterPullerCallback: adding puller for tag %d", atomTag);
    StatsdStats::getInstance().notePullerCallbackRegistrationChanged(atomTag, /*registered=*/true);
    kAllPullAtomInfo[{.atomTag = atomTag}] = {
            .puller = new StatsCallbackPullerDeprecated(atomTag, callback)};
}

void StatsPullerManager::RegisterPullAtomCallback(const int uid, const int32_t atomTag,
                                                  const int64_t coolDownNs, const int64_t timeoutNs,
                                                  const vector<int32_t>& additiveFields,
                                                  const sp<IPullAtomCallback>& callback) {
    AutoMutex _l(mLock);
    VLOG("RegisterPullerCallback: adding puller for tag %d", atomTag);
    // TODO: linkToDeath with the callback so that we can remove it and delete the puller.
    StatsdStats::getInstance().notePullerCallbackRegistrationChanged(atomTag, /*registered=*/true);
    kAllPullAtomInfo[{.atomTag = atomTag}] = {
            .additiveFields = additiveFields,
            .coolDownNs = coolDownNs,
            .puller = new StatsCallbackPuller(atomTag, callback, timeoutNs),
            .pullTimeoutNs = timeoutNs,
    };
}

void StatsPullerManager::UnregisterPullerCallback(int32_t atomTag) {
    AutoMutex _l(mLock);
    // Platform pullers cannot be changed.
    if (!isVendorPulledAtom(atomTag)) {
        return;
    }
    StatsdStats::getInstance().notePullerCallbackRegistrationChanged(atomTag, /*registered=*/false);
    kAllPullAtomInfo.erase({.atomTag = atomTag});
}

void StatsPullerManager::UnregisterPullAtomCallback(const int uid, const int32_t atomTag) {
    AutoMutex _l(mLock);
    StatsdStats::getInstance().notePullerCallbackRegistrationChanged(atomTag, /*registered=*/false);
    kAllPullAtomInfo.erase({.atomTag = atomTag});
}

}  // namespace statsd
}  // namespace os
}  // namespace android
