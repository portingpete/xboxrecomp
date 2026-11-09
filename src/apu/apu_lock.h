#ifndef MCPX_APU_LOCK_H
#define MCPX_APU_LOCK_H

/* Included after MCPXAPUState and the platform synchronization shims. External
 * callers announce contention before taking the recursive device mutex. */
static inline void mcpx_apu_lock(MCPXAPUState *d)
{
    InterlockedIncrement(&d->lock_waiters);
    qemu_mutex_lock(&d->lock);
    InterlockedDecrement(&d->lock_waiters);
    if (d->lock_handoff_waiting)
        qemu_cond_signal(&d->lock_handoff_cond);
}

/* Caller owns the device mutex once, at an audio epoch boundary. Yielding and
 * immediately reacquiring it can still starve a waiter under Wine. Wait for an
 * acquiring caller to signal instead. The timeout bounds audio's wait if that
 * caller is descheduled; no delay is added when the mutex is uncontended. */
static inline void mcpx_apu_handoff(MCPXAPUState *d, bool irq_declined)
{
    if (!irq_declined && !InterlockedCompareExchange(&d->lock_waiters, 0, 0))
        return;
    d->lock_handoff_waiting = true;
    qemu_cond_timedwait(&d->lock_handoff_cond, &d->lock, 1);
    d->lock_handoff_waiting = false;
}

#endif
