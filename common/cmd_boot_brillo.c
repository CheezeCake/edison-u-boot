/*
 * Copyright (c) 2015 Intel Corporation
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * * Redistributions of source code must retain the above copyright
 *   notice, this list of conditions and the following disclaimer.
 *
 * * Redistributions in binary form must reproduce the above copyright
 *   notice, this list of conditions and the following disclaimer in
 *   the documentation and/or other materials provided with the
 *   distribution.
 *
 * * Neither the name of the Intel Corporation nor the names of its
 *   contributors may be used to endorse or promote products derived
 *   from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * INTEL OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
 * OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * EXPORT LAWS: THIS LICENSE ADDS NO RESTRICTIONS TO THE EXPORT LAWS
 * OF YOUR JURISDICTION. It is licensee's responsibility to comply
 * with any export regulations applicable in licensee's
 * jurisdiction. Under CURRENT (May 2000) U.S. export regulations this
 * software is eligible for export from the U.S. and can be downloaded
 * by or otherwise exported or reexported worldwide EXCEPT to
 * U.S. embargoed destinations which include Cuba, Iraq, Libya, North
 * Korea, Iran, Syria, Sudan, Afghanistan and any other country to
 * which the U.S. has embargoed goods and services.
 */

/*
 * SPDX-License-Identifier:	Intel
 */

#include <common.h>
#include <asm/bootm.h>
#include <asm/zimage.h>
#include <android_image.h>
#include <command.h>
#include <errno.h>
#include <memalign.h>
#include <android_bootloader.h>
#include <cmd_boot_brillo.h>
#include <mmc.h>

#define BOOT_SIGNATURE_MAX_SIZE 4096
#define BOOT_MAX_IMAGE_SIZE (32 * 1024 * 1024) /* 32 MiB */

#ifndef CONFIG_BRILLO_MMC_BOOT_DEVICE
#define CONFIG_BRILLO_MMC_BOOT_DEVICE CONFIG_FASTBOOT_FLASH_MMC_DEV
#endif

#define SLOT_SUFFIX_STR "androidboot.slot_suffix="

#define BOOTCTRL_SUFFIX_A           "_a"
#define BOOTCTRL_SUFFIX_B           "_b"
#define BOOTCTRL_SUFFIX_NA			"no suffix available"

#define BOOT_CONTROL_VERSION    1

struct slot_metadata {
	uint8_t priority : 4;
	uint8_t tries_remaining : 3;
	uint8_t successful_boot : 1;
} __attribute__((packed));

#define BOOT_CTRL_MAGIC 0x42424100

struct boot_ctrl {
	/* Magic for identification - '\0ABB' (Boot Contrl Magic) */
	uint32_t magic;

	/* Version of struct. */
	uint8_t version;

	/* Information about each slot. */
	struct slot_metadata slot_info[2];
	uint8_t recovery_tries_remaining;
} __attribute__((packed));

/* To be overridden by whatever the board's implementation is of
 * 'adb reboot fastboot' and the like.
 */
__weak char* board_get_reboot_target(void) { return NULL; }

/* Properly initialise the metadata partition */
static void ab_init_default_metadata(struct boot_ctrl *ctrl)
{
	ctrl->magic = BOOT_CTRL_MAGIC;
	ctrl->version = 1;

	ctrl->slot_info[0].priority = 0;
	ctrl->slot_info[0].tries_remaining = 0;
	ctrl->slot_info[0].successful_boot = 0;

	ctrl->slot_info[1].priority = 0;
	ctrl->slot_info[1].tries_remaining = 0;
	ctrl->slot_info[1].successful_boot = 0;

	ctrl->recovery_tries_remaining = 0;
}

