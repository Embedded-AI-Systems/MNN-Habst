//
//  CPUBackend.cpp
//  MNN
//
//  Created by MNN on 2018/07/06.
//  Copyright © 2018, Alibaba Group Holding Limited
//

#include "backend/cpu/CPUBackend.hpp"
#include <cmath>
#include <mutex>
#include "CPUResizeCache.hpp"
#include "core/BufferAllocator.hpp"
#include "CPUTensorConvert.hpp"
#include "compute/CommonOptFunction.h"
#include "core/TensorUtils.hpp"
#include "ThreadPool.hpp"
#include "core/Concurrency.h"
#include "CPUCast.hpp"
#include "core/OpCommonUtils.hpp"
#include "core/WrapExecution.hpp"
#include "core/MNNFileUtils.h"
#ifdef _OPENMP
#include <omp.h>
#endif // _OPENMP
#include "backend/cpu/CPURuntime.hpp"
#include "core/Macro.h"
#ifdef MNN_USE_ARMV82
#include "backend/arm82/Arm82Backend.hpp"
#endif
#define MAX_THREAD_NUMBER 32
#define LARGE_MEMORY 1024 * 1024 * 500
#ifdef MNN_SUPPORT_BF16
#include "bf16/BF16Functions.hpp"
#endif

#ifdef MNN_USE_SSE
#include "x86_x64/AVX2Backend.hpp"
#endif

