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
#define XEN_ERRNO_EBUSY 16
#define XEN_ERRNO_EPERM 1
#define XEN_ERRNO_EINVAL 22
#define XEN_ERRNO_EOPNOTSUPP 95

static const uint16_t test_pt_irq_spis[] = {
	35, 36, 37, 38,
	48, 49, 50, 51, 52, 53, 54, 55,
	56, 57, 58, 59, 60, 61, 62, 63,
	64, 65, 66, 67, 68, 69, 70, 71,
	72, 73, 74, 75, 76, 77, 78, 79,
};

static int check_xen_version(void)
{
	int version;
	int xenver;
	char extra[XEN_EXTRAVERSION_LEN];

	printk("xen_smoke: xen_version: START scenario=query Xen major/minor and "
	       "extraversion through xen_version hypercall\n");

	version = xen_version();
	if (version < 0) {
		printk("xen_smoke: xen_version failed: %d\n", version);
		return version;
	}

	printk("xen_smoke: Xen version %d.%d\n", version >> 16, version & 0xffff);
	xenver = version;

	version = xen_version_extraversion(extra, sizeof(extra));
	if (version < 0) {
		printk("xen_smoke: xen_version_extraversion failed: %d\n", version);
		return version;
	}

	printk("xen_smoke: Xen extra version %s\n", extra);
	printk("xen_smoke: xen_version: PASS version=%d.%d extraversion=%s\n",
	       xenver >> 16, xenver & 0xffff, extra);

	return 0;
}

