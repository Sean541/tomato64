/*
 *
 * Tomato Firmware
 * NVMe Partition Management
 *
 */


#include "rc.h"

/* needed by logmsg() */
#define LOGMSG_DISABLE		DISABLE_SYSLOG_OSM
#define LOGMSG_NVDEBUG		"nvme_debug"

#define NVME_MAX_PARTS			128
#define MAX_PARTITIONS_PER_DRIVE	16
#define NVME_ALIGN_SECTORS		2048 /* 1 MiB */

typedef struct {
	unsigned long long start;
	unsigned long long end;
} nvme_span_t;

static unsigned long long nvme_sysfs_ull(const char *name, const char *attr)
{
	char path[160], buf[32];

	snprintf(path, sizeof(path), "/sys/class/block/%s/%s", name, attr);
	if (f_read_string(path, buf, sizeof(buf)) > 0)
		return strtoull(buf, NULL, 10);

	return 0;
}

/* Scan the partitions of "disk" (e.g. nvme0n1). Marks existing partition numbers in used[],
 * returns the total sectors already allocated to existing partitions, and (if max_contig_out
 * is non-NULL) computes the largest 1 MiB-aligned contiguous free sector range on the disk. */
static unsigned long long nvme_scan_partitions(const char *disk, unsigned char *used,
                                               unsigned long long disk_sectors,
                                               unsigned long long *max_contig_out)
{
	DIR *d;
	struct dirent *de;
	size_t len = strlen(disk);
	unsigned long long allocated = 0;
	nvme_span_t spans[NVME_MAX_PARTS];
	int span_count = 0, i, j;

	if (max_contig_out)
		*max_contig_out = 0;

	if ((d = opendir("/sys/class/block")) == NULL)
		return 0;

	while ((de = readdir(d)) != NULL) {
		if ((strncmp(de->d_name, disk, len) != 0) || (de->d_name[len] != 'p') || !isdigit((unsigned char)de->d_name[len + 1]))
			continue;

		int num = atoi(de->d_name + len + 1);
		if ((num > 0) && (num < NVME_MAX_PARTS))
			used[num] = 1;

		unsigned long long start = nvme_sysfs_ull(de->d_name, "start");
		unsigned long long size = nvme_sysfs_ull(de->d_name, "size");
		allocated += size;

		if ((size > 0) && (span_count < NVME_MAX_PARTS)) {
			spans[span_count].start = start;
			spans[span_count].end = start + size;
			span_count++;
		}
	}
	closedir(d);

	if (max_contig_out && (disk_sectors > NVME_ALIGN_SECTORS + 33ULL)) {
		unsigned long long usable_end = disk_sectors - 33ULL;
		unsigned long long cur = NVME_ALIGN_SECTORS;
		unsigned long long max_contig = 0;
		unsigned long long aligned_cur;

		for (i = 0; i < span_count; ++i) {
			for (j = i + 1; j < span_count; ++j) {
				if (spans[j].start < spans[i].start) {
					nvme_span_t tmp = spans[i];
					spans[i] = spans[j];
					spans[j] = tmp;
				}
			}
		}

		for (i = 0; i < span_count; ++i) {
			aligned_cur = ((cur + NVME_ALIGN_SECTORS - 1) / NVME_ALIGN_SECTORS) * NVME_ALIGN_SECTORS;
			unsigned long long gap_end = (spans[i].start < usable_end) ? spans[i].start : usable_end;
			if ((gap_end > aligned_cur) && ((gap_end - aligned_cur) > max_contig))
				max_contig = gap_end - aligned_cur;
			if (spans[i].end > cur)
				cur = spans[i].end;
		}

		aligned_cur = ((cur + NVME_ALIGN_SECTORS - 1) / NVME_ALIGN_SECTORS) * NVME_ALIGN_SECTORS;
		if ((usable_end > aligned_cur) && ((usable_end - aligned_cur) > max_contig))
			max_contig = usable_end - aligned_cur;

		*max_contig_out = max_contig;
	}

	return allocated;
}

