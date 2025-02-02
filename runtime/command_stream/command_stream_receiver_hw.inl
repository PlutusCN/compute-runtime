/*
 * Copyright (C) 2018-2019 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 *
 */

#include "runtime/command_queue/gpgpu_walker.h"
#include "runtime/command_stream/command_stream_receiver_hw.h"
#include "runtime/command_stream/experimental_command_buffer.h"
#include "runtime/command_stream/linear_stream.h"
#include "runtime/command_stream/preemption.h"
#include "runtime/command_stream/scratch_space_controller_base.h"
#include "runtime/device/device.h"
#include "runtime/event/event.h"
#include "runtime/gtpin/gtpin_notify.h"
#include "runtime/helpers/blit_commands_helper.h"
#include "runtime/helpers/cache_policy.h"
#include "runtime/helpers/flat_batch_buffer_helper_hw.h"
#include "runtime/helpers/flush_stamp.h"
#include "runtime/helpers/hw_helper.h"
#include "runtime/helpers/options.h"
#include "runtime/helpers/preamble.h"
#include "runtime/helpers/ptr_math.h"
#include "runtime/helpers/state_base_address.h"
#include "runtime/helpers/timestamp_packet.h"
#include "runtime/indirect_heap/indirect_heap.h"
#include "runtime/memory_manager/internal_allocation_storage.h"
#include "runtime/memory_manager/memory_manager.h"
#include "runtime/os_interface/debug_settings_manager.h"
#include "runtime/os_interface/os_context.h"
#include "runtime/utilities/tag_allocator.h"

