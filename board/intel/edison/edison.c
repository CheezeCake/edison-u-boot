#include <common.h>
#include <dwc3-uboot.h>
#include <asm/cache.h>
#include <usb.h>
#include <linux/usb/gadget.h>
#include <intel_scu_ipc.h>
#include <watchdog.h>
#include <asm/u-boot-x86.h>
#include <part_efi.h>
#include <fb_mmc.h>

DECLARE_GLOBAL_DATA_PTR;

#ifndef CONFIG_WATCHDOG_HEARTBEAT
#define WATCHDOG_HEARTBEAT 30
#else
#define WATCHDOG_HEARTBEAT CONFIG_WATCHDOG_HEARTBEAT
#endif

#define PRODUCT_NAME "edison"

#define BLOCK_SIZE 512
#define ONE_MiB (1024 * 1024)

#define OSIP_MAGIC 0x24534f24;
struct osii_entry {
	uint16_t os_rev_minor;
	uint16_t os_rev_major;
	uint32_t image_lba;
	uint32_t load_address;
	uint32_t start_address;
	uint32_t image_size_blocks;
	uint8_t  attribute;
	uint8_t  reserved3[3];
} __attribute__((packed));

struct osip {
	uint32_t osip_magic;
	uint8_t  reserved;
	uint8_t  version_minor;
	uint8_t  version_major;
	uint8_t  header_checksum;
	uint8_t  number_of_pointers;
	uint8_t  number_of_images;
	uint16_t header_size;
	uint32_t reserved2[5];
	struct osii_entry osii[0];
} __attribute__((packed));

enum {
	SCU_WATCHDOG_START = 0,
	SCU_WATCHDOG_STOP,
	SCU_WATCHDOG_KEEPALIVE,
	SCU_WATCHDOG_SET_ACTION_ON_TIMEOUT
};

#define IPC_CMD(cmd, sub) (sub << 12 | cmd)

static struct dwc3_device dwc3_device_data = {
	.maximum_speed = USB_SPEED_HIGH,
	.base = CONFIG_SYS_USB_OTG_BASE,
	.dr_mode = USB_DR_MODE_PERIPHERAL,
	.index = 0,
};

int usb_gadget_handle_interrupts(int controller_index)
{
	dwc3_uboot_handle_interrupt(controller_index);
	WATCHDOG_RESET();
	return 0;
}

int board_usb_init(int index, enum usb_init_type init)
{
	if (index == 0 && init == USB_INIT_DEVICE)
		return dwc3_uboot_init(&dwc3_device_data);
	else
		return -EINVAL;
}

int board_usb_cleanup(int index, enum usb_init_type init)
{
	if (index == 0 && init == USB_INIT_DEVICE) {
		dwc3_uboot_exit(index);
		return 0;
	} else
		return -EINVAL;
}

const char* fb_get_product_name(void)
{
	return PRODUCT_NAME;
}

void watchdog_reset(void)
{
	ulong now = timer_get_us();

	/* do not flood SCU */
	if (unlikely((now - gd->arch.tsc_prev) > (WATCHDOG_HEARTBEAT * 1000000)))
	{
		gd->arch.tsc_prev = now;
		intel_scu_ipc_send_command(IPC_CMD(IPCMSG_WATCHDOG_TIMER,
					SCU_WATCHDOG_KEEPALIVE));
	}
}

int watchdog_disable(void)
{
	return (intel_scu_ipc_simple_command(IPCMSG_WATCHDOG_TIMER,
				SCU_WATCHDOG_STOP));
}

int watchdog_init(void)
{
	return (intel_scu_ipc_simple_command(IPCMSG_WATCHDOG_TIMER,
				SCU_WATCHDOG_START));
}

char* board_get_reboot_target(void)
{
	uint8_t recovery_mode_gpio = !(*(uint8_t*)0xff00800b & 0x20);

	if (recovery_mode_gpio)
		return "fastboot";

	uint8_t target = *(uint8_t*)0xfffff807;
	*(uint8_t*)0xfffff807 = 0;
	*(uint8_t*)0xfffff81f += target;

	intel_scu_ipc_raw_cmd(0xe4, 0, NULL, 0, NULL, 0, 0, 0xffffffff);

	switch (target) {
	case 0x0c: return "recovery";
	case 0x0e: return "fastboot";
	default: return "";
	}
}

int fb_set_reboot_flag(void)
{
	uint8_t previous_target = *(uint8_t*)0xfffff807;
	*(uint8_t*)0xfffff807 = 0x0e;
	*(uint8_t*)0xfffff81f += previous_target - 0x0e;

	intel_scu_ipc_raw_cmd(0xe4, 0, NULL, 0, NULL, 0, 0, 0xffffffff);

	return 0;
}