static int ab_read_bootloader_message(block_dev_desc_t *dev,
		disk_partition_t *misc_part, struct bootloader_message *msg)
{
	void *tmp;
	lbaint_t blkcnt;
	struct boot_ctrl *ctrl = (struct boot_ctrl*)&msg->slot_suffix;

	blkcnt = BLOCK_CNT(sizeof(*msg), dev);
	tmp = calloc(blkcnt, dev->blksz);

	if (!tmp)
		return -ENOMEM;

	if (dev->block_read(dev->dev, misc_part->start, blkcnt, tmp)
			!= blkcnt) {
		free(tmp);
		return -EIO;
	}

	memcpy(msg, tmp, sizeof(*msg));
	free(tmp);

	/* Check if the metadata is correct. */
	if (ctrl->magic != BOOT_CTRL_MAGIC) {
		printf("WARNING: A/B Selector Metadata is not initialised"
		       " or corrupted, using defaults\n");
		ab_init_default_metadata(ctrl);
	}

	return 0;
}

static int ab_write_bootloader_message(block_dev_desc_t *dev,
		disk_partition_t *misc_part, struct bootloader_message *msg)
{
	void *tmp;
	lbaint_t blkcnt, i;

	blkcnt = BLOCK_CNT(sizeof(*msg), dev);
	tmp = calloc(blkcnt, dev->blksz);

	if (!tmp)
		return -ENOMEM;

	memcpy(tmp, msg, sizeof(*msg));

	for (i = 0; i < blkcnt; i++) {
		if (dev->block_write(dev->dev, misc_part->start + i, 1,
				tmp + i * dev->blksz) != 1) {
			free(tmp);
			return -EIO;
		}
	}

	free(tmp);
	return 0;
}

int ab_set_active(int slot_num)
{
	struct bootloader_message message;
	struct boot_ctrl *metadata = (struct boot_ctrl*)&message.slot_suffix;
	struct slot_metadata *slot = &metadata->slot_info[slot_num];
	struct slot_metadata *other_slot = &metadata->slot_info[slot_num ^ 1];
	block_dev_desc_t *dev;
	disk_partition_t misc_part;
	int ret;

	if (slot_num < 0 || slot_num > 1)
		return -EINVAL;

	if (!(dev = get_dev("mmc", CONFIG_BRILLO_MMC_BOOT_DEVICE)))
		return -ENODEV;

	if (get_partition_info_efi_by_name(dev, "misc", &misc_part))
		return -ENOENT;

	if ((ret = ab_read_bootloader_message(dev, &misc_part, &message)))
		return ret;

	slot->priority = 15;
	slot->tries_remaining = 7;
	slot->successful_boot = 0;
	if (other_slot->priority == 15)
		other_slot->priority = 14;

	if ((ret = ab_write_bootloader_message(dev, &misc_part, &message)))
		return ret;

	return 0;
}

/* Loads an android boot image from the given device and named
 * partition into CONFIG_SYS_LOAD_ADDR.
 */
static int load_boot_image(block_dev_desc_t *dev, const char *part_name)
{
	struct andr_img_hdr *hdr = (struct andr_img_hdr*)CONFIG_SYS_LOAD_ADDR;
	void *rest;
	disk_partition_t boot_part;
	lbaint_t hdr_blkcnt, rest_blkcnt;

	if (get_partition_info_efi_by_name(dev, part_name, &boot_part))
		return -ENOENT;

	hdr_blkcnt = BLOCK_CNT(sizeof(*hdr), dev);
	if (dev->block_read(dev->dev, boot_part.start, hdr_blkcnt,
			(void*)CONFIG_SYS_LOAD_ADDR) != hdr_blkcnt)
		return -EIO;

	if (android_image_check_header(hdr))
		return -EINVAL;

	if (hdr->page_size < 2048 || hdr->page_size > 16384
			|| hdr->page_size & (hdr->page_size - 1))
		return -EINVAL;

	rest = ((void*)hdr) + (hdr_blkcnt * dev->blksz);

	rest_blkcnt = BLOCK_CNT((uint64_t)hdr->page_size
		+ (uint64_t)ROUND(hdr->kernel_size,  hdr->page_size)
		+ (uint64_t)ROUND(hdr->ramdisk_size, hdr->page_size)
		+ (uint64_t)ROUND(hdr->second_size,  hdr->page_size)
		+ (uint64_t)BOOT_SIGNATURE_MAX_SIZE,
		dev) - hdr_blkcnt;

	if (hdr_blkcnt + rest_blkcnt > BLOCK_CNT(BOOT_MAX_IMAGE_SIZE, dev))
		return -EINVAL;

	if (dev->block_read(dev->dev, boot_part.start + hdr_blkcnt,
			rest_blkcnt, rest) != rest_blkcnt)
		return -EIO;

	return 0;
}

