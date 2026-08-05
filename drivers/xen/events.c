/*
 * Copyright (c) 2021-2025 EPAM Systems
 * Copyright (c) 2022 Arm Limited (or its affiliates). All rights reserved.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT xen_xen

#include <xen/public/xen.h>
#include <xen/public/event_channel.h>

#include <zephyr/arch/arm64/hypercall.h>
#include <zephyr/xen/events.h>
#include <zephyr/sys/barrier.h>

#include <errno.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/irq.h>

LOG_MODULE_REGISTER(xen_events);

extern shared_info_t *HYPERVISOR_shared_info;

/*
 * Zephyr-owned per-port dispatch state.
 *
 * event_channels[] maps a Xen event-channel port to the callback and private
 * data that Zephyr should dispatch for that port. It also stores a Zephyr-only
 * sticky flag used when an event arrived but the port still had the empty
 * callback installed. Protected by event_state_lock.
 *
 * event_state_lock also serializes Zephyr-side access to shared
 * evtchn_pending[] and evtchn_mask[] words. Xen can still update these shared
 * words concurrently, so writes use atomic read/modify/write helpers instead
 * of sys_bitfield_* load/store helpers.
 *
 * Per-vCPU selector/upcall fields live in vcpu_info_t. Those fields are
 * consumed using the ordering and atomic read-clear rules defined by the Xen
 * shared-memory ABI.
 */
struct evtchn_handle {
	evtchn_cb_t cb;
	void *priv;
	bool missed;
};

static struct evtchn_handle event_channels[EVTCHN_2L_NR_CHANNELS];
static struct k_spinlock event_state_lock;

#define EVTCHN_WORD_BITS (8 * sizeof(xen_ulong_t))

/* Default handler for a port that has no Zephyr callback bound. */
static void empty_callback(void *data)
{
	ARG_UNUSED(data);
}

static xen_ulong_t shared_event_bit(evtchn_port_t port)
{
	return ((xen_ulong_t)1) << (port % EVTCHN_WORD_BITS);
}

static xen_ulong_t shared_event_word(const xen_ulong_t *bitmap, uint32_t word)
{
	return __atomic_load_n(&bitmap[word], __ATOMIC_SEQ_CST);
}

static void set_shared_event_bit(xen_ulong_t *bitmap, evtchn_port_t port)
{
	uint32_t word = port / EVTCHN_WORD_BITS;
	xen_ulong_t bit = shared_event_bit(port);

	(void)__atomic_fetch_or(&bitmap[word], bit, __ATOMIC_SEQ_CST);
}

static void clear_shared_event_bit(xen_ulong_t *bitmap, evtchn_port_t port)
{
	uint32_t word = port / EVTCHN_WORD_BITS;
	xen_ulong_t bit = shared_event_bit(port);

	(void)__atomic_fetch_and(&bitmap[word], ~bit, __ATOMIC_SEQ_CST);
}

int alloc_unbound_event_channel(domid_t remote_dom)
{
	int rc;
	struct evtchn_alloc_unbound alloc = {
		.dom = DOMID_SELF,
		.remote_dom = remote_dom,
	};

	rc = HYPERVISOR_event_channel_op(EVTCHNOP_alloc_unbound, &alloc);
	if (rc == 0) {
		rc = alloc.port;
	}

	return rc;
}

#ifdef CONFIG_XEN_DOM0
int alloc_unbound_event_channel_dom0(domid_t dom, domid_t remote_dom)
{
	int rc;
	struct evtchn_alloc_unbound alloc = {
		.dom = dom,
		.remote_dom = remote_dom,
	};

	rc = HYPERVISOR_event_channel_op(EVTCHNOP_alloc_unbound, &alloc);
	if (rc == 0) {
		rc = alloc.port;
	}

	return rc;
}
#endif /* CONFIG_XEN_DOM0 */

int bind_interdomain_event_channel(domid_t remote_dom, evtchn_port_t remote_port,
		evtchn_cb_t cb, void *data)
{
	int rc;
	struct evtchn_bind_interdomain bind = {
		.remote_dom = remote_dom,
		.remote_port = remote_port,
	};

	rc = HYPERVISOR_event_channel_op(EVTCHNOP_bind_interdomain, &bind);
	if (rc < 0) {
		return rc;
	}

	rc = bind_event_channel(bind.local_port, cb, data);
	if (rc < 0) {
		return rc;
	}

	return bind.local_port;
}

int evtchn_status(evtchn_status_t *status)
{
	return HYPERVISOR_event_channel_op(EVTCHNOP_status, status);
}