int board_verify_gpt_parts(void *_frag)
{
	uint8_t u_boot_label[] = {
		'u', 0, '-', 0, 'b', 0, 'o', 0, 'o', 0, 't', 0, 0, 0,
	};
	uint8_t factory_label[] = {
		'f', 0, 'a', 0, 'c', 0, 't', 0, 'o', 0, 'r', 0, 'y', 0, 0, 0,
	};

	uint8_t security_label[] = {
		's', 0, 'e', 0, 'c', 0, 'u', 0, 'r', 0, 'i', 0, 't', 0, 'y', 0, 0, 0,
	};

	struct gpt_frag *frag = _frag;
	legacy_mbr *mbr = _frag;
	gpt_header *gpt_h = _frag + (GPT_PRIMARY_PARTITION_TABLE_LBA * BLOCK_SIZE);
	gpt_entry *gpt_e = _frag + (le64_to_cpu(gpt_h->partition_entry_lba) * BLOCK_SIZE);
	unsigned long long part_size = 0;

	if (mbr->signature == MSDOS_MBR_SIGNATURE) {
		if (memcmp(u_boot_label, gpt_e->partition_name, sizeof(u_boot_label)))
			return -EINVAL;
		part_size = (gpt_e->ending_lba - gpt_e->starting_lba + 1) * BLOCK_SIZE / ONE_MiB;
		if (part_size != 5)
			return -EINVAL;

		gpt_e++;
		if (memcmp(factory_label, gpt_e->partition_name, sizeof(factory_label)))
			return -EINVAL;
		part_size = (gpt_e->ending_lba - gpt_e->starting_lba + 1) * BLOCK_SIZE / ONE_MiB;
		if (part_size != 1)
			return -EINVAL;

		gpt_e++;
		if (memcmp(security_label, gpt_e->partition_name, sizeof(security_label)))
			return -EINVAL;
		part_size = (gpt_e->ending_lba - gpt_e->starting_lba + 1) * BLOCK_SIZE / ONE_MiB;
		if (part_size != 1)
			return -EINVAL;
	} else {

		if (memcmp(u_boot_label, frag->parts[0].label, sizeof(u_boot_label)))
			return -EINVAL;
		if (frag->parts[0].size_mib != 5)
			return -EINVAL;

		if (memcmp(factory_label, frag->parts[1].label, sizeof(factory_label)))
			return -EINVAL;
		if (frag->parts[1].size_mib != 1)
			return -EINVAL;

		if (memcmp(security_label, frag->parts[2].label, sizeof(security_label)))
			return -EINVAL;
		if (frag->parts[2].size_mib != 1)
			return -EINVAL;
	}

	return 0;
}

int board_populate_mbr_boot_code(legacy_mbr *mbr)
{
	struct osip *osip = (struct osip*)mbr->boot_code;
	uint8_t *i, checksum = 0;

	osip->osip_magic                = OSIP_MAGIC;
	osip->version_major             = 1;
	osip->version_minor             = 0;
	osip->number_of_pointers        = 1;
	osip->number_of_images          = 1;
	osip->header_size               = sizeof(*osip) + sizeof(osip->osii[0]);
	if (mbr->signature == MSDOS_MBR_SIGNATURE)
		osip->osii[0].image_lba = 0x00000028;
	else
		osip->osii[0].image_lba = 0x00000800; /* 1 MiB */
	osip->osii[0].load_address      = 0x01100000;
	osip->osii[0].start_address     = 0x01101000;
	osip->osii[0].image_size_blocks = 0x00002800; /* 5 MiB */
	osip->osii[0].attribute         = 0x0000000f;

	for (i = (uint8_t*)osip; i < ((uint8_t*)osip) + osip->header_size; i++)
		checksum ^= *i;
	osip->header_checksum = checksum;

	memset(&osip->osii[1], 0xff, sizeof(struct osii_entry) * 14);

	return 0;
}

const char* fb_get_wipe_userdata_message(void)
{
	return "Press RM button for YES or FW button for NO";
}

bool fb_get_wipe_userdata_response(void)
{
	uint8_t y_gpio, n_gpio, retries = 100;

	do {
		y_gpio = !(*(uint8_t*)0xff00800b & 0x20); // default 0
		n_gpio = !(*(uint8_t*)0xff00800b & 0x40); // default 1
		mdelay(100);
		retries--;
	} while (retries && y_gpio == 0 && n_gpio == 1);

	if (y_gpio == 1) {
		return true;
	}
	return false;
}