static void append_to_bootargs(const char *str) {
	char *bootargs = getenv("bootargs");
	if (!bootargs) {
		if (*str == ' ') str++;
		setenv("bootargs", str);
		return;
	}
	int newlen = strlen(str) + strlen(bootargs) + 1;
	char *newbootargs = malloc(newlen);
	snprintf(newbootargs, newlen, "%s%s", bootargs, str);
	setenv("bootargs", newbootargs);
	free(newbootargs);
}

/* Boots to the image already successfully loaded at
 * CONFIG_SYS_LOAD_ADDR. If the function returns then booting has
 * failed.
 */
static void boot_image(void)
{
	struct andr_img_hdr *hdr = (struct andr_img_hdr*)CONFIG_SYS_LOAD_ADDR;
	ulong load_address;
	char *bootargs = getenv("bootargs");

	if (bootargs) {
		bootargs = strdup(bootargs);
		setenv("bootargs", hdr->cmdline);
		append_to_bootargs(" ");
		append_to_bootargs(bootargs);
		free(bootargs);
	} else
		setenv("bootargs", hdr->cmdline);

	struct boot_params *params =
		load_zimage((void*)CONFIG_SYS_LOAD_ADDR + hdr->page_size,
			hdr->kernel_size, &load_address);
	setup_zimage(params, ((void*)params) + 0x9000, 0,
		(ulong)(((void*)hdr) + ROUND(hdr->kernel_size, hdr->page_size)
		        + hdr->page_size),
		hdr->ramdisk_size);
	boot_linux_kernel((ulong)params, load_address, false);
}

static void boot_recovery_image(block_dev_desc_t *dev, struct boot_ctrl *metadata,
		disk_partition_t *misc_part, struct bootloader_message *message)
{
	if (metadata->recovery_tries_remaining > 0) {
		metadata->recovery_tries_remaining--;
		if (load_boot_image(dev, "recovery")) {
			/* Failed to load, set remaining tries to zero */
			metadata->recovery_tries_remaining = 0;
			ab_write_bootloader_message(dev, misc_part, message);
			return;
		}
		ab_write_bootloader_message(dev, misc_part, message);
		boot_image();
		/* Failed to boot, set remaining tries to zero */
		metadata->recovery_tries_remaining = 0;
		ab_write_bootloader_message(dev, misc_part, message);
	}
}

static void brillo_do_recovery(void)
{
	struct bootloader_message message;
	struct boot_ctrl *metadata = (struct boot_ctrl*)&message.slot_suffix;
	struct slot_metadata *slot;
	block_dev_desc_t *dev;
	disk_partition_t misc_part;
	int index;

	if (!(dev = get_dev("mmc", CONFIG_BRILLO_MMC_BOOT_DEVICE)))
		return;

	if (get_partition_info_efi_by_name(dev, "misc", &misc_part))
		return;

	if (ab_read_bootloader_message(dev, &misc_part, &message))
		return;

	/* Slots need to be marked as failed before loading recovery mode */
	for (index = 0; index < ARRAY_SIZE(metadata->slot_info); index++) {
		slot = &metadata->slot_info[index];
		slot->successful_boot = 0;
		slot->tries_remaining = 0;
		slot->priority = 0;
	}

	boot_recovery_image(dev, metadata, &misc_part, &message);
}

static void brillo_do_fastboot(void)
{
	char *fastboot_args[] = {NULL, "0"};
	cmd_tbl_t *fastboot_cmd = find_cmd("fastboot");
	if (fastboot_cmd) {
		printf("FASTBOOT MODE...\n");
		fastboot_cmd->cmd(fastboot_cmd, 0, 2, fastboot_args);
	}
}

