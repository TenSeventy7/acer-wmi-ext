/* SPDX-License-Identifier: GPL-2.0-or-later */
/**
 * acer-wmi-ext.c: Extensions for the Acer WMI driver
 *
 * This is a driver for the Acer WMI interface that provides
 * additional functionality for battery health control and
 * calibration modes, among other features. This module is meant
 * to be an extension to the existing Acer WMI driver, allowing
 * users to control functions on their Acer laptops that are
 * not yet available in the mainline kernel.
 * 
 * This module allows both this and the mainline Acer WMI driver
 * to coexist, as it uses a different GUID for its WMI methods.
 * It also provides sysfs attributes to control battery health
 * and calibration modes, which can be set during module
 * initialization or at runtime.
 */

#include <linux/init.h>
#include <linux/delay.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/acpi.h>
#include <linux/wmi.h>

#include <linux/dmi.h>
#include <linux/platform_device.h>
#include <linux/platform_profile.h>
#include <linux/bitfield.h>

MODULE_DESCRIPTION("Acer WMI control extension driver");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Frederik Harwath <frederik@harwath.name>, John Vincent Corcega <git@tenseventyseven.xyz>");
#define WMI_GUID1 	"79772EC5-04B1-4bfd-843C-61E7F77B6CC9"
#define WMI_GUID2	"61EF69EA-865C-4BC3-A502-A0DEBA0CB531"
MODULE_ALIAS("wmi:" WMI_GUID1);

#define ACER_WMID_SET_FUNCTION 1
#define ACER_WMID_GET_FUNCTION 2

#ifdef pr_fmt
#undef pr_fmt
#define pr_fmt(fmt) "%s: " fmt, KBUILD_MODNAME
#endif

struct get_battery_health_control_status_input {
	u8 uBatteryNo;
	u8 uFunctionQuery;
	u8 uReserved[2];
} __packed;

struct get_battery_health_control_status_output {
	u8 uFunctionList;
	u8 uReturn[2];
	u8 uFunctionStatus[5];
} __packed;

struct set_battery_health_control_input {
	u8 uBatteryNo;
	u8 uFunctionMask;
	u8 uFunctionStatus;
	u8 uReservedIn[5];
} __packed;

struct set_battery_health_control_output {
	u8 uReturn;
	u8 uReservedOut;
} __packed;

enum battery_mode { HEALTH_MODE = 1, CALIBRATION_MODE = 2 };

#define ACER_SYSTEM_CONTROL_MODE_EC_OFFSET 0x45
enum system_control_mode {
	   	SYSTEM_CONTROL_BALANCED = 1,
		SYSTEM_CONTROL_SILENT = 2,
		SYSTEM_CONTROL_PERFORMANCE = 3,
};

struct battery_info {
	s8 health_mode;
	s8 calibration_mode;
};

static struct battery_info battery_status;
static short control_mode = -1;

static short enable_health_mode = -1;
static short enable_system_control_mode = -1;

