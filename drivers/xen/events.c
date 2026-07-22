/*
 * Copyright (c) 2021-2025 EPAM Systems
 * Copyright (c) 2022 Arm Limited (or its affiliates). All rights reserved.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT xen_xen

#include <zephyr/arch/arm64/hypercall.h>
#include <zephyr/xen/public/xen.h>
#include <zephyr/xen/public/event_channel.h>
#include <zephyr/xen/public/vcpu.h>
#include <zephyr/xen/generic.h>
#include <zephyr/xen/events.h>
#include <zephyr/sys/barrier.h>

#include <errno.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/irq.h>
#include <zephyr/spinlock.h>

LOG_MODULE_REGISTER(xen_events);

extern shared_info_t *HYPERVISOR_shared_info;

static evtchn_handle_t event_channels[EVTCHN_2L_NR_CHANNELS];
static bool events_missed[EVTCHN_2L_NR_CHANNELS];
static struct k_spinlock event_channels_lock;

#define EVTCHN_WORD_BITS	(sizeof(xen_ulong_t) * BITS_PER_BYTE)
#define EVTCHN_WORD(port)	((port) / EVTCHN_WORD_BITS)
#define EVTCHN_BIT(port)	((xen_ulong_t)1 << ((port) % EVTCHN_WORD_BITS))

static inline void evtchn_set_bit(xen_ulong_t *bitmap, evtchn_port_t port)
{
	(void)__atomic_fetch_or(&bitmap[EVTCHN_WORD(port)], EVTCHN_BIT(port),
				__ATOMIC_ACQ_REL);
}

static inline void evtchn_clear_bit(xen_ulong_t *bitmap, evtchn_port_t port)
{
	(void)__atomic_fetch_and(&bitmap[EVTCHN_WORD(port)], ~EVTCHN_BIT(port),
				 __ATOMIC_ACQ_REL);
}

static void empty_callback(void *data)
{
	k_spinlock_key_t key;
	/* data is the event_channels entry, subtracting the base, it's the port */
	unsigned int port = (((evtchn_handle_t *)data) - event_channels);

	key = k_spin_lock(&event_channels_lock);
	events_missed[port] = true;
	k_spin_unlock(&event_channels_lock, key);
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

