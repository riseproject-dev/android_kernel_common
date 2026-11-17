// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2021-2024 Alibaba Group Holding Limited.
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/firmware/zhihe/ipc.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/reboot.h>
#include <linux/regmap.h>
#include <linux/watchdog.h>

#define DRV_NAME	"zhihe-wdt"

/*
 * Watchdog selector to timeout in seconds.
 * 0: WDT disabled;
 * others: timeout = 2048 ms * 2^(TWDSCALE-1).
 */
static const unsigned int wdt_timeout[] = { 8, 16, 32, 128 };
#define ZHIHE_TWDSCALE_DISABLE		0
#define ZHIHE_TWDSCALE_MIN		1
#define ZHIHE_TWDSCALE_MAX		(ARRAY_SIZE(wdt_timeout) - 1)
#define ZHIHE_WDT_MIN_TIMEOUT		wdt_timeout[ZHIHE_TWDSCALE_MIN]
#define ZHIHE_WDT_MAX_TIMEOUT		wdt_timeout[ZHIHE_TWDSCALE_MAX]
#define ZHIHE_WDT_TIMEOUT		wdt_timeout[3]
#define ZHIHE_RESET_PROTECTION_MS	1000
#define ZHIHE_WDT_MIN_HEARTBEAT_MS	256

struct aon_msg_wdg_ctrl {
	struct aon_rpc_msg_hdr hdr;
	u32 timeout;
	u32 running_state;
	u32 reserved[1];
} __packed __aligned(1);

struct aon_msg_wdg_ctrl_ack {
	struct aon_rpc_ack_common ack_hdr;
	u32 timeout;
	u32 running_state;
	u32 reserved[1];
} __packed __aligned(1);

struct zhihe_wdt_device {
	struct device *dev;
	struct zhihe_aon_ipc *ipc_handle;
	struct aon_msg_wdg_ctrl msg;
	unsigned int is_aon_wdt_ena;
	struct notifier_block reboot_nb;
};

static struct zhihe_wdt_device *zhihe_power_off_wdt;

static unsigned int zhihe_wdt_timeout_to_sel(unsigned int secs)
{
	unsigned int i;

	for (i = ZHIHE_TWDSCALE_MIN; i <= ZHIHE_TWDSCALE_MAX; i++) {
		if (wdt_timeout[i] >= secs)
			return i;
	}

	return ZHIHE_TWDSCALE_MAX;
}

static void zhihe_wdt_msg_hdr_fill(struct aon_rpc_msg_hdr *hdr,
				   enum aon_wdg_func func)
{
	memset(hdr, 0, sizeof(*hdr));
	hdr->svc = (uint8_t)AON_RPC_SVC_WDG;
	hdr->func = (uint8_t)func;
	hdr->size = AON_RPC_MSG_NUM;
}

static int zhihe_wdt_is_running(struct zhihe_wdt_device *wdt_dev)
{
	struct zhihe_aon_ipc *ipc = wdt_dev->ipc_handle;
	struct aon_msg_wdg_ctrl_ack ack_msg = { 0 };
	int ret;

	zhihe_wdt_msg_hdr_fill(&wdt_dev->msg.hdr, AON_WDG_FUNC_GET_STATE);
	wdt_dev->msg.running_state = -1;

	ret = zhihe_aon_call_rpc(ipc, &wdt_dev->msg, &ack_msg, true);
	if (ret)
		return ret;

	RPC_GET_BE32(&ack_msg.timeout, 4, &wdt_dev->msg.running_state);

	pr_debug("ret = %d, timeout = %d, running_state = %d\n", ret,
		 wdt_dev->msg.timeout, wdt_dev->msg.running_state);
	return wdt_dev->msg.running_state;
}

static int zhihe_wdt_update_timeout(struct zhihe_wdt_device *wdt_dev,
				    unsigned int timeout)
{
	struct zhihe_aon_ipc *ipc = wdt_dev->ipc_handle;
	struct aon_msg_wdg_ctrl_ack ack_msg = { 0 };
	int ret;

	zhihe_wdt_msg_hdr_fill(&wdt_dev->msg.hdr, AON_WDG_FUNC_TIMEOUTSET);
	RPC_SET_BE32(&wdt_dev->msg.timeout, 0, timeout);

	ret = zhihe_aon_call_rpc(ipc, &wdt_dev->msg, &ack_msg, true);
	if (ret)
		return ret;

	return 0;
}

