/*
 * Copyright (c) 2026 EPAM Systems
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/xen/dom0/domctl.h>
#include <zephyr/xen/dom0/sysctl.h>
#include <zephyr/xen/public/xen.h>
#include <zephyr/xen/version.h>

#define DOMAININFO_MAX 8
#define TBUF_TEST_EVT_MASK 0x0000ffffU
#define TBUF_TEST_SIZE_PAGES 1U
#define XEN_ERRNO_ENOSYS 38
#define XEN_ERRNO_EINVAL 22
#define XEN_ERRNO_EOPNOTSUPP 95

static int check_xen_version(void)
{
	int version;
	char extra[XEN_EXTRAVERSION_LEN];

	version = xen_version();
	if (version < 0) {
		printk("xen_smoke: xen_version failed: %d\n", version);
		return version;
	}

	printk("xen_smoke: Xen version %d.%d\n", version >> 16, version & 0xffff);

	version = xen_version_extraversion(extra, sizeof(extra));
	if (version < 0) {
		printk("xen_smoke: xen_version_extraversion failed: %d\n", version);
		return version;
	}

	printk("xen_smoke: Xen extra version %s\n", extra);

	return 0;
}

static int check_sysctl_physinfo(void)
{
	int ret;
	struct xen_sysctl_physinfo info;

	ret = xen_sysctl_physinfo(&info);
	if (ret < 0) {
		printk("xen_smoke: xen_sysctl_physinfo failed: %d\n", ret);
		return ret;
	}

	printk("xen_smoke: physinfo cpus=%u max_cpu_id=%u total_pages=%llu free_pages=%llu\n",
	       info.nr_cpus, info.max_cpu_id,
	       (unsigned long long)info.total_pages,
	       (unsigned long long)info.free_pages);

	return 0;
}

static int tbuf_get_info(struct xen_sysctl_tbuf_op *tbuf)
{
	int ret;

	*tbuf = (struct xen_sysctl_tbuf_op) {
		.cmd = XEN_SYSCTL_TBUFOP_get_info,
	};

	ret = xen_sysctl_tbuf_op(tbuf);
	if (ret < 0) {
		printk("xen_smoke: xen_sysctl_tbuf_op(get_info) failed: %d\n", ret);
		return ret;
	}

	printk("xen_smoke: tbuf mfn=0x%llx size=%u evt_mask=0x%x\n",
	       (unsigned long long)tbuf->buffer_mfn, tbuf->size, tbuf->evt_mask);

	return 0;
}

static int tbuf_set_evt_mask(uint32_t evt_mask)
{
	struct xen_sysctl_tbuf_op tbuf = {
		.cmd = XEN_SYSCTL_TBUFOP_set_evt_mask,
		.evt_mask = evt_mask,
	};

	return xen_sysctl_tbuf_op(&tbuf);
}

static int tbuf_set_cpu0_mask(void)
{
	uint8_t cpu0_mask = 0x1U;
	struct xen_sysctl_tbuf_op tbuf = {
		.cmd = XEN_SYSCTL_TBUFOP_set_cpu_mask,
		.cpu_mask.nr_bits = 1,
	};

	set_xen_guest_handle(tbuf.cpu_mask.bitmap, &cpu0_mask);

	return xen_sysctl_tbuf_op(&tbuf);
}

static int tbuf_set_size(uint32_t pages)
{
	struct xen_sysctl_tbuf_op tbuf = {
		.cmd = XEN_SYSCTL_TBUFOP_set_size,
		.size = pages,
	};

	return xen_sysctl_tbuf_op(&tbuf);
}

static int tbuf_set_enabled(bool enable)
{
	struct xen_sysctl_tbuf_op tbuf = {
		.cmd = enable ? XEN_SYSCTL_TBUFOP_enable : XEN_SYSCTL_TBUFOP_disable,
	};

	return xen_sysctl_tbuf_op(&tbuf);
}

static int check_sysctl_tbuf_ops(void)
{
	int ret;
	struct xen_sysctl_tbuf_op info;
	uint32_t original_evt_mask;

	printk("xen_smoke: tbuf/get_info: START cmd=get_info request=trace_buffer_state\n");
	ret = tbuf_get_info(&info);
	if (ret < 0) {
		return ret;
	}
	printk("xen_smoke: tbuf/get_info: PASS buffer_mfn=0x%llx size_bytes=%u "
	       "evt_mask=0x%x\n",
	       (unsigned long long)info.buffer_mfn, info.size, info.evt_mask);

	original_evt_mask = info.evt_mask;

	printk("xen_smoke: tbuf/set_evt_mask: START cmd=set_evt_mask request_evt_mask=0x%x "
	       "original_evt_mask=0x%x\n",
	       TBUF_TEST_EVT_MASK, original_evt_mask);
	ret = tbuf_set_evt_mask(TBUF_TEST_EVT_MASK);
	if (ret < 0) {
		printk("xen_smoke: xen_sysctl_tbuf_op(set_evt_mask) failed: %d\n", ret);
		return ret;
	}
	ret = tbuf_get_info(&info);
	if (ret < 0) {
		return ret;
	}
	if (info.evt_mask != TBUF_TEST_EVT_MASK) {
		printk("xen_smoke: tbuf evt_mask mismatch: expected=0x%x actual=0x%x\n",
		       TBUF_TEST_EVT_MASK, info.evt_mask);
		return -EINVAL;
	}
	ret = tbuf_set_evt_mask(original_evt_mask);
	if (ret < 0) {
		printk("xen_smoke: xen_sysctl_tbuf_op(restore evt_mask) failed: %d\n", ret);
		return ret;
	}
	printk("xen_smoke: tbuf/set_evt_mask: PASS observed_evt_mask=0x%x "
	       "restored_evt_mask=0x%x\n",
	       info.evt_mask, original_evt_mask);

	printk("xen_smoke: tbuf/set_cpu_mask: START cmd=set_cpu_mask nr_bits=1 bitmap=0x1\n");
	ret = tbuf_set_cpu0_mask();
	if (ret < 0) {
		printk("xen_smoke: xen_sysctl_tbuf_op(set_cpu_mask) failed: %d\n", ret);
		return ret;
	}
	printk("xen_smoke: tbuf/set_cpu_mask: PASS accepted nr_bits=1 bitmap=0x1\n");

	printk("xen_smoke: tbuf/set_size: START cmd=set_size requested_pages=%u\n",
	       TBUF_TEST_SIZE_PAGES);
	ret = tbuf_get_info(&info);
	if (ret < 0) {
		return ret;
	}
	printk("xen_smoke: tbuf/set_size: current size_bytes=%u buffer_mfn=0x%llx\n",
	       info.size, (unsigned long long)info.buffer_mfn);
	if (info.size == 0) {
		ret = tbuf_set_size(TBUF_TEST_SIZE_PAGES);
		if (ret < 0) {
			printk("xen_smoke: xen_sysctl_tbuf_op(set_size) failed: %d\n", ret);
			return ret;
		}
		ret = tbuf_get_info(&info);
		if (ret < 0) {
			return ret;
		}
		printk("xen_smoke: tbuf/set_size: PASS requested_pages=%u "
		       "observed_size_bytes=%u buffer_mfn=0x%llx\n",
		       TBUF_TEST_SIZE_PAGES, info.size,
		       (unsigned long long)info.buffer_mfn);
	} else {
		printk("xen_smoke: tbuf/set_size: SKIP requested_pages=%u already_configured "
		       "size_bytes=%u buffer_mfn=0x%llx\n",
		       TBUF_TEST_SIZE_PAGES, info.size,
		       (unsigned long long)info.buffer_mfn);
	}

	printk("xen_smoke: tbuf/disable: START cmd=disable request=stop_trace_writes\n");
	ret = tbuf_set_enabled(false);
	if (ret < 0) {
		printk("xen_smoke: xen_sysctl_tbuf_op(disable) failed: %d\n", ret);
		return ret;
	}
	printk("xen_smoke: tbuf/disable: PASS command_accepted ret=0\n");

	printk("xen_smoke: tbuf/enable: START cmd=enable request=start_trace_writes\n");
	ret = tbuf_set_enabled(true);
	if (ret < 0) {
		printk("xen_smoke: xen_sysctl_tbuf_op(enable) failed: %d\n", ret);
		return ret;
	}
	printk("xen_smoke: tbuf/enable: PASS command_accepted ret=0\n");

	return 0;
}

static int check_sysctl_cpu_hotplug(void)
{
	int ret;
	struct xen_sysctl_cpu_hotplug hotplug = {
		.cpu = 0,
		.op = XEN_SYSCTL_CPU_HOTPLUG_ONLINE,
	};

	printk("xen_smoke: cpu_hotplug/online: START cmd=cpu_hotplug op=online "
	       "cpu=%u request=probe_arch_support\n",
	       hotplug.cpu);

	ret = xen_sysctl_cpu_hotplug(&hotplug);
	if (ret == -XEN_ERRNO_ENOSYS || ret == -XEN_ERRNO_EOPNOTSUPP) {
		printk("xen_smoke: cpu_hotplug/online: SKIP ret=%d reason=unsupported\n",
		       ret);
		return 0;
	}
	if (ret < 0) {
		printk("xen_smoke: xen_sysctl_cpu_hotplug(online cpu=%u) failed: %d\n",
		       hotplug.cpu, ret);
		return ret;
	}

	printk("xen_smoke: cpu_hotplug/online: PASS cpu=%u op=online ret=0\n",
	       hotplug.cpu);

	return 0;
}

static int check_sysctl_getdomaininfo(void)
{
	int ret;
	struct xen_domctl_getdomaininfo domains[DOMAININFO_MAX];

	ret = xen_sysctl_getdomaininfo(domains, 0, ARRAY_SIZE(domains));
	if (ret < 0) {
		printk("xen_smoke: xen_sysctl_getdomaininfo failed: %d\n", ret);
		return ret;
	}

	printk("xen_smoke: sysctl domain count=%d\n", ret);
	for (int i = 0; i < ret; i++) {
		printk("xen_smoke: domain[%d] domid=%u flags=0x%x vcpus=%u pages=%llu\n",
		       i, domains[i].domain, domains[i].flags,
		       domains[i].nr_online_vcpus,
		       (unsigned long long)domains[i].tot_pages);
	}

	return 0;
}

static int check_domctl_getdomaininfo(void)
{
	int ret;
	xen_domctl_getdomaininfo_t info;

	ret = xen_domctl_getdomaininfo(0, &info);
	if (ret < 0) {
		printk("xen_smoke: xen_domctl_getdomaininfo failed: %d\n", ret);
		return ret;
	}

	printk("xen_smoke: self domid=%u flags=0x%x max_vcpu_id=%u shared_info=0x%llx\n",
	       info.domain, info.flags, info.max_vcpu_id,
	       (unsigned long long)info.shared_info_frame);

	return 0;
}

static int check_domctl_unbind_pt_irq(void)
{
	int ret;
	uint32_t machine_irq = 32;
	uint16_t spi = 33;

	printk("xen_smoke: domctl/unbind_pt_irq: START cmd=unbind_pt_irq "
	       "irq_type=spi machine_irq=%u spi=%u request=invalid_mapping_probe\n",
	       machine_irq, spi);

	ret = xen_domctl_unbind_pt_irq(0, machine_irq, PT_IRQ_TYPE_SPI,
				       0, 0, 0, 0, spi);
	if (ret == -XEN_ERRNO_ENOSYS || ret == -XEN_ERRNO_EOPNOTSUPP) {
		printk("xen_smoke: domctl/unbind_pt_irq: SKIP ret=%d "
		       "reason=unsupported\n",
		       ret);
		return 0;
	}
	if (ret == -XEN_ERRNO_EINVAL) {
		printk("xen_smoke: domctl/unbind_pt_irq: PASS ret=%d "
		       "expected_reject=machine_irq_spi_mismatch\n",
		       ret);
		return 0;
	}
	if (ret == 0) {
		printk("xen_smoke: domctl/unbind_pt_irq: FAIL ret=0 "
		       "expected_reject=machine_irq_spi_mismatch\n");
		return -EINVAL;
	}

	printk("xen_smoke: xen_domctl_unbind_pt_irq failed: %d\n", ret);
	return ret;
}

int main(void)
{
	int ret;

	printk("xen_smoke: START\n");

	ret = check_xen_version();
	if (ret < 0) {
		goto fail;
	}

	ret = check_sysctl_physinfo();
	if (ret < 0) {
		goto fail;
	}

	ret = check_sysctl_tbuf_ops();
	if (ret < 0) {
		goto fail;
	}

	ret = check_sysctl_cpu_hotplug();
	if (ret < 0) {
		goto fail;
	}

	ret = check_sysctl_getdomaininfo();
	if (ret < 0) {
		goto fail;
	}

	ret = check_domctl_getdomaininfo();
	if (ret < 0) {
		goto fail;
	}

	ret = check_domctl_unbind_pt_irq();
	if (ret < 0) {
		goto fail;
	}

	printk("xen_smoke: PASS\n");
	return 0;

fail:
	printk("xen_smoke: FAIL ret=%d\n", ret);
	return ret;
}
