/*
 * Copyright (C) 2008 The Android Open Source Project
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
/*
 * Garbage-collecting memory allocator.
 */
#include "Dalvik.h"
#include "alloc/HeapBitmap.h"
#include "alloc/Verify.h"
#include "alloc/HeapTable.h"
#include "alloc/Heap.h"
#include "alloc/HeapInternal.h"
#include "alloc/DdmHeap.h"
#include "alloc/HeapSource.h"
#include "alloc/MarkSweep.h"

#include "utils/threads.h"      // need Android thread priorities
#define kInvalidPriority        10000

#include <cutils/sched_policy.h>

#include <sys/time.h>
#include <sys/resource.h>
#include <limits.h>
#include <errno.h>

static const char* GcReasonStr[] = {
    [GC_FOR_MALLOC] = "GC_FOR_MALLOC",
    [GC_EXPLICIT] = "GC_EXPLICIT",
    [GC_EXTERNAL_ALLOC] = "GC_EXTERNAL_ALLOC",
    [GC_HPROF_DUMP_HEAP] = "GC_HPROF_DUMP_HEAP"
};

/*
 * Initialize the GC heap.
 *
 * Returns true if successful, false otherwise.
 */
bool dvmHeapStartup()
{
    GcHeap *gcHeap;

#if defined(WITH_ALLOC_LIMITS)
    gDvm.checkAllocLimits = false;
    gDvm.allocationLimit = -1;
#endif

    gcHeap = dvmHeapSourceStartup(gDvm.heapSizeStart, gDvm.heapSizeMax);
    if (gcHeap == NULL) {
        return false;
    }
    gcHeap->heapWorkerCurrentObject = NULL;
    gcHeap->heapWorkerCurrentMethod = NULL;
    gcHeap->heapWorkerInterpStartTime = 0LL;
    gcHeap->ddmHpifWhen = 0;
    gcHeap->ddmHpsgWhen = 0;
    gcHeap->ddmHpsgWhat = 0;
    gcHeap->ddmNhsgWhen = 0;
    gcHeap->ddmNhsgWhat = 0;
#if WITH_HPROF
    gcHeap->hprofDumpOnGc = false;
    gcHeap->hprofContext = NULL;
#endif
    gDvm.gcHeap = gcHeap;

    /* Set up the lists and lock we'll use for finalizable
     * and reference objects.
     */
    dvmInitMutex(&gDvm.heapWorkerListLock);
    gcHeap->finalizableRefs = NULL;
    gcHeap->pendingFinalizationRefs = NULL;
    gcHeap->referenceOperations = NULL;

    /* Initialize the HeapWorker locks and other state
     * that the GC uses.
     */
    dvmInitializeHeapWorkerState();

    return true;
}

void dvmHeapStartupAfterZygote()
{
    /* Update our idea of the last GC start time so that we
     * don't use the last time that Zygote happened to GC.
     */
    gDvm.gcHeap->gcStartTime = dvmGetRelativeTimeUsec();
}

void dvmHeapShutdown()
{
//TODO: make sure we're locked
    if (gDvm.gcHeap != NULL) {
        /* Tables are allocated on the native heap;
         * they need to be cleaned up explicitly.
         * The process may stick around, so we don't
         * want to leak any native memory.
         */
        dvmHeapFreeLargeTable(gDvm.gcHeap->finalizableRefs);
        gDvm.gcHeap->finalizableRefs = NULL;

        dvmHeapFreeLargeTable(gDvm.gcHeap->pendingFinalizationRefs);
        gDvm.gcHeap->pendingFinalizationRefs = NULL;

        dvmHeapFreeLargeTable(gDvm.gcHeap->referenceOperations);
        gDvm.gcHeap->referenceOperations = NULL;

        /* Destroy the heap.  Any outstanding pointers
         * will point to unmapped memory (unless/until
         * someone else maps it).  This frees gDvm.gcHeap
         * as a side-effect.
         */
        dvmHeapSourceShutdown(&gDvm.gcHeap);
    }
}