static int check_sysctl_physinfo(void)
{
	int ret;
	struct xen_sysctl_physinfo info;

	printk("xen_smoke: xen_sysctl_physinfo: START scenario=query host CPU and memory "
	       "topology through XEN_SYSCTL_physinfo\n");

	ret = xen_sysctl_physinfo(&info);
	if (ret < 0) {
		printk("xen_smoke: xen_sysctl_physinfo failed: %d\n", ret);
		return ret;
	}

	printk("xen_smoke: xen_sysctl_physinfo: PASS cpus=%u max_cpu_id=%u "
	       "total_pages=%llu free_pages=%llu\n",
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

	printk("xen_smoke: xen_sysctl_tbuf_op/get_info: START scenario=query Xen trace buffer state "
	       "with XEN_SYSCTL_TBUFOP_get_info\n");
	ret = tbuf_get_info(&info);
	if (ret < 0) {
		return ret;
	}
	printk("xen_smoke: xen_sysctl_tbuf_op/get_info: PASS buffer_mfn=0x%llx size_bytes=%u "
	       "evt_mask=0x%x\n",
	       (unsigned long long)info.buffer_mfn, info.size, info.evt_mask);

	original_evt_mask = info.evt_mask;

	printk("xen_smoke: xen_sysctl_tbuf_op/set_evt_mask: START scenario=change trace event mask, "
	       "verify through get_info, then restore original mask request_evt_mask=0x%x "
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
	printk("xen_smoke: xen_sysctl_tbuf_op/set_evt_mask: PASS observed_evt_mask=0x%x "
	       "restored_evt_mask=0x%x\n",
	       info.evt_mask, original_evt_mask);

	printk("xen_smoke: xen_sysctl_tbuf_op/set_cpu_mask: START scenario=limit tracing to CPU0 "
	       "through XEN_SYSCTL_TBUFOP_set_cpu_mask nr_bits=1 bitmap=0x1\n");
	ret = tbuf_set_cpu0_mask();
	if (ret < 0) {
		printk("xen_smoke: xen_sysctl_tbuf_op(set_cpu_mask) failed: %d\n", ret);
		return ret;
	}
	printk("xen_smoke: xen_sysctl_tbuf_op/set_cpu_mask: PASS accepted nr_bits=1 bitmap=0x1\n");

	printk("xen_smoke: xen_sysctl_tbuf_op/set_size: START scenario=configure trace buffer size "
	       "when Xen has not allocated one yet requested_pages=%u\n",
	       TBUF_TEST_SIZE_PAGES);
	ret = tbuf_get_info(&info);
	if (ret < 0) {
		return ret;
	}
	printk("xen_smoke: xen_sysctl_tbuf_op/set_size: current size_bytes=%u buffer_mfn=0x%llx\n",
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
		printk("xen_smoke: xen_sysctl_tbuf_op/set_size: PASS requested_pages=%u "
		       "observed_size_bytes=%u buffer_mfn=0x%llx\n",
		       TBUF_TEST_SIZE_PAGES, info.size,
		       (unsigned long long)info.buffer_mfn);
	} else {
		printk("xen_smoke: xen_sysctl_tbuf_op/set_size: SKIP requested_pages=%u already_configured "
		       "size_bytes=%u buffer_mfn=0x%llx\n",
		       TBUF_TEST_SIZE_PAGES, info.size,
		       (unsigned long long)info.buffer_mfn);
	}

	printk("xen_smoke: xen_sysctl_tbuf_op/disable: START scenario=disable Xen trace buffer writes\n");
	ret = tbuf_set_enabled(false);
	if (ret < 0) {
		printk("xen_smoke: xen_sysctl_tbuf_op(disable) failed: %d\n", ret);
		return ret;
	}
	printk("xen_smoke: xen_sysctl_tbuf_op/disable: PASS command_accepted ret=0\n");

	printk("xen_smoke: xen_sysctl_tbuf_op/enable: START scenario=enable Xen trace buffer writes\n");
	ret = tbuf_set_enabled(true);
	if (ret < 0) {
		printk("xen_smoke: xen_sysctl_tbuf_op(enable) failed: %d\n", ret);
		return ret;
	}
	printk("xen_smoke: xen_sysctl_tbuf_op/enable: PASS command_accepted ret=0\n");

	return 0;
}

static int check_sysctl_cpu_hotplug(void)
{
	int ret;
	struct xen_sysctl_cpu_hotplug hotplug = {
		.cpu = 0,
		.op = XEN_SYSCTL_CPU_HOTPLUG_ONLINE,
	};

	printk("xen_smoke: xen_sysctl_cpu_hotplug/online: START scenario=probe Xen CPU hotplug "
	       "online operation for CPU0 cmd=cpu_hotplug op=online "
	       "cpu=%u request=probe_arch_support\n",
	       hotplug.cpu);

	ret = xen_sysctl_cpu_hotplug(&hotplug);
	if (ret == -XEN_ERRNO_ENOSYS || ret == -XEN_ERRNO_EOPNOTSUPP) {
		printk("xen_smoke: xen_sysctl_cpu_hotplug/online: SKIP ret=%d reason=unsupported\n",
		       ret);
		return 0;
	}
	if (ret < 0) {
		printk("xen_smoke: xen_sysctl_cpu_hotplug(online cpu=%u) failed: %d\n",
		       hotplug.cpu, ret);
		return ret;
	}

	printk("xen_smoke: xen_sysctl_cpu_hotplug/online: PASS cpu=%u op=online ret=0\n",
	       hotplug.cpu);

	return 0;
}

static int check_sysctl_getdomaininfo(void)
{
	int ret;
	struct xen_domctl_getdomaininfo domains[DOMAININFO_MAX];

	printk("xen_smoke: xen_sysctl_getdomaininfo: START scenario=list Xen domains "
	       "through XEN_SYSCTL_getdomaininfolist\n");

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

	printk("xen_smoke: xen_sysctl_getdomaininfo: PASS domains=%d\n", ret);

	return 0;
}

static int check_domctl_getdomaininfo(void)
{
	int ret;
	xen_domctl_getdomaininfo_t info;

	printk("xen_smoke: xen_domctl_getdomaininfo: START scenario=query Dom0 domain "
	       "metadata through XEN_DOMCTL_getdomaininfo\n");

	ret = xen_domctl_getdomaininfo(0, &info);
	if (ret < 0) {
		printk("xen_smoke: xen_domctl_getdomaininfo failed: %d\n", ret);
		return ret;
	}

	printk("xen_smoke: xen_domctl_getdomaininfo: PASS domid=%u flags=0x%x "
	       "max_vcpu_id=%u shared_info=0x%llx\n",
	       info.domain, info.flags, info.max_vcpu_id,
	       (unsigned long long)info.shared_info_frame);

	return 0;
}

static int find_test_domu(domid_t *domid)
{
	int ret;
	struct xen_domctl_getdomaininfo domains[DOMAININFO_MAX];

	ret = xen_sysctl_getdomaininfo(domains, 0, ARRAY_SIZE(domains));
	if (ret < 0) {
		return ret;
	}

	for (int i = 0; i < ret; i++) {
		if (domains[i].domain != 0) {
			*domid = domains[i].domain;
			return 0;
		}
	}

	return -ENOENT;
}

static int check_domctl_assign_deassign_device(void)
{
	int ret;
	domid_t domid = DOMID_INVALID;
	char dtdev_path[] = "/virtio_mmio@a000000";
	struct xen_domctl_assign_device device = {
		.dev = XEN_DOMCTL_DEV_DT,
		.flags = 0,
		.u.dt.size = sizeof(dtdev_path) - 1,
	};

	printk("xen_smoke: xen_domctl_assign_device/xen_domctl_deassign_device: START "
	       "scenario=temporarily assign a QEMU virtio-mmio DT device from Dom0 "
	       "to DomU with XEN_DOMCTL_assign_device, then deassign it back to Dom0 "
	       "with XEN_DOMCTL_deassign_device\n");

	ret = find_test_domu(&domid);
	if (ret == -ENOENT) {
		printk("xen_smoke: xen_domctl_assign_device/xen_domctl_deassign_device: "
		       "SKIP reason=no_domu\n");
		return 0;
	}
	if (ret < 0) {
		printk("xen_smoke: xen_domctl_assign_device/xen_domctl_deassign_device: "
		       "FAIL find_domu_ret=%d\n",
		       ret);
		return ret;
	}

	set_xen_guest_handle(device.u.dt.path, dtdev_path);

	printk("xen_smoke: xen_domctl_assign_device/xen_domctl_deassign_device: "
	       "assign domid=%u path=%s\n",
	       domid, dtdev_path);
	ret = xen_domctl_assign_device(domid, &device);
	if (ret == -XEN_ERRNO_ENOSYS || ret == -XEN_ERRNO_EOPNOTSUPP) {
		printk("xen_smoke: xen_domctl_assign_device/xen_domctl_deassign_device: "
		       "SKIP ret=%d reason=unsupported\n",
		       ret);
		return 0;
	}
	if (ret < 0) {
		printk("xen_smoke: xen_domctl_assign_device/xen_domctl_deassign_device: "
		       "FAIL assign_ret=%d domid=%u path=%s\n",
		       ret, domid, dtdev_path);
		return ret;
	}

	printk("xen_smoke: xen_domctl_assign_device/xen_domctl_deassign_device: "
	       "deassign domid=%u path=%s\n",
	       domid, dtdev_path);
	ret = xen_domctl_deassign_device(domid, &device);
	if (ret < 0) {
		printk("xen_smoke: xen_domctl_assign_device/xen_domctl_deassign_device: "
		       "FAIL deassign_ret=%d domid=%u path=%s\n",
		       ret, domid, dtdev_path);
		(void)xen_domctl_assign_device(0, &device);
		return ret;
	}

	printk("xen_smoke: xen_domctl_assign_device/xen_domctl_deassign_device: "
	       "restore domid=0 path=%s\n",
	       dtdev_path);
	ret = xen_domctl_assign_device(0, &device);
	if (ret < 0) {
		printk("xen_smoke: xen_domctl_assign_device/xen_domctl_deassign_device: "
		       "FAIL restore_ret=%d path=%s\n",
		       ret, dtdev_path);
		return ret;
	}

	printk("xen_smoke: xen_domctl_assign_device/xen_domctl_deassign_device: "
	       "PASS domid=%u restored_owner=0 path=%s\n",
	       domid, dtdev_path);
	return 0;
}

static int check_domctl_bind_unbind_pt_irq(void)
{
	int ret;
	int last_ret = 0;
	domid_t domid = DOMID_INVALID;
	uint32_t machine_irq;
	uint16_t spi;

	printk("xen_smoke: xen_domctl_bind_pt_irq/xen_domctl_unbind_pt_irq: START scenario=temporarily "
	       "move a real QEMU PCIe INTx or virtio-mmio GIC SPI mapping from Dom0 to DomU "
	       "with XEN_DOMCTL_bind_pt_irq, then exercise XEN_DOMCTL_unbind_pt_irq "
	       "and accept Xen ARM live-domain -EBUSY\n");

	ret = find_test_domu(&domid);
	if (ret == -ENOENT) {
		printk("xen_smoke: xen_domctl_bind_pt_irq/xen_domctl_unbind_pt_irq: SKIP reason=no_domu\n");
		return 0;
	}
	if (ret < 0) {
		printk("xen_smoke: xen_domctl_bind_pt_irq/xen_domctl_unbind_pt_irq: FAIL find_domu_ret=%d\n", ret);
		return ret;
	}

	for (int i = 0; i < ARRAY_SIZE(test_pt_irq_spis); i++) {
		domid_t restore_domid = DOMID_INVALID;

		spi = test_pt_irq_spis[i];
		machine_irq = spi;

		printk("xen_smoke: xen_domctl_bind_pt_irq/xen_domctl_unbind_pt_irq: bind candidate=%d "
		       "domid=%u machine_irq=%u spi=%u\n",
		       i, domid, machine_irq, spi);
		ret = xen_domctl_bind_pt_irq(domid, machine_irq, PT_IRQ_TYPE_SPI,
					     0, 0, 0, 0, spi);
		if (ret == -XEN_ERRNO_ENOSYS || ret == -XEN_ERRNO_EOPNOTSUPP) {
			printk("xen_smoke: xen_domctl_bind_pt_irq/xen_domctl_unbind_pt_irq: SKIP ret=%d "
			       "reason=unsupported\n",
			       ret);
			return 0;
		}
		if (ret == -XEN_ERRNO_EBUSY) {
			printk("xen_smoke: xen_domctl_bind_pt_irq/xen_domctl_unbind_pt_irq: transfer_from_domu "
			       "candidate=%d machine_irq=%u spi=%u\n",
			       i, machine_irq, spi);
			ret = xen_domctl_unbind_pt_irq(domid, machine_irq, PT_IRQ_TYPE_SPI,
						       0, 0, 0, 0, spi);
			if (ret < 0) {
				printk("xen_smoke: xen_domctl_bind_pt_irq/xen_domctl_unbind_pt_irq: "
				       "transfer_from_dom0 candidate=%d machine_irq=%u "
				       "spi=%u\n",
				       i, machine_irq, spi);
				ret = xen_domctl_unbind_pt_irq(0, machine_irq,
							       PT_IRQ_TYPE_SPI, 0, 0, 0,
							       0, spi);
			} else {
				restore_domid = domid;
			}
			if (ret < 0) {
				printk("xen_smoke: xen_domctl_bind_pt_irq/xen_domctl_unbind_pt_irq: candidate_skip "
				       "unbind_owner_ret=%d machine_irq=%u spi=%u\n",
				       ret, machine_irq, spi);
				last_ret = ret;
				continue;
			}
			if (restore_domid == DOMID_INVALID) {
				restore_domid = 0;
			}

			ret = xen_domctl_bind_pt_irq(domid, machine_irq, PT_IRQ_TYPE_SPI,
						     0, 0, 0, 0, spi);
		}
		if (ret == -XEN_ERRNO_EPERM || ret == -XEN_ERRNO_EINVAL) {
			printk("xen_smoke: xen_domctl_bind_pt_irq/xen_domctl_unbind_pt_irq: candidate_skip "
			       "bind_ret=%d domid=%u machine_irq=%u spi=%u\n",
			       ret, domid, machine_irq, spi);
			if (restore_domid != DOMID_INVALID) {
				(void)xen_domctl_bind_pt_irq(restore_domid, machine_irq,
							     PT_IRQ_TYPE_SPI,
							     0, 0, 0, 0, spi);
			}
			last_ret = ret;
			continue;
		}
		if (ret < 0) {
			printk("xen_smoke: xen_domctl_bind_pt_irq/xen_domctl_unbind_pt_irq: FAIL bind_ret=%d "
			       "domid=%u machine_irq=%u spi=%u\n",
			       ret, domid, machine_irq, spi);
			if (restore_domid != DOMID_INVALID) {
				(void)xen_domctl_bind_pt_irq(restore_domid, machine_irq,
							     PT_IRQ_TYPE_SPI,
							     0, 0, 0, 0, spi);
			}
			return ret;
		}

		printk("xen_smoke: xen_domctl_bind_pt_irq/xen_domctl_unbind_pt_irq: unbind domid=%u "
		       "machine_irq=%u spi=%u\n",
		       domid, machine_irq, spi);
		ret = xen_domctl_unbind_pt_irq(domid, machine_irq, PT_IRQ_TYPE_SPI,
					       0, 0, 0, 0, spi);
		if (ret < 0) {
			if (ret == -XEN_ERRNO_EBUSY) {
				printk("xen_smoke: xen_domctl_bind_pt_irq/xen_domctl_unbind_pt_irq: PASS domid=%u "
				       "machine_irq=%u spi=%u candidate=%d "
				       "unbind_ret=%d reason=live_domain_unbind_busy\n",
				       domid, machine_irq, spi, i, ret);
				return 0;
			}
			printk("xen_smoke: xen_domctl_bind_pt_irq/xen_domctl_unbind_pt_irq: FAIL unbind_ret=%d "
			       "domid=%u machine_irq=%u spi=%u\n",
			       ret, domid, machine_irq, spi);
			if (restore_domid != DOMID_INVALID) {
				(void)xen_domctl_bind_pt_irq(restore_domid, machine_irq,
							     PT_IRQ_TYPE_SPI,
							     0, 0, 0, 0, spi);
			}
			return ret;
		}

		if (restore_domid != DOMID_INVALID) {
			ret = xen_domctl_bind_pt_irq(restore_domid, machine_irq, PT_IRQ_TYPE_SPI,
						     0, 0, 0, 0, spi);
			if (ret < 0) {
				printk("xen_smoke: xen_domctl_bind_pt_irq/xen_domctl_unbind_pt_irq: FAIL "
				       "restore_owner_ret=%d owner=%u machine_irq=%u spi=%u\n",
				       ret, restore_domid, machine_irq, spi);
				return ret;
			}
		}

		printk("xen_smoke: xen_domctl_bind_pt_irq/xen_domctl_unbind_pt_irq: PASS domid=%u "
		       "machine_irq=%u spi=%u candidate=%d restored_owner=%u\n",
		       domid, machine_irq, spi, i, restore_domid);

		return 0;
	}

	printk("xen_smoke: xen_domctl_bind_pt_irq/xen_domctl_unbind_pt_irq: FAIL no_free_candidate "
	       "last_ret=%d\n",
	       last_ret);
	return last_ret ? last_ret : -ENOENT;
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

	ret = check_domctl_assign_deassign_device();
	if (ret < 0) {
		goto fail;
	}

	ret = check_domctl_bind_unbind_pt_irq();
	if (ret < 0) {
		goto fail;
	}

	printk("xen_smoke: PASS\n");
	return 0;

fail:
	printk("xen_smoke: FAIL ret=%d\n", ret);
	return ret;
}