static void brillo_do_reset(void)
{
	char *reset_args[] = {NULL};
	cmd_tbl_t *reset_cmd = find_cmd("reset");
	if (reset_cmd)
		reset_cmd->cmd(reset_cmd, 0, 1, reset_args);
	hang();
}

static int brillo_setup_bootargs(void)
{
	char serial_arg[56] = "androidboot.serialno=";
	char *serial;

	serial = getenv("serial#");
	if (serial) {
		strncat(serial_arg, serial, sizeof(serial_arg) - strlen(serial_arg) - 1);
		setenv("bootargs", serial_arg);
	}
}

char* get_active_slot(void) {
	struct bootloader_message message;
	struct boot_ctrl *metadata = (struct boot_ctrl*)&message.slot_suffix;
	block_dev_desc_t *dev;
	disk_partition_t misc_part;
	char *suffixes[] = { BOOTCTRL_SUFFIX_A, BOOTCTRL_SUFFIX_B };

	if (!(dev = get_dev("mmc", CONFIG_BRILLO_MMC_BOOT_DEVICE)))
		return BOOTCTRL_SUFFIX_NA;

	if (get_partition_info_efi_by_name(dev, "misc", &misc_part))
		return BOOTCTRL_SUFFIX_NA;

	if (ab_read_bootloader_message(dev, &misc_part, &message))
		return BOOTCTRL_SUFFIX_NA;

	if(metadata->slot_info[1].priority > metadata->slot_info[0].priority)
		return suffixes[1];

	return suffixes[0];
}

int get_slot_retry_count(char *suffix) {
	struct bootloader_message message;
	struct boot_ctrl *metadata = (struct boot_ctrl*)&message.slot_suffix;
	block_dev_desc_t *dev;
	disk_partition_t misc_part;
	int i = 0;
	int ret;
	char *suffixes[] = { BOOTCTRL_SUFFIX_A, BOOTCTRL_SUFFIX_B };

	if (!(dev = get_dev("mmc", CONFIG_BRILLO_MMC_BOOT_DEVICE)))
		return -ENODEV;

	if (get_partition_info_efi_by_name(dev, "misc", &misc_part))
		return -ENOENT;

	if ((ret = ab_read_bootloader_message(dev, &misc_part, &message)))
		return ret;

	for(i = 0; i < 2; i++){
		if(!strcmp(suffixes[i], suffix)) {
			return metadata->slot_info[i].tries_remaining;
		}
	}
	return -EINVAL;
}

bool is_successful_slot (char *suffix) {
	struct bootloader_message message;
	struct boot_ctrl *metadata = (struct boot_ctrl*)&message.slot_suffix;
	block_dev_desc_t *dev;
	disk_partition_t misc_part;
	int i = 0;

	char *suffixes[] = { BOOTCTRL_SUFFIX_A, BOOTCTRL_SUFFIX_B };

	if (!(dev = get_dev("mmc", CONFIG_BRILLO_MMC_BOOT_DEVICE)))
		return false;

	if (get_partition_info_efi_by_name(dev, "misc", &misc_part))
		return false;

	if (ab_read_bootloader_message(dev, &misc_part, &message))
		return false;

	for(i = 0; i < 2; i++){
		if(!strcmp(suffixes[i], suffix)) {
			if(metadata->slot_info[i].successful_boot == 1) {
				return true;
			}
		}
	}

	return false;
}

bool is_unbootable_slot (char *suffix) {
	struct bootloader_message message;
	struct boot_ctrl *metadata = (struct boot_ctrl*)&message.slot_suffix;
	block_dev_desc_t *dev;
	disk_partition_t misc_part;
	int i = 0;

	char *suffixes[] = { BOOTCTRL_SUFFIX_A, BOOTCTRL_SUFFIX_B };

	if (!(dev = get_dev("mmc", CONFIG_BRILLO_MMC_BOOT_DEVICE)))
		return false;

	if (get_partition_info_efi_by_name(dev, "misc", &misc_part))
		return false;

	if (ab_read_bootloader_message(dev, &misc_part, &message))
		return false;

	for(i = 0; i < 2; i++){
		if(!strcmp(suffixes[i], suffix)) {
			return (metadata->slot_info[i].priority == 0);
		}
	}

	return false;
}