int evtchn_set_priority(evtchn_port_t port, uint32_t priority)
{
	struct evtchn_set_priority set = {
		.port = port,
		.priority = priority,
	};

	return HYPERVISOR_event_channel_op(EVTCHNOP_set_priority, &set);
}

int notify_evtchn(evtchn_port_t port)
{
	struct evtchn_send send;

	__ASSERT(port < EVTCHN_2L_NR_CHANNELS,
		"%s: trying to send notify for invalid evtchn #%u\n",
		__func__, port);

	send.port = port;

	return HYPERVISOR_event_channel_op(EVTCHNOP_send, &send);
}

/*
 * Reset Zephyr-owned state for an unused port. Callers must either hold
 * event_state_lock or run before event-channel interrupts are enabled.
 */
static void reset_event_channel_state(evtchn_port_t port)
{
	event_channels[port].cb = empty_callback;
	event_channels[port].priv = NULL;
	event_channels[port].missed = false;
}

int bind_event_channel(evtchn_port_t port, evtchn_cb_t cb, void *data)
{
	k_spinlock_key_t key;
	bool rebound;

	__ASSERT(port < EVTCHN_2L_NR_CHANNELS,
		"%s: trying to bind invalid evtchn #%u\n",
		__func__, port);
	__ASSERT(cb != NULL, "%s: NULL callback for evtchn #%u\n",
		__func__, port);

	key = k_spin_lock(&event_state_lock);

	rebound = (event_channels[port].cb != empty_callback);

	event_channels[port].priv = data;
	event_channels[port].cb = cb;
	event_channels[port].missed = false;

	k_spin_unlock(&event_state_lock, key);

	if (rebound) {
		LOG_WRN("%s: re-bind callback for evtchn #%u\n",
				__func__, port);
	}

	return 0;
}

int unbind_event_channel(evtchn_port_t port)
{
	k_spinlock_key_t key;

	__ASSERT(port < EVTCHN_2L_NR_CHANNELS,
		"%s: trying to unbind invalid evtchn #%u\n",
		__func__, port);

	key = k_spin_lock(&event_state_lock);

	event_channels[port].cb = empty_callback;
	event_channels[port].priv = NULL;
	event_channels[port].missed = false;

	k_spin_unlock(&event_state_lock, key);

	return 0;
}

int get_missed_events(evtchn_port_t port)
{
	k_spinlock_key_t key;
	bool missed;

	__ASSERT(port < EVTCHN_2L_NR_CHANNELS,
		"%s: trying to get missed event from invalid port #%u\n",
		__func__, port);

	key = k_spin_lock(&event_state_lock);
	missed = event_channels[port].missed;
	event_channels[port].missed = false;
	k_spin_unlock(&event_state_lock, key);

	if (missed) {
		return 1;
	}

	return 0;
}

int mask_event_channel(evtchn_port_t port)
{
	shared_info_t *s = HYPERVISOR_shared_info;
	k_spinlock_key_t key;

	__ASSERT(port < EVTCHN_2L_NR_CHANNELS,
		"%s: trying to mask invalid evtchn #%u\n",
		__func__, port);

	key = k_spin_lock(&event_state_lock);
	set_shared_event_bit(s->evtchn_mask, port);
	k_spin_unlock(&event_state_lock, key);

	return 0;
}

int unmask_event_channel(evtchn_port_t port)
{
	shared_info_t *s = HYPERVISOR_shared_info;
	k_spinlock_key_t key;

	__ASSERT(port < EVTCHN_2L_NR_CHANNELS,
		"%s: trying to unmask invalid evtchn #%u\n",
		__func__, port);

	key = k_spin_lock(&event_state_lock);
	clear_shared_event_bit(s->evtchn_mask, port);
	k_spin_unlock(&event_state_lock, key);

	return 0;
}

void clear_event_channel(evtchn_port_t port)
{
	shared_info_t *s = HYPERVISOR_shared_info;
	k_spinlock_key_t key;

	__ASSERT(port < EVTCHN_2L_NR_CHANNELS,
		"%s: trying to clear invalid evtchn #%u\n",
		__func__, port);

	key = k_spin_lock(&event_state_lock);
	clear_shared_event_bit(s->evtchn_pending, port);
	k_spin_unlock(&event_state_lock, key);
}

int evtchn_close(evtchn_port_t port)
{
	shared_info_t *s = HYPERVISOR_shared_info;
	struct evtchn_close close = {
		.port = port,
	};
	k_spinlock_key_t key;
	int rc;

	__ASSERT(port < EVTCHN_2L_NR_CHANNELS,
		"%s: trying to close invalid evtchn #%u\n",
		__func__, port);

	rc = HYPERVISOR_event_channel_op(EVTCHNOP_close, &close);
	if (rc != 0) {
		return rc;
	}

	/*
	 * Clear shared event-channel delivery state for a port that Xen has
	 * closed. This prevents a stale pending bit from being dispatched after
	 * Xen reuses the same port number for a later channel.
	 */
	key = k_spin_lock(&event_state_lock);
	set_shared_event_bit(s->evtchn_mask, port);
	clear_shared_event_bit(s->evtchn_pending, port);
	reset_event_channel_state(port);
	k_spin_unlock(&event_state_lock, key);

	return 0;
}

