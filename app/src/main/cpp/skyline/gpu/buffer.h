// SPDX-License-Identifier: MPL-2.0
// Copyright © 2021 Skyline Team and Contributors (https://github.com/skyline-emu/)

#pragma once

#include <boost/functional/hash.hpp>
#include <common/linear_allocator.h>
#include <nce.h>
#include <gpu/tag_allocator.h>
#include "megabuffer.h"
#include "memory_manager.h"

namespace skyline::gpu {
    using GuestBuffer = span<u8>; //!< The CPU mapping for the guest buffer, multiple mappings for buffers aren't supported since overlaps cannot be reconciled

    class BufferView;
    class BufferManager;
    class BufferDelegate;

    /**
     * @brief A buffer which is backed by host constructs while being synchronized with the underlying guest buffer
     * @note This class conforms to the Lockable and BasicLockable C++ named requirements
     */
    class Buffer : public std::enable_shared_from_this<Buffer> {
      private:
        GPU &gpu;
        std::mutex mutex; //!< Synchronizes any mutations to the buffer or its backing
        std::atomic<ContextTag> tag{}; //!< The tag associated with the last lock call
        memory::Buffer backing;
        std::optional<GuestBuffer> guest;
        std::shared_ptr<FenceCycle> cycle{}; //!< A fence cycle for when any host operation mutating the buffer has completed, it must be waited on prior to any mutations to the backing
        size_t id;

        span<u8> mirror{}; //!< A contiguous mirror of all the guest mappings to allow linear access on the CPU
        span<u8> alignedMirror{}; //!< The mirror mapping aligned to page size to reflect the full mapping
        std::optional<nce::NCE::TrapHandle> trapHandle{}; //!< The handle of the traps for the guest mappings

        enum class DirtyState {
            Clean, //!< The CPU mappings are in sync with the GPU buffer
            CpuDirty, //!< The CPU mappings have been modified but the GPU buffer is not up to date
            GpuDirty, //!< The GPU buffer has been modified but the CPU mappings have not been updated
        } dirtyState{DirtyState::CpuDirty}; //!< The state of the CPU mappings with respect to the GPU buffer

        enum class BackingImmutability {
            None, //!< Backing can be freely written to and read from
            SequencedWrites, //!< Sequenced writes must not modify the backing on the CPU due to it being read directly on the GPU, but non-sequenced writes can freely occur (SynchroniseHost etc)
            AllWrites //!< No CPU writes to the backing can be performed, all must be sequenced on the GPU or delayed till this is no longer the case
        } backingImmutability{}; //!< Describes how the buffer backing should be accessed by the current context
        std::recursive_mutex stateMutex; //!< Synchronizes access to the dirty state and backing immutability

        bool everHadInlineUpdate{}; //!< Whether the buffer has ever had an inline update since it was created, if this is set then megabuffering will be attempted by views to avoid the cost of inline GPU updates

      public:

        static constexpr u64 InitialSequenceNumber{1}; //!< Sequence number that all buffers start off with

      private:
        u64 sequenceNumber{InitialSequenceNumber}; //!< Sequence number that is incremented after all modifications to the host side `backing` buffer, used to prevent redundant copies of the buffer being stored in the megabuffer by views


      private:
        BufferDelegate *delegate;

        friend BufferView;
        friend BufferManager;

        /**
         * @brief Sets up mirror mappings for the guest mappings, this must be called after construction for the mirror to be valid
         */
        void SetupGuestMappings();

      public:
        void UpdateCycle(const std::shared_ptr<FenceCycle> &newCycle) {
            std::scoped_lock lock{stateMutex};
            newCycle->ChainCycle(cycle);
            cycle = newCycle;
        }

        constexpr vk::Buffer GetBacking() {
            return backing.vkBuffer;
        }

        /**
         * @return A span over the backing of this buffer
         * @note This operation **must** be performed only on host-only buffers since synchronization is handled internally for guest-backed buffers
         */
        span<u8> GetBackingSpan() {
            if (guest)
                throw exception("Attempted to get a span of a guest-backed buffer");
            return span<u8>(backing);
        }

        /**
         * @brief Creates a buffer object wrapping the guest buffer with a backing that can represent the guest buffer data
         * @note The guest mappings will not be setup until SetupGuestMappings() is called
         */
        Buffer(LinearAllocatorState<> &delegateAllocator, GPU &gpu, GuestBuffer guest, size_t id);

        /**
         * @brief Creates a host-only Buffer which isn't backed by any guest buffer
         * @note The created buffer won't have a mirror so any operations cannot depend on a mirror existing
         */
        Buffer(LinearAllocatorState<> &delegateAllocator, GPU &gpu, vk::DeviceSize size, size_t id);