static int zhihe_wdt_set_timeout(struct watchdog_device *wdd,
				 unsigned int timeout)
{
	struct zhihe_wdt_device *wdt_dev = watchdog_get_drvdata(wdd);
	int ret = 0;

	if (watchdog_active(wdd))
		ret = zhihe_wdt_update_timeout(wdt_dev, timeout);
	else
		wdd->timeout = wdt_timeout[zhihe_wdt_timeout_to_sel(timeout)];

	return ret;
}

static int zhihe_wdt_start(struct watchdog_device *wdd)
{
	struct zhihe_wdt_device *wdt_dev = watchdog_get_drvdata(wdd);
	struct zhihe_aon_ipc *ipc = wdt_dev->ipc_handle;
	struct aon_rpc_ack_common ack_msg = { 0 };

	zhihe_wdt_msg_hdr_fill(&wdt_dev->msg.hdr, AON_WDG_FUNC_START);
	return zhihe_aon_call_rpc(ipc, &wdt_dev->msg, &ack_msg, true);
}

static int zhihe_wdt_stop(struct watchdog_device *wdd)
{
	struct zhihe_wdt_device *wdt_dev = watchdog_get_drvdata(wdd);
	struct zhihe_aon_ipc *ipc = wdt_dev->ipc_handle;
	struct aon_rpc_ack_common ack_msg = { 0 };

	zhihe_wdt_msg_hdr_fill(&wdt_dev->msg.hdr, AON_WDG_FUNC_STOP);
	return zhihe_aon_call_rpc(ipc, &wdt_dev->msg, &ack_msg, true);
}

static int zhihe_wdt_ping(struct watchdog_device *wdd)
{
	struct zhihe_wdt_device *wdt_dev = watchdog_get_drvdata(wdd);
	struct zhihe_aon_ipc *ipc = wdt_dev->ipc_handle;
	struct aon_rpc_ack_common ack_msg = { 0 };

	zhihe_wdt_msg_hdr_fill(&wdt_dev->msg.hdr, AON_WDG_FUNC_PING);
	return zhihe_aon_call_rpc(ipc, &wdt_dev->msg, &ack_msg, true);
}

static int zhihe_wdt_restart(struct watchdog_device *wdd,
			     unsigned long action, void *data)
{
	struct zhihe_wdt_device *wdt_dev = watchdog_get_drvdata(wdd);
	struct zhihe_aon_ipc *ipc = wdt_dev->ipc_handle;
	int ret;

	zhihe_wdt_msg_hdr_fill(&wdt_dev->msg.hdr, AON_WDG_FUNC_RESTART);

	pr_debug("[%s,%d]: Inform aon to restart the whole system\n",
		 __func__, __LINE__);
	ret = zhihe_aon_call_rpc(ipc, &wdt_dev->msg, NULL, false);
	if (ret)
		return ret;

	/* Give AON E902 time to cycle the SY70209 PMIC before fallback warm reset */
	mdelay(ZHIHE_RESET_PROTECTION_MS);
	return 0;
}

static const struct watchdog_info zhihe_watchdog_info = {
	.options = WDIOF_SETTIMEOUT | WDIOF_KEEPALIVEPING,
	.identity = "zhihe Watchdog",
};

static const struct watchdog_ops zhihe_watchdog_ops = {
	.owner = THIS_MODULE,
	.start = zhihe_wdt_start,
	.stop = zhihe_wdt_stop,
	.ping = zhihe_wdt_ping,
	.set_timeout = zhihe_wdt_set_timeout,
	.restart = zhihe_wdt_restart,
};

static ssize_t aon_sys_wdt_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct zhihe_wdt_device *wdt_dev = platform_get_drvdata(pdev);

	return sysfs_emit(buf, "%u\n", wdt_dev->is_aon_wdt_ena);
}