int evtchn_close(evtchn_port_t port)
{
	struct evtchn_close close = {
		.port = port,
	};

	return HYPERVISOR_event_channel_op(EVTCHNOP_close, &close);
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

int bind_event_channel(evtchn_port_t port, evtchn_cb_t cb, void *data)
{
	k_spinlock_key_t key;

	__ASSERT(port < EVTCHN_2L_NR_CHANNELS,
		"%s: trying to bind invalid evtchn #%u\n",
		__func__, port);
	__ASSERT(cb != NULL, "%s: NULL callback for evtchn #%u\n",
		__func__, port);

	key = k_spin_lock(&event_channels_lock);
	if (event_channels[port].cb != empty_callback) {
		LOG_WRN("%s: re-bind callback for evtchn #%u\n",
				__func__, port);
	}

	event_channels[port].priv = data;
	event_channels[port].cb = cb;
	k_spin_unlock(&event_channels_lock, key);

	return 0;
}

int unbind_event_channel(evtchn_port_t port)
{
	k_spinlock_key_t key;

	__ASSERT(port < EVTCHN_2L_NR_CHANNELS,
		"%s: trying to unbind invalid evtchn #%u\n",
		__func__, port);

	key = k_spin_lock(&event_channels_lock);
	event_channels[port].cb = empty_callback;
	event_channels[port].priv = &event_channels[port];
	events_missed[port] = false;
	k_spin_unlock(&event_channels_lock, key);

	return 0;
}

int get_missed_events(evtchn_port_t port)
{
	k_spinlock_key_t key;
	bool missed;

	__ASSERT(port < EVTCHN_2L_NR_CHANNELS,
		"%s: trying to get missed event from invalid port #%u\n",
		__func__, port);

	key = k_spin_lock(&event_channels_lock);
	missed = events_missed[port];
	if (missed) {
		events_missed[port] = false;
	}
	k_spin_unlock(&event_channels_lock, key);

	return missed ? 1 : 0;
}

int mask_event_channel(evtchn_port_t port)
{
	shared_info_t *s = HYPERVISOR_shared_info;

	__ASSERT(port < EVTCHN_2L_NR_CHANNELS,
		"%s: trying to mask invalid evtchn #%u\n",
		__func__, port);


	evtchn_set_bit(s->evtchn_mask, port);

	return 0;
}

int unmask_event_channel(evtchn_port_t port)
{
	struct evtchn_unmask unmask = {
		.port = port,
	};

	__ASSERT(port < EVTCHN_2L_NR_CHANNELS,
		"%s: trying to unmask invalid evtchn #%u\n",
		__func__, port);

	/*
	 * Let Xen unmask the port because the hypervisor knows which vCPU owns
	 * the channel. If an event became pending while the port was masked, Xen
	 * also replays the notification to that vCPU; a local bit clear would
	 * lose that edge on SMP guests.
	 */
	return HYPERVISOR_event_channel_op(EVTCHNOP_unmask, &unmask);
}

void clear_event_channel(evtchn_port_t port)
{
	shared_info_t *s = HYPERVISOR_shared_info;

	evtchn_clear_bit(s->evtchn_pending, port);
}

static inline xen_ulong_t get_pending_events(xen_ulong_t pos)
{
	shared_info_t *s = HYPERVISOR_shared_info;
	xen_ulong_t pending = __atomic_load_n(&s->evtchn_pending[pos],
					      __ATOMIC_ACQUIRE);
	xen_ulong_t mask = __atomic_load_n(&s->evtchn_mask[pos],
					   __ATOMIC_ACQUIRE);

	return pending & ~mask;
}

static void process_event(evtchn_port_t port)
{
	evtchn_handle_t channel;
	k_spinlock_key_t key;

	key = k_spin_lock(&event_channels_lock);
	channel = event_channels[port];
	k_spin_unlock(&event_channels_lock, key);
	clear_event_channel(port);
	channel.cb(channel.priv);
}

/*
 * Per-CPU vcpu_info.
 *
 * On arm64 the shared_info page only holds a single vcpu_info slot (see
 * XEN_LEGACY_MAX_VCPUS == 1 in arch-arm.h), which Xen assigns to the boot CPU.
 * Every secondary CPU must register its own vcpu_info via VCPUOP_register_vcpu_info,
 * otherwise Xen has nowhere to deliver that vCPU's event-channel upcall state and
 * events bound to it are never seen by the guest. The backing storage lives in a
 * page-aligned array so no vcpu_info struct straddles a page boundary.
 */
#if defined(CONFIG_SMP) && (CONFIG_MP_MAX_NUM_CPUS > 1)
static vcpu_info_t secondary_vcpu_info[CONFIG_MP_MAX_NUM_CPUS - 1]
	__aligned(XEN_PAGE_SIZE);
#endif

static inline vcpu_info_t *this_cpu_vcpu_info(void)
{
#if defined(CONFIG_SMP) && (CONFIG_MP_MAX_NUM_CPUS > 1)
	unsigned int cpu = arch_curr_cpu()->id;

	if (cpu != 0) {
		return &secondary_vcpu_info[cpu - 1];
	}
#endif
	return &HYPERVISOR_shared_info->vcpu_info[0];
}

static void events_isr(void *data)
{
	ARG_UNUSED(data);

	/* Needed for 2-level unwrapping */
	xen_ulong_t pos_selector;   /* bits are positions in pending array */
	xen_ulong_t events_pending; /* bits - events in pos_selector element */
	uint32_t pos_index, event_index; /* bit indexes */

	evtchn_port_t port; /* absolute event index */

	/* Use the vcpu_info of the CPU this ISR is running on (SMP-safe). */
	vcpu_info_t *vcpu = this_cpu_vcpu_info();

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
		event_channels[i].cb = empty_callback;
		event_channels[i].priv = &event_channels[i];
		events_missed[i] = false;
	}

	IRQ_CONNECT(DT_INST_IRQ_BY_IDX(0, 0, irq),
		DT_INST_IRQ_BY_IDX(0, 0, priority), events_isr,
		NULL, DT_INST_IRQ_BY_IDX(0, 0, flags));

	irq_enable(DT_INST_IRQ_BY_IDX(0, 0, irq));

	LOG_INF("%s: events inited\n", __func__);
	return 0;
}

/*
 * Per-CPU bring-up of the Xen event-channel path on a secondary CPU.
 *
 * Two things are needed, neither of which xen_events_init() does for anything
 * other than the boot CPU:
 *
 *   1. Register a vcpu_info for this vCPU (arm64 shared_info only has vcpu0's),
 *      so Xen has a place to deliver this vCPU's upcall pending state.
 *   2. Enable the event-channel PPI on this CPU's redistributor, since it is a
 *      per-CPU interrupt and irq_enable() in xen_events_init() only affected
 *      the boot CPU.
 */
void xen_evtchn_per_cpu_init(void)
{
#if defined(CONFIG_SMP) && (CONFIG_MP_MAX_NUM_CPUS > 1)
	unsigned int cpu = arch_curr_cpu()->id;
	vcpu_info_t *vi;
	struct vcpu_register_vcpu_info reg;
	int rc;

	if (cpu == 0) {
		irq_enable(DT_INST_IRQ_BY_IDX(0, 0, irq));
		return;
	}

	vi = &secondary_vcpu_info[cpu - 1];

	/*
	 * Fresh vcpu_info (BSS-zeroed). On arm64 there is no PV upcall
	 * mask (see struct vcpu_info); delivery is gated by the GIC PPI
	 * and the per-event-channel masks, so we only need to hand Xen
	 * the location of this vCPU's vcpu_info.
	 */
	vi->evtchn_upcall_pending = 0;
	vi->evtchn_pending_sel = 0;

	reg.mfn = xen_virt_to_gfn(vi);
	reg.offset = (uint32_t)((uintptr_t)vi & (XEN_PAGE_SIZE - 1));
	reg.rsvd = 0;

	rc = HYPERVISOR_vcpu_op(VCPUOP_register_vcpu_info, cpu, &reg);
	if (rc) {
		LOG_ERR("%s: register vcpu_info for CPU %u failed: %d\n",
			__func__, cpu, rc);
		return;
	}
#endif

	irq_enable(DT_INST_IRQ_BY_IDX(0, 0, irq));
}