        ~Buffer();

        /**
         * @brief Acquires an exclusive lock on the buffer for the calling thread
         * @note Naming is in accordance to the BasicLockable named requirement
         */
        void lock();

        /**
         * @brief Acquires an exclusive lock on the buffer for the calling thread
         * @param tag A tag to associate with the lock, future invocations with the same tag prior to the unlock will acquire the lock without waiting (A default initialised tag will disable this behaviour)
         * @return If the lock was acquired by this call as opposed to the buffer already being locked with the same tag
         * @note All locks using the same tag **must** be from the same thread as it'll only have one corresponding unlock() call
         */
        bool LockWithTag(ContextTag tag);

        /**
         * @brief Relinquishes an existing lock on the buffer by the calling thread
         * @note Naming is in accordance to the BasicLockable named requirement
         */
        void unlock();

        /**
         * @brief Attempts to acquire an exclusive lock but returns immediately if it's captured by another thread
         * @note Naming is in accordance to the Lockable named requirement
         */
        bool try_lock();

        /**
         * @brief Marks the buffer as dirty on the GPU, it will be synced on the next call to SynchronizeGuest
         * @note This **must** be called after syncing the buffer to the GPU not before
         * @note The buffer **must** be locked prior to calling this
         */
        void MarkGpuDirty();

        /**
         * @brief Prevents sequenced writes to this buffer's backing from occuring on the CPU, forcing sequencing on the GPU instead for the duration of the context. Unsequenced writes such as those from the guest can still occur however.
         * @note The buffer **must** be locked prior to calling this
         */
        void BlockSequencedCpuBackingWrites() {
            std::scoped_lock lock{stateMutex};
            if (backingImmutability == BackingImmutability::None)
                backingImmutability = BackingImmutability::SequencedWrites;
        }

        /**
         * @brief Prevents *any* writes to this buffer's backing from occuring on the CPU, forcing sequencing on the GPU instead for the duration of the context.
         * @note The buffer **must** be locked prior to calling this
         */
        void BlockAllCpuBackingWrites() {
            std::scoped_lock lock{stateMutex};
            backingImmutability = BackingImmutability::AllWrites;
        }

        /**
         * @return If sequenced writes to the backing must not occur on the CPU
         * @note The buffer **must** be locked prior to calling this
         */
        bool SequencedCpuBackingWritesBlocked() {
            std::scoped_lock lock{stateMutex};
            return backingImmutability == BackingImmutability::SequencedWrites || backingImmutability == BackingImmutability::AllWrites;
        }

        /**
         * @return If no writes to the backing are allowed to occur on the CPU
         * @note The buffer **must** be locked prior to calling this
         */
        bool AllCpuBackingWritesBlocked() {
            std::scoped_lock lock{stateMutex};
            return backingImmutability == BackingImmutability::AllWrites;
        }

        /**
         * @return If the cycle needs to be attached to the buffer before ending the current context
         * @note This is an alias for `SequencedCpuBackingWritesBlocked()` since this is only ever set when the backing is accessed on the GPU in some form
         * @note The buffer **must** be locked prior to calling this
         */
        bool RequiresCycleAttach() {
            return SequencedCpuBackingWritesBlocked();
        }

        /**
         * @note The buffer **must** be locked prior to calling this
         */
        bool EverHadInlineUpdate() const {
            return everHadInlineUpdate;
        }

        /**
         * @brief Waits on a fence cycle if it exists till it's signalled and resets it after
         * @note The buffer **must** be locked prior to calling this
         */
        void WaitOnFence();

        /**
         * @brief Polls a fence cycle if it exists and resets it if signalled
         * @return Whether the fence cycle was signalled
         * @note The buffer **must** be locked prior to calling this
         */
        bool PollFence();

        /**
         * @brief Invalidates the Buffer on the guest and deletes the trap that backs this buffer as it is no longer necessary
         * @note This will not clear any views or delegates on the buffer, it will only remove guest mappings and delete the trap
         * @note The buffer **must** be locked prior to calling this
         */
        void Invalidate();

        /**
         * @brief Synchronizes the host buffer with the guest
         * @param skipTrap If true, setting up a CPU trap will be skipped
         * @note The buffer **must** be locked prior to calling this
         */
        void SynchronizeHost(bool skipTrap = false);

