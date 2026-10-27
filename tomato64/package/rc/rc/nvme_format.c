/*
 *
 * Tomato Firmware
 * NVMe Partition Formatting
 *
 */


#include "rc.h"

/* needed by logmsg() */
#define LOGMSG_DISABLE	DISABLE_SYSLOG_OSM
#define LOGMSG_NVDEBUG	"nvme_debug"

static void nvme_update_mount_uuid(const char *dev, const char *old_uuid, const char *new_uuid)
{
	const char *mounts = nvram_safe_get("nvme_mounts");
	char *buf, *ptr, *rec;
	char new_mounts[2048] = {0};
	int changed = 0, should_mount = 0;

	if (!mounts || (*mounts == '\0'))
		return;

	if ((buf = strdup(mounts)) == NULL)
		return;

	ptr = buf;
	while ((rec = strsep(&ptr, "<")) != NULL) {
		if (*rec == '\0')
			continue;

		char rec_copy[256];
		strlcpy(rec_copy, rec, sizeof(rec_copy));

		char *d = strsep(&rec, ">");
		char *mp = strsep(&rec, ">");
		char *boot = strsep(&rec, ">");
		char *u = strsep(&rec, ">");

		int match = nvme_record_matches(d, u, dev, old_uuid);
		if (match && d && mp && boot) {
			char updated[256];
			snprintf(updated, sizeof(updated), "%s>%s>%s>%s<", dev, mp, boot, new_uuid ? new_uuid : "");
			strlcat(new_mounts, updated, sizeof(new_mounts));
			changed = 1;
			if (strcmp(boot, "1") == 0)
				should_mount = 1;
		}
		else {
			strlcat(new_mounts, rec_copy, sizeof(new_mounts));
			strlcat(new_mounts, "<", sizeof(new_mounts));
		}
	}

	if (changed) {
		nvram_set("nvme_mounts", new_mounts);
		nvram_commit();
	}
	free(buf);

	if (should_mount)
		start_nvme();
}

/* Format an NVMe partition.
 * dev: /dev/nvme<N>n<M>p<K>
 * fs: ext4, ext3, btrfs, vfat or ntfs
 * label: optional volume label (may be empty)
 * All parameters come from nvmecmd.cgi and are validated again here. */
int nvme_execute_format(const char *dev, const char *fs, const char *label)
{
	int ret = -1;
	int have_label;
	const char *tool = NULL;
	char old_uuid[40] = {0}, new_uuid[40] = {0};

	if (!nvme_valid_partition(dev)) {
		logmsg(LOG_ERR, "nvme: invalid format target");
		nvme_set_error("Invalid format target.");
		return 1;
	}

	if (!nvme_valid_label(label)) {
		logmsg(LOG_ERR, "nvme: invalid volume label");
		nvme_set_error("Invalid volume label.");
		return 1;
	}
	have_label = (label[0] != '\0');

	if ((strcmp(fs, "vfat") == 0) && (strlen(label) > 11)) {
		logmsg(LOG_ERR, "nvme: a FAT volume label is limited to 11 characters");
		nvme_set_error("A FAT volume label is limited to 11 characters.");
		return 1;
	}

	if (strcmp(fs, "ext4") == 0)
		tool = "mkfs.ext4";
	else if (strcmp(fs, "ext3") == 0)
		tool = "mkfs.ext3";
	else if (strcmp(fs, "btrfs") == 0)
		tool = "mkfs.btrfs";
	else if (strcmp(fs, "vfat") == 0)
		tool = "mkfs.vfat";
	else if (strcmp(fs, "ntfs") == 0)
		tool = "mkfs.ntfs";
	else {
		logmsg(LOG_ERR, "nvme: unsupported filesystem type requested: %s", fs);
		nvme_set_error("Unsupported filesystem type.");
		return 1;
	}

	if (!nvme_have_tool(tool)) {
		logmsg(LOG_ERR, "nvme: %s is not installed", tool);
		nvme_set_error("Required mkfs tool is not installed.");
		return 1;
	}

	logmsg(LOG_NOTICE, "nvme: formatting partition %s with filesystem %s...", dev, fs);

	nvme_get_partition_uuid(dev, old_uuid, sizeof(old_uuid));

	/* unmount first if active */
	if (nvme_unmount_partition(dev) != 0) {
		logmsg(LOG_ERR, "nvme: %s is in use and could not be unmounted before formatting.", dev);
		nvme_set_error("Partition is in use and could not be unmounted.");
		return 1;
	}

	/* Only tools that ship with Tomato64 are used: e2fsprogs (mkfs.ext3/4), btrfs-progs (mkfs.btrfs),
	 * BusyBox mkdosfs (mkfs.vfat) and ntfs-3g's ntfsprogs (mkfs.ntfs, only if NTFS support is built). */
	if ((strcmp(fs, "ext4") == 0) || (strcmp(fs, "ext3") == 0))
		ret = have_label ? eval((char *)tool, "-F", "-L", (char *)label, (char *)dev, NULL) : eval((char *)tool, "-F", (char *)dev, NULL);
	else if (strcmp(fs, "btrfs") == 0)
		ret = have_label ? eval("mkfs.btrfs", "-f", "-L", (char *)label, (char *)dev, NULL) : eval("mkfs.btrfs", "-f", (char *)dev, NULL);
	else if (strcmp(fs, "vfat") == 0)
		ret = have_label ? eval("mkfs.vfat", "-n", (char *)label, (char *)dev, NULL) : eval("mkfs.vfat", (char *)dev, NULL);
	else if (strcmp(fs, "ntfs") == 0)
		ret = have_label ? eval("mkfs.ntfs", "-f", "-L", (char *)label, (char *)dev, NULL) : eval("mkfs.ntfs", "-f", (char *)dev, NULL);

	if (ret == 0) {
		logmsg(LOG_NOTICE, "nvme: successfully formatted %s as %s.", dev, fs);
		if (nvme_have_tool("sgdisk")) {
			char disk[64], type_spec[32], *p_ptr;
			strlcpy(disk, dev, sizeof(disk));
			p_ptr = strrchr(disk, 'p');
			if (p_ptr) {
				int pnum = atoi(p_ptr + 1);
				*p_ptr = '\0';
				const char *tcode = ((strcmp(fs, "vfat") == 0) || (strcmp(fs, "ntfs") == 0)) ? "0700" : "8300";
				snprintf(type_spec, sizeof(type_spec), "%d:%s", pnum, tcode);
				eval("sgdisk", "-t", type_spec, disk, NULL);
			}
		}
		nvme_get_partition_uuid(dev, new_uuid, sizeof(new_uuid));
		nvme_update_mount_uuid(dev, old_uuid, new_uuid);
	}
	else {
		logmsg(LOG_ERR, "nvme: failed to format %s (exit code %d).", dev, ret);
		nvme_set_error("Failed to format partition.");
	}

	return ret;
}
