/*
 * Copyright (c) 2021-2025 EPAM Systems
 * Copyright (c) 2022 Arm Limited (or its affiliates). All rights reserved.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT xen_xen

#include <xen/public/xen.h>
#include <xen/public/event_channel.h>
#include <xen/public/vcpu.h>

#include <zephyr/arch/arm64/hypercall.h>
#include <zephyr/xen/generic.h>
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
#define EVTCHN_WORDS (EVTCHN_2L_NR_CHANNELS / EVTCHN_WORD_BITS)

/*
 * Driver-side port ownership cache.
 *
 * Xen's evtchn_pending_sel is per-vCPU, but it selects a word in the
 * domain-wide evtchn_pending bitmap. If ports for different vCPUs share that
 * word, an ISR running on one CPU must not drain the other CPU's ports just
 * because their pending bits live in the same word.
 *
 * Protected by event_state_lock after event-channel interrupts are enabled.
 */
static xen_ulong_t event_channel_cpu_mask[CONFIG_MP_MAX_NUM_CPUS][EVTCHN_WORDS];

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

static bool shared_event_bit_is_set(const xen_ulong_t *bitmap, evtchn_port_t port)
{
	uint32_t word = port / EVTCHN_WORD_BITS;
	xen_ulong_t bit = shared_event_bit(port);

	return (shared_event_word(bitmap, word) & bit) != 0;
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
 * Move the port in the driver-side ownership cache. Callers must either hold
 * event_state_lock or run before event-channel interrupts are enabled.
 */
static void set_event_channel_cpu(evtchn_port_t port, uint32_t vcpu)
{
	uint32_t old_vcpu;
	uint32_t word = port / EVTCHN_WORD_BITS;
	uint32_t bit = port % EVTCHN_WORD_BITS;
	xen_ulong_t bit_mask = ((xen_ulong_t)1) << bit;

	for (old_vcpu = 0; old_vcpu < CONFIG_MP_MAX_NUM_CPUS; old_vcpu++) {
		event_channel_cpu_mask[old_vcpu][word] &= ~bit_mask;
	}

	event_channel_cpu_mask[vcpu][word] |= bit_mask;
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
	set_event_channel_cpu(port, 0);
}

int set_event_channel_affinity(evtchn_port_t port, uint32_t vcpu)
{
	shared_info_t *s = HYPERVISOR_shared_info;
	struct evtchn_bind_vcpu bind = {
		.port = port,
		.vcpu = vcpu,
	};
	struct evtchn_unmask unmask = {
		.port = port,
	};
	k_spinlock_key_t key;
	bool was_masked;
	int rc;

	__ASSERT(port < EVTCHN_2L_NR_CHANNELS,
		"%s: trying to set affinity for invalid evtchn #%u\n",
		__func__, port);
	__ASSERT(vcpu < CONFIG_MP_MAX_NUM_CPUS,
		"%s: trying to set affinity for evtchn #%u to invalid vCPU %u\n",
		__func__, port, vcpu);

	/*
	 * Keep the port masked while Xen and the driver's dispatch cache move
	 * to the new vCPU. Otherwise Xen can set the new vCPU's pending
	 * selector before event_channel_cpu_mask[] lets that CPU drain the
	 * port, leaving the event pending without a matching selector kick.
	 */
	key = k_spin_lock(&event_state_lock);
	was_masked = shared_event_bit_is_set(s->evtchn_mask, port);
	set_shared_event_bit(s->evtchn_mask, port);
	k_spin_unlock(&event_state_lock, key);

	rc = HYPERVISOR_event_channel_op(EVTCHNOP_bind_vcpu, &bind);
	if (rc != 0) {
		if (!was_masked) {
			int unmask_rc;

			unmask_rc = HYPERVISOR_event_channel_op(EVTCHNOP_unmask,
								&unmask);
			if (unmask_rc != 0) {
				LOG_ERR("%s: restore evtchn #%u unmask failed: %d\n",
					__func__, port, unmask_rc);
			}
		}
		return rc;
	}

	key = k_spin_lock(&event_state_lock);
	set_event_channel_cpu(port, vcpu);
	k_spin_unlock(&event_state_lock, key);

	if (!was_masked) {
		return HYPERVISOR_event_channel_op(EVTCHNOP_unmask, &unmask);
	}

	return 0;
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
	uint32_t cpu = arch_curr_cpu()->id;
	k_spinlock_key_t key;
	xen_ulong_t cpu_mask;
	xen_ulong_t events;

	key = k_spin_lock(&event_state_lock);
	cpu_mask = event_channel_cpu_mask[cpu][pos];
	events = shared_event_word(s->evtchn_pending, pos) &
		 ~shared_event_word(s->evtchn_mask, pos) & cpu_mask;
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

/*
 * Secondary per-vCPU shared state storage.
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

/*
 * Select the Xen vcpu_info for the CPU currently running the ISR.
 *
 * CPU0 uses the legacy vcpu_info embedded in shared_info. Secondary CPUs use
 * the page-aligned storage registered with Xen during per-CPU bring-up.
 */
static inline vcpu_info_t *this_cpu_vcpu_info(void)
{
#if defined(CONFIG_SMP) && (CONFIG_MP_MAX_NUM_CPUS > 1)
	unsigned int cpu = arch_curr_cpu()->id;

	if (cpu >= CONFIG_MP_MAX_NUM_CPUS) {
		return NULL;
	}

	if (cpu != 0) {
		return &secondary_vcpu_info[cpu - 1];
	}
#endif
	return &HYPERVISOR_shared_info->vcpu_info[0];
}

/*
 * Xen event-channel interrupt handler.
 *
 * The interrupt only says that this vCPU has event-channel work. The ISR first
 * drains this CPU's selector, then scans the selected domain-wide pending words
 * to recover concrete port numbers.
 */
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

	__ASSERT_NO_MSG(vcpu != NULL);

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

/*
 * Secondary CPU bring-up of the Xen event-channel path.
 *
 * The boot CPU gets its vcpu_info from the legacy shared_info slot after
 * shared_info is mapped. A secondary CPU needs its own registered vcpu_info
 * storage because arm64 shared_info does not contain slots for CPU1+.
 *
 * xen_events_init() also enables the event-channel PPI while running on the
 * boot CPU. Because a PPI is enabled per CPU, each secondary CPU must enable
 * the same interrupt locally.
 */
void xen_evtchn_secondary_cpu_init(void)
{
#if defined(CONFIG_SMP) && (CONFIG_MP_MAX_NUM_CPUS > 1)
	unsigned int cpu = arch_curr_cpu()->id;
	vcpu_info_t *vi;
	struct vcpu_register_vcpu_info reg;
	int rc;

	__ASSERT(cpu != 0, "%s: unexpected secondary CPU id 0\n", __func__);

	vi = this_cpu_vcpu_info();
	__ASSERT(vi != NULL, "%s: unexpected secondary CPU id %u\n",
		 __func__, cpu);

	/*
	 * Fresh vcpu_info (BSS-zeroed). On arm64 there is no PV upcall
	 * mask (see struct vcpu_info); delivery is gated by the GIC PPI
	 * and the per-event-channel masks, so the zero-initialized state is
	 * ready before we hand Xen the location of this vCPU's vcpu_info.
	 */
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