/* A disk must not be touched if it carries a filesystem directly (no partitions) or an MBR-only partition table. */
static int nvme_disk_is_unsafe(const char *disk_path, const unsigned char *used)
{
	FILE *fp;
	char cmd[128], line[256];
	int i, has_parts = 0, unsafe = 0;

	for (i = 1; i < NVME_MAX_PARTS; ++i) {
		if (used[i]) {
			has_parts = 1;
			break;
		}
	}

	if (!has_parts) {
		/* BusyBox/util-linux blkid prints a line with TYPE= only if the whole disk holds a filesystem */
		snprintf(cmd, sizeof(cmd), "blkid %s", disk_path);
		if ((fp = popen(cmd, "r")) != NULL) {
			while (fgets(line, sizeof(line), fp)) {
				if (strstr(line, " TYPE=\"") != NULL)
					unsafe = 1;
			}
			pclose(fp);
		}
		return unsafe;
	}

	/* partitions exist: only continue for GPT (sgdisk -p reports "MBR: MBR only" for a plain DOS table) */
	snprintf(cmd, sizeof(cmd), "sgdisk -p %s", disk_path);
	if ((fp = popen(cmd, "r")) != NULL) {
		while (fgets(line, sizeof(line), fp)) {
			if (strstr(line, "MBR only") != NULL)
				unsafe = 1;
		}
		pclose(fp);
	}
	return unsafe;
}

/* Create a GPT partition of size_mb MiB in the free space and format it as ext4.
 * Uses sgdisk (gptfdisk) and BusyBox partprobe, both part of Tomato64; parted is not available.
 * disk_arg: /dev/nvme<N>n<M>, size_str: size in MiB, label: optional volume label (may be empty).
 * All parameters come from nvmecmd.cgi and are validated again here. */