static ssize_t aon_sys_wdt_store(struct device *dev,
				 struct device_attribute *attr,
				 const char *buf, size_t size)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct zhihe_wdt_device *wdt_dev = platform_get_drvdata(pdev);
	struct aon_rpc_ack_common ack_msg = { 0 };
	struct zhihe_aon_ipc *ipc = wdt_dev->ipc_handle;
	unsigned long val;
	int ret;

	ret = kstrtoul(buf, 0, &val);
	if (ret)
		return ret;

	wdt_dev->is_aon_wdt_ena = val;
	if (val)
		zhihe_wdt_msg_hdr_fill(&wdt_dev->msg.hdr, AON_WDG_FUNC_AON_WDT_ON);
	else
		zhihe_wdt_msg_hdr_fill(&wdt_dev->msg.hdr, AON_WDG_FUNC_AON_WDT_OFF);

	ret = zhihe_aon_call_rpc(ipc, &wdt_dev->msg, &ack_msg, true);
	if (ret) {
		dev_err(dev, "%s: err:%d\n", __func__, ret);
		return -EINVAL;
	}
	return size;
}

static void zhihe_pm_power_off(void)
{
	struct zhihe_wdt_device *wdt_dev = zhihe_power_off_wdt;
	struct aon_rpc_ack_common ack_msg = { 0 };
	int ret;

	if (!wdt_dev)
		return;

	pr_info("[%s,%d] poweroff system...\n", __func__, __LINE__);
	zhihe_wdt_msg_hdr_fill(&wdt_dev->msg.hdr, AON_WDG_FUNC_POWER_OFF);
	ret = zhihe_aon_call_rpc(wdt_dev->ipc_handle, &wdt_dev->msg, &ack_msg, true);
	if (ret)
		pr_err("failed to power off the system\n");
}

static DEVICE_ATTR_RW(aon_sys_wdt);

static struct attribute *aon_sys_wdt_sysfs_entries[] = {
	&dev_attr_aon_sys_wdt.attr,
	NULL
};

static const struct attribute_group dev_attr_aon_sys_wdt_group = {
	.attrs = aon_sys_wdt_sysfs_entries,
};

/*
 * Cold-power-cycling the SY70209 PMIC (including avdd33_emmc) requires an RPC
 * to the AON E902 MCU while the mailbox IRQ path is still active, before
 * device_shutdown() runs mmc_shutdown() (EXT_CSD_POWER_OFF_LONG) and tears
 * down mailbox interrupts. Send the RPC at the top of kernel_restart_prepare()
 * via the reboot notifier chain and hold for ZHIHE_RESET_PROTECTION_MS while
 * the AON resets the PMIC.
 */
static int zhihe_wdt_reboot_notify(struct notifier_block *nb,
				   unsigned long action, void *data)
{
	struct zhihe_wdt_device *wdt_dev =
		container_of(nb, struct zhihe_wdt_device, reboot_nb);
	enum aon_wdg_func func;

	switch (action) {
	case SYS_RESTART:
		func = AON_WDG_FUNC_RESTART;
		break;
	case SYS_POWER_OFF:
		func = AON_WDG_FUNC_POWER_OFF;
		break;
	default:
		return NOTIFY_DONE;
	}

	zhihe_wdt_msg_hdr_fill(&wdt_dev->msg.hdr, func);
	zhihe_aon_call_rpc(wdt_dev->ipc_handle, &wdt_dev->msg, NULL, false);
	mdelay(ZHIHE_RESET_PROTECTION_MS);

	return NOTIFY_DONE;
}