static void __mark_slot_as_failed(struct slot_metadata *slot, char *slot_name)
{
	slot->successful_boot = 0;
	slot->tries_remaining = 0;
	slot->priority = 0;
	printf("WARNING: failed to boot %s slot!\n", slot_name);
}
static int brillo_boot_ab(void)
{
	struct bootloader_message message;
	struct boot_ctrl *metadata = (struct boot_ctrl*)&message.slot_suffix;
	block_dev_desc_t *dev;
	disk_partition_t misc_part;
	int ret, index, slots_by_priority[2] = {0, 1};
	char *suffixes[] = { BOOTCTRL_SUFFIX_A, BOOTCTRL_SUFFIX_B };
	char boot_part[8];
	char *old_bootargs;
	struct mmc *mmc;

	if (!(dev = get_dev("mmc", CONFIG_BRILLO_MMC_BOOT_DEVICE)))
		return -ENODEV;

	if (get_partition_info_efi_by_name(dev, "misc", &misc_part))
		return -ENOENT;

	if ((ret = ab_read_bootloader_message(dev, &misc_part, &message)))
		return ret;

	if (metadata->slot_info[1].priority > metadata->slot_info[0].priority) {
		slots_by_priority[0] = 1;
		slots_by_priority[1] = 0;
	}

	/* Enable power on write protection on the first 8MB of the booting MMC
	 * Use this space for protecting GPT header, u-boot, factory and security
	 * partitions
	 */
	mmc = find_mmc_device(dev->dev);
	if (!mmc)
		printf("WARNING: Cannot access EMMC device, power on write protection is disabled\n");
	else
		if (mmc_usr_power_on_wp(mmc, 0ULL, 8 * 1024 * 1024))
			printf("WARNING: Cannot enable power on write protection\n");

	for (index = 0; index < ARRAY_SIZE(slots_by_priority); index++) {
		int slot_num = slots_by_priority[index];
		struct slot_metadata *slot = &metadata->slot_info[slot_num];

		if (slot->successful_boot == 0 && slot->tries_remaining == 0) {
			/* This is a failed slot, lower priority to zero */
			slot->priority = 0;
		} else {
			if (!slot->priority)
				continue;

			/* either previously successful or tries
			   remaining. attempt to boot. */
			snprintf(boot_part, sizeof(boot_part), "boot%s",
				suffixes[slot_num]);
			if (load_boot_image(dev, boot_part)) {
				/* Failed to load, mark as failed */
				__mark_slot_as_failed(slot, suffixes[slot_num]);
				continue;
			}
			if (slot->tries_remaining > 0)
				slot->tries_remaining--;
			metadata->recovery_tries_remaining = 7;

			old_bootargs = strdup(getenv("bootargs"));
			append_to_bootargs(" " SLOT_SUFFIX_STR);
			append_to_bootargs(suffixes[slot_num]);

			ab_write_bootloader_message(dev, &misc_part, &message);
			boot_image();

			/* Failed to boot, mark as failed */
			setenv("bootargs", old_bootargs);
			__mark_slot_as_failed(slot, suffixes[slot_num]);
			ab_write_bootloader_message(dev, &misc_part, &message);
		}
	}

	/* Failed to boot any partition. Try recovery. */
	boot_recovery_image(dev, metadata, &misc_part, &message);

	return -ENOENT;
}

static int do_boot_brillo(cmd_tbl_t *cmdtp, int flag, int argc,
		char * const argv[])
{
#ifdef CONFIG_BRILLO_FASTBOOT_ONLY
	brillo_do_fastboot();
	brillo_do_reset();
	hang();
#endif

	char *reboot_target = board_get_reboot_target();
	if (!strcmp(reboot_target, "recovery")) {
		brillo_do_recovery();
		goto boot_failed;
	} else if (!strcmp(reboot_target, "fastboot")) {
		brillo_do_fastboot();
		brillo_do_reset();
	}

	brillo_setup_bootargs();

	brillo_boot_ab();

boot_failed:
	/* Return from boot_ab means normal and recovery via disk
	 * failed. Here we do fastboot as 'Diskless Recovery' */
	brillo_do_fastboot();

	brillo_do_reset();

	hang();

	return -ENOENT;
}