/*
 * We've been asked to allocate something we can't, e.g. an array so
 * large that (length * elementWidth) is larger than 2^31.
 *
 * _The Java Programming Language_, 4th edition, says, "you can be sure
 * that all SoftReferences to softly reachable objects will be cleared
 * before an OutOfMemoryError is thrown."
 *
 * It's unclear whether that holds for all situations where an OOM can
 * be thrown, or just in the context of an allocation that fails due
 * to lack of heap space.  For simplicity we just throw the exception.
 *
 * (OOM due to actually running out of space is handled elsewhere.)
 */
void dvmThrowBadAllocException(const char* msg)
{
    dvmThrowException("Ljava/lang/OutOfMemoryError;", msg);
}

/*
 * Grab the lock, but put ourselves into THREAD_VMWAIT if it looks like
 * we're going to have to wait on the mutex.
 */
bool dvmLockHeap()
{
    if (dvmTryLockMutex(&gDvm.gcHeapLock) != 0) {
        Thread *self;
        ThreadStatus oldStatus;

        self = dvmThreadSelf();
        if (self != NULL) {
            oldStatus = dvmChangeStatus(self, THREAD_VMWAIT);
        } else {
            LOGI("ODD: waiting on heap lock, no self\n");
            oldStatus = -1; // shut up gcc
        }
        dvmLockMutex(&gDvm.gcHeapLock);
        if (self != NULL) {
            dvmChangeStatus(self, oldStatus);
        }
    }

    return true;
}

void dvmUnlockHeap()
{
    dvmUnlockMutex(&gDvm.gcHeapLock);
}

/* Pop an object from the list of pending finalizations and
 * reference clears/enqueues, and return the object.
 * The caller must call dvmReleaseTrackedAlloc()
 * on the object when finished.
 *
 * Typically only called by the heap worker thread.
 */
Object *dvmGetNextHeapWorkerObject(HeapWorkerOperation *op)
{
    Object *obj;
    GcHeap *gcHeap = gDvm.gcHeap;

    assert(op != NULL);

    dvmLockMutex(&gDvm.heapWorkerListLock);

    obj = dvmHeapGetNextObjectFromLargeTable(&gcHeap->referenceOperations);
    if (obj != NULL) {
        *op = WORKER_ENQUEUE;
    } else {
        obj = dvmHeapGetNextObjectFromLargeTable(
                &gcHeap->pendingFinalizationRefs);
        if (obj != NULL) {
            *op = WORKER_FINALIZE;
        }
    }

    if (obj != NULL) {
        /* Don't let the GC collect the object until the
         * worker thread is done with it.
         */
        dvmAddTrackedAlloc(obj, NULL);
    }

    dvmUnlockMutex(&gDvm.heapWorkerListLock);

    return obj;
}

/* Whenever the effective heap size may have changed,
 * this function must be called.
 */
void dvmHeapSizeChanged()
{
}

/* Do a full garbage collection, which may grow the
 * heap as a side-effect if the live set is large.
 */
static void gcForMalloc(bool collectSoftReferences)
{
#ifdef WITH_PROFILER
    if (gDvm.allocProf.enabled) {
        Thread* self = dvmThreadSelf();
        gDvm.allocProf.gcCount++;
        if (self != NULL) {
            self->allocProf.gcCount++;
        }
    }
#endif
    /* This may adjust the soft limit as a side-effect.
     */
    LOGD_HEAP("dvmMalloc initiating GC%s\n",
            collectSoftReferences ? "(collect SoftReferences)" : "");
    dvmCollectGarbageInternal(collectSoftReferences, GC_FOR_MALLOC);
}

/* Try as hard as possible to allocate some memory.
 */