        /**
         * @brief Synchronizes the guest buffer with the host buffer
         * @param skipTrap If true, setting up a CPU trap will be skipped
         * @param nonBlocking If true, the call will return immediately if the fence is not signalled, skipping the sync
         * @return If the buffer's contents were successfully synchronized, this'll only be false on non-blocking operations or lack of a guest buffer
         * @note The buffer **must** be locked prior to calling this
         */
        bool SynchronizeGuest(bool skipTrap = false, bool nonBlocking = false);

        /**
         * @brief Synchronizes the guest buffer with the host buffer immediately, flushing GPU work if necessary
         * @param isFirstUsage If this is the first usage of this resource in the context as returned from LockWithTag(...)
         * @param flushHostCallback Callback to flush and execute all pending GPU work to allow for synchronisation of GPU dirty buffers
         * @note The buffer **must** be locked prior to calling this
         */
        void SynchronizeGuestImmediate(bool isFirstUsage, const std::function<void()> &flushHostCallback);

        /**
         * @brief Reads data at the specified offset in the buffer
         * @param isFirstUsage If this is the first usage of this resource in the context as returned from LockWithTag(...)
         * @param flushHostCallback Callback to flush and execute all pending GPU work to allow for synchronisation of GPU dirty buffers
         */
        void Read(bool isFirstUsage, const std::function<void()> &flushHostCallback, span<u8> data, vk::DeviceSize offset);

        /**
         * @brief Writes data at the specified offset in the buffer, falling back to GPU side copies if the buffer is host immutable
         * @param isFirstUsage If this is the first usage of this resource in the context as returned from LockWithTag(...)
         * @param flushHostCallback Callback to flush and execute all pending GPU work to allow for synchronisation of GPU dirty buffers
         * @param gpuCopyCallback Optional callback to perform a GPU-side copy for this Write if necessary, if such a copy is needed and this is not supplied `true` will be returned to indicate that the write needs to be repeated with the callback present
         * @return Whether the write needs to be repeated with `gpuCopyCallback` provided, always false if `gpuCopyCallback` is provided
         */
        bool Write(bool isFirstUsage, const std::function<void()> &flushHostCallback, span<u8> data, vk::DeviceSize offset, const std::function<void()> &gpuCopyCallback = {});

        /**
         * @return A view into this buffer with the supplied attributes
         * @note The buffer **must** be locked prior to calling this
         */
        BufferView GetView(vk::DeviceSize offset, vk::DeviceSize size);

        /**
         * @return A view into this buffer containing the given mapping, if the buffer doesn't contain the mapping an empty view will be returned
         * @note The buffer **must** be locked prior to calling this
         */
        BufferView TryGetView(span<u8> mapping);

        /**
         * @brief Attempts to return the current sequence number and prepare the buffer for read accesses from the returned span
         * @return The current sequence number and a span of the buffers guest mirror given that the buffer is not GPU dirty, if it is then a zero sequence number is returned
         * @note The contents of the returned span can be cached safely given the sequence number is unchanged
         * @note The buffer **must** be locked prior to calling this
         * @note An implicit CPU -> GPU sync will be performed when calling this, an immediate GPU -> CPU sync will also be attempted if the buffer is GPU dirty
         */
        std::pair<u64, span<u8>> AcquireCurrentSequence();

        /**
         * @brief Increments the sequence number of the buffer, any futher calls to AcquireCurrentSequence will return this new sequence number. See the comment for `sequenceNumber`
         * @note The buffer **must** be locked prior to calling this
         * @note This **must** be called after any modifications of the backing buffer data (but not mirror)
         */
        void AdvanceSequence();

        /**
         * @param isFirstUsage If this is the first usage of this resource in the context as returned from LockWithTag(...)
         * @param flushHostCallback Callback to flush and execute all pending GPU work to allow for synchronisation of GPU dirty buffers
         * @return A span of the backing buffer contents
         * @note The returned span **must** not be written to
         * @note The buffer **must** be kept locked until the span is no longer in use
         */
        span<u8> GetReadOnlyBackingSpan(bool isFirstUsage, const std::function<void()> &flushHostCallback);
    };

    /**
     * @brief A delegate for a strong reference to a Buffer by a BufferView which can be changed to another Buffer transparently
     */
    class BufferDelegate {
      private:
        union {
            BufferDelegate *link{};
            Buffer *buffer;
        };
        vk::DeviceSize offset{};

        bool linked{};

      public:
        BufferDelegate(Buffer *buffer);

        /**
         * @brief Follows links to get the underlying target buffer of the delegate
         */
        Buffer *GetBuffer();

        /**
         * @brief Links the delegate to target a new buffer object
         * @note Both the current target buffer object and new target buffer object **must** be locked prior to calling this
         */
        void Link(BufferDelegate *newTarget, vk::DeviceSize newOffset);