int nvme_execute_partition(const char *disk_arg, const char *size_str, const char *label)
{
	char range_s[80], cname_s[32], new_part[64], err_msg[160];
	unsigned char before[NVME_MAX_PARTS], after[NVME_MAX_PARTS];
	unsigned long long allocated_sectors, req_sectors, max_contig_sectors = 0, disk_sectors;
	const char *disk;
	long size_mb;
	char *endp;
	int ret, i, retries, part_count = 0;

	if (!nvme_valid_disk(disk_arg)) {
		logmsg(LOG_ERR, "nvme: invalid partition target");
		nvme_set_error("Invalid partition target.");
		return 1;
	}
	if (!nvme_valid_label(label)) {
		logmsg(LOG_ERR, "nvme: invalid volume label");
		nvme_set_error("Invalid volume label.");
		return 1;
	}

	size_mb = strtol(size_str, &endp, 10);
	if ((*size_str == '\0') || (*endp != '\0') || (size_mb <= 0)) {
		logmsg(LOG_ERR, "nvme: invalid partition size (%s)", size_str);
		nvme_set_error("Invalid partition size.");
		return 1;
	}

	disk = disk_arg + 5; /* skip "/dev/" */

	if (access(disk_arg, F_OK) != 0) {
		logmsg(LOG_ERR, "nvme: device node %s does not exist.", disk_arg);
		nvme_set_error("Target drive does not exist.");
		return 1;
	}

	if (!nvme_have_tool("sgdisk")) {
		logmsg(LOG_ERR, "nvme: sgdisk is not installed.");
		nvme_set_error("sgdisk is not installed.");
		return 1;
	}
	if (!nvme_have_tool("mkfs.ext4")) {
		logmsg(LOG_ERR, "nvme: mkfs.ext4 is not installed.");
		nvme_set_error("mkfs.ext4 is not installed.");
		return 1;
	}

	disk_sectors = nvme_sysfs_ull(disk, "size");
	memset(before, 0, sizeof(before));
	allocated_sectors = nvme_scan_partitions(disk, before, disk_sectors, &max_contig_sectors);

	if (nvme_disk_is_unsafe(disk_arg, before)) {
		logmsg(LOG_ERR, "nvme: %s holds a filesystem or a non-GPT partition table; refusing to modify it.", disk_arg);
		nvme_set_error("Drive holds a raw filesystem or a non-GPT partition table; refusing to modify it.");
		return 1;
	}
	for (i = 1; i < NVME_MAX_PARTS; ++i) {
		if (before[i])
			part_count++;
	}
	if (part_count >= MAX_PARTITIONS_PER_DRIVE) {
		logmsg(LOG_ERR, "nvme: maximum number of partitions (%d) reached on %s.", MAX_PARTITIONS_PER_DRIVE, disk_arg);
		nvme_set_error("Maximum number of partitions (16) reached on this drive.");
		return 1;
	}
	req_sectors = (unsigned long long)size_mb * 2048ULL;

	if ((max_contig_sectors == 0) || (disk_sectors <= allocated_sectors) || (req_sectors > disk_sectors - allocated_sectors)) {
		logmsg(LOG_ERR, "nvme: not enough free space on %s for %ld MB.", disk_arg, size_mb);
		nvme_set_error("Not enough free space on the drive.");
		return 1;
	}

	/* Check if free space is fragmented such that the largest contiguous block cannot hold the requested size
	 * (allowing up to 1 MiB + 33 sectors for GPT alignment/backup overhead when filling the remaining space). */
	if (req_sectors > max_contig_sectors + NVME_ALIGN_SECTORS + 33ULL) {
		logmsg(LOG_ERR, "nvme: not enough contiguous free space on %s for %ld MB (largest block: %llu MB).",
		       disk_arg, size_mb, max_contig_sectors / 2048ULL);
		snprintf(err_msg, sizeof(err_msg),
		         "Not enough contiguous free space for %ld MB (largest free block is %llu MB).",
		         size_mb, max_contig_sectors / 2048ULL);
		nvme_set_error(err_msg);
		return 1;
	}

	logmsg(LOG_NOTICE, "nvme: partitioning drive %s (new partition size: %ld MB, label: %s)...", disk_arg, size_mb, label);

	/* sgdisk creates the GPT automatically on a blank disk; "0" selects the first free partition number.
	 * If the requested MB equals the full contiguous capacity (within GPT header/backup overhead), use 0:0:0 to fill it. */
	if (req_sectors > max_contig_sectors)
		strlcpy(range_s, "0:0:0", sizeof(range_s));
	else
		snprintf(range_s, sizeof(range_s), "0:0:+%ldM", size_mb);

	if (label[0] != '\0')
		snprintf(cname_s, sizeof(cname_s), "0:%s", label);

	/* If the drive currently has no partitions, instruct sgdisk to initialize
	 * a fresh GPT layout (-o) so it does not fail with exit code 3 on blank/non-GPT disks. */
	if (part_count == 0) {
		if (label[0] != '\0')
			ret = eval("sgdisk", "-o", "-n", range_s, "-t", "0:8300", "-c", cname_s, (char *)disk_arg);
		else
			ret = eval("sgdisk", "-o", "-n", range_s, "-t", "0:8300", (char *)disk_arg);
	}
	else {
		if (label[0] != '\0')
			ret = eval("sgdisk", "-n", range_s, "-t", "0:8300", "-c", cname_s, (char *)disk_arg);
		else
			ret = eval("sgdisk", "-n", range_s, "-t", "0:8300", (char *)disk_arg);
	}

	if (ret != 0) {
		logmsg(LOG_ERR, "nvme: failed to create partition on %s (exit code %d).", disk_arg, ret);
		nvme_set_error("Failed to create partition with sgdisk.");
		return ret;
	}

	eval("partprobe", (char *)disk_arg, NULL); /* BusyBox applet; sgdisk already asks the kernel to re-read */
	new_part[0] = '\0';
	for (retries = 0; retries < 50; ++retries) {
		memset(after, 0, sizeof(after));
		nvme_scan_partitions(disk, after, 0, NULL);

		for (i = 1; i < NVME_MAX_PARTS; ++i) {
			if (after[i] && !before[i]) {
				snprintf(new_part, sizeof(new_part), "/dev/%sp%d", disk, i);
				break;
			}
		}

		/* Break early if the new partition registered and its device node exists */
		if ((new_part[0] != '\0') && f_exists(new_part))
			break;

		new_part[0] = '\0';
		usleep(100000);
	}

	if (new_part[0] == '\0') {
		logmsg(LOG_ERR, "nvme: could not determine newly created partition path for formatting.");
		nvme_set_error("Could not find newly created partition device node.");
		return 1;
	}

	logmsg(LOG_NOTICE, "nvme: formatting new partition %s as ext4...", new_part);
	return nvme_execute_format(new_part, "ext4", label);
}