/*
 * Called while the ISR is draining the current CPU's evtchn_pending_sel.
 * pos selects one word in the domain-wide pending bitmap. The returned word has
 * one bit set for each unmasked port in that word that still needs dispatch.
 */
static inline xen_ulong_t get_pending_events(xen_ulong_t pos)
{
	shared_info_t *s = HYPERVISOR_shared_info;
	k_spinlock_key_t key;
	xen_ulong_t events;

	key = k_spin_lock(&event_state_lock);
	events = shared_event_word(s->evtchn_pending, pos) &
		 ~shared_event_word(s->evtchn_mask, pos);
	k_spin_unlock(&event_state_lock, key);

	return events;
}

/*
 * Dispatch one pending port. If the port has no bound callback, remember that
 * an event arrived so the caller can observe it later with get_missed_events().
 */
static void process_event(evtchn_port_t port)
{
	shared_info_t *s = HYPERVISOR_shared_info;
	struct evtchn_handle channel;
	k_spinlock_key_t key;
	bool missed;

	key = k_spin_lock(&event_state_lock);
	channel = event_channels[port];
	missed = (channel.cb == empty_callback);
	if (missed) {
		event_channels[port].missed = true;
	}
	clear_shared_event_bit(s->evtchn_pending, port);
	k_spin_unlock(&event_state_lock, key);

	if (!missed) {
		/*
		 * Invoke a consistent callback/private-data snapshot. The lock
		 * protects the table lookup, but must not be held while running
		 * driver-owned callback code.
		 */
		channel.cb(channel.priv);
	}
}

static void events_isr(void *data)
{
	ARG_UNUSED(data);

	/* Needed for 2-level unwrapping */
	xen_ulong_t pos_selector;   /* bits are positions in pending array */
	xen_ulong_t events_pending; /* bits - events in pos_selector element */
	uint32_t pos_index, event_index; /* bit indexes */

	evtchn_port_t port; /* absolute event index */

	/* TODO: SMP? XEN_LEGACY_MAX_VCPUS == 1*/
	vcpu_info_t *vcpu = &HYPERVISOR_shared_info->vcpu_info[0];

	/*
	 * Need to set it to 0 /before/ checking for pending work, thus
	 * avoiding a set-and-check race (check struct vcpu_info_t)
	 */
	vcpu->evtchn_upcall_pending = 0;

	barrier_dmem_fence_full();

	/* Can not use system atomic_t/atomic_set() due to 32-bit casting */
	pos_selector = __atomic_exchange_n(&vcpu->evtchn_pending_sel,
					0, __ATOMIC_SEQ_CST);

	while (pos_selector) {
		/* Find first position, clear it in selector and process */
		pos_index = __builtin_ffsl(pos_selector) - 1;
		pos_selector &= ~(((xen_ulong_t) 1) << pos_index);

		/* Find all active evtchn on selected position */
		while ((events_pending = get_pending_events(pos_index)) != 0) {
			event_index =  __builtin_ffsl(events_pending) - 1;
			events_pending &= (((xen_ulong_t) 1) << event_index);

			port = (pos_index * 8 * sizeof(xen_ulong_t))
					+ event_index;
			process_event(port);
		}
	}
}

int xen_events_init(void)
{
	int i;

	if (!HYPERVISOR_shared_info) {
		/* shared info was not mapped */
		LOG_ERR("%s: shared_info - NULL, can't setup events\n", __func__);
		return -EINVAL;
	}

	/* bind all ports with default callback */
	for (i = 0; i < EVTCHN_2L_NR_CHANNELS; i++) {
		reset_event_channel_state(i);
	}

	/*
	 * DT_DRV_COMPAT selects the xen,xen devicetree compatible. Instance 0
	 * is the first xen,xen node, and IRQ index 0 is its event-channel IRQ.
	 */
	IRQ_CONNECT(DT_INST_IRQ_BY_IDX(0, 0, irq),
		DT_INST_IRQ_BY_IDX(0, 0, priority), events_isr,
		NULL, DT_INST_IRQ_BY_IDX(0, 0, flags));

	irq_enable(DT_INST_IRQ_BY_IDX(0, 0, irq));

	LOG_INF("%s: events inited\n", __func__);
	return 0;
}