static void *tryMalloc(size_t size)
{
    void *ptr;

    /* Don't try too hard if there's no way the allocation is
     * going to succeed.  We have to collect SoftReferences before
     * throwing an OOME, though.
     */
    if (size >= gDvm.heapSizeMax) {
        LOGW_HEAP("dvmMalloc(%zu/0x%08zx): "
                "someone's allocating a huge buffer\n", size, size);
        ptr = NULL;
        goto collect_soft_refs;
    }

//TODO: figure out better heuristics
//    There will be a lot of churn if someone allocates a bunch of
//    big objects in a row, and we hit the frag case each time.
//    A full GC for each.
//    Maybe we grow the heap in bigger leaps
//    Maybe we skip the GC if the size is large and we did one recently
//      (number of allocations ago) (watch for thread effects)
//    DeflateTest allocs a bunch of ~128k buffers w/in 0-5 allocs of each other
//      (or, at least, there are only 0-5 objects swept each time)

    ptr = dvmHeapSourceAlloc(size);
    if (ptr != NULL) {
        return ptr;
    }

    /* The allocation failed.  Free up some space by doing
     * a full garbage collection.  This may grow the heap
     * if the live set is sufficiently large.
     */
    gcForMalloc(false);
    ptr = dvmHeapSourceAlloc(size);
    if (ptr != NULL) {
        return ptr;
    }

    /* Even that didn't work;  this is an exceptional state.
     * Try harder, growing the heap if necessary.
     */
    ptr = dvmHeapSourceAllocAndGrow(size);
    dvmHeapSizeChanged();
    if (ptr != NULL) {
        size_t newHeapSize;

        newHeapSize = dvmHeapSourceGetIdealFootprint();
//TODO: may want to grow a little bit more so that the amount of free
//      space is equal to the old free space + the utilization slop for
//      the new allocation.
        LOGI_HEAP("Grow heap (frag case) to "
                "%zu.%03zuMB for %zu-byte allocation\n",
                FRACTIONAL_MB(newHeapSize), size);
        return ptr;
    }

    /* Most allocations should have succeeded by now, so the heap
     * is really full, really fragmented, or the requested size is
     * really big.  Do another GC, collecting SoftReferences this
     * time.  The VM spec requires that all SoftReferences have
     * been collected and cleared before throwing an OOME.
     */
//TODO: wait for the finalizers from the previous GC to finish
collect_soft_refs:
    LOGI_HEAP("Forcing collection of SoftReferences for %zu-byte allocation\n",
            size);
    gcForMalloc(true);
    ptr = dvmHeapSourceAllocAndGrow(size);
    dvmHeapSizeChanged();
    if (ptr != NULL) {
        return ptr;
    }
//TODO: maybe wait for finalizers and try one last time

    LOGE_HEAP("Out of memory on a %zd-byte allocation.\n", size);
//TODO: tell the HeapSource to dump its state
    dvmDumpThread(dvmThreadSelf(), false);

    return NULL;
}

/* Throw an OutOfMemoryError if there's a thread to attach it to.
 * Avoid recursing.
 *
 * The caller must not be holding the heap lock, or else the allocations
 * in dvmThrowException() will deadlock.
 */
static void throwOOME()
{
    Thread *self;

    if ((self = dvmThreadSelf()) != NULL) {
        /* If the current (failing) dvmMalloc() happened as part of thread
         * creation/attachment before the thread became part of the root set,
         * we can't rely on the thread-local trackedAlloc table, so
         * we can't keep track of a real allocated OOME object.  But, since
         * the thread is in the process of being created, it won't have
         * a useful stack anyway, so we may as well make things easier
         * by throwing the (stackless) pre-built OOME.
         */
        if (dvmIsOnThreadList(self) && !self->throwingOOME) {
            /* Let ourselves know that we tried to throw an OOM
             * error in the normal way in case we run out of
             * memory trying to allocate it inside dvmThrowException().
             */
            self->throwingOOME = true;

            /* Don't include a description string;
             * one fewer allocation.
             */
            dvmThrowException("Ljava/lang/OutOfMemoryError;", NULL);
        } else {
            /*
             * This thread has already tried to throw an OutOfMemoryError,
             * which probably means that we're running out of memory
             * while recursively trying to throw.
             *
             * To avoid any more allocation attempts, "throw" a pre-built
             * OutOfMemoryError object (which won't have a useful stack trace).
             *
             * Note that since this call can't possibly allocate anything,
             * we don't care about the state of self->throwingOOME
             * (which will usually already be set).
             */
            dvmSetException(self, gDvm.outOfMemoryObj);
        }
        /* We're done with the possible recursion.
         */
        self->throwingOOME = false;
    }
}