U_BOOT_CMD(
	boot_brillo, 1, 0, do_boot_brillo,
	"Boot to Brillo",
	""
);

static int brillo_fb_write_security_partition(struct security_flags sec_flag)
{
	void *tmp;
	lbaint_t blkcnt, i;
	block_dev_desc_t *dev;
	disk_partition_t security_part;

	if (!(dev = get_dev("mmc", CONFIG_FASTBOOT_FLASH_MMC_DEV))) {
		printf("ERROR: cannot access the eMMC\n");
		return -ENODEV;
	}

	if (get_partition_info_efi_by_name(dev, "security", &security_part)) {
		printf("ERROR: cannot find security partition\n");
		return -ENOENT;
	}


	blkcnt = BLOCK_CNT(sizeof(sec_flag), dev);
	tmp = calloc(blkcnt, dev->blksz);

	if (!tmp)
		return -ENOMEM;

	memcpy(tmp, &sec_flag, sizeof(sec_flag));

	for (i = 0; i < blkcnt; i++) {
		if (dev->block_write(dev->dev, security_part.start + i, 1,
				     tmp + i * dev->blksz) != 1) {
			free(tmp);
			return -EIO;
		}
	}

	free(tmp);
	return 0;
}

static int brillo_fb_read_security_partition(struct security_flags *security_data)
{
	void *tmp;
	lbaint_t blkcnt;
	block_dev_desc_t *dev;
	disk_partition_t security_partition;

	if (!(dev = get_dev("mmc", CONFIG_FASTBOOT_FLASH_MMC_DEV))) {
		printf("ERROR: cannot access the eMMC\n");
		return -ENODEV;
	}

	if (get_partition_info_efi_by_name(dev, "security", &security_partition)) {
		printf("ERROR: cannot find security partition\n");
		return -ENOENT;
	}

	blkcnt = BLOCK_CNT(sizeof(struct security_flags), dev);
	tmp = calloc(blkcnt, dev->blksz);

	if (!tmp)
		return -ENOMEM;

	if (dev->block_read(dev->dev, security_partition.start, blkcnt, tmp) != blkcnt) {
		free(tmp);
		return -EIO;
	}

	memcpy(security_data, tmp, sizeof(struct security_flags));
	free(tmp);
	return 0;
}

int fb_write_lock_state(uint8_t lock)
{
	struct security_flags sec;
	int ret;
	ret = brillo_fb_read_security_partition(&sec);
	if (ret)
		return ret;
	sec.lock = lock;

	return brillo_fb_write_security_partition(sec);
}

int fb_write_dev_key(uint8_t* devkey, unsigned int number_of_bytes)
{
	struct security_flags sec;
	int ret;

	if (number_of_bytes > BVB_DEVKEY_MAX) {
		printf("ERROR: key size is bigger than %d\n", BVB_DEVKEY_MAX);
		return -ENOMEM;
	}

	ret = brillo_fb_read_security_partition(&sec);
	if (ret)
		return ret;

	memcpy(sec.devkey, devkey, number_of_bytes);

	return brillo_fb_write_security_partition(sec);
}

int fb_read_dev_key(uint8_t* dev_key)
{
	struct security_flags sec_flag;
	int ret;

	ret = brillo_fb_read_security_partition(&sec_flag);
	if (ret)
		return ret;

	memcpy(dev_key, sec_flag.devkey, sizeof(sec_flag.devkey));
	return 0;
}


int fb_read_lock_state(uint8_t* lock_state)
{
	struct security_flags sec_flag;
	int ret;

	ret = brillo_fb_read_security_partition(&sec_flag);
	if (ret)
		return ret;

	*lock_state = sec_flag.lock;
	return 0;
}