namespace NEO {

template <typename GfxFamily>
size_t CommandStreamReceiverHw<GfxFamily>::getSshHeapSize() {
    return defaultHeapSize;
}

template <typename GfxFamily>
CommandStreamReceiverHw<GfxFamily>::CommandStreamReceiverHw(ExecutionEnvironment &executionEnvironment)
    : CommandStreamReceiver(executionEnvironment) {

    auto &hwHelper = HwHelper::get(peekHwInfo().pPlatform->eRenderCoreFamily);
    localMemoryEnabled = hwHelper.getEnableLocalMemory(peekHwInfo());

    requiredThreadArbitrationPolicy = PreambleHelper<GfxFamily>::getDefaultThreadArbitrationPolicy();
    resetKmdNotifyHelper(new KmdNotifyHelper(&peekHwInfo().capabilityTable.kmdNotifyProperties));
    flatBatchBufferHelper.reset(new FlatBatchBufferHelperHw<GfxFamily>(executionEnvironment));
    defaultSshSize = getSshHeapSize();

    timestampPacketWriteEnabled = hwHelper.timestampPacketWriteSupported();
    if (DebugManager.flags.EnableTimestampPacket.get() != -1) {
        timestampPacketWriteEnabled = !!DebugManager.flags.EnableTimestampPacket.get();
    }
    createScratchSpaceController();
}

template <typename GfxFamily>
FlushStamp CommandStreamReceiverHw<GfxFamily>::flush(BatchBuffer &batchBuffer, ResidencyContainer &allocationsForResidency) {
    return flushStamp->peekStamp();
}

template <typename GfxFamily>
inline void CommandStreamReceiverHw<GfxFamily>::addBatchBufferEnd(LinearStream &commandStream, void **patchLocation) {
    typedef typename GfxFamily::MI_BATCH_BUFFER_END MI_BATCH_BUFFER_END;

    auto pCmd = (MI_BATCH_BUFFER_END *)commandStream.getSpace(sizeof(MI_BATCH_BUFFER_END));
    *pCmd = GfxFamily::cmdInitBatchBufferEnd;
    if (patchLocation) {
        *patchLocation = pCmd;
    }
}

template <typename GfxFamily>
inline void CommandStreamReceiverHw<GfxFamily>::addBatchBufferStart(MI_BATCH_BUFFER_START *commandBufferMemory, uint64_t startAddress, bool secondary) {
    *commandBufferMemory = GfxFamily::cmdInitBatchBufferStart;
    commandBufferMemory->setBatchBufferStartAddressGraphicsaddress472(startAddress);
    commandBufferMemory->setAddressSpaceIndicator(MI_BATCH_BUFFER_START::ADDRESS_SPACE_INDICATOR_PPGTT);
    if (secondary) {
        commandBufferMemory->setSecondLevelBatchBuffer(MI_BATCH_BUFFER_START::SECOND_LEVEL_BATCH_BUFFER_SECOND_LEVEL_BATCH);
    }
    if (DebugManager.flags.FlattenBatchBufferForAUBDump.get()) {
        flatBatchBufferHelper->registerBatchBufferStartAddress(reinterpret_cast<uint64_t>(commandBufferMemory), startAddress);
    }
}

template <typename GfxFamily>
inline void CommandStreamReceiverHw<GfxFamily>::alignToCacheLine(LinearStream &commandStream) {
    auto used = commandStream.getUsed();
    auto alignment = MemoryConstants::cacheLineSize;
    auto partialCacheline = used & (alignment - 1);
    if (partialCacheline) {
        auto amountToPad = alignment - partialCacheline;
        auto pCmd = commandStream.getSpace(amountToPad);
        memset(pCmd, 0, amountToPad);
    }
}

template <typename GfxFamily>
inline size_t CommandStreamReceiverHw<GfxFamily>::getRequiredCmdSizeForPreamble(Device &device) const {
    size_t size = 0;

    if (mediaVfeStateDirty) {
        size += sizeof(typename GfxFamily::PIPE_CONTROL) + sizeof(typename GfxFamily::MEDIA_VFE_STATE);
    }
    if (!this->isPreambleSent) {
        size += PreambleHelper<GfxFamily>::getAdditionalCommandsSize(device);
    }
    if (!this->isPreambleSent || this->lastSentThreadArbitrationPolicy != this->requiredThreadArbitrationPolicy) {
        size += PreambleHelper<GfxFamily>::getThreadArbitrationCommandsSize();
    }
    return size;
}

template <typename GfxFamily>
inline typename GfxFamily::PIPE_CONTROL *CommandStreamReceiverHw<GfxFamily>::addPipeControlCmd(LinearStream &commandStream) {
    typedef typename GfxFamily::PIPE_CONTROL PIPE_CONTROL;
    auto pCmd = reinterpret_cast<PIPE_CONTROL *>(commandStream.getSpace(sizeof(PIPE_CONTROL)));
    *pCmd = GfxFamily::cmdInitPipeControl;
    pCmd->setCommandStreamerStallEnable(true);
    return pCmd;
}

template <typename GfxFamily>
void CommandStreamReceiverHw<GfxFamily>::programPipelineSelect(LinearStream &commandStream, DispatchFlags &dispatchFlags) {
    if (csrSizeRequestFlags.mediaSamplerConfigChanged || !isPreambleSent) {
        PreambleHelper<GfxFamily>::programPipelineSelect(&commandStream, dispatchFlags);
        this->lastMediaSamplerConfig = dispatchFlags.mediaSamplerRequired;
    }
}

template <typename GfxFamily>
inline size_t CommandStreamReceiverHw<GfxFamily>::getCmdSizeForPipelineSelect() const {
    if (csrSizeRequestFlags.mediaSamplerConfigChanged || !isPreambleSent) {
        return sizeof(typename GfxFamily::PIPELINE_SELECT);
    }
    return 0;
}

template <typename GfxFamily>
CompletionStamp CommandStreamReceiverHw<GfxFamily>::flushTask(
    LinearStream &commandStreamTask,
    size_t commandStreamStartTask,
    const IndirectHeap &dsh,
    const IndirectHeap &ioh,
    const IndirectHeap &ssh,
    uint32_t taskLevel,
    DispatchFlags &dispatchFlags,
    Device &device) {
    typedef typename GfxFamily::MI_BATCH_BUFFER_START MI_BATCH_BUFFER_START;
    typedef typename GfxFamily::MI_BATCH_BUFFER_END MI_BATCH_BUFFER_END;
    typedef typename GfxFamily::PIPE_CONTROL PIPE_CONTROL;
    typedef typename GfxFamily::STATE_BASE_ADDRESS STATE_BASE_ADDRESS;

    DEBUG_BREAK_IF(&commandStreamTask == &commandStream);
    DEBUG_BREAK_IF(!(dispatchFlags.preemptionMode == PreemptionMode::Disabled ? device.getPreemptionMode() == PreemptionMode::Disabled : true));
    DEBUG_BREAK_IF(taskLevel >= Event::eventNotReady);

    DBG_LOG(LogTaskCounts, __FUNCTION__, "Line: ", __LINE__, "taskLevel", taskLevel);

    auto levelClosed = false;
    void *currentPipeControlForNooping = nullptr;
    void *epiloguePipeControlLocation = nullptr;

    if (DebugManager.flags.ForceCsrFlushing.get()) {
        flushBatchedSubmissions();
    }
    if (detectInitProgrammingFlagsRequired(dispatchFlags)) {
        initProgrammingFlags();
    }

    if (dispatchFlags.blocking || dispatchFlags.dcFlush || dispatchFlags.guardCommandBufferWithPipeControl) {
        if (this->dispatchMode == DispatchMode::ImmediateDispatch) {
            //for ImmediateDispatch we will send this right away, therefore this pipe control will close the level
            //for BatchedSubmissions it will be nooped and only last ppc in batch will be emitted.
            levelClosed = true;
            //if we guard with ppc, flush dc as well to speed up completion latency
            if (dispatchFlags.guardCommandBufferWithPipeControl) {
                dispatchFlags.dcFlush = true;
            }
        }

        epiloguePipeControlLocation = ptrOffset(commandStreamTask.getCpuBase(), commandStreamTask.getUsed());

        if ((dispatchFlags.outOfOrderExecutionAllowed || timestampPacketWriteEnabled) &&
            !dispatchFlags.dcFlush) {
            currentPipeControlForNooping = epiloguePipeControlLocation;
        }

        //Some architectures (SKL) requires to have pipe control prior to pipe control with tag write, add it here
        PipeControlHelper<GfxFamily>::addPipeControlWA(commandStreamTask);

        auto address = getTagAllocation()->getGpuAddress();
        auto pCmd = PipeControlHelper<GfxFamily>::obtainPipeControlAndProgramPostSyncOperation(&commandStreamTask, PIPE_CONTROL::POST_SYNC_OPERATION_WRITE_IMMEDIATE_DATA, address, taskCount + 1, dispatchFlags.dcFlush);

        if (DebugManager.flags.FlushAllCaches.get()) {
            pCmd->setDcFlushEnable(true);
            pCmd->setRenderTargetCacheFlushEnable(true);
            pCmd->setInstructionCacheInvalidateEnable(true);
            pCmd->setTextureCacheInvalidationEnable(true);
            pCmd->setPipeControlFlushEnable(true);
            pCmd->setVfCacheInvalidationEnable(true);
            pCmd->setConstantCacheInvalidationEnable(true);
            pCmd->setStateCacheInvalidationEnable(true);
        }

        this->latestSentTaskCount = taskCount + 1;
        DBG_LOG(LogTaskCounts, __FUNCTION__, "Line: ", __LINE__, "taskCount", taskCount);
        if (DebugManager.flags.AddPatchInfoCommentsForAUBDump.get()) {
            flatBatchBufferHelper->setPatchInfoData(PatchInfoData(address, 0u,
                                                                  PatchInfoAllocationType::TagAddress,
                                                                  commandStreamTask.getGraphicsAllocation()->getGpuAddress(),
                                                                  commandStreamTask.getUsed() - 2 * sizeof(uint64_t),
                                                                  PatchInfoAllocationType::Default));
            flatBatchBufferHelper->setPatchInfoData(PatchInfoData(address, 0u,
                                                                  PatchInfoAllocationType::TagValue,
                                                                  commandStreamTask.getGraphicsAllocation()->getGpuAddress(),
                                                                  commandStreamTask.getUsed() - sizeof(uint64_t),
                                                                  PatchInfoAllocationType::Default));
        }
    }

    if (DebugManager.flags.ForceSLML3Config.get()) {
        dispatchFlags.useSLM = true;
    }
    if (DebugManager.flags.OverrideThreadArbitrationPolicy.get() != -1) {
        requestThreadArbitrationPolicy(static_cast<uint32_t>(DebugManager.flags.OverrideThreadArbitrationPolicy.get()));
    }

    auto newL3Config = PreambleHelper<GfxFamily>::getL3Config(peekHwInfo(), dispatchFlags.useSLM);

    csrSizeRequestFlags.l3ConfigChanged = this->lastSentL3Config != newL3Config;
    csrSizeRequestFlags.coherencyRequestChanged = this->lastSentCoherencyRequest != static_cast<int8_t>(dispatchFlags.requiresCoherency);
    csrSizeRequestFlags.preemptionRequestChanged = this->lastPreemptionMode != dispatchFlags.preemptionMode;
    csrSizeRequestFlags.mediaSamplerConfigChanged = this->lastMediaSamplerConfig != static_cast<int8_t>(dispatchFlags.mediaSamplerRequired);
    csrSizeRequestFlags.numGrfRequiredChanged = this->lastSentNumGrfRequired != dispatchFlags.numGrfRequired;
    csrSizeRequestFlags.specialPipelineSelectModeChanged = this->lastSpecialPipelineSelectMode != dispatchFlags.specialPipelineSelectMode;

    auto force32BitAllocations = getMemoryManager()->peekForce32BitAllocations();
    bool stateBaseAddressDirty = false;

    bool checkVfeStateDirty = false;
    if (requiredScratchSize) {
        scratchSpaceController->setRequiredScratchSpace(ssh.getCpuBase(),
                                                        requiredScratchSize,
                                                        this->taskCount,
                                                        this->osContext->getContextId(),
                                                        stateBaseAddressDirty,
                                                        checkVfeStateDirty);
        if (checkVfeStateDirty) {
            setMediaVFEStateDirty(true);
        }
        makeResident(*scratchSpaceController->getScratchSpaceAllocation());
    }

    auto &commandStreamCSR = this->getCS(getRequiredCmdStreamSizeAligned(dispatchFlags, device));
    auto commandStreamStartCSR = commandStreamCSR.getUsed();

    TimestampPacketHelper::programCsrDependencies<GfxFamily>(commandStreamCSR, dispatchFlags.csrDependencies);

    if (stallingPipeControlOnNextFlushRequired) {
        stallingPipeControlOnNextFlushRequired = false;
        auto stallingPipeControlCmd = commandStream.getSpaceForCmd<PIPE_CONTROL>();
        *stallingPipeControlCmd = GfxFamily::cmdInitPipeControl;
        stallingPipeControlCmd->setCommandStreamerStallEnable(true);
    }
    initPageTableManagerRegisters(commandStreamCSR);
    programPreemption(commandStreamCSR, device, dispatchFlags);
    programComputeMode(commandStreamCSR, dispatchFlags);
    programL3(commandStreamCSR, dispatchFlags, newL3Config);
    programPipelineSelect(commandStreamCSR, dispatchFlags);
    programPreamble(commandStreamCSR, device, dispatchFlags, newL3Config);
    programMediaSampler(commandStreamCSR, dispatchFlags);

    if (this->lastSentThreadArbitrationPolicy != this->requiredThreadArbitrationPolicy) {
        PreambleHelper<GfxFamily>::programThreadArbitration(&commandStreamCSR, this->requiredThreadArbitrationPolicy);
        this->lastSentThreadArbitrationPolicy = this->requiredThreadArbitrationPolicy;
    }

    stateBaseAddressDirty |= ((GSBAFor32BitProgrammed ^ dispatchFlags.GSBA32BitRequired) && force32BitAllocations);

    programVFEState(commandStreamCSR, dispatchFlags);

    bool dshDirty = dshState.updateAndCheck(&dsh);
    bool iohDirty = iohState.updateAndCheck(&ioh);
    bool sshDirty = sshState.updateAndCheck(&ssh);

    auto isStateBaseAddressDirty = dshDirty || iohDirty || sshDirty || stateBaseAddressDirty;

    auto requiredL3Index = CacheSettings::l3CacheOn;
    if (this->disableL3Cache) {
        requiredL3Index = CacheSettings::l3CacheOff;
        this->disableL3Cache = false;
    }

    if (requiredL3Index != latestSentStatelessMocsConfig) {
        isStateBaseAddressDirty = true;
    }

    //Reprogram state base address if required
    if (isStateBaseAddressDirty || device.isSourceLevelDebuggerActive()) {
        auto pCmd = addPipeControlCmd(commandStreamCSR);
        pCmd->setTextureCacheInvalidationEnable(true);
        pCmd->setDcFlushEnable(true);

        uint64_t newGSHbase = 0;
        GSBAFor32BitProgrammed = false;
        if (is64bit && scratchSpaceController->getScratchSpaceAllocation() && !force32BitAllocations) {
            newGSHbase = scratchSpaceController->calculateNewGSH();
        } else if (is64bit && force32BitAllocations && dispatchFlags.GSBA32BitRequired) {
            newGSHbase = getMemoryManager()->getExternalHeapBaseAddress();
            GSBAFor32BitProgrammed = true;
        }

        auto stateBaseAddressCmdOffset = commandStreamCSR.getUsed();

        StateBaseAddressHelper<GfxFamily>::programStateBaseAddress(
            commandStreamCSR,
            dsh,
            ioh,
            ssh,
            newGSHbase,
            requiredL3Index,
            getMemoryManager()->getInternalHeapBaseAddress(),
            device.getGmmHelper(),
            dispatchFlags);

        if (sshDirty) {
            StateBaseAddressHelper<GfxFamily>::programBindingTableBaseAddress(commandStreamCSR, ssh, stateBaseAddressCmdOffset,
                                                                              device.getGmmHelper());
        }

        programStateSip(commandStreamCSR, device);

        latestSentStatelessMocsConfig = requiredL3Index;

        if (DebugManager.flags.AddPatchInfoCommentsForAUBDump.get()) {
            collectStateBaseAddresPatchInfo(commandStream.getGraphicsAllocation()->getGpuAddress(), stateBaseAddressCmdOffset, dsh, ioh, ssh, newGSHbase);
        }
    }

    DBG_LOG(LogTaskCounts, __FUNCTION__, "Line: ", __LINE__, "this->taskLevel", (uint32_t)this->taskLevel);

    if (device.getWaTable()->waSamplerCacheFlushBetweenRedescribedSurfaceReads) {
        if (this->samplerCacheFlushRequired != SamplerCacheFlushState::samplerCacheFlushNotRequired) {
            auto pCmd = addPipeControlCmd(commandStreamCSR);
            pCmd->setTextureCacheInvalidationEnable(true);
            if (this->samplerCacheFlushRequired == SamplerCacheFlushState::samplerCacheFlushBefore) {
                this->samplerCacheFlushRequired = SamplerCacheFlushState::samplerCacheFlushAfter;
            } else {
                this->samplerCacheFlushRequired = SamplerCacheFlushState::samplerCacheFlushNotRequired;
            }
        }
    }

    if (experimentalCmdBuffer.get() != nullptr) {
        size_t startingOffset = experimentalCmdBuffer->programExperimentalCommandBuffer<GfxFamily>();
        experimentalCmdBuffer->injectBufferStart<GfxFamily>(commandStreamCSR, startingOffset);
    }

    // Add a PC if we have a dependency on a previous walker to avoid concurrency issues.
    if (taskLevel > this->taskLevel) {
        if (!timestampPacketWriteEnabled) {
            PipeControlHelper<GfxFamily>::addPipeControl(commandStreamCSR, false);
        }
        this->taskLevel = taskLevel;
        DBG_LOG(LogTaskCounts, __FUNCTION__, "Line: ", __LINE__, "this->taskCount", this->taskCount);
    }

    auto dshAllocation = dsh.getGraphicsAllocation();
    auto iohAllocation = ioh.getGraphicsAllocation();
    auto sshAllocation = ssh.getGraphicsAllocation();

    this->makeResident(*dshAllocation);
    dshAllocation->setEvictable(false);
    this->makeResident(*iohAllocation);
    this->makeResident(*sshAllocation);
    iohAllocation->setEvictable(false);

    this->makeResident(*tagAllocation);

    if (preemptionCsrAllocation)
        makeResident(*preemptionCsrAllocation);

    if (dispatchFlags.preemptionMode == PreemptionMode::MidThread || device.isSourceLevelDebuggerActive()) {
        auto sipType = SipKernel::getSipKernelType(device.getHardwareInfo().pPlatform->eRenderCoreFamily, device.isSourceLevelDebuggerActive());
        makeResident(*device.getExecutionEnvironment()->getBuiltIns()->getSipKernel(sipType, device).getSipAllocation());
        if (debugSurface) {
            makeResident(*debugSurface);
        }
    }

    if (experimentalCmdBuffer.get() != nullptr) {
        experimentalCmdBuffer->makeResidentAllocations();
    }

    // If the CSR has work in its CS, flush it before the task
    bool submitTask = commandStreamStartTask != commandStreamTask.getUsed();
    bool submitCSR = (commandStreamStartCSR != commandStreamCSR.getUsed()) || this->isMultiOsContextCapable();
    bool submitCommandStreamFromCsr = false;
    void *bbEndLocation = nullptr;
    auto bbEndPaddingSize = this->dispatchMode == DispatchMode::ImmediateDispatch ? 0 : sizeof(MI_BATCH_BUFFER_START) - sizeof(MI_BATCH_BUFFER_END);
    size_t chainedBatchBufferStartOffset = 0;
    GraphicsAllocation *chainedBatchBuffer = nullptr;

    if (submitTask) {
        this->addBatchBufferEnd(commandStreamTask, &bbEndLocation);
        this->emitNoop(commandStreamTask, bbEndPaddingSize);
        this->alignToCacheLine(commandStreamTask);

        if (submitCSR) {
            chainedBatchBufferStartOffset = commandStreamCSR.getUsed();
            chainedBatchBuffer = commandStreamTask.getGraphicsAllocation();
            // Add MI_BATCH_BUFFER_START to chain from CSR -> Task
            auto pBBS = reinterpret_cast<MI_BATCH_BUFFER_START *>(commandStreamCSR.getSpace(sizeof(MI_BATCH_BUFFER_START)));
            addBatchBufferStart(pBBS, ptrOffset(commandStreamTask.getGraphicsAllocation()->getGpuAddress(), commandStreamStartTask), false);
            if (DebugManager.flags.FlattenBatchBufferForAUBDump.get()) {
                flatBatchBufferHelper->registerCommandChunk(commandStreamTask.getGraphicsAllocation()->getGpuAddress(),
                                                            reinterpret_cast<uint64_t>(commandStreamTask.getCpuBase()),
                                                            commandStreamStartTask,
                                                            static_cast<uint64_t>(ptrDiff(bbEndLocation,
                                                                                          commandStreamTask.getGraphicsAllocation()->getGpuAddress())) +
                                                                sizeof(MI_BATCH_BUFFER_START));
            }

            auto commandStreamAllocation = commandStreamTask.getGraphicsAllocation();
            DEBUG_BREAK_IF(commandStreamAllocation == nullptr);

            this->makeResident(*commandStreamAllocation);
            this->alignToCacheLine(commandStreamCSR);
            submitCommandStreamFromCsr = true;
        }
    } else if (submitCSR) {
        this->addBatchBufferEnd(commandStreamCSR, &bbEndLocation);
        this->emitNoop(commandStreamCSR, bbEndPaddingSize);
        this->alignToCacheLine(commandStreamCSR);
        DEBUG_BREAK_IF(commandStreamCSR.getUsed() > commandStreamCSR.getMaxAvailableSpace());
        submitCommandStreamFromCsr = true;
    }

    size_t startOffset = submitCommandStreamFromCsr ? commandStreamStartCSR : commandStreamStartTask;
    auto &streamToSubmit = submitCommandStreamFromCsr ? commandStreamCSR : commandStreamTask;
    BatchBuffer batchBuffer{streamToSubmit.getGraphicsAllocation(), startOffset, chainedBatchBufferStartOffset, chainedBatchBuffer, dispatchFlags.requiresCoherency, dispatchFlags.lowPriority, dispatchFlags.throttle, streamToSubmit.getUsed(), &streamToSubmit};

    if (submitCSR | submitTask) {
        if (this->dispatchMode == DispatchMode::ImmediateDispatch) {
            flushStamp->setStamp(this->flush(batchBuffer, this->getResidencyAllocations()));
            this->latestFlushedTaskCount = this->taskCount + 1;
            this->makeSurfacePackNonResident(this->getResidencyAllocations());
        } else {
            auto commandBuffer = new CommandBuffer(device);
            commandBuffer->batchBuffer = batchBuffer;
            commandBuffer->surfaces.swap(this->getResidencyAllocations());
            commandBuffer->batchBufferEndLocation = bbEndLocation;
            commandBuffer->taskCount = this->taskCount + 1;
            commandBuffer->flushStamp->replaceStampObject(dispatchFlags.flushStampReference);
            commandBuffer->pipeControlThatMayBeErasedLocation = currentPipeControlForNooping;
            commandBuffer->epiloguePipeControlLocation = epiloguePipeControlLocation;
            this->submissionAggregator->recordCommandBuffer(commandBuffer);
        }
    } else {
        this->makeSurfacePackNonResident(this->getResidencyAllocations());
    }

    //check if we are not over the budget, if we are do implicit flush
    if (getMemoryManager()->isMemoryBudgetExhausted()) {
        if (this->totalMemoryUsed >= device.getDeviceInfo().globalMemSize / 4) {
            dispatchFlags.implicitFlush = true;
        }
    }

    if (this->dispatchMode == DispatchMode::BatchedDispatch && (dispatchFlags.blocking || dispatchFlags.implicitFlush)) {
        this->flushBatchedSubmissions();
    }

    ++taskCount;
    DBG_LOG(LogTaskCounts, __FUNCTION__, "Line: ", __LINE__, "taskCount", taskCount);
    DBG_LOG(LogTaskCounts, __FUNCTION__, "Line: ", __LINE__, "Current taskCount:", tagAddress ? *tagAddress : 0);

    CompletionStamp completionStamp = {
        taskCount,
        this->taskLevel,
        flushStamp->peekStamp()};

    this->taskLevel += levelClosed ? 1 : 0;

    if (gtpinIsGTPinInitialized()) {
        gtpinNotifyFlushTask(completionStamp.taskCount);
    }

    return completionStamp;
}

template <typename GfxFamily>
inline void CommandStreamReceiverHw<GfxFamily>::flushBatchedSubmissions() {
    if (this->dispatchMode == DispatchMode::ImmediateDispatch) {
        return;
    }
    typedef typename GfxFamily::MI_BATCH_BUFFER_START MI_BATCH_BUFFER_START;
    typedef typename GfxFamily::PIPE_CONTROL PIPE_CONTROL;
    std::unique_lock<MutexType> lockGuard(ownershipMutex);

    auto &commandBufferList = this->submissionAggregator->peekCmdBufferList();
    if (!commandBufferList.peekIsEmpty()) {
        const auto totalMemoryBudget = static_cast<size_t>(commandBufferList.peekHead()->device.getDeviceInfo().globalMemSize / 2);

        ResidencyContainer surfacesForSubmit;
        ResourcePackage resourcePackage;
        auto pipeControlLocationSize = PipeControlHelper<GfxFamily>::getRequiredPipeControlSize();
        void *currentPipeControlForNooping = nullptr;
        void *epiloguePipeControlLocation = nullptr;

        while (!commandBufferList.peekIsEmpty()) {
            size_t totalUsedSize = 0u;
            this->submissionAggregator->aggregateCommandBuffers(resourcePackage, totalUsedSize, totalMemoryBudget, osContext->getContextId());
            auto primaryCmdBuffer = commandBufferList.removeFrontOne();
            auto nextCommandBuffer = commandBufferList.peekHead();
            auto currentBBendLocation = primaryCmdBuffer->batchBufferEndLocation;
            auto lastTaskCount = primaryCmdBuffer->taskCount;

            FlushStampUpdateHelper flushStampUpdateHelper;
            flushStampUpdateHelper.insert(primaryCmdBuffer->flushStamp->getStampReference());

            currentPipeControlForNooping = primaryCmdBuffer->pipeControlThatMayBeErasedLocation;
            epiloguePipeControlLocation = primaryCmdBuffer->epiloguePipeControlLocation;

            if (DebugManager.flags.FlattenBatchBufferForAUBDump.get()) {
                flatBatchBufferHelper->registerCommandChunk(primaryCmdBuffer.get()->batchBuffer, sizeof(MI_BATCH_BUFFER_START));
            }
            while (nextCommandBuffer && nextCommandBuffer->inspectionId == primaryCmdBuffer->inspectionId) {
                //noop pipe control
                if (currentPipeControlForNooping) {
                    if (DebugManager.flags.AddPatchInfoCommentsForAUBDump.get()) {
                        flatBatchBufferHelper->removePipeControlData(pipeControlLocationSize, currentPipeControlForNooping);
                    }
                    memset(currentPipeControlForNooping, 0, pipeControlLocationSize);
                }
                //obtain next candidate for nooping
                currentPipeControlForNooping = nextCommandBuffer->pipeControlThatMayBeErasedLocation;
                //track epilogue pipe control
                epiloguePipeControlLocation = nextCommandBuffer->epiloguePipeControlLocation;

                flushStampUpdateHelper.insert(nextCommandBuffer->flushStamp->getStampReference());
                auto nextCommandBufferAddress = nextCommandBuffer->batchBuffer.commandBufferAllocation->getGpuAddress();
                auto offsetedCommandBuffer = (uint64_t)ptrOffset(nextCommandBufferAddress, nextCommandBuffer->batchBuffer.startOffset);
                addBatchBufferStart((MI_BATCH_BUFFER_START *)currentBBendLocation, offsetedCommandBuffer, false);
                if (DebugManager.flags.FlattenBatchBufferForAUBDump.get()) {
                    flatBatchBufferHelper->registerCommandChunk(nextCommandBuffer->batchBuffer, sizeof(MI_BATCH_BUFFER_START));
                }

                currentBBendLocation = nextCommandBuffer->batchBufferEndLocation;
                lastTaskCount = nextCommandBuffer->taskCount;
                nextCommandBuffer = nextCommandBuffer->next;
                commandBufferList.removeFrontOne();
            }
            surfacesForSubmit.reserve(resourcePackage.size() + 1);
            for (auto &surface : resourcePackage) {
                surfacesForSubmit.push_back(surface);
            }

            //make sure we flush DC if needed
            if (epiloguePipeControlLocation) {
                bool flushDcInEpilogue = true;
                if (DebugManager.flags.DisableDcFlushInEpilogue.get()) {
                    flushDcInEpilogue = false;
                }
                ((PIPE_CONTROL *)epiloguePipeControlLocation)->setDcFlushEnable(flushDcInEpilogue);
            }
            auto flushStamp = this->flush(primaryCmdBuffer->batchBuffer, surfacesForSubmit);

            //after flush task level is closed
            this->taskLevel++;

            flushStampUpdateHelper.updateAll(flushStamp);

            this->latestFlushedTaskCount = lastTaskCount;
            this->flushStamp->setStamp(flushStamp);
            this->makeSurfacePackNonResident(surfacesForSubmit);
            resourcePackage.clear();
        }
        this->totalMemoryUsed = 0;
    }
}

template <typename GfxFamily>
size_t CommandStreamReceiverHw<GfxFamily>::getRequiredCmdStreamSizeAligned(const DispatchFlags &dispatchFlags, Device &device) {
    size_t size = getRequiredCmdStreamSize(dispatchFlags, device);
    return alignUp(size, MemoryConstants::cacheLineSize);
}

template <typename GfxFamily>
size_t CommandStreamReceiverHw<GfxFamily>::getRequiredStateBaseAddressSize() const {
    return sizeof(typename GfxFamily::STATE_BASE_ADDRESS) + sizeof(PIPE_CONTROL);
}

template <typename GfxFamily>
size_t CommandStreamReceiverHw<GfxFamily>::getRequiredCmdStreamSize(const DispatchFlags &dispatchFlags, Device &device) {
    size_t size = getRequiredCmdSizeForPreamble(device);
    size += getRequiredStateBaseAddressSize();
    if (!this->isStateSipSent || device.isSourceLevelDebuggerActive()) {
        size += PreemptionHelper::getRequiredStateSipCmdSize<GfxFamily>(device);
    }
    size += PipeControlHelper<GfxFamily>::getRequiredPipeControlSize();
    size += sizeof(typename GfxFamily::MI_BATCH_BUFFER_START);

    size += getCmdSizeForL3Config();
    size += getCmdSizeForComputeMode();
    size += getCmdSizeForMediaSampler(dispatchFlags.mediaSamplerRequired);
    size += getCmdSizeForPipelineSelect();
    size += getCmdSizeForPreemption(dispatchFlags);

    if (device.getWaTable()->waSamplerCacheFlushBetweenRedescribedSurfaceReads) {
        if (this->samplerCacheFlushRequired != SamplerCacheFlushState::samplerCacheFlushNotRequired) {
            size += sizeof(typename GfxFamily::PIPE_CONTROL);
        }
    }
    if (experimentalCmdBuffer.get() != nullptr) {
        size += experimentalCmdBuffer->getRequiredInjectionSize<GfxFamily>();
    }

    size += TimestampPacketHelper::getRequiredCmdStreamSize<GfxFamily>(dispatchFlags.csrDependencies);

    if (stallingPipeControlOnNextFlushRequired) {
        size += sizeof(typename GfxFamily::PIPE_CONTROL);
    }
    return size;
}

template <typename GfxFamily>
inline void CommandStreamReceiverHw<GfxFamily>::emitNoop(LinearStream &commandStream, size_t bytesToUpdate) {
    if (bytesToUpdate) {
        auto ptr = commandStream.getSpace(bytesToUpdate);
        memset(ptr, 0, bytesToUpdate);
    }
}

template <typename GfxFamily>
inline void CommandStreamReceiverHw<GfxFamily>::waitForTaskCountWithKmdNotifyFallback(uint32_t taskCountToWait, FlushStamp flushStampToWait, bool useQuickKmdSleep, bool forcePowerSavingMode) {
    int64_t waitTimeout = 0;
    bool enableTimeout = kmdNotifyHelper->obtainTimeoutParams(waitTimeout, useQuickKmdSleep, *getTagAddress(), taskCountToWait, flushStampToWait, forcePowerSavingMode);

    auto status = waitForCompletionWithTimeout(enableTimeout, waitTimeout, taskCountToWait);
    if (!status) {
        waitForFlushStamp(flushStampToWait);
        //now call blocking wait, this is to ensure that task count is reached
        waitForCompletionWithTimeout(false, 0, taskCountToWait);
    }
    UNRECOVERABLE_IF(*getTagAddress() < taskCountToWait);

    if (kmdNotifyHelper->quickKmdSleepForSporadicWaitsEnabled()) {
        kmdNotifyHelper->updateLastWaitForCompletionTimestamp();
    }
}

template <typename GfxFamily>
inline void CommandStreamReceiverHw<GfxFamily>::programPreemption(LinearStream &csr, Device &device, DispatchFlags &dispatchFlags) {
    PreemptionHelper::programCmdStream<GfxFamily>(csr, dispatchFlags.preemptionMode, this->lastPreemptionMode, preemptionCsrAllocation, device);
    this->lastPreemptionMode = dispatchFlags.preemptionMode;
}

template <typename GfxFamily>
inline size_t CommandStreamReceiverHw<GfxFamily>::getCmdSizeForPreemption(const DispatchFlags &dispatchFlags) const {
    return PreemptionHelper::getRequiredCmdStreamSize<GfxFamily>(dispatchFlags.preemptionMode, this->lastPreemptionMode);
}

template <typename GfxFamily>
inline void CommandStreamReceiverHw<GfxFamily>::programStateSip(LinearStream &cmdStream, Device &device) {
    if (!this->isStateSipSent || device.isSourceLevelDebuggerActive()) {
        PreemptionHelper::programStateSip<GfxFamily>(cmdStream, device);
        this->isStateSipSent = true;
    }
}

template <typename GfxFamily>
inline void CommandStreamReceiverHw<GfxFamily>::programL3(LinearStream &csr, DispatchFlags &dispatchFlags, uint32_t &newL3Config) {
    typedef typename GfxFamily::PIPE_CONTROL PIPE_CONTROL;
    if (csrSizeRequestFlags.l3ConfigChanged && this->isPreambleSent) {
        // Add a PIPE_CONTROL w/ CS_stall
        auto pCmd = (PIPE_CONTROL *)csr.getSpace(sizeof(PIPE_CONTROL));
        *pCmd = GfxFamily::cmdInitPipeControl;
        pCmd->setCommandStreamerStallEnable(true);
        pCmd->setDcFlushEnable(true);
        addClearSLMWorkAround(pCmd);

        PreambleHelper<GfxFamily>::programL3(&csr, newL3Config);
        this->lastSentL3Config = newL3Config;
    }
}

template <typename GfxFamily>
inline size_t CommandStreamReceiverHw<GfxFamily>::getCmdSizeForL3Config() const {
    if (!this->isPreambleSent) {
        return sizeof(typename GfxFamily::MI_LOAD_REGISTER_IMM);
    } else if (csrSizeRequestFlags.l3ConfigChanged) {
        return sizeof(typename GfxFamily::MI_LOAD_REGISTER_IMM) + sizeof(typename GfxFamily::PIPE_CONTROL);
    }
    return 0;
}

template <typename GfxFamily>
inline void CommandStreamReceiverHw<GfxFamily>::programPreamble(LinearStream &csr, Device &device, DispatchFlags &dispatchFlags, uint32_t &newL3Config) {
    if (!this->isPreambleSent) {
        PreambleHelper<GfxFamily>::programPreamble(&csr, device, newL3Config, this->requiredThreadArbitrationPolicy, this->preemptionCsrAllocation);
        this->isPreambleSent = true;
        this->lastSentL3Config = newL3Config;
        this->lastSentThreadArbitrationPolicy = this->requiredThreadArbitrationPolicy;
    }
}

template <typename GfxFamily>
inline void CommandStreamReceiverHw<GfxFamily>::programVFEState(LinearStream &csr, DispatchFlags &dispatchFlags) {
    if (mediaVfeStateDirty) {
        PreambleHelper<GfxFamily>::programVFEState(&csr, peekHwInfo(), requiredScratchSize, getScratchPatchAddress());
        setMediaVFEStateDirty(false);
    }
}

template <typename GfxFamily>
void CommandStreamReceiverHw<GfxFamily>::programMediaSampler(LinearStream &commandStream, DispatchFlags &dispatchFlags) {
}

template <typename GfxFamily>
size_t CommandStreamReceiverHw<GfxFamily>::getCmdSizeForMediaSampler(bool mediaSamplerRequired) const {
    return 0;
}

template <typename GfxFamily>
void CommandStreamReceiverHw<GfxFamily>::collectStateBaseAddresPatchInfo(
    uint64_t baseAddress,
    uint64_t commandOffset,
    const LinearStream &dsh,
    const LinearStream &ioh,
    const LinearStream &ssh,
    uint64_t generalStateBase) {

    typedef typename GfxFamily::STATE_BASE_ADDRESS STATE_BASE_ADDRESS;

    PatchInfoData dynamicStatePatchInfo = {dsh.getGraphicsAllocation()->getGpuAddress(), 0u, PatchInfoAllocationType::DynamicStateHeap, baseAddress, commandOffset + STATE_BASE_ADDRESS::PATCH_CONSTANTS::DYNAMICSTATEBASEADDRESS_BYTEOFFSET, PatchInfoAllocationType::Default};
    PatchInfoData generalStatePatchInfo = {generalStateBase, 0u, PatchInfoAllocationType::GeneralStateHeap, baseAddress, commandOffset + STATE_BASE_ADDRESS::PATCH_CONSTANTS::GENERALSTATEBASEADDRESS_BYTEOFFSET, PatchInfoAllocationType::Default};
    PatchInfoData surfaceStatePatchInfo = {ssh.getGraphicsAllocation()->getGpuAddress(), 0u, PatchInfoAllocationType::SurfaceStateHeap, baseAddress, commandOffset + STATE_BASE_ADDRESS::PATCH_CONSTANTS::SURFACESTATEBASEADDRESS_BYTEOFFSET, PatchInfoAllocationType::Default};
    PatchInfoData indirectObjectPatchInfo = {ioh.getGraphicsAllocation()->getGpuAddress(), 0u, PatchInfoAllocationType::IndirectObjectHeap, baseAddress, commandOffset + STATE_BASE_ADDRESS::PATCH_CONSTANTS::INDIRECTOBJECTBASEADDRESS_BYTEOFFSET, PatchInfoAllocationType::Default};

    flatBatchBufferHelper->setPatchInfoData(dynamicStatePatchInfo);
    flatBatchBufferHelper->setPatchInfoData(generalStatePatchInfo);
    flatBatchBufferHelper->setPatchInfoData(surfaceStatePatchInfo);
    flatBatchBufferHelper->setPatchInfoData(indirectObjectPatchInfo);
}

template <typename GfxFamily>
void CommandStreamReceiverHw<GfxFamily>::resetKmdNotifyHelper(KmdNotifyHelper *newHelper) {
    kmdNotifyHelper.reset(newHelper);
    kmdNotifyHelper->updateAcLineStatus();
    if (kmdNotifyHelper->quickKmdSleepForSporadicWaitsEnabled()) {
        kmdNotifyHelper->updateLastWaitForCompletionTimestamp();
    }
}

template <typename GfxFamily>
void CommandStreamReceiverHw<GfxFamily>::addClearSLMWorkAround(typename GfxFamily::PIPE_CONTROL *pCmd) {
}

template <typename GfxFamily>
void CommandStreamReceiverHw<GfxFamily>::createScratchSpaceController() {
    scratchSpaceController = std::make_unique<ScratchSpaceControllerBase>(executionEnvironment, *internalAllocationStorage.get());
}

template <typename GfxFamily>
uint64_t CommandStreamReceiverHw<GfxFamily>::getScratchPatchAddress() {
    return scratchSpaceController->getScratchPatchAddress();
}

template <typename GfxFamily>
bool CommandStreamReceiverHw<GfxFamily>::detectInitProgrammingFlagsRequired(const DispatchFlags &dispatchFlags) const {
    return DebugManager.flags.ForceCsrReprogramming.get();
}

template <typename GfxFamily>
void CommandStreamReceiverHw<GfxFamily>::blitFromHostPtr(MemObj &destinationMemObj, void *sourceHostPtr, uint64_t sourceSize) {
    using MI_BATCH_BUFFER_END = typename GfxFamily::MI_BATCH_BUFFER_END;
    using MI_FLUSH_DW = typename GfxFamily::MI_FLUSH_DW;

    UNRECOVERABLE_IF(osContext->getEngineType() != aub_stream::EngineType::ENGINE_BCS);

    auto lock = obtainUniqueOwnership();
    auto &commandStream = getCS(BlitCommandsHelper<GfxFamily>::estimateBlitCommandsSize(sourceSize));
    auto commandStreamStart = commandStream.getUsed();
    auto newTaskCount = taskCount + 1;
    latestSentTaskCount = newTaskCount;

    HostPtrSurface hostPtrSurface(sourceHostPtr, static_cast<size_t>(sourceSize), true);
    bool success = createAllocationForHostSurface(hostPtrSurface, false);
    UNRECOVERABLE_IF(!success);

    UNRECOVERABLE_IF(destinationMemObj.peekClMemObjType() != CL_MEM_OBJECT_BUFFER);
    BlitCommandsHelper<GfxFamily>::dispatchBlitCommandsForBuffer(static_cast<Buffer &>(destinationMemObj), commandStream, *hostPtrSurface.getAllocation(), sourceSize);

    auto miFlushDwCmd = reinterpret_cast<MI_FLUSH_DW *>(commandStream.getSpace(sizeof(MI_FLUSH_DW)));
    *miFlushDwCmd = GfxFamily::cmdInitMiFlushDw;
    miFlushDwCmd->setPostSyncOperation(MI_FLUSH_DW::POST_SYNC_OPERATION_WRITE_IMMEDIATE_DATA_QWORD);
    miFlushDwCmd->setDestinationAddress(tagAllocation->getGpuAddress());
    miFlushDwCmd->setImmediateData(newTaskCount);

    auto batchBufferEnd = reinterpret_cast<MI_BATCH_BUFFER_END *>(commandStream.getSpace(sizeof(MI_BATCH_BUFFER_END)));
    *batchBufferEnd = GfxFamily::cmdInitBatchBufferEnd;

    alignToCacheLine(commandStream);

    makeResident(*hostPtrSurface.getAllocation());
    makeResident(*destinationMemObj.getGraphicsAllocation());
    makeResident(*commandStream.getGraphicsAllocation());
    makeResident(*tagAllocation);

    BatchBuffer batchBuffer{commandStream.getGraphicsAllocation(), commandStreamStart, 0, nullptr, false, false, QueueThrottle::MEDIUM,
                            commandStream.getUsed(), &commandStream};

    flushStamp->setStamp(flush(batchBuffer, getResidencyAllocations()));
    makeSurfacePackNonResident(getResidencyAllocations());

    latestFlushedTaskCount = newTaskCount;
    taskCount = newTaskCount;
    auto flushStampToWait = flushStamp->peekStamp();

    lock.unlock();
    waitForTaskCountWithKmdNotifyFallback(newTaskCount, flushStampToWait, false, false);
}

} // namespace NEO