/*
 * Allocate storage on the GC heap.  We guarantee 8-byte alignment.
 *
 * The new storage is zeroed out.
 *
 * Note that, in rare cases, this could get called while a GC is in
 * progress.  If a non-VM thread tries to attach itself through JNI,
 * it will need to allocate some objects.  If this becomes annoying to
 * deal with, we can block it at the source, but holding the allocation
 * mutex should be enough.
 *
 * In rare circumstances (JNI AttachCurrentThread) we can be called
 * from a non-VM thread.
 *
 * Use ALLOC_DONT_TRACK when we either don't want to track an allocation
 * (because it's being done for the interpreter "new" operation and will
 * be part of the root set immediately) or we can't (because this allocation
 * is for a brand new thread).
 *
 * Returns NULL and throws an exception on failure.
 *
 * TODO: don't do a GC if the debugger thinks all threads are suspended
 */
void* dvmMalloc(size_t size, int flags)
{
    GcHeap *gcHeap = gDvm.gcHeap;
    void *ptr;

#if defined(WITH_ALLOC_LIMITS)
    /*
     * See if they've exceeded the allocation limit for this thread.
     *
     * A limit value of -1 means "no limit".
     *
     * This is enabled at compile time because it requires us to do a
     * TLS lookup for the Thread pointer.  This has enough of a performance
     * impact that we don't want to do it if we don't have to.  (Now that
     * we're using gDvm.checkAllocLimits we may want to reconsider this,
     * but it's probably still best to just compile the check out of
     * production code -- one less thing to hit on every allocation.)
     */
    if (gDvm.checkAllocLimits) {
        Thread* self = dvmThreadSelf();
        if (self != NULL) {
            int count = self->allocLimit;
            if (count > 0) {
                self->allocLimit--;
            } else if (count == 0) {
                /* fail! */
                assert(!gDvm.initializing);
                self->allocLimit = -1;
                dvmThrowException("Ldalvik/system/AllocationLimitError;",
                    "thread allocation limit exceeded");
                return NULL;
            }
        }
    }

    if (gDvm.allocationLimit >= 0) {
        assert(!gDvm.initializing);
        gDvm.allocationLimit = -1;
        dvmThrowException("Ldalvik/system/AllocationLimitError;",
            "global allocation limit exceeded");
        return NULL;
    }
#endif

    dvmLockHeap();

    /* Try as hard as possible to allocate some memory.
     */
    ptr = tryMalloc(size);
    if (ptr != NULL) {
        /* We've got the memory.
         */
        if ((flags & ALLOC_FINALIZABLE) != 0) {
            /* This object is an instance of a class that
             * overrides finalize().  Add it to the finalizable list.
             */
            if (!dvmHeapAddRefToLargeTable(&gcHeap->finalizableRefs,
                                    (Object *)ptr))
            {
                LOGE_HEAP("dvmMalloc(): no room for any more "
                        "finalizable objects\n");
                dvmAbort();
            }
        }

#ifdef WITH_PROFILER
        if (gDvm.allocProf.enabled) {
            Thread* self = dvmThreadSelf();
            gDvm.allocProf.allocCount++;
            gDvm.allocProf.allocSize += size;
            if (self != NULL) {
                self->allocProf.allocCount++;
                self->allocProf.allocSize += size;
            }
        }
#endif
    } else {
        /* The allocation failed.
         */

#ifdef WITH_PROFILER
        if (gDvm.allocProf.enabled) {
            Thread* self = dvmThreadSelf();
            gDvm.allocProf.failedAllocCount++;
            gDvm.allocProf.failedAllocSize += size;
            if (self != NULL) {
                self->allocProf.failedAllocCount++;
                self->allocProf.failedAllocSize += size;
            }
        }
#endif
    }

    dvmUnlockHeap();

    if (ptr != NULL) {
        /*
         * If caller hasn't asked us not to track it, add it to the
         * internal tracking list.
         */
        if ((flags & ALLOC_DONT_TRACK) == 0) {
            dvmAddTrackedAlloc(ptr, NULL);
        }
    } else {
        /*
         * The allocation failed; throw an OutOfMemoryError.
         */
        throwOOME();
    }

    return ptr;
}