static int zhihe_wdt_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct zhihe_wdt_device *wdt_dev;
	struct watchdog_device *wdd;
	int ret;

	wdt_dev = devm_kzalloc(dev, sizeof(*wdt_dev), GFP_KERNEL);
	if (!wdt_dev)
		return -ENOMEM;

	ret = zhihe_aon_get_handle(&wdt_dev->ipc_handle, "aon0");
	if (ret)
		return ret;

	wdd = devm_kzalloc(dev, sizeof(*wdd), GFP_KERNEL);
	if (!wdd)
		return -ENOMEM;

	wdd->info = &zhihe_watchdog_info;
	wdd->ops = &zhihe_watchdog_ops;
	wdd->min_timeout = ZHIHE_WDT_MIN_TIMEOUT;
	wdd->max_timeout = ZHIHE_WDT_MAX_TIMEOUT;
	wdd->min_hw_heartbeat_ms = ZHIHE_WDT_MIN_HEARTBEAT_MS;
	wdd->status = WATCHDOG_NOWAYOUT_INIT_STATUS;

	/* Higher than dw_wdt (128) so emergency restart prefers AON PMIC cold reset */
	watchdog_set_restart_priority(wdd, 192);
	watchdog_set_drvdata(wdd, wdt_dev);

	wdd->timeout = ZHIHE_WDT_TIMEOUT;
	watchdog_init_timeout(wdd, 0, dev);
	zhihe_wdt_set_timeout(wdd, wdd->timeout);

	platform_set_drvdata(pdev, wdt_dev);
	ret = zhihe_wdt_is_running(wdt_dev);
	if (ret < 0) {
		dev_err(dev, "failed to get pmic wdt running state: %d\n", ret);
		return ret;
	}

	if (ret) {
		zhihe_wdt_update_timeout(wdt_dev, wdd->timeout);
		set_bit(WDOG_HW_RUNNING, &wdd->status);
	}

	ret = devm_watchdog_register_device(dev, wdd);
	if (ret)
		return ret;

	wdt_dev->reboot_nb.notifier_call = zhihe_wdt_reboot_notify;
	ret = devm_register_reboot_notifier(dev, &wdt_dev->reboot_nb);
	if (ret)
		return ret;

	pm_power_off = zhihe_pm_power_off;
	zhihe_power_off_wdt = wdt_dev;

	ret = sysfs_create_group(&pdev->dev.kobj, &dev_attr_aon_sys_wdt_group);
	if (ret) {
		dev_err(dev, "Failed to create aon_sys_wdt sysfs\n");
		return ret;
	}

	pr_info("succeed to register a210 pmic watchdog\n");
	return 0;
}

static void zhihe_wdt_remove(struct platform_device *pdev)
{
	sysfs_remove_group(&pdev->dev.kobj, &dev_attr_aon_sys_wdt_group);
	if (pm_power_off == zhihe_pm_power_off)
		pm_power_off = NULL;
	zhihe_power_off_wdt = NULL;
}

static int zhihe_wdt_suspend(struct device *dev)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct zhihe_wdt_device *wdt_dev = platform_get_drvdata(pdev);
	struct aon_rpc_ack_common ack_msg = { 0 };
	int ret;

	zhihe_wdt_msg_hdr_fill(&wdt_dev->msg.hdr, AON_WDG_FUNC_STOP);
	ret = zhihe_aon_call_rpc(wdt_dev->ipc_handle, &wdt_dev->msg, &ack_msg, true);
	if (ret)
		dev_err(dev, "zhihe_wdt_suspend stop failed: %d\n", ret);

	return 0;
}

static int zhihe_wdt_resume(struct device *dev)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct zhihe_wdt_device *wdt_dev = platform_get_drvdata(pdev);
	struct aon_rpc_ack_common ack_msg = { 0 };
	int ret;

	zhihe_wdt_msg_hdr_fill(&wdt_dev->msg.hdr, AON_WDG_FUNC_START);
	ret = zhihe_aon_call_rpc(wdt_dev->ipc_handle, &wdt_dev->msg, &ack_msg, true);
	if (ret)
		dev_err(dev, "zhihe_wdt_resume start failed: %d\n", ret);

	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(zhihe_wdt_pm_ops, zhihe_wdt_suspend, zhihe_wdt_resume);

static const struct of_device_id zhihe_wdt_of_match[] = {
	{ .compatible = "zhihe,aon_wdt" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, zhihe_wdt_of_match);

static struct platform_driver zhihe_wdt_driver = {
	.probe = zhihe_wdt_probe,
	.remove = zhihe_wdt_remove,
	.driver = {
		.name = DRV_NAME,
		.of_match_table = zhihe_wdt_of_match,
		.pm = pm_sleep_ptr(&zhihe_wdt_pm_ops),
	},
};

module_platform_driver(zhihe_wdt_driver);

MODULE_AUTHOR("Wei.Liu <lw312886@linux.alibaba.com>");
MODULE_DESCRIPTION("PMIC Watchdog Driver for a210");
MODULE_LICENSE("GPL");