int nvme_execute_delete(const char *dev)
{
	char disk[64];
	char *p_ptr;
	int part_num;
	int ret, retries;
	char part_str[16];
	char part_uuid[40] = {0};
	unsigned char used[NVME_MAX_PARTS];
	char *mounts;

	if (!nvme_valid_partition(dev)) {
		logmsg(LOG_ERR, "nvme: invalid delete target");
		nvme_set_error("Invalid delete target.");
		return 1;
	}

	/* Extract the parent disk path and the partition number */
	strlcpy(disk, dev, sizeof(disk));
	p_ptr = strrchr(disk, 'p');
	if (!p_ptr) {
		nvme_set_error("Invalid delete target.");
		return 1;
	}

	*p_ptr = '\0'; /* Truncate to leave /dev/nvme0n1 */
	part_num = atoi(p_ptr + 1);

	if ((part_num <= 0) || (part_num >= NVME_MAX_PARTS)) {
		logmsg(LOG_ERR, "nvme: invalid partition number %d", part_num);
		nvme_set_error("Invalid partition number.");
		return 1;
	}

	if (!nvme_have_tool("sgdisk")) {
		logmsg(LOG_ERR, "nvme: sgdisk is not installed.");
		nvme_set_error("sgdisk is not installed.");
		return 1;
	}

	memset(used, 0, sizeof(used));
	used[part_num] = 1;
	if (nvme_disk_is_unsafe(disk, used)) {
		logmsg(LOG_ERR, "nvme: %s holds a non-GPT partition table; refusing to modify it.", disk);
		nvme_set_error("Drive holds a non-GPT partition table; refusing to modify it.");
		return 1;
	}

	logmsg(LOG_NOTICE, "nvme: permanently deleting partition %d on %s...", part_num, disk);

	nvme_get_partition_uuid(dev, part_uuid, sizeof(part_uuid));

	/* Unmount the partition first; abort if it is busy so we do not delete an active mount */
	if (nvme_unmount_partition(dev) != 0) {
		logmsg(LOG_ERR, "nvme: %s is in use and could not be unmounted before deletion.", dev);
		nvme_set_error("Partition is in use and could not be unmounted.");
		return 1;
	}

	snprintf(part_str, sizeof(part_str), "%d", part_num);

	/* Instruct sgdisk to delete the specific partition number */
	ret = eval("sgdisk", "-d", part_str, disk, NULL);
	if (ret != 0) {
		logmsg(LOG_ERR, "nvme: failed to delete partition %s (exit code %d).", dev, ret);
		nvme_set_error("Failed to delete partition with sgdisk.");
		return ret;
	}

	/* Remove any saved mount entry matching this partition device or UUID now that deletion succeeded */
	mounts = nvram_safe_get("nvme_mounts");
	if (mounts && (*mounts != '\0')) {
		char *buf = strdup(mounts);
		if (buf) {
			char *ptr = buf, *rec;
			char new_mounts[2048] = {0};
			int changed = 0;
			while ((rec = strsep(&ptr, "<")) != NULL) {
				if (*rec == '\0')
					continue;
				char rec_copy[256];
				strlcpy(rec_copy, rec, sizeof(rec_copy));
				char *d = strsep(&rec, ">");
				(void)strsep(&rec, ">");
				(void)strsep(&rec, ">");
				char *u = strsep(&rec, ">");
				int match = nvme_record_matches(d, u, dev, part_uuid);
				if (!match) {
					strlcat(new_mounts, rec_copy, sizeof(new_mounts));
					strlcat(new_mounts, "<", sizeof(new_mounts));
				}
				else
					changed = 1;
			}
			if (changed) {
				nvram_set("nvme_mounts", new_mounts);
				nvram_commit();
			}
			free(buf);
		}
	}

	/* Tell the kernel to re-read the partition table */
	eval("partprobe", disk, NULL);

	/* Poll for up to 5 seconds to ensure the block device node is removed by mdev */
	for (retries = 0; retries < 50; ++retries) {
		if (!f_exists(dev))
			break;
		usleep(100000);
	}

	logmsg(LOG_NOTICE, "nvme: successfully deleted partition %s.", dev);
	return 0;
}