/*
 * Returns true iff <obj> points to a valid allocated object.
 */
bool dvmIsValidObject(const Object* obj)
{
    /* Don't bother if it's NULL or not 8-byte aligned.
     */
    if (obj != NULL && ((uintptr_t)obj & (8-1)) == 0) {
        /* Even if the heap isn't locked, this shouldn't return
         * any false negatives.  The only mutation that could
         * be happening is allocation, which means that another
         * thread could be in the middle of a read-modify-write
         * to add a new bit for a new object.  However, that
         * RMW will have completed by the time any other thread
         * could possibly see the new pointer, so there is no
         * danger of dvmIsValidObject() being called on a valid
         * pointer whose bit isn't set.
         *
         * Freeing will only happen during the sweep phase, which
         * only happens while the heap is locked.
         */
        return dvmHeapSourceContains(obj);
    }
    return false;
}

/*
 * Returns true iff <obj> points to a word-aligned address within Heap
 * address space.
 */
bool dvmIsValidObjectAddress(const void* ptr)
{
    /* Don't bother if it's not 4-byte aligned.
     */
    if (((uintptr_t)ptr & (4-1)) == 0) {
        return dvmHeapSourceContainsAddress(ptr);
    }
    return false;
}

size_t dvmObjectSizeInHeap(const Object *obj)
{
    return dvmHeapSourceChunkSize(obj);
}

/*
 * Scan every live object in the heap, holding the locks.
 */
static void verifyHeap()
{
    // TODO: check the locks.
    HeapBitmap *liveBits = dvmHeapSourceGetLiveBits();
    dvmVerifyBitmap(liveBits);
}

/*
 * Suspend the VM as for a GC, and assert-fail if any object has any
 * corrupt references.
 */
void dvmHeapSuspendAndVerify()
{
    /* Suspend the VM. */
    dvmSuspendAllThreads(SUSPEND_FOR_VERIFY);
    dvmLockMutex(&gDvm.heapWorkerLock);
    dvmAssertHeapWorkerThreadRunning();
    dvmLockMutex(&gDvm.heapWorkerListLock);

    verifyHeap();

    /* Resume the VM. */
    dvmUnlockMutex(&gDvm.heapWorkerListLock);
    dvmUnlockMutex(&gDvm.heapWorkerLock);
    dvmResumeAllThreads(SUSPEND_FOR_VERIFY);
}

/*
 * Initiate garbage collection.
 *
 * NOTES:
 * - If we don't hold gDvm.threadListLock, it's possible for a thread to
 *   be added to the thread list while we work.  The thread should NOT
 *   start executing, so this is only interesting when we start chasing
 *   thread stacks.  (Before we do so, grab the lock.)
 *
 * We are not allowed to GC when the debugger has suspended the VM, which
 * is awkward because debugger requests can cause allocations.  The easiest
 * way to enforce this is to refuse to GC on an allocation made by the
 * JDWP thread -- we have to expand the heap or fail.
 */