module_param(enable_health_mode, short, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
MODULE_PARM_DESC(
	enable_health_mode,
	"Turn battery health mode on (value > 0) or off (value = 0) during module "
	"initialization (default value < 0: do not modify existing settings.)");

module_param(enable_system_control_mode, short, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
MODULE_PARM_DESC(
	enable_system_control_mode,
	"Set system fan control mode (0: balanced, 1: silent, 2: performance) during "
	"module initialization (default value < 0: do not modify existing settings.)");


 /*
  * WMID ApgeAction interface
  */
static acpi_status
acer_wmi_apgeaction_exec_u64(u32 method_id, u64 in, u64 *out) {
	struct acpi_buffer input = { (acpi_size) sizeof(u64), (void *)(&in) };
	struct acpi_buffer result = { ACPI_ALLOCATE_BUFFER, NULL };
	union acpi_object *obj;
	u64 tmp = 0;
	acpi_status status;
	status = wmi_evaluate_method(WMI_GUID2, 0, method_id, &input, &result);

	if (ACPI_FAILURE(status))
		return status;
	obj = (union acpi_object *) result.pointer;

	if (obj) {
		if (obj->type == ACPI_TYPE_BUFFER) {
			if (obj->buffer.length == sizeof(u32))
				tmp = *((u32 *) obj->buffer.pointer);
			else if (obj->buffer.length == sizeof(u64))
				tmp = *((u64 *) obj->buffer.pointer);
		} else if (obj->type == ACPI_TYPE_INTEGER) {
			tmp = (u64) obj->integer.value;
		}
	}

	if (out)
		*out = tmp;

	kfree(result.pointer);

	return status;
}

struct quirk_entry {
	u8 system_control_mode;
	u8 usb_charge_mode;
};

static struct quirk_entry *quirks;
static struct quirk_entry quirk_unknown = {
};
static struct quirk_entry quirk_acer_system_control_mode = {
	.system_control_mode = 1,
};
static struct quirk_entry quirk_acer_sfg174_73 = {
	.system_control_mode = 1, // Enable system control mode for this model
	.usb_charge_mode = 1, // Enable USB charge mode for this model
};

 /*
  * This quirk table is only for Acer/Gateway/Packard Bell family
  * that those machines are supported by acer-wmi driver.
  */
static int __init dmi_matched(const struct dmi_system_id *dmi)
{
	pr_info("DMI matched: %s\n", dmi->ident);
	quirks = dmi->driver_data;
	return 1;
}

static const struct dmi_system_id acer_quirks[] __initconst = {
	{
		.callback = dmi_matched,
		.ident = "Acer Swift SFG14-73",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Acer"),
			DMI_MATCH(DMI_PRODUCT_NAME, "Swift SFG14-73"),
		},
		.driver_data = &quirk_acer_sfg174_73,
	},
};

/* Find which quirks are needed for a particular vendor/ model pair */
static void __init find_quirks(void)
{
	// For this module, only dynamically loaded quirks are supported.
	dmi_check_system(acer_quirks);

	if (quirks == NULL)
		quirks = &quirk_unknown;
}

static acpi_status
get_battery_health_control_status(struct battery_info *bat_status)
{
	union acpi_object *obj;
	acpi_status status;

	/* Acer Care Center seems to always call the WMI method
	   with fixed parameters. This yields information about
	   the availability and state of both health and
	   calibration mode. The modes probably apply to
	   all batteries of the system - if there are
	   Acer laptops with multiple batteries? */
	struct get_battery_health_control_status_input params = {
		.uBatteryNo = 0x1,
		.uFunctionQuery = 0x1,
		.uReserved = { 0x0, 0x0 }
	};
	struct get_battery_health_control_status_output ret;

	struct acpi_buffer input = {
		sizeof(struct get_battery_health_control_status_input), &params
	};

	struct acpi_buffer output = { ACPI_ALLOCATE_BUFFER, NULL };

	status = wmi_evaluate_method(WMI_GUID1, 0, 20, &input, &output);
	if (ACPI_FAILURE(status))
		return status;

	obj = output.pointer;
	if (!obj)
		return AE_ERROR;
	else if (obj->type != ACPI_TYPE_BUFFER) {
		kfree(obj);
		return AE_ERROR;
	}

	ret = *((struct get_battery_health_control_status_output *)
			obj->buffer.pointer);
	if (obj->buffer.length != 8) {
		pr_err("WMI battery status call returned a buffer of "
		       "unexpected length %d\n", obj->buffer.length);
		kfree(obj);
		return AE_ERROR;
	}

	bat_status->health_mode = ret.uFunctionList & HEALTH_MODE ?
					  ret.uFunctionStatus[0] > 0 :
					  -1;
	bat_status->calibration_mode = ret.uFunctionList & CALIBRATION_MODE ?
					       ret.uFunctionStatus[1] > 0 :
					       -1;

	kfree(obj);

	return status;
}

static acpi_status set_battery_health_control(u8 function, bool function_status)
{
	union acpi_object *obj;
	acpi_status status;

	/* Cf. comment regarding constant argument values in
	   get_battery_health_control_status. */
	struct set_battery_health_control_input params = {
		.uBatteryNo = 0x1,
		.uFunctionMask = function,
		.uFunctionStatus = (u8)function_status,
		.uReservedIn = { 0x0, 0x0, 0x0, 0x0, 0x0 }
	};
	struct set_battery_health_control_output ret;

	struct acpi_buffer input = {
		sizeof(struct set_battery_health_control_input), &params
	};

	struct acpi_buffer output = { ACPI_ALLOCATE_BUFFER, NULL };
	status = wmi_evaluate_method(WMI_GUID1, 0, 21, &input, &output);

	if (ACPI_FAILURE(status))
		return status;

	obj = output.pointer;

	if (!obj)
		return AE_ERROR;
	else if (obj->type != ACPI_TYPE_BUFFER) {
		kfree(obj);
		return AE_ERROR;
	}

	ret = *((struct set_battery_health_control_output *)obj->buffer.pointer);

	if (obj->buffer.length != 4) {
		pr_err("WMI battery status set operation returned "
			"a buffer of unexpected length %d\n",
			obj->buffer.length);
		status = AE_ERROR;
	}

	kfree(obj);

	return status;
}

static void print_modes(const char *prefix, bool print_if_empty,
			bool health_mode, bool calib_mode)
{
	if (!health_mode && !calib_mode && !print_if_empty)
		return;

	pr_info("%s modes: %s%s%s\n", prefix, health_mode ? "health mode" : "",
		health_mode && calib_mode ? ", " : "",
		calib_mode ? "calibration mode" : "");
}

static acpi_status init_state(void)
{
	bool print_state_if_empty;
	acpi_status status;
	status = get_battery_health_control_status(&battery_status);

	if (ACPI_FAILURE(status))
		return status;

	print_state_if_empty = true;
	print_modes("available", print_state_if_empty,
		    battery_status.health_mode >= 0,
		    battery_status.calibration_mode >= 0);

	print_state_if_empty = false;
	print_modes("active", print_state_if_empty,
		    battery_status.health_mode > 0,
		    battery_status.calibration_mode > 0);

	return status;
}

static void update_state(void)
{
	struct battery_info old_state = battery_status;
	get_battery_health_control_status(&battery_status);
	if (battery_status.calibration_mode != old_state.calibration_mode)
		pr_info("%s calibration mode\n",
			battery_status.calibration_mode ? "enabled" :
							  "disabled");
	if (battery_status.health_mode != old_state.health_mode)
		pr_info("%s health mode\n",
			battery_status.health_mode ? "enabled" : "disabled");
}

static ssize_t health_mode_show(struct device_driver *driver, char *buf)
{
	int len = sprintf(buf, "%d\n", battery_status.health_mode);
	if (len <= 0)
		pr_err("Invalid sprintf len: %d\n", len);

	return len;
}

static ssize_t health_mode_store(struct device_driver *driver, const char *buf,
				 size_t count)
{
	bool param_val;
	int err;
	if (battery_status.health_mode < 0)
		return 0;

	err = kstrtobool(buf, &param_val);
	if (err)
		return err;

	set_battery_health_control(HEALTH_MODE, param_val);
	update_state();

	return count;
}

static ssize_t calibration_mode_show(struct device_driver *driver, char *buf)
{
	int len = sprintf(buf, "%d\n", battery_status.calibration_mode);
	if (len <= 0)
		pr_err("Invalid sprintf len: %d\n", len);

	return len;
}

static ssize_t calibration_mode_store(struct device_driver *driver,
				      const char *buf, size_t count)
{
	bool param_val;
	int err;

	if (battery_status.calibration_mode < 0)
		return 0;

	err = kstrtobool(buf, &param_val);
	if (err)
		return err;

	set_battery_health_control(CALIBRATION_MODE, param_val);
	update_state();

	return count;
}

static ssize_t system_control_mode_show(struct device_driver *driver, char *buf)
{
	int len = sprintf(buf, "%d\n", control_mode);
	if (len <= 0)
		pr_err("Invalid sprintf len: %d\n", len);

	return len;
}

static ssize_t system_control_mode_store(struct device_driver *driver,
					 const char *buf, size_t count)
{
	int param_val;
	int err;

	if (control_mode < 0)
		return 0;

	err = kstrtoint(buf, 10, &param_val);
	if (err)
		return err;

	if (param_val < SYSTEM_CONTROL_BALANCED ||
	    param_val > SYSTEM_CONTROL_PERFORMANCE) {
		pr_err("Invalid system control mode value: %d\n", param_val);
		return -EINVAL;
	}

	pr_err("Setting system control mode to %d\n", param_val);

	err = ec_write(ACER_SYSTEM_CONTROL_MODE_EC_OFFSET, param_val);
	if (err < 0) {
		pr_err("Failed to write system control mode to EC: %d\n", err);
		return err;
	}

	control_mode = param_val;
	pr_info("System control mode set to %d\n", control_mode);

	return count;
}

static int usb_charge_mode_enable = 0;
static void init_usb_charge_mode(void)
{
	acpi_status status;
	u64 result;

	if (quirks->usb_charge_mode == 0) {
		pr_info("USB charging mode quirk not enabled, skipping initialization\n");
		return;
	}

	if (!wmi_has_guid(WMI_GUID2)) {
		pr_info("Acer USB charging control guid not found\n");
		return;
	}

	status = acer_wmi_apgeaction_exec_u64(ACER_WMID_GET_FUNCTION, 0x4, &result);
	if (ACPI_FAILURE(status)) {
		pr_err("Error getting usb charging status: %s\n", acpi_format_exception(status));
		return;
	}

	pr_info("usb charging get status: %llu\n", result);
	switch (result) {
	case 663296: // Turn off usb charging
		usb_charge_mode_enable = 0;
		break;
	case 659200: // Set usb charging to 10%
	case 1314560: // Set usb charging to 20%
	case 1969920: // Set usb charging to 30%
		usb_charge_mode_enable = 1;
		break;
	default:
		usb_charge_mode_enable = -1; // Unknown value
	}
}

static ssize_t usb_charge_mode_show(struct device_driver *driver, char *buf)
{
     return sprintf(buf, "%d\n", usb_charge_mode_enable); //-1 means unknown value
}

static ssize_t usb_charge_mode_store(struct device_driver *driver,
				      const char *buf, size_t count)
{
	acpi_status status;
	u64 result;
	u64 input_value;
	u8 val;

	if (quirks->usb_charge_mode == 0) {
		pr_info("USB charging mode quirk not enabled, skipping store\n");
		return -EOPNOTSUPP;
	}

	if (sscanf(buf, "%hhd", &val) != 1)
		return -EINVAL;

	switch (val) {
	case 0:
		input_value = 663300; // Turn off usb charging
		break;
	case 1:
		input_value = 1969924; // Set usb charging to 30%
		break;
	default:
		pr_err("Unknown usb charging value: %d\n", val);
		return -EINVAL;
	}

	pr_info("usb charging set value: %d\n", val);
	usb_charge_mode_enable = val;
	status = acer_wmi_apgeaction_exec_u64(ACER_WMID_SET_FUNCTION, input_value, &result);

	if (ACPI_FAILURE(status)) {
		pr_err("Error setting usb charging status: %s\n", acpi_format_exception(status));
		return -ENODEV;
	}

	pr_info("usb charging set status: %llu\n", result);
	return count;
}

static ssize_t usb_charge_limit_show(struct device_driver *driver, char *buf)
{

     acpi_status status;
     u64 result;
	 int ret;

	 if (quirks->usb_charge_mode == 0) {
		 pr_info("USB charging limit quirk not enabled, skipping show\n");
		 return -EOPNOTSUPP;
	 }

     status = acer_wmi_apgeaction_exec_u64(ACER_WMID_GET_FUNCTION, 0x4, &result);
     if (ACPI_FAILURE(status)) {
         pr_err("Error getting usb charging limit: %s\n", acpi_format_exception(status));
         return -ENODEV;
     }

     pr_info("usb charging get limit: %llu\n", result);
	 switch (result) {
		case 659200: // Set usb charging to 10%
			ret = 10;
			break;
		case 1314560: // Set usb charging to 20%
			ret = 20;
			break;
		case 1969920: // Set usb charging to 30%
			ret = 30;
			break;
		default:
			ret = -1; // Unknown value or off
	 }

     return sprintf(buf, "%d\n", ret); //-1 means unknown value

}

static ssize_t usb_charge_limit_store(struct device_driver *driver,
				      const char *buf, size_t count)
{
	acpi_status status;
	u64 result;
	u64 input_value;
	u8 val;

	if (quirks->usb_charge_mode == 0) {
		pr_info("USB charging limit quirk not enabled, skipping store\n");
		return -EOPNOTSUPP;
	}

	// Ensure current value isn't 'off'
	if (usb_charge_mode_enable == 0) {
		pr_err("USB charging is off, cannot set limit\n");
		return -EINVAL;
	}

	if (sscanf(buf, "%hhd", &val) != 1)
		return -EINVAL;

	switch (val) {
	case 10:
		input_value = 659204; // Set usb charging to 10%
		break;
	case 20:
		input_value = 1314564; // Set usb charging to 20%
		break;
	case 30:
		input_value = 1969924; // Set usb charging to 30%
		break;
	default:
		pr_err("Unknown usb charging limit value: %d\n", val);
		return -EINVAL;
	}

	pr_info("usb charging set limit value: %d\n", val);
	status = acer_wmi_apgeaction_exec_u64(ACER_WMID_SET_FUNCTION, input_value, &result);

	if (ACPI_FAILURE(status)) {
		pr_err("Error setting usb charging limit: %s\n", acpi_format_exception(status));
		return -ENODEV;
	}

	pr_info("usb charging set limit: %llu\n", result);
	return count;
}

static DRIVER_ATTR_RW(health_mode);
static DRIVER_ATTR_RW(calibration_mode);
static DRIVER_ATTR_RW(system_control_mode);
static DRIVER_ATTR_RW(usb_charge_mode);
static DRIVER_ATTR_RW(usb_charge_limit);

static struct attribute *acer_wmi_ext_attrs[] = {
	&driver_attr_health_mode.attr, &driver_attr_calibration_mode.attr, &driver_attr_system_control_mode.attr, &driver_attr_usb_charge_mode.attr, &driver_attr_usb_charge_limit.attr, NULL
};

ATTRIBUTE_GROUPS(acer_wmi_ext);

static const struct wmi_device_id acer_wmi_ext_id_table[] = {
	{ .guid_string = WMI_GUID1 },
	{},
};

static struct wmi_driver acer_wmi_ext_driver = {
	.driver = { .name = "acer-wmi-ext",
		    .groups = acer_wmi_ext_groups },
};

static int system_control_mode_inited = 0;
static int
acer_system_control_mode_init(void)
{
	u8 tp;
	int err;

	system_control_mode_inited = 1;

	err = ec_read(ACER_SYSTEM_CONTROL_MODE_EC_OFFSET, &tp);
	if (err < 0) {
		pr_err("Failed to read system control mode from EC: %d\n", err);
		return err;
	}

	pr_err("System control mode: %d\n", tp);
	control_mode = tp;

	if (enable_system_control_mode >= 0) {
		if (enable_system_control_mode < SYSTEM_CONTROL_BALANCED ||
		    enable_system_control_mode > SYSTEM_CONTROL_PERFORMANCE) {
			pr_err("Invalid system control mode value: %d\n",
			       enable_system_control_mode);
			return -EINVAL;
		}

		pr_info("Setting system control mode to %d\n",
			enable_system_control_mode);

		err = ec_write(ACER_SYSTEM_CONTROL_MODE_EC_OFFSET,
			       enable_system_control_mode);
		if (err < 0) {
			pr_err("Failed to write system control mode to EC: %d\n",
			       err);
			return err;
		}

		control_mode = enable_system_control_mode;
		pr_info("System control mode set to %d\n", control_mode);
	}

	return 0;
}

/*
 * Platform profile support
 */
static struct device *platform_profile_device;
static bool platform_profile_support;

static int
acer_platform_profile_probe(void *drvdata, unsigned long *choices)
{
	int err;

	if (!quirks->system_control_mode) {
		pr_info("System control mode quirk not enabled, skipping platform profile probe\n");
		return -EOPNOTSUPP;
	}

	// Initialize the system control mode if it hasn't been done yet
	if (!system_control_mode_inited) {
		err = acer_system_control_mode_init();
		if (err)
			return err;
	}

	// Add choices for platform profiles
    set_bit(PLATFORM_PROFILE_LOW_POWER, choices);
	set_bit(PLATFORM_PROFILE_BALANCED, choices);
	set_bit(PLATFORM_PROFILE_PERFORMANCE, choices);

	return 0;
}

static int
acer_platform_profile_get(struct device *dev,
					enum platform_profile_option *profile)
{
	switch (control_mode) {
	case SYSTEM_CONTROL_BALANCED:
		*profile = PLATFORM_PROFILE_BALANCED;
		break;
	case SYSTEM_CONTROL_PERFORMANCE:
		*profile = PLATFORM_PROFILE_PERFORMANCE;
		break;
	case SYSTEM_CONTROL_SILENT:
		*profile = PLATFORM_PROFILE_LOW_POWER;
		break;
	default:
		return -EOPNOTSUPP;
	}

	return 0;
}

static int
acer_platform_profile_set(struct device *dev,
					enum platform_profile_option profile)
{
	int err;
	if (!quirks->system_control_mode) {
		pr_info("System control mode quirk not enabled, skipping platform profile set\n");
		return -EOPNOTSUPP;
	}

	int new_mode;
	switch (profile) {
	case PLATFORM_PROFILE_BALANCED:
		new_mode = SYSTEM_CONTROL_BALANCED;
		break;
	case PLATFORM_PROFILE_PERFORMANCE:
		new_mode = SYSTEM_CONTROL_PERFORMANCE;
		break;
	case PLATFORM_PROFILE_LOW_POWER:
		new_mode = SYSTEM_CONTROL_SILENT;
		break;
	default:
		pr_err("Unsupported platform profile option: %d\n", profile);
		return -EOPNOTSUPP;
	}

	if (new_mode == control_mode) {
		pr_info("Platform profile already set to %d, no change needed\n",
			new_mode);
		return 0;
	}

	pr_info("Setting platform profile to %d\n", new_mode);
	err = ec_write(ACER_SYSTEM_CONTROL_MODE_EC_OFFSET, new_mode);
	if (err < 0) {
		pr_err("Failed to set platform profile: %d\n", err);
		return err;
	}

	control_mode = new_mode;
	pr_info("Platform profile set to %d\n", control_mode);

	return 0;
}
 

static const struct platform_profile_ops acer_platform_profile_ops = {
	.probe = acer_platform_profile_probe,
	.profile_get = acer_platform_profile_get,
	.profile_set = acer_platform_profile_set,
};

static int acer_platform_profile_setup(struct platform_device *pdev)
{
	const int max_retries = 10;
	int delay_ms = 100;
	if (!quirks->system_control_mode) {
		pr_info("System control mode quirk not enabled, skipping platform profile setup\n");
		return 0;
	}

	pr_info("Setting up platform profile support\n");

	for (int attempt = 1; attempt <= max_retries; attempt++) {
		platform_profile_device = devm_platform_profile_register(
			&pdev->dev, "acer-wmi-ext", NULL, &acer_platform_profile_ops);

		if (!IS_ERR(platform_profile_device)) {
			platform_profile_support = true;
			pr_info("Platform profile registered successfully (attempt %d)\n", attempt);
			return 0;
		}

		pr_warn("Platform profile registration failed (attempt %d/%d), error: %ld\n",
				attempt, max_retries, PTR_ERR(platform_profile_device));

		if (attempt < max_retries) {
			msleep(delay_ms);
			delay_ms = min(delay_ms * 2, 1000);
		}
	}

	return PTR_ERR(platform_profile_device);
}

/*
 * Platform device
 */
#ifdef CONFIG_PM_SLEEP
static int acer_ext_suspend(struct device *dev)
{
	return 0;
}

static int acer_ext_resume(struct device *dev)
{
	return 0;
}
#else
#define acer_ext_suspend	NULL
#define acer_ext_resume	NULL
#endif

static SIMPLE_DEV_PM_OPS(acer_ext_pm, acer_ext_suspend, acer_ext_resume);
static int acer_ext_platform_probe(struct platform_device *device)
{
	int err;

	pr_info("Acer WMI extension platform driver probe\n");
	err = acer_platform_profile_setup(device);
	if (err)
		return err;

	return 0;
}

static void acer_ext_platform_remove(struct platform_device *device)
{
}

static void acer_ext_platform_shutdown(struct platform_device *device)
{
}

static struct platform_device *acer_ext_platform_device;
static struct platform_driver acer_ext_platform_driver = {
	.driver = {
		.name = "acer-wmi-ext",
		.pm = &acer_ext_pm,
	},
	.probe = acer_ext_platform_probe,
	.remove = acer_ext_platform_remove,
	.shutdown = acer_ext_platform_shutdown,
};
 

static int __init acer_wmi_ext_init(void)
{
	int err;
    find_quirks();

	if (wmi_has_guid(WMI_GUID1)) {
		if (enable_health_mode >= 0) {
			acpi_status status;
			status = set_battery_health_control(HEALTH_MODE,
								enable_health_mode);

			if (ACPI_FAILURE(status))
				return -EIO;
		}

		if (ACPI_FAILURE(init_state()))
			return -EIO;
	} else {
		pr_info("Acer battery control guid not found\n");
	}
	
	if (quirks->system_control_mode) {
		acer_system_control_mode_init();
	}

	if (quirks->usb_charge_mode) {
		init_usb_charge_mode();
	}

	err = platform_driver_register(&acer_ext_platform_driver);
	if (err) {
		pr_err("Unable to register platform driver\n");
		goto error_platform_register;
	}

	acer_ext_platform_device = platform_device_alloc("acer-wmi-ext", PLATFORM_DEVID_NONE);
	if (!acer_ext_platform_device) {
		err = -ENOMEM;
		goto error_device_alloc;
	}

	err = platform_device_add(acer_ext_platform_device);
	if (err)
		goto error_device_add;

	pr_info("Acer WMI extension driver initialized\n");
	return wmi_driver_register(&acer_wmi_ext_driver);

error_device_add:
	platform_device_put(acer_ext_platform_device);
error_device_alloc:
	platform_driver_unregister(&acer_ext_platform_driver);
error_platform_register:
	wmi_driver_unregister(&acer_wmi_ext_driver);
	return err;
}

static void __exit acer_wmi_ext_exit(void)
{
	platform_device_unregister(acer_ext_platform_device);
	platform_driver_unregister(&acer_ext_platform_driver);
	wmi_driver_unregister(&acer_wmi_ext_driver);
}

module_init(acer_wmi_ext_init);
module_exit(acer_wmi_ext_exit);