        /**
         * @return The offset of the delegate in the buffer
         * @note The target buffer **must** be locked prior to calling this
         */
        vk::DeviceSize GetOffset();
    };

    /**
     * @brief A contiguous view into a Vulkan Buffer that represents a single guest buffer (as opposed to Buffer objects which contain multiple)
     * @note The object **must** be locked prior to accessing any members as values will be mutated
     * @note This class conforms to the Lockable and BasicLockable C++ named requirements
     */
    class BufferView {
      private:
        constexpr static vk::DeviceSize MegaBufferingDisableThreshold{1024 * 128}; //!< The threshold at which the view is considered to be too large to be megabuffered (128KiB)

        BufferDelegate *delegate{};
        vk::DeviceSize offset{};

        /**
         * @brief Resolves the delegate's pointer chain so it directly points to the target buffer, updating offset accordingly
         * @note The view **must** be locked prior to calling this
         */
        void ResolveDelegate();

      public:
        vk::DeviceSize size{};

        BufferView();

        BufferView(BufferDelegate *delegate, vk::DeviceSize offset, vk::DeviceSize size);

        /**
         * @return A pointer to the current underlying buffer of the view
         * @note The view **must** be locked prior to calling this
         */
        Buffer *GetBuffer() const;

        /**
         * @return The offset of the view in the underlying buffer
         * @note The view **must** be locked prior to calling this
         */
        vk::DeviceSize GetOffset() const;

        /**
         * @brief Templated lock function that ensures correct locking of the delegate's underlying buffer
         */
        template<bool TryLock, typename LockFunction, typename UnlockFunction>
        std::conditional_t<TryLock, bool, void> LockWithFunction(LockFunction lock, UnlockFunction unlock) {
            while (true) {
                auto preLockBuffer{delegate->GetBuffer()};
                if constexpr (TryLock) {
                    if (!lock(preLockBuffer))
                        return false;
                } else {
                    lock(preLockBuffer);
                }
                auto postLockBuffer{delegate->GetBuffer()};
                if (preLockBuffer == postLockBuffer)
                    break;

                preLockBuffer->unlock();
            };

            ResolveDelegate();

            if constexpr (TryLock)
                return true;
            else
                return;
        }

        void lock() {
            LockWithFunction<false>([](Buffer *buffer) { buffer->lock(); }, [](Buffer *buffer) { buffer->unlock(); });
        }

        bool try_lock() {
            return LockWithFunction<true>([](Buffer *buffer) { return buffer->try_lock(); }, [](Buffer *buffer) { buffer->unlock(); });
        }

        bool LockWithTag(ContextTag tag) {
            bool result{};
            LockWithFunction<false>([&result, tag](Buffer *buffer) { result = buffer->LockWithTag(tag); }, [](Buffer *buffer) { buffer->unlock(); });
            return result;
        }

        void unlock() {
            delegate->GetBuffer()->unlock();
        }

        /**
         * @brief Reads data at the specified offset in the view
         * @note The view **must** be locked prior to calling this
         * @note See Buffer::Read
         */
        void Read(bool isFirstUsage, const std::function<void()> &flushHostCallback, span<u8> data, vk::DeviceSize readOffset) const;

        /**
         * @brief Writes data at the specified offset in the view
         * @note The view **must** be locked prior to calling this
         * @note See Buffer::Write
         */
        bool Write(bool isFirstUsage, const std::shared_ptr<FenceCycle> &cycle, const std::function<void()> &flushHostCallback, span<u8> data, vk::DeviceSize writeOffset, const std::function<void()> &gpuCopyCallback = {}) const;

        /**
         * @brief If megabuffering is beneficial for the view, pushes its contents into the megabuffer and returns the offset of the pushed data
         * @return The megabuffer allocation for the view, may be invalid if megabuffering is not beneficial
         * @note The view **must** be locked prior to calling this
         */
        MegaBufferAllocator::Allocation AcquireMegaBuffer(const std::shared_ptr<FenceCycle> &pCycle, MegaBufferAllocator &allocator) const;

        /**
         * @return A span of the backing buffer contents
         * @note The returned span **must** not be written to
         * @note The view **must** be kept locked until the span is no longer in use
         * @note See Buffer::GetReadOnlyBackingSpan
         */
        span<u8> GetReadOnlyBackingSpan(bool isFirstUsage, const std::function<void()> &flushHostCallback);

        constexpr operator bool() {
            return delegate != nullptr;
        }
    };
}