void dvmCollectGarbageInternal(bool clearSoftRefs, GcReason reason)
{
    GcHeap *gcHeap = gDvm.gcHeap;
    u8 now;
    s8 timeSinceLastGc;
    s8 gcElapsedTime;
    int numFreed;
    size_t sizeFreed;
    GcMode gcMode;

    /* The heap lock must be held.
     */

    if (gcHeap->gcRunning) {
        LOGW_HEAP("Attempted recursive GC\n");
        return;
    }
    gcMode = (reason == GC_FOR_MALLOC) ? GC_PARTIAL : GC_FULL;
    gcHeap->gcRunning = true;
    now = dvmGetRelativeTimeUsec();
    if (gcHeap->gcStartTime != 0) {
        timeSinceLastGc = (now - gcHeap->gcStartTime) / 1000;
    } else {
        timeSinceLastGc = 0;
    }
    gcHeap->gcStartTime = now;

    LOGV_HEAP("%s starting -- suspending threads\n", GcReasonStr[reason]);

    dvmSuspendAllThreads(SUSPEND_FOR_GC);

    /* Get the priority (the "nice" value) of the current thread.  The
     * getpriority() call can legitimately return -1, so we have to
     * explicitly test errno.
     */
    errno = 0;
    int oldThreadPriority = kInvalidPriority;
    int priorityResult = getpriority(PRIO_PROCESS, 0);
    if (errno != 0) {
        LOGI_HEAP("getpriority(self) failed: %s\n", strerror(errno));
    } else if (priorityResult > ANDROID_PRIORITY_NORMAL) {
        /* Current value is numerically greater than "normal", which
         * in backward UNIX terms means lower priority.
         */

        if (priorityResult >= ANDROID_PRIORITY_BACKGROUND) {
            set_sched_policy(dvmGetSysThreadId(), SP_FOREGROUND);
        }

        if (setpriority(PRIO_PROCESS, 0, ANDROID_PRIORITY_NORMAL) != 0) {
            LOGI_HEAP("Unable to elevate priority from %d to %d\n",
                priorityResult, ANDROID_PRIORITY_NORMAL);
        } else {
            /* priority elevated; save value so we can restore it later */
            LOGD_HEAP("Elevating priority from %d to %d\n",
                priorityResult, ANDROID_PRIORITY_NORMAL);
            oldThreadPriority = priorityResult;
        }
    }

    /* Wait for the HeapWorker thread to block.
     * (It may also already be suspended in interp code,
     * in which case it's not holding heapWorkerLock.)
     */
    dvmLockMutex(&gDvm.heapWorkerLock);

    /* Make sure that the HeapWorker thread hasn't become
     * wedged inside interp code.  If it has, this call will
     * print a message and abort the VM.
     */
    dvmAssertHeapWorkerThreadRunning();

    /* Lock the pendingFinalizationRefs list.
     *
     * Acquire the lock after suspending so the finalizer
     * thread can't block in the RUNNING state while
     * we try to suspend.
     */
    dvmLockMutex(&gDvm.heapWorkerListLock);

    if (gDvm.preVerify) {
        LOGV_HEAP("Verifying heap before GC");
        verifyHeap();
    }

#ifdef WITH_PROFILER
    dvmMethodTraceGCBegin();
#endif

#if WITH_HPROF

/* Set DUMP_HEAP_ON_DDMS_UPDATE to 1 to enable heap dumps
 * whenever DDMS requests a heap update (HPIF chunk).
 * The output files will appear in /data/misc, which must
 * already exist.
 * You must define "WITH_HPROF := true" in your buildspec.mk
 * and recompile libdvm for this to work.
 *
 * To enable stack traces for each allocation, define
 * "WITH_HPROF_STACK := true" in buildspec.mk.  This option slows down
 * allocations and also requires 8 additional bytes per object on the
 * GC heap.
 */
#define DUMP_HEAP_ON_DDMS_UPDATE 0
#if DUMP_HEAP_ON_DDMS_UPDATE
    gcHeap->hprofDumpOnGc |= (gcHeap->ddmHpifWhen != 0);
#endif

    if (gcHeap->hprofDumpOnGc) {
        char nameBuf[128];

        gcHeap->hprofResult = -1;

        if (gcHeap->hprofFileName == NULL) {
            /* no filename was provided; invent one */
            sprintf(nameBuf, "/data/misc/heap-dump-tm%d-pid%d.hprof",
                (int) time(NULL), (int) getpid());
            gcHeap->hprofFileName = nameBuf;
        }
        gcHeap->hprofContext = hprofStartup(gcHeap->hprofFileName,
                gcHeap->hprofDirectToDdms);
        if (gcHeap->hprofContext != NULL) {
            hprofStartHeapDump(gcHeap->hprofContext);
        }
        gcHeap->hprofDumpOnGc = false;
        gcHeap->hprofFileName = NULL;
    }
#endif

    if (timeSinceLastGc < 10000) {
        LOGD_HEAP("GC! (%dms since last GC)\n",
                (int)timeSinceLastGc);
    } else {
        LOGD_HEAP("GC! (%d sec since last GC)\n",
                (int)(timeSinceLastGc / 1000));
    }

    /* Set up the marking context.
     */
    if (!dvmHeapBeginMarkStep(gcMode)) {
        LOGE_HEAP("dvmHeapBeginMarkStep failed; aborting\n");
        dvmAbort();
    }

    /* Mark the set of objects that are strongly reachable from the roots.
     */
    LOGD_HEAP("Marking...");
    dvmHeapMarkRootSet();

    /* dvmHeapScanMarkedObjects() will build the lists of known
     * instances of the Reference classes.
     */
    gcHeap->softReferences = NULL;
    gcHeap->weakReferences = NULL;
    gcHeap->phantomReferences = NULL;

    /* Recursively mark any objects that marked objects point to strongly.
     * If we're not collecting soft references, soft-reachable
     * objects will also be marked.
     */
    LOGD_HEAP("Recursing...");
    dvmHeapScanMarkedObjects();

    /* All strongly-reachable objects have now been marked.
     */
    LOGD_HEAP("Handling soft references...");
    if (!clearSoftRefs) {
        dvmHandleSoftRefs(&gcHeap->softReferences);
    }
    dvmClearWhiteRefs(&gcHeap->softReferences);

    LOGD_HEAP("Handling weak references...");
    dvmClearWhiteRefs(&gcHeap->weakReferences);

    /* Once all weak-reachable objects have been taken
     * care of, any remaining unmarked objects can be finalized.
     */
    LOGD_HEAP("Finding finalizations...");
    dvmHeapScheduleFinalizations();

    LOGD_HEAP("Handling f-reachable soft references...");
    dvmClearWhiteRefs(&gcHeap->softReferences);

    LOGD_HEAP("Handling f-reachable weak references...");
    dvmClearWhiteRefs(&gcHeap->weakReferences);

    /* Any remaining objects that are not pending finalization
     * could be phantom-reachable.  This will mark any phantom-reachable
     * objects, as well as enqueue their references.
     */
    LOGD_HEAP("Handling phantom references...");
    dvmClearWhiteRefs(&gcHeap->phantomReferences);

#ifdef WITH_DEADLOCK_PREDICTION
    dvmDumpMonitorInfo("before sweep");
#endif
    LOGD_HEAP("Sweeping...");
    dvmHeapSweepUnmarkedObjects(gcMode, &numFreed, &sizeFreed);
#ifdef WITH_DEADLOCK_PREDICTION
    dvmDumpMonitorInfo("after sweep");
#endif

    LOGD_HEAP("Cleaning up...");
    dvmHeapFinishMarkStep();

    LOGD_HEAP("Done.");

    /* Now's a good time to adjust the heap size, since
     * we know what our utilization is.
     *
     * This doesn't actually resize any memory;
     * it just lets the heap grow more when necessary.
     */
    dvmHeapSourceGrowForUtilization();
    dvmHeapSizeChanged();

#if WITH_HPROF
    if (gcHeap->hprofContext != NULL) {
        hprofFinishHeapDump(gcHeap->hprofContext);
//TODO: write a HEAP_SUMMARY record
        if (hprofShutdown(gcHeap->hprofContext))
            gcHeap->hprofResult = 0;    /* indicate success */
        gcHeap->hprofContext = NULL;
    }
#endif

    /* Now that we've freed up the GC heap, return any large
     * free chunks back to the system.  They'll get paged back
     * in the next time they're used.  Don't do it immediately,
     * though;  if the process is still allocating a bunch of
     * memory, we'll be taking a ton of page faults that we don't
     * necessarily need to.
     *
     * Cancel any old scheduled trims, and schedule a new one.
     */
    dvmScheduleHeapSourceTrim(5);  // in seconds

#ifdef WITH_PROFILER
    dvmMethodTraceGCEnd();
#endif
    LOGV_HEAP("GC finished");

    if (gDvm.postVerify) {
        LOGV_HEAP("Verifying heap after GC");
        verifyHeap();
    }

    gcHeap->gcRunning = false;

    LOGV_HEAP("Resuming threads");
    dvmUnlockMutex(&gDvm.heapWorkerListLock);
    dvmUnlockMutex(&gDvm.heapWorkerLock);

#if defined(WITH_JIT)
    /*
     * Patching a chaining cell is very cheap as it only updates 4 words. It's
     * the overhead of stopping all threads and synchronizing the I/D cache
     * that makes it expensive.
     *
     * Therefore we batch those work orders in a queue and go through them
     * when threads are suspended for GC.
     */
    dvmCompilerPerformSafePointChecks();
#endif

    dvmResumeAllThreads(SUSPEND_FOR_GC);
    if (oldThreadPriority != kInvalidPriority) {
        if (setpriority(PRIO_PROCESS, 0, oldThreadPriority) != 0) {
            LOGW_HEAP("Unable to reset priority to %d: %s\n",
                oldThreadPriority, strerror(errno));
        } else {
            LOGD_HEAP("Reset priority to %d\n", oldThreadPriority);
        }

        if (oldThreadPriority >= ANDROID_PRIORITY_BACKGROUND) {
            set_sched_policy(dvmGetSysThreadId(), SP_BACKGROUND);
        }
    }
    gcElapsedTime = (dvmGetRelativeTimeUsec() - gcHeap->gcStartTime) / 1000;
    LOGD("%s freed %d objects / %zd bytes in %dms\n",
         GcReasonStr[reason], numFreed, sizeFreed, (int)gcElapsedTime);
    dvmLogGcStats(numFreed, sizeFreed, gcElapsedTime);

    if (gcHeap->ddmHpifWhen != 0) {
        LOGD_HEAP("Sending VM heap info to DDM\n");
        dvmDdmSendHeapInfo(gcHeap->ddmHpifWhen, false);
    }
    if (gcHeap->ddmHpsgWhen != 0) {
        LOGD_HEAP("Dumping VM heap to DDM\n");
        dvmDdmSendHeapSegments(false, false);
    }
    if (gcHeap->ddmNhsgWhen != 0) {
        LOGD_HEAP("Dumping native heap to DDM\n");
        dvmDdmSendHeapSegments(false, true);
    }
}

#if WITH_HPROF
/*
 * Perform garbage collection, writing heap information to the specified file.
 *
 * If "fileName" is NULL, a suitable name will be generated automatically.
 *
 * Returns 0 on success, or an error code on failure.
 */
int hprofDumpHeap(const char* fileName, bool directToDdms)
{
    int result;

    dvmLockMutex(&gDvm.gcHeapLock);

    gDvm.gcHeap->hprofDumpOnGc = true;
    gDvm.gcHeap->hprofFileName = fileName;
    gDvm.gcHeap->hprofDirectToDdms = directToDdms;
    dvmCollectGarbageInternal(false, GC_HPROF_DUMP_HEAP);
    result = gDvm.gcHeap->hprofResult;

    dvmUnlockMutex(&gDvm.gcHeapLock);

    return result;
}

void dvmHeapSetHprofGcScanState(hprof_heap_tag_t state, u4 threadSerialNumber)
{
    if (gDvm.gcHeap->hprofContext != NULL) {
        hprofSetGcScanState(gDvm.gcHeap->hprofContext, state,
                threadSerialNumber);
    }
}
#endif