#define MNN_CPU_MAX_BUFFER_INDEX 2
#define MNN_CPU_CHECK_NAN 1
#define MNN_CPU_USE_DEFAULT_BACKEND 4
namespace MNN {
void registerCPUOps();
ErrorCode CastWrapExecution::onExecute(const std::vector<Tensor*>& inputs, const std::vector<Tensor*>& outputs) {
    auto convertType = mRunType == DataType_DT_INT8 ? CPUCastCreator::FlOAT_TO_INT8 : CPUCastCreator::INT8_TO_FlOAT;
    auto cpuBackend = ((CPUBackend*)backend());
    CPUCastCreator::cast(inputs[0], outputs[0], cpuBackend, convertType);
    return NO_ERROR;
}

int getMajorCPUNumber(const std::vector<CPUGroup>& groups) {
    int sum = 0;
    for (const auto& g: groups) {
        if (g.cpuType != CPUGroup::Efficient) { sum+=g.ids.size(); }
    }
    return sum;
}
void CPUBackend::computeDivideSizes(int size, int* dst, float avgDiv) const {
    if (mGroupWithComputeRate.size() <= 1 || (avgDiv > 0 && avgDiv < mComputeI)) {
        // Avg divide
        int length = UP_DIV(size, mThreadNumber);
        int cur = length;
        for (int i=0; i<mThreadNumber; ++i) {
            dst[i] = cur;
            cur = cur + length;
            cur = ALIMIN(cur, size);
        }
        return;
    }

    int cur = 0;
    int curPos = 0;
    for (auto& group : mGroupWithComputeRate) {
        int currentGroupTotal = (int)(ceilf((float)size*group.first));
        int length = UP_DIV(currentGroupTotal, group.second);
        for (int i=0; i<group.second; ++i) {
            cur = cur + length;
            cur = ALIMIN(cur, size);
            dst[curPos+i] = cur;
        }
        curPos += group.second;
    }
}

void CPURuntime::_bindCPUCore() {
    if (mPower == BackendConfig::Power_Normal) {
        return;
    }
    auto tid = MNNGetCurrentPid();
    if (tid == mCurrentTID) {
        return;
    }
    mCurrentTID = tid;
    // Bind CPU Core
    auto cpuInfo = MNNGetCPUInfo();
    if (cpuInfo->groups.size() == 0) {
        return;
    }
    std::vector<std::pair<const int*, int>> lockCPUIndexes(mThreadNumber);
    switch (mPower) {
        case BackendConfig::Power_Low:
            for (int v=0; v<mThreadNumber; ++v) {
                lockCPUIndexes[v] = std::make_pair(cpuInfo->groups[0].ids.data(), cpuInfo->groups[0].ids.size());
            }
            break;
        case BackendConfig::Power_High:
        {
            int selectCPUSize = 0;
            int groupIndex = cpuInfo->groups.size() - 1;
            while (selectCPUSize < mThreadNumber && groupIndex >= 0) {
                auto& group = cpuInfo->groups[groupIndex];
                int size = ALIMIN(group.ids.size(), mThreadNumber - selectCPUSize);
                for (int v=0; v<size; ++v) {
                    lockCPUIndexes[v + selectCPUSize] = std::make_pair(group.ids.data(), group.ids.size());
                }
                groupIndex--;
                selectCPUSize += group.ids.size();
            }
        }
            break;
        // MemoryBoundTune Binding Begin 
        case BackendConfig::Power_MemoryBoundTune1:
        {
            mTuneLws.executed = true; // executed.
            // get the mTuneLws.mMemoryBoundTune.back() as the current tune plan.
            auto coreMap = mTuneLws.mMemoryBoundTune.back().second;
            for (int v=0; v<mThreadNumber; ++v) {
                lockCPUIndexes[v] = std::make_pair(cpuInfo->groups[coreMap[v]].ids.data(), cpuInfo->groups[coreMap[v]].ids.size());
            }
            break;
        }
            break;
        case BackendConfig::Power_MemoryBoundTune2:
        {
            mTuneLws.executed = true; // executed.
            // get the mTuneLws.mMemoryBoundTune.back() as the current tune plan.
            auto coreMap = mTuneLws.mMemoryBoundTune[mTuneLws.currentTunePlan].second;
            for (int v=0; v<mThreadNumber; ++v) {
                lockCPUIndexes[v] = std::make_pair(cpuInfo->groups[coreMap[v]].ids.data(), cpuInfo->groups[coreMap[v]].ids.size());
            }
            break;
        }
            break;
        case BackendConfig::Power_MemoryBound:
        {
            // get mTuneLws.mMemoryBoundTuned as the tuned plan.
            auto coreMap = (mTuneLws.mMemoryBoundTuned.get()==nullptr) \
                                ? (getTune1Sched(MEMORYBOUNDTUNE_START)) \
                                : (mTuneLws.mMemoryBoundTuned->second);
            for (int v=0; v<mThreadNumber; ++v) {
                lockCPUIndexes[v] = std::make_pair(cpuInfo->groups[coreMap[v]].ids.data(), cpuInfo->groups[coreMap[v]].ids.size());
            }
        }
            break;
        // MemoryBoundTune Binding End
        case BackendConfig::Power_SelectCore:
        {
            // get mTuneLws.mMemoryBoundTuned as the tuned plan.
            auto coreMap = (hint().cpuCoreConfig.empty()) \
                                ? (getTune1Sched(getMajorCPUNumber(MNNGetCPUInfo()->groups))) \
                                : (hint().cpuCoreConfig);
            for (int v=0; v<mThreadNumber; ++v) {
                lockCPUIndexes[v] = std::make_pair(cpuInfo->groups[coreMap[v]].ids.data(), cpuInfo->groups[coreMap[v]].ids.size());
            }
        }
            break;
        default:
            break;
    }
        // Set CPU Affinity
#ifdef _OPENMP
    auto threadsNumber = mThreadNumber;
    std::vector<int> result(threadsNumber, 0);
#pragma omp parallel for
    for (int i = 0; i < threadsNumber; ++i) {
        result[i] = MNNSetSchedAffinity(lockCPUIndexes[i].first, lockCPUIndexes[i].second);
    }
#endif
#ifdef MNN_USE_THREAD_POOL
    ThreadPool::active(mThreadNumber);
    ThreadPool::enqueue(std::make_pair([&](int i) {
        MNNSetSchedAffinity(lockCPUIndexes[i].first, lockCPUIndexes[i].second);
        return 0;
    }, mThreadNumber), mTaskIndex, mThreadNumber);
    ThreadPool::deactive(mThreadNumber);
#endif
}

/* 
 * tune_list: list of thread affinity (i.e., group id for each thread). 
 */
std::vector<int> CPURuntime::getTune1Sched(int numThread) const {
    std::vector<int> tune_list;
    auto cpuInfo = MNNGetCPUInfo();
    auto& groups = cpuInfo->groups;
    // Assign in a greedy order of CPU performance.
    for (int i=groups.size()-1; i>=0; --i) {
        if (tune_list.size()==numThread) { break; }
        for (int j=0; (j<groups[i].ids.size()) && (tune_list.size()<numThread); ++j) { 
            tune_list.push_back(i); // push back group id
        }
    }
    // Debug print
    MNN_PRINT("[CPU Debug] Tune1: \n");
    for (auto& id: tune_list) { MNN_PRINT("%d ", id); }
    MNN_PRINT("\n");
    MNN_PRINT("extimated power: %.5f\n", estimatePower(tune_list));
    // Debug end
    return tune_list;
}

float CPURuntime::estimatePower(const std::vector<int>& config) const {
    // unit: mW
    const float static_power = 1000.;
    const float scale_factor = 200.;
    float sum = static_power;
#ifdef MNN_USE_THREAD_POOL
    auto cpuInfo = MNNGetCPUInfo();
    auto& groups = cpuInfo->groups;
    std::vector<int> groupSelect(groups.size(), 0);
    std::vector<int> groupSize(groups.size(), 0);
    auto maxFreq = groups.back().maxFreq;
    auto selectedMaxFreq = groups.front().maxFreq;
    for (auto id : config) {
        groupSelect[id]++; 
        selectedMaxFreq=std::max(selectedMaxFreq, groups[id].maxFreq);
    }
    for (int i=0; i<groups.size(); ++i) { groupSize[i]=groups[i].ids.size(); }
    for (int i=0; i<groups.size(); ++i) {
        if (groupSelect[i]==0) { continue; }
        float groupFreq = groups[i].maxFreq * ((float)selectedMaxFreq/maxFreq);
        groupFreq /= 1000000; // unit: GHz
        float idleFactor = groupSelect[i] \
            + ((float)(hint().coreIdleFactor)/100)*(groupSize[i]-groupSelect[i]);
        float powerFactor = 1.;
        if (groups[i].cpuType == CPUGroup::Performance) { powerFactor=(float)hint().performanceCorePowerScale/100; }
        if (groups[i].cpuType == CPUGroup::Efficient) { powerFactor=(float)hint().efficientCorePowerScale/100; }
        sum += scale_factor * powerFactor * idleFactor * (groupFreq*groupFreq);
    }
#endif
    return sum;
}

bool CPURuntime::compareConfigPower(const std::vector<int>& config1, const std::vector<int>& config2) const {
    // return if P(config1) <= P(config2)
    return (estimatePower(config1) <= estimatePower(config2));
}

void CPURuntime::reduceTune2Core(const std::vector<int>& base, std::vector<std::vector<int>>& branch) {
    // tier1 only
    if (base.size() > MEMORYBOUNDTUNE_START) {
        branch.push_back(base);
        branch.back().pop_back(); // remove the last core
    }
}

void CPURuntime::changeTune2Core(const std::vector<int>& base, std::vector<std::vector<int>>& branch) {
    auto cpuInfo = MNNGetCPUInfo();
    auto& groups = cpuInfo->groups;
    std::vector<int> groupSelect(groups.size(), 0);
    for (auto id : base) { groupSelect[id]++; }
    std::vector<int> new_config;
    for (int i=base.size()-1; i>=0; --i) {
        for (int j=base[i]-1; j>=0; --j) {
            // find a smaller group that has idle core
            if (groups[j].cpuType==CPUGroup::Efficient) { continue; }
            if (groupSelect[j]>0 && groupSelect[j]<groups[j].ids.size()) {
                // not opening a new group
                new_config=base;
                new_config[i]=j;
                break;
            }
        }
        if (!new_config.empty()) { break; }
    }
    // tier1/tier2 both ok.
    if (!new_config.empty()) { branch.insert(branch.begin(), new_config); }
}

void CPURuntime::changeTune2Group(const std::vector<int>& base, std::vector<std::vector<int>>& branch) {
    auto cpuInfo = MNNGetCPUInfo();
    auto& groups = cpuInfo->groups;
    std::vector<int> groupSelect(groups.size(), 0);
    for (auto id : base) { groupSelect[id]++; }
    std::vector<int> new_config;
    for (int i=base.size()-1; i>=0; --i) {
        for (int j=base[i]-1; j>=0; --j) {
            // find an idle smaller group
            if (groups[j].cpuType==CPUGroup::Efficient) { continue; }
            if (groupSelect[j]==0 \
                    && groupSelect[base[i]]<=groups[j].ids.size() \
                    && groups[j].ids.size()<=groups[base[i]].ids.size()+1) {
                // closing the original group, opening up the new group, and accomodate the new group.
                new_config=base;
                for (auto& id:new_config) { if(id==base[i]) { id=j; } }
                if (compareConfigPower(new_config, base)) { break; } // found
                else { new_config.clear(); } // not valid! new_config is evaluated to consume more power!
            }
        }
        if (!new_config.empty()) { break; }
    }
    // insert new_config and maintain power order.
    if (!new_config.empty()) {
        if (branch.empty()) {
            // tier 1 directly push back
            branch.push_back(new_config);
        } else {
            // tier 2, compare with the front.
            if (compareConfigPower(new_config, branch.front())) {
                branch.insert(branch.begin(), new_config);
            } else {
                branch.insert(branch.begin()+1, new_config);
            }
        }
    }
}

std::vector<std::vector<int>> CPURuntime::mergeSortTune2Config(const std::vector<std::vector<int>>& branch1,
                                                               const std::vector<std::vector<int>>& branch2) const {
    std::vector<std::vector<int>> result;
    auto it1=branch1.begin();
    auto it2=branch2.begin();
    while (it1!=branch1.end() && it2!=branch2.end()) {
        if (compareConfigPower(*it1, *it2)) { result.push_back(*(it1++)); } 
        else { result.push_back(*(it2++)); }
    }
    // insert the possible remaining
    result.insert(result.end(), it1, branch1.end());
    result.insert(result.end(), it2, branch2.end());
    return result;
}

void CPURuntime::getTune2Sched(std::vector<int> tune1_list) {
    // the sched is directly stored in mTuneLws.mMemoryBoundTune
    // tier 1 reduce 2/1 core 
    std::vector<std::vector<int>> branch0; // remove 2
    std::vector<std::vector<int>> branch1; // remove 1
    reduceTune2Core(tune1_list, branch1); // remove 1
    if (!branch1.empty()) {
        reduceTune2Core(branch1.back(), branch0); // remove 1 more
        if (!branch0.empty()) {
            changeTune2Core(branch0.back(), branch0);
            changeTune2Group(branch0.back(), branch0);
        }
    }
    if (!branch1.empty()) { 
        changeTune2Core(branch1.back(), branch1);
        changeTune2Group(branch1.back(), branch1);
    }
    // tier 1 big -> small (no new group)
    std::vector<std::vector<int>> branch2;
    changeTune2Core(tune1_list, branch2);
    if (!branch2.empty()) {
        changeTune2Core(branch2.back(), branch2);
        changeTune2Group(branch2.back(), branch2);
    }
    // tier 1 big group -> small group
    std::vector<std::vector<int>> branch3;
    changeTune2Group(tune1_list, branch3);
    if (!branch3.empty()) {
        changeTune2Core(branch3.back(), branch3);
        changeTune2Group(branch3.back(), branch3);
    }
    // merge 3 branches, generate mTuneLws.mMemoryBoundTune
    std::vector<std::vector<int>> mergedTree;
    mergedTree = mergeSortTune2Config(branch0, branch1);
    mergedTree = mergeSortTune2Config(mergedTree, branch2);
    mergedTree = mergeSortTune2Config(mergedTree, branch3);
    mTuneLws.mMemoryBoundTune.clear();
    for (auto& config: mergedTree) {
        mTuneLws.mMemoryBoundTune.push_back(std::make_pair(config.size(), config));
    }
    // push back the root.
    mTuneLws.mMemoryBoundTune.push_back(std::make_pair(tune1_list.size(), tune1_list));
} 

void CPURuntime::_resetThreadPool() {
    if (mThreadNumber <= 0) { mThreadNumber=getMajorCPUNumber(MNNGetCPUInfo()->groups); }
    mThreadNumber = std::max(1, mThreadNumber);
    mThreadNumber = std::min(mThreadNumber, MAX_THREAD_NUMBER);
#ifdef MNN_USE_THREAD_POOL
    ThreadPool::releaseWorkIndex(mTaskIndex);
    auto cpuInfo = MNNGetCPUInfo();
    int systemThreadNumber = (int)cpuInfo->cpuNumber;
    // MemoryBoundTune1
    if (systemThreadNumber > 1) {
        if (mPower == BackendConfig::Power_SelectCore) {
            if (hint().cpuCoreConfig.empty()) {
                // no selection, use all major cores.
                mThreadNumber=getMajorCPUNumber(MNNGetCPUInfo()->groups);
            }
        }
        if (mPower == BackendConfig::Power_MemoryBoundTune1) {
            // 1. initialize next tuning plan
            mTuneLws.tune1 = true;
            if (mTuneLws.mMemoryBoundTune.empty()) {
                // initial assignments
                mTuneLws.mMemoryBoundTune.push_back(std::make_pair(MEMORYBOUNDTUNE_START, getTune1Sched(MEMORYBOUNDTUNE_START)));
            } else {
                int majorCPUNumber = getMajorCPUNumber(cpuInfo->groups); // Prime + Performance
                // If executed, next plan. If not executed, remain the last one. 
                if (mTuneLws.executed) {
                    if (mTuneLws.mMemoryBoundTune.back().first < majorCPUNumber) {
                        // normal case: add 1 more core greedily and tune.
                        auto& tune_case = mTuneLws.mMemoryBoundTune.back();
                        mTuneLws.mMemoryBoundTune.push_back(std::make_pair(tune_case.first+1, getTune1Sched(tune_case.first+1)));
                    } else {
                        // last case: no more major cores available, copy the last plan and tune.
                        auto& tune_case = mTuneLws.mMemoryBoundTune.back();
                        mTuneLws.mMemoryBoundTune.push_back(std::make_pair(tune_case.first, getTune1Sched(tune_case.first)));
                    }
                }
            }
            // The current tuning plan is mTuneLws.mMemoryBoundTune.back()
            mThreadNumber = mTuneLws.mMemoryBoundTune.back().first;
            mTuneLws.executed = false; // not executed.
            mHint.powerEstimate = estimatePower(mTuneLws.mMemoryBoundTune.back().second);
        }
        if (mPower == BackendConfig::Power_MemoryBoundTune2) {
            if (mTuneLws.mMemoryBoundTune.empty()) {
                MNN_ERROR("Error: Tune1 hasn't performed yet!\n");
            }

            if (mTuneLws.tune1) {
                // first time tuned, generate candidate schedules.
                mTuneLws.tune1 = false; // tune2.
                // the tune2 root is the tune1 result.
                auto& root = (mTuneLws.mMemoryBoundTune.size()==1) \
                            ? mTuneLws.mMemoryBoundTune.back().second \
                            : (mTuneLws.mMemoryBoundTune.end()-2)->second;
                getTune2Sched(root); // generate new mMemoryBoundTune2.
                mTuneLws.currentTunePlan = 0;
            } else {
                // iterate to the next config
                if (mTuneLws.executed) {
                    if (mTuneLws.currentTunePlan != mTuneLws.mMemoryBoundTune.size()-1) { 
                        mTuneLws.currentTunePlan++; 
                    } 
                }
            }

            // Debug print
            if (mTuneLws.executed) {
                MNN_PRINT("[CPU Debug] Tune2: \n");
                for (auto& id: mTuneLws.mMemoryBoundTune[mTuneLws.currentTunePlan].second) { MNN_PRINT("%d ", id); }
                MNN_PRINT("\n");
                MNN_PRINT("extimated power: %.5f\n", estimatePower(mTuneLws.mMemoryBoundTune[mTuneLws.currentTunePlan].second));
                mHint.powerEstimate = estimatePower(mTuneLws.mMemoryBoundTune[mTuneLws.currentTunePlan].second);
            }
            // Debug end

            // The current tuning plan is mTuneLws.mMemoryBoundTune[mTuneLws.currentTunePlan]
            mThreadNumber = mTuneLws.mMemoryBoundTune[mTuneLws.currentTunePlan].first;
            mTuneLws.executed = false; // not executed.
        }
        if (mPower == BackendConfig::Power_MemoryBound) {
            if (mTuneLws.mMemoryBoundTuned.get() == nullptr) {
                if (!mTuneLws.mMemoryBoundTune.empty()) {
                    if (mTuneLws.mMemoryBoundTune.size() == 1) { mTuneLws.mMemoryBoundTuned.reset(new std::pair<int, std::vector<int>>(mTuneLws.mMemoryBoundTune.back())); }
                    else { 
                        if (mTuneLws.tune1) { mTuneLws.mMemoryBoundTuned.reset(new std::pair<int, std::vector<int>>(*(mTuneLws.mMemoryBoundTune.end()-2))); } // tune1: terminater with the previous peaked one.
                        else { 
                            if (hint().cpuCoreSearchIndex<0 || hint().cpuCoreSearchIndex>=mTuneLws.mMemoryBoundTune.size()) // tune2: terminate with the current first satisfying one without energy hint.
                                { mTuneLws.mMemoryBoundTuned.reset(new std::pair<int, std::vector<int>>(mTuneLws.mMemoryBoundTune[mTuneLws.currentTunePlan])); }
                            else { mTuneLws.mMemoryBoundTuned.reset(new std::pair<int, std::vector<int>>(mTuneLws.mMemoryBoundTune[hint().cpuCoreSearchIndex])); } // external hint selection.
                        } 
                    } // second last one. (based on the bitonic assumption.)
                    mTuneLws.mMemoryBoundTune.clear(); // clear last tuning intermediate results.
                    mHint.cpuCoreConfig = mTuneLws.mMemoryBoundTuned->second; // update the tuned results to the RuntimeHint.
                    // Debug print
                    MNN_PRINT("[CPU Debug] Memory Bound Core Plan: \n");
                    for (const auto& id: mTuneLws.mMemoryBoundTuned->second) { MNN_PRINT("%d ", id); }
                    MNN_PRINT("\n");
                    // Debug end
                }
            }
            if (mTuneLws.mMemoryBoundTuned.get() != nullptr) { mThreadNumber = mTuneLws.mMemoryBoundTuned->first; }
            else mThreadNumber = MEMORYBOUNDTUNE_START;
        }
    }
    if (mThreadNumber > 1) {
        if (systemThreadNumber == 0) {
            systemThreadNumber = mThreadNumber;
        }
        mThreadNumber = ALIMIN(ThreadPool::init(systemThreadNumber), mThreadNumber);
    }
    if (mThreadNumber > 1) {
        mTaskIndex = ThreadPool::acquireWorkIndex();
        if (-1 == mTaskIndex) {
            MNN_ERROR("The ThreadPool has been used to MNN_THREAD_POOL_MAX_TASKS, can't use thread pool\n");
            mThreadNumber = 1;
        }
    } else {
        mTaskIndex = -1;
    }
#endif
    // Reset tid to rebind cpu if necessary
    mCurrentTID = 0;
}
void CPURuntime::onReset(int numberThread, const BackendConfig* config, bool full) {
    if (config != nullptr) {
        mPower = config->power;
        if (full) {
            mPrecision = config->precision;
            mMemory = config->memory;
            mFlags = config->flags;
        }
    }
    mThreadNumber = numberThread;
    _resetThreadPool();
}

CPURuntime::CPURuntime(const Backend::Info& info) {
    auto rawAlloc = BufferAllocator::Allocator::createDefault();
    mStaticAllocator.reset(new EagerBufferAllocator(rawAlloc));
    mDynamic.resize(MNN_CPU_MAX_BUFFER_INDEX);
    for (auto& buf : mDynamic) {
        buf.root = rawAlloc;
    }
    mThreadNumber = info.numThread;
    mPower   = BackendConfig::Power_Normal;
    mMemory  = BackendConfig::Memory_Normal;
    mPrecision = BackendConfig::Precision_Normal;
    if (info.user != nullptr) {
        mPrecision = info.user->precision;
        mPower = info.user->power;
        mMemory = info.user->memory;
        mFlags = info.user->flags;
    }
    _resetThreadPool();
#ifdef LOG_VERBOSE
    MNN_PRINT("create CPURuntime:%p\n", this);
#endif
}

CPURuntime:: ~ CPURuntime() {
#ifdef MNN_USE_THREAD_POOL
    ThreadPool::releaseWorkIndex(mTaskIndex);
#endif
}
float CPURuntime::onGetMemoryInMB() {
    auto staticMemoryInMB = mStaticAllocator->totalSize() / 1024.0f / 1024.0f;
    float dynamicMemoryInMB = 0.0f;
    for (auto& buf : mDynamic) {
        dynamicMemoryInMB += buf.currentSize / 1024.0f / 1024.0f;
    }
    return staticMemoryInMB + dynamicMemoryInMB;
}
bool CPURuntime::onCheckInfo(Backend::Info& info) const {
    info.numThread = mThreadNumber;
    return true;
}
SingleBufferWithAllocator* CPURuntime::buffer(int index) const {
    if (mDynamicMmap.empty()) {
        return mDynamic.data() + index;
    }
    return mDynamicMmap.data() + index;
}

Backend* CPURuntime::onCreate(const BackendConfig* config, Backend* origin) const {
    if (hint().midMemoryPath.size() > 0) {
        if (mDynamicMmap.empty()) {
            // Only support set featuremap dir once
            mDynamicMmap.resize(2);
            auto mmapMem = BufferAllocator::Allocator::createMmap(hint().midMemoryPath.c_str(), "", "dynamic");
            for (auto& buf : mDynamicMmap) {
                buf.root = mmapMem;
            }
        }
    }
    if (hint().weightMemoryPath.size() > 0) {
        // forward_type, precision_type, memory_type, power_type
        std::string prefix = "0_0_0_0_";
        prefix[2] += mPrecision;
        prefix[4] += mMemory;
        prefix[6] += mPower;
        // prefix += hint().modelUUID + "_";
        bool autoRemove = true;
        if (hint().useCachedMmap) {
            autoRemove = false;
            std::string fileName = MNNFilePathConcat(hint().weightMemoryPath, prefix + "sync.static");
            const_cast<RuntimeHint&>(hint()).useCachedMmap += MNNFileExist(fileName.c_str());
        }
        if (nullptr == mStaticAllocatorCache.get()) {
            // Only support set weightmap dir once
            mStaticAllocatorCache = mStaticAllocator;
            auto mmapMem = BufferAllocator::Allocator::createMmap(hint().weightMemoryPath.c_str(), prefix.c_str(), "static", autoRemove);
            size_t mmapSize = static_cast<size_t>(hint().mmapFileSize) * 1024 * 1024;
            mStaticAllocator.reset(new EagerBufferAllocator(mmapMem, 32, mmapSize));
        }
    }
    auto precision = mPrecision;
    auto memory = mMemory;
    size_t flags = mFlags;
    if (nullptr != origin) {
        auto cpuBn = static_cast<CPUBackend*>(origin);
        mSharedDmaInfo = cpuBn->mDmaInfo;
    }
    if (nullptr != config) {
        precision = config->precision;
        flags = config->flags;
        memory = config->memory;
    }
#ifdef LOG_VERBOSE
    MNN_PRINT("cpu backend was created by runtime:%p\n", this);
#endif
    CPUBackend* res = nullptr;
    do {
#ifdef MNN_USE_ARMV82
        auto core = MNNGetCoreFunctions();
        if (core->supportFp16arith && precision == BackendConfig::Precision_Low) {
            res = new Arm82Backend(this, memory);
            break;
        }
#endif
#ifdef MNN_SUPPORT_BF16
        if (precision == BackendConfig::Precision_Low_BF16 && BF16Functions::get()) {
            res = new CPUBackend(this, precision, memory, MNN_FORWARD_CPU_EXTENSION, 0);
            res->mCoreFunctions = BF16Functions::get();
            break;
        }
#endif
        if (flags == MNN_CPU_USE_DEFAULT_BACKEND) {
            res = new CPUBackend(this, precision, memory, MNN_FORWARD_CPU, 0);
            break;
        }
#ifdef MNN_USE_SSE
        if (AVX2Backend::isValid()) {
            res = new AVX2Backend(this, memory, flags);
            break;
        }
#endif
        res = new CPUBackend(this, precision, memory, MNN_FORWARD_CPU, flags);
    } while (false);
    mSharedDmaInfo = nullptr;
    return res;
}

int CPURuntime::onGetRuntimeStatus(RuntimeStatus statusEnum) const {
    switch (statusEnum) {
        case STATUS_SUPPORT_FP16: {
            return MNNGetCoreFunctions()->supportFp16arith;
            break;
        }
        case STATUS_SUPPORT_DOT_PRODUCT: {
            return MNNGetCoreFunctions()->supportSDot;
            break;
        }
        default: {
            MNN_ERROR("unsupported interface");
            break;
        }
    }

    return 0;
}

void CPURuntime::onGabageCollect(int level) {
    mStaticAllocator->release(false);
    if (level >= 100) {
        for (auto& buf : mDynamic) {
            buf.release();
        }
    }
}


void CPURuntime::onConcurrencyBegin() {
#ifdef MNN_USE_THREAD_POOL
    if (mTaskIndex >= 0) {
        ThreadPool::active(mThreadNumber);
        mThreadOpen = true;
    }
#else
#ifdef _OPENMP
    omp_set_dynamic(0);
    omp_set_num_threads(mThreadNumber);
#endif
#endif
    _bindCPUCore();
}

void CPURuntime::onConcurrencyEnd() const {
#ifdef MNN_USE_THREAD_POOL
    if (mTaskIndex >= 0) {
        ThreadPool::deactive(mThreadNumber);
        mThreadOpen = false;
    }
#endif
}

std::map<OpType, CPUBackend::Creator*>* CPUBackend::gCreator = nullptr;
void CPUBackend::initCreatorMap() {
    gCreator = new std::map<OpType, CPUBackend::Creator*>;
}

bool CPUBackend::addCreator(OpType t, Creator* c) {
    auto map = gCreator;
    if (map->find(t) != map->end()) {
        MNN_PRINT("Error: %d type has be added\n", t);
        return false;
    }
    map->insert(std::make_pair(t, c));
    return true;
}
BufferAllocator* CPURuntime::createDynamicBufferAlloctor(int index) const {
    if (hint().memoryAllocatorType == Runtime::Allocator_Defer) {
        return new DeferBufferAllocator(buffer(index));
    }
    if (nullptr != mStaticAllocatorCache.get()) {
        return new EagerBufferAllocator(BufferAllocator::Allocator::createRecurse(mStaticAllocatorCache.get()));
    }
    return new EagerBufferAllocator(BufferAllocator::Allocator::createRecurse(mStaticAllocator.get()));
}
void CPUBackend::computeGroupRate() {
    {
        if (mThreadNumber <= 1 || mRuntime->mPower == BackendConfig::Power_Low) {
            return;
        }
        auto rate = mRuntime->hint().cpuDecreaseRate;
        if (rate >= 100 || rate <= 0) {
            return;
        }
        auto cpuInfo = MNNGetCPUInfo();
        if (cpuInfo->groups.size() < 2) {
            return;
        }
        if (cpuInfo->i8mm) {
            mComputeI = 28.f;
        } else if (cpuInfo->dot) {
            mComputeI = 14.f;
        } else {
            mComputeI = 7.f;
        }
        mGroupWithComputeRate.clear();
        float decreaseRate = (float)(rate) / 100.0f;
        int validCpuSize = (int)(cpuInfo->groups[cpuInfo->groups.size()-1].ids.size());
        int groupIndex = (int)cpuInfo->groups.size()-2;
        validCpuSize = ALIMIN(validCpuSize, mThreadNumber);
        float totalComputeRate = 1.0f * validCpuSize;
        mGroupWithComputeRate.emplace_back(std::make_pair(totalComputeRate, validCpuSize));
        float currentRate = 1.0f;
        while (validCpuSize < mThreadNumber && groupIndex >= 0) {
            auto& group = cpuInfo->groups[groupIndex];
            int selectSize = ALIMIN(mThreadNumber - validCpuSize, (int)group.ids.size());
            validCpuSize += group.ids.size();
            currentRate *= decreaseRate;
            totalComputeRate += currentRate * selectSize;
            mGroupWithComputeRate.emplace_back(std::make_pair(currentRate * selectSize, selectSize));
        }
        for (auto& g : mGroupWithComputeRate) {
            g.first = g.first / totalComputeRate;
        }
    } 
}
CPUBackend::CPUBackend(const CPURuntime* runtime, BackendConfig::PrecisionMode precision, BackendConfig::MemoryMode memory, MNNForwardType type, size_t flags) : Backend(type) {
#ifdef LOG_VERBOSE
    MNN_PRINT("cpu backend create\n");
#endif
    mMemory = memory;
    mRuntime = const_cast<CPURuntime*>(runtime);
    mThreadNumber = mRuntime->mThreadNumber;
    // Compute Group Rate
    computeGroupRate();
    // initialize Allocator
    auto dynamicAlloc = mRuntime->mSharedDmaInfo;
    if (nullptr == dynamicAlloc.get()) {
        mDmaInfo.reset(new CPURuntime::DynamicAllocator);
        mDmaInfo->mDynamicAllocator.reset(mRuntime->createDynamicBufferAlloctor(0));
        mDmaInfo->mCurrentDynamicAllocator = mDmaInfo->mDynamicAllocator.get();
    } else {
        mDmaInfo = dynamicAlloc;
    }
    mStaticAllocator = runtime->mStaticAllocator;
    mPrecisionMode = precision;
    mCoreFunctions = MNNGetCoreFunctions();
    mInt8CoreFunctions = MNNGetInt8CoreFunctions();
    mCacheGroup.resize(MNN_CPU_MAX_BUFFER_INDEX);
    for (int i=0; i<mCacheGroup.size(); ++i) {
        mCacheGroup[i].reset(new CPUResizeCache);
    }
    mCache = mCacheGroup[0].get();
}

CPUBackend::~CPUBackend() {
    mCacheGroup.clear();
}
void CPUBackend::_resetDynamicMemory() const {
    mRuntime->pCurrentStatus = mDmaInfo->mDynamicAllocator->apply();
    if (NO_ERROR != mRuntime->pCurrentStatus) {
        return;
    }
    if (nullptr != mDmaInfo->mDynamicAllocatorBackup.get()) {
        mRuntime->pCurrentStatus  = mDmaInfo->mDynamicAllocatorBackup->apply();
    }
}

void CPUBackend::onExecuteBegin() const {
    _resetDynamicMemory();
    mRuntime->onConcurrencyBegin();
}

void CPUBackend::onExecuteEnd() const {
    mRuntime->onConcurrencyEnd();
}

void CPUBackend::onResizeBegin() {
    mDmaInfo->mCurrentDynamicAllocator->reset();
}
bool CPUBackend::onSelectDynamicAllocator(int index, int maxIndex) {
    if (maxIndex > 2) {
        return false;
    }
    if (maxIndex == 2 && mDmaInfo->mDynamicAllocatorBackup.get() == nullptr) {
        mDmaInfo->mDynamicAllocatorBackup.reset(mRuntime->createDynamicBufferAlloctor(1));
    }
    if (1 == index) {
        mDmaInfo->mCurrentDynamicAllocator = mDmaInfo->mDynamicAllocatorBackup.get();
    } else {
        mRuntime->buffer(0)->release();
        mDmaInfo->mCurrentDynamicAllocator = mDmaInfo->mDynamicAllocator.get();
    }
    mCache = mCacheGroup[index].get();
    return true;
}

ErrorCode CPUBackend::onResizeEnd() {
    getCache()->release();
    auto code = mDmaInfo->mCurrentDynamicAllocator->compute();
    if (NO_ERROR != code) {
        return code;
    }
    return NO_ERROR;
}

Backend::MemObj* CPUBackend::allocBuffer(size_t size, Tensor* dest, StorageType storageType) {
    auto originMem = TensorUtils::getDescribeOrigin(dest)->mem.get();
    if (nullptr != originMem) {
        if (static_cast<CPUMemObj*>(originMem)->getSize() >= size) {
            return originMem;
        } else {
            TensorUtils::getDescribeOrigin(dest)->mem = nullptr;
        }
    }
    // MNN_PRINT("Acquire size = %d\n", size);
    if (size <= 0) {
        MNN_PRINT("Acquire buffer size = %lu\n", size);
        MNN_ASSERT(false);
        return nullptr;
    }
    // if (size > LARGE_MEMORY) {
    //     MNN_PRINT("Size larger than 500 M :%d\n", size);
    // }
    auto& buffer = dest->buffer();
    auto des = TensorUtils::getDescribe(dest);
    MemChunk chunk;
    switch (storageType) {
        case STATIC: {
            chunk = mStaticAllocator->alloc(size, false);
            break;
        }
        case DYNAMIC: {
            chunk = mDmaInfo->mCurrentDynamicAllocator->alloc(size, false);
            break;
        }
        case DYNAMIC_SEPERATE: {
            chunk = mDmaInfo->mCurrentDynamicAllocator->alloc(size, true);
            break;
        }
        default:
            MNN_ASSERT(false);
            break;
    }

    if (chunk.invalid()) {
        MNN_ERROR("Alloc buffer error for cpu backend\n");
        return nullptr;
    }

    Backend::MemObj* res = nullptr;

    if (storageType == STATIC) {
        res = new CPUMemObj(mStaticAllocator.get(), chunk, size);
    } else {
        res = new CPUMemObj(mDmaInfo->mCurrentDynamicAllocator, chunk, size);
        chunk.attach(dest);
    }
    if (chunk.ptr()) {
        buffer.host = chunk.ptr();
    }
    des->extra.offset = 0;
    return res;
}

Backend::MemObj* CPUBackend::onAcquire(const MNN::Tensor* nativeTensorConst, StorageType storageType) {
    if (nativeTensorConst == nullptr) {
        return nullptr;
    }
    //FUNC_PRINT_ALL(nativeTensorConst, p);
    auto nativeTensor = (Tensor*)nativeTensorConst;
    auto size = getTensorSize(nativeTensor, true);
    return allocBuffer(size, nativeTensor, storageType);
}

static OpType _getRealOpType(OpType opType) {
    switch (opType) {
        case OpType_Convolution:
            return OpType_ConvInt8;
        case OpType_ConvolutionDepthwise:
            return OpType_DepthwiseConvInt8;
        case OpType_Pooling:
            return OpType_PoolInt8;

        // case OpType_Eltwise:
        //     // TODO: just support EltwiseAdd
        //     return OpType_EltwiseInt8;
        default:
            return opType;
    }
}
void* CPUBackend::onMapTensor(Tensor::MapType mtype, Tensor::DimensionType dtype, const Tensor* srcTensor) {
    if (getBytes(this, srcTensor) != srcTensor->getType().bytes()) {
        return nullptr;
    }
    if (OpCommonUtils:: convertDimType(TensorUtils::getDescribe(srcTensor)->dimensionFormat) != dtype) {
        return nullptr;
    }
    return srcTensor->host<void>();
}

bool CPUBackend::onUnmapTensor(Tensor::MapType mtype, Tensor::DimensionType dtype, const Tensor* dstTensor, void* mapPtr) {
    if (getBytes(this, dstTensor) != dstTensor->getType().bytes()) {
        return false;
    }
    if (OpCommonUtils:: convertDimType(TensorUtils::getDescribe(dstTensor)->dimensionFormat) != dtype) {
        return false;
    }
    return true;
}

size_t CPUBackend::getTensorSize(const Tensor* tensor, bool multiBytes) const {
    auto core = mCoreFunctions;
    size_t dataSize = 1;
    auto des = TensorUtils::getDescribe(tensor);
    for (int i = 0; i < tensor->dimensions(); i++) {
        size_t currentDimSize = tensor->length(i);
        if (des->dimensionFormat == MNN_DATA_FORMAT_NC4HW4 && 1 == i) {
            currentDimSize = UP_DIV(currentDimSize, core->pack) * core->pack;
        }
        dataSize *= currentDimSize;
    }
    if (multiBytes) {
        size_t bytes = tensor->getType().bytes();
        if (TensorUtils::getDescribe(tensor)->quantAttr != nullptr) {
            if (TensorUtils::getDescribe(tensor)->type == DataType_DT_FLOAT) {
                bytes = 4;
            } else {
                bytes = 1;
            }
        }
        return dataSize * bytes;
    }
    return dataSize;
}

int CPUBackend::getBytes(const Backend* backend, const Tensor* output) {
    auto bytes = output->getType().bytes();
    auto core = static_cast<const CPUBackend*>(backend)->functions();
    auto quant = TensorUtils::getDescribe(output)->quantAttr.get();
    if (output->getType().code == halide_type_float) {
        bytes = core->bytes;
    }
    if (nullptr != quant && TensorUtils::getDescribe(output)->type == DataType_DT_INT8) {
        bytes = 1;
    }
    return bytes;
}

DataType CPUBackend::getDataType(const Tensor* tensor) {
    auto des = TensorUtils::getDescribe(tensor);
    if (nullptr == des->quantAttr.get()) {
        return DataType_DT_FLOAT;
    }
    return des->type;
}

/// get execution
Execution* CPUBackend::onCreate(const std::vector<Tensor*>& inputs, const std::vector<Tensor*>& outputs,
                                const MNN::Op* op) {
    /**
     BatchNorm it will be converted to scale
     for model convert, don't print error log
     */
    if (op->type() == OpType_BatchNorm) {
        return nullptr;
    }
    auto opType = op->type();
    if (outputs.size() > 0) {
        if (TensorUtils::getDescribe(outputs[0])->quantAttr != nullptr && TensorUtils::getDescribe(outputs[0])->type == DataType_DT_INT8) {
            opType = _getRealOpType(opType);
        }
    }

    // TODO: rm this convert when merge diff datatyoe of op
    auto map  = gCreator;
    auto iter = map->find(opType);
    if (iter == map->end() ) {
        MNN_PRINT("Don't support type [%s]\n", MNN::EnumNameOpType(op->type()));
        return nullptr;
    }
    Execution* exe = nullptr;
    bool needCast = false;
    if (exe == nullptr) {
        exe = iter->second->onCreate(inputs, outputs, op, this);
    }
    return exe;
}
const Runtime* CPUBackend::getRuntime() {
    return mRuntime;
}

bool CPUBackend::onClearBuffer() {
    if (nullptr != mRuntime->mStaticAllocatorCache.get()) {
        mStaticAllocator->sync();
        mStaticAllocator = mRuntime->mStaticAllocatorCache;
    }
    mCache->reset();
    mDmaInfo->mCurrentDynamicAllocator->release(true);
    return true;
}

std::pair<int, int> CPUBackend::multiThreadDivide(int size) const {
    int sizeDivide = size / threadNumber();
    sizeDivide = UP_DIV(sizeDivide, mCoreFunctions->pack) * mCoreFunctions->pack;
    int scheduleNumber = 1;
    if (sizeDivide > 0) {
        scheduleNumber = UP_DIV(size, sizeDivide);
    }
    return std::make_pair(sizeDivide, scheduleNumber);
}
void CPUBackend::onCopyBuffer(const Tensor* srcTensor, const Tensor* dstTensor) const {
    _resetDynamicMemory();
    auto& srcBuffer = srcTensor->buffer();
    auto& dstBuffer = dstTensor->buffer();
    if (srcBuffer.dimensions != dstBuffer.dimensions ) {
        if (srcBuffer.dim[srcBuffer.dimensions - 1].extent != 1 && dstBuffer.dim[dstBuffer.dimensions - 1].extent != 1) {
            MNN_ERROR("srcBuffer dimension not equal to dstBuffer, can't copy buffer\n");
        }
    }
    if (srcTensor->getDimensionType() == dstTensor->getDimensionType()) {
        for (int i = 0; i < srcBuffer.dimensions; ++i) {
            MNN_ASSERT(srcBuffer.dim[i].extent <= dstBuffer.dim[i].extent);
        }
    }
    if (nullptr == srcBuffer.host || nullptr == dstBuffer.host) {
        return;
    }
    std::unique_ptr<Tensor> wrapTensor;
    if (getDataType(srcTensor) != getDataType(dstTensor)) {
        auto dimType =  OpCommonUtils::convertDimType(TensorUtils::getDescribe(srcTensor)->dimensionFormat);
        auto convertType = CPUCastCreator::FlOAT_TO_INT8;
        if (getDataType(srcTensor) == DataType_DT_INT8) {
            convertType = CPUCastCreator::INT8_TO_FlOAT;
        }
        wrapTensor.reset(Tensor::createDevice(srcTensor->shape(), dstTensor->getType(), dimType));
        auto dstType = getDataType(dstTensor);
        if (dstType != DataType_DT_FLOAT) {
            wrapTensor->setType(dstType);
        }
        wrapTensor->buffer().host = (uint8_t*)MNNMemoryAllocAlign(getTensorSize(wrapTensor.get()) * wrapTensor->getType().bytes(), MNN_MEMORY_ALIGN_DEFAULT);

#ifdef LOG_VERBOSE
        MNN_PRINT("CPU backend copy tensor ptr:%p -> ptr:%p hostPtr:%p -> %p, format %d -> %d, dims: [",
        srcTensor, dstTensor, srcTensor->host<void>(), dstTensor->host<void>(), TensorUtils::getDescribe(srcTensor)->dimensionFormat, TensorUtils::getDescribe(dstTensor)->dimensionFormat);
        for (int i=0; i<srcTensor->dimensions(); ++i) {
            MNN_PRINT("%d ", srcTensor->length(i));
        }
        MNN_PRINT("]\n");
#endif

        TensorUtils::getDescribe(wrapTensor.get())->memoryType = Tensor::InsideDescribe::MEMORY_HOST;
        auto code = CPUCastCreator::cast(srcTensor, wrapTensor.get(), this, convertType);
        if (NO_ERROR != code) {
            MNN_ERROR("Error in CPUBackend::onCopyBuffer:cast\n");
        }
        srcTensor = wrapTensor.get();
    } else if (srcTensor->getType() != dstTensor->getType()) {
        MNN_ERROR("Input type not match session's tensor\n");
        return;
    }
    auto code = CPUTensorConverter::convert(srcTensor, dstTensor);
    if (NO_ERROR != code) {
        MNN_ERROR("Error in CPUBackend::onCopyBuffer:convert\n");
    }
}

class CPURuntimeCreator : public RuntimeCreator {
public:
    virtual Runtime* onCreate(const Backend::Info& info) const override {
        return new CPURuntime(info);
    }
};


#ifdef MNN_SUPPORT_BF16
extern void registerBF16Backend();
#endif
#ifdef ENABLE_ARMV82
extern void registerArm82RuntimeCreator();
#endif
void registerCPURuntimeCreator() {
    MNNCoreFunctionInit();
    CPUBackend::initCreatorMap();
    registerCPUOps();
#ifdef MNN_SUPPORT_BF16
    registerBF16Backend();
#endif
#ifdef MNN_USE_ARMV82
    registerArm82RuntimeCreator();
#endif
    // TODO: Merge _initCoreFunction MNNFunctionInit and cpuinfo_arm_init
    MNNInsertExtraRuntimeCreator(MNN_FORWARD_CPU, new CPURuntimeCreator);
};
} // namespace MNN
