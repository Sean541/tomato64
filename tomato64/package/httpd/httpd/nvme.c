/*
 *
 * Tomato Firmware
 * NVMe Storage Support
 *
 */


#include "tomato.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <time.h>

#define MAX_DETECTED_DRIVES		4
#define MAX_PARTITIONS_PER_DRIVE	16
#define NVME_MAX_MOUNTS			16
#define NVME_MOUNT_PATH_MAX		64
#define NVME_BUSY_FILE			"/tmp/nvme_busy"
#define NVME_ERR_FILE			"/tmp/nvme_err"

typedef struct {
	char name[32];
	char label[32];
	char partlabel[36];
	unsigned long long blocks;
	char fstype[16];
	char uuid[40];
} httpd_nvme_partition_t;

typedef struct {
	char name[32];
	unsigned long long blocks;
	httpd_nvme_partition_t partitions[MAX_PARTITIONS_PER_DRIVE];
	int partition_count;
} httpd_nvme_drive_t;

static httpd_nvme_drive_t detected_drives[MAX_DETECTED_DRIVES];
static int detected_drives_count = 0;

/* ---- input validation (mirrors the checks in rc/nvme.c) ---- */

static int nvme_skip_num(const char **p)
{
	if (!isdigit((unsigned char)**p))
		return 0;
	while (isdigit((unsigned char)**p))
		(*p)++;
	return 1;
}

static int nvme_valid_disk(const char *s)
{
	if (!s || (strncmp(s, "/dev/nvme", 9) != 0))
		return 0;
	s += 9;
	if (!nvme_skip_num(&s) || (*s++ != 'n') || !nvme_skip_num(&s))
		return 0;
	return (*s == '\0');
}

static int nvme_valid_partition(const char *s)
{
	if (!s || (strncmp(s, "/dev/nvme", 9) != 0))
		return 0;
	s += 9;
	if (!nvme_skip_num(&s) || (*s++ != 'n') || !nvme_skip_num(&s))
		return 0;
	if ((*s++ != 'p') || !nvme_skip_num(&s))
		return 0;
	return (*s == '\0');
}

static int nvme_valid_label(const char *s)
{
	size_t n = 0;

	if (!s)
		return 0;
	for (; *s; ++s, ++n) {
		if (!isalnum((unsigned char)*s) && (*s != '_') && (*s != '.') && (*s != '-'))
			return 0;
	}
	return (n <= 16);
}

static int nvme_valid_uuid(const char *s)
{
	size_t n = 0;

	if (!s)
		return 0;
	for (; *s; ++s, ++n) {
		if (!isalnum((unsigned char)*s) && (*s != '-'))
			return 0;
	}
	return (n <= 36);
}

static int nvme_valid_mount_path(const char *s)
{
	const char *p;

	if (!s)
		return 0;
	if (strcmp(s, "/opt") == 0)
		return 1;
	if ((strncmp(s, "/mnt/", 5) != 0) || (s[5] == '\0') || (strcmp(s, "/mnt/opt") == 0) || (strncmp(s, "/mnt/opt/", 9) == 0))
		return 0;
	if (strlen(s) >= NVME_MOUNT_PATH_MAX)
		return 0;
	for (p = s; *p; ++p) {
		if (!isalnum((unsigned char)*p) && (*p != '_') && (*p != '.') && (*p != '-') && (*p != '/'))
			return 0;
		if ((*p == '/') && (p[1] == '.') && ((p[2] == '/') || (p[2] == '\0')))
			return 0;
	}
	if ((strstr(s, "..") != NULL) || (strstr(s, "//") != NULL) || (p[-1] == '/'))
		return 0;
	return 1;
}

/* Validate an nvme_mounts string (<device>><mount path>><0|1>[><uuid>]< repeated) before it is stored in nvram.
 * Returns 1 if valid (an empty string is valid), 0 otherwise. */
int nvme_mounts_valid(const char *data)
{
	char *buf, *ptr, *rec, *dev, *path, *boot, *uuid, *extra;
	char devs[NVME_MAX_MOUNTS][32], paths[NVME_MAX_MOUNTS][NVME_MOUNT_PATH_MAX];
	int i, count = 0, ok = 1;

	if (!data || (*data == '\0'))
		return 1;

	if ((buf = strdup(data)) == NULL)
		return 0;

	ptr = buf;
	while (ok && ((rec = strsep(&ptr, "<")) != NULL)) {
		if (*rec == '\0')
			continue;

		dev = strsep(&rec, ">");
		path = strsep(&rec, ">");
		boot = strsep(&rec, ">");
		uuid = strsep(&rec, ">");
		extra = rec; /* nothing allowed after the uuid */

		if (!dev || !path || !boot || extra || (count >= NVME_MAX_MOUNTS) ||
		    !nvme_valid_partition(dev) || !nvme_valid_mount_path(path) ||
		    ((strcmp(boot, "0") != 0) && (strcmp(boot, "1") != 0)) ||
		    (uuid && !nvme_valid_uuid(uuid)) ||
		    (strlen(dev) >= sizeof(devs[0]))) {
			ok = 0;
			break;
		}

		for (i = 0; i < count; ++i) {
			if ((strcmp(devs[i], dev) == 0) || (strcmp(paths[i], path) == 0)) {
				ok = 0; /* duplicate device or mount path */
				break;
			}
		}
		if (!ok)
			break;

		strlcpy(devs[count], dev, sizeof(devs[0]));
		strlcpy(paths[count], path, sizeof(paths[0]));
		count++;
	}

	free(buf);
	return ok;
}

/* ---- tools: only offer what is actually installed on this build ---- */

static int probe_tool(const char *name)
{
	static const char *dirs[] = { "/sbin", "/bin", "/usr/sbin", "/usr/bin", NULL };
	char path[64];
	int i;

	for (i = 0; dirs[i]; ++i) {
		snprintf(path, sizeof(path), "%s/%s", dirs[i], name);
		if (access(path, X_OK) == 0)
			return 1;
	}
	return 0;
}

/* Filesystems that can be created on Tomato64: e2fsprogs, btrfs-progs, BusyBox mkdosfs, ntfs-3g ntfsprogs (if NTFS is built) */
static const struct { const char *fs; const char *tool; } nvme_fs_tools[] = {
	{ "ext4", "mkfs.ext4" }, { "ext3", "mkfs.ext3" }, { "btrfs", "mkfs.btrfs" },
	{ "vfat", "mkfs.vfat" }, { "ntfs", "mkfs.ntfs" }, { NULL, NULL }
};

static int nvme_tools_cached = 0;
static unsigned int nvme_fs_mask = 0;
static int nvme_has_sgdisk = 0;

static void init_nvme_tools(void)
{
	int i;

	if (nvme_tools_cached)
		return;
	for (i = 0; nvme_fs_tools[i].fs; ++i) {
		if (probe_tool(nvme_fs_tools[i].tool))
			nvme_fs_mask |= (1U << i);
	}
	nvme_has_sgdisk = probe_tool("sgdisk");
	nvme_tools_cached = 1;
}

static int nvme_fs_available(const char *fs)
{
	int i;

	init_nvme_tools();
	for (i = 0; nvme_fs_tools[i].fs; ++i) {
		if (strcmp(nvme_fs_tools[i].fs, fs) == 0)
			return ((nvme_fs_mask & (1U << i)) != 0);
	}
	return 0;
}

/* ---- hardware discovery ---- */

/* size in 1 KiB blocks (sysfs reports 512 byte sectors); same helper exists in rc/nvme_partition.c */
static unsigned long long get_partition_blocks(const char *part_name)
{
	char sys_path[128];
	char size_buf[32];
	unsigned long long sectors = 0;

	snprintf(sys_path, sizeof(sys_path), "/sys/class/block/%s/size", part_name);
	if (f_read_string(sys_path, size_buf, sizeof(size_buf)) > 0)
		sectors = strtoull(size_buf, NULL, 10);
	else {
		FILE *fp;
		char cmd[128];
		snprintf(cmd, sizeof(cmd), "blockdev --getsz /dev/%s 2>/dev/null", part_name);
		if ((fp = popen(cmd, "r")) != NULL) {
			if (fgets(size_buf, sizeof(size_buf), fp) != NULL)
				sectors = strtoull(size_buf, NULL, 10);
			pclose(fp);
		}
	}

	return sectors / 2;
}

/* Value of KEY="value" in a blkid output line (does not match PARTUUID=, PARTLABEL= ...) */
static void blkid_attr(const char *line, const char *key, char *out, size_t len)
{
	char pat[32];
	const char *p, *end, *s = line;

	out[0] = '\0';
	snprintf(pat, sizeof(pat), "%s=\"", key);
	while ((p = strstr(s, pat)) != NULL) {
		if ((p == line) || (p[-1] == ' '))
			break;
		s = p + 1;
	}
	if (!p)
		return;

	p += strlen(pat);
	if ((end = strchr(p, '"')) != NULL)
		snprintf(out, len, "%.*s", (int)(end - p), p);
}

static void get_sysfs_partlabel(const char *part_name, char *out, size_t len)
{
	char path[128], buf[512], *p;

	out[0] = '\0';
	snprintf(path, sizeof(path), "/sys/class/block/%s/uevent", part_name);
	if (f_read_string(path, buf, sizeof(buf)) <= 0)
		return;

	p = buf;
	while (p && *p) {
		char *next = strchr(p, '\n');
		if (next)
			*next++ = '\0';
		if (strncmp(p, "PARTNAME=", 9) == 0) {
			char *val = p + 9;
			val[strcspn(val, "\r")] = '\0';
			strlcpy(out, val, len);
			break;
		}
		p = next;
	}
}

/* Probe only the discovered NVMe partitions so blkid does not scan unrelated block devices */
static void load_blkid(void)
{
	FILE *fp;
	char cmd[2048], line[512], dev[64], fstype[16];
	int i, j, has_parts = 0;

	strlcpy(cmd, "blkid", sizeof(cmd));
	for (i = 0; i < detected_drives_count; ++i) {
		httpd_nvme_drive_t *drv = &detected_drives[i];
		for (j = 0; j < drv->partition_count; ++j) {
			get_sysfs_partlabel(drv->partitions[j].name, drv->partitions[j].partlabel, sizeof(drv->partitions[j].partlabel));
			strlcat(cmd, " /dev/", sizeof(cmd));
			strlcat(cmd, drv->partitions[j].name, sizeof(cmd));
			has_parts = 1;
		}
	}
	if (!has_parts || ((fp = popen(cmd, "r")) == NULL))
		return;

	while (fgets(line, sizeof(line), fp)) {
		if ((sscanf(line, "%63[^:]:", dev) != 1) || !nvme_valid_partition(dev))
			continue;

		for (i = 0; i < detected_drives_count; ++i) {
			httpd_nvme_drive_t *drv = &detected_drives[i];
			for (j = 0; j < drv->partition_count; ++j) {
				httpd_nvme_partition_t *part = &drv->partitions[j];
				if (strcmp(part->name, dev + 5) == 0) {
					char plabel[36];
					blkid_attr(line, "LABEL", part->label, sizeof(part->label));
					blkid_attr(line, "PARTLABEL", plabel, sizeof(plabel));
					if (plabel[0] != '\0')
						strlcpy(part->partlabel, plabel, sizeof(part->partlabel));
					blkid_attr(line, "TYPE", fstype, sizeof(fstype));
					if (fstype[0] != '\0')
						strlcpy(part->fstype, fstype, sizeof(part->fstype));
					blkid_attr(line, "UUID", part->uuid, sizeof(part->uuid));
					break;
				}
			}
		}
	}
	pclose(fp);
}

/* shorter names first, then alphabetical: nvme0n1p2 < nvme0n1p10 */
static int name_cmp(const char *a, const char *b)
{
	size_t la = strlen(a), lb = strlen(b);

	if (la != lb)
		return (la < lb) ? -1 : 1;
	return strcmp(a, b);
}

/* Enumerate NVMe drives and their partitions through sysfs in a single pass, so that drives without
 * any partition (nothing for blkid to report) are listed too and can be partitioned. */
static void discover_nvme_hardware(void)
{
	DIR *d;
	struct dirent *de;
	char path[64], parent[32];
	char part_names[MAX_DETECTED_DRIVES * MAX_PARTITIONS_PER_DRIVE][32];
	int part_names_count = 0;
	int i, j, k, idx;
	char *p;

	detected_drives_count = 0;

	if ((d = opendir("/sys/class/block")) == NULL)
		return;

	while ((de = readdir(d)) != NULL) {
		if (strncmp(de->d_name, "nvme", 4) != 0)
			continue;

		snprintf(path, sizeof(path), "/dev/%s", de->d_name);
		if (nvme_valid_disk(path)) {
			if (detected_drives_count >= MAX_DETECTED_DRIVES)
				continue;
			httpd_nvme_drive_t *drv = &detected_drives[detected_drives_count++];
			memset(drv, 0, sizeof(*drv));
			strlcpy(drv->name, de->d_name, sizeof(drv->name));
			drv->blocks = get_partition_blocks(de->d_name);
		}
		else if (nvme_valid_partition(path)) {
			if (part_names_count < (int)(sizeof(part_names) / sizeof(part_names[0])))
				strlcpy(part_names[part_names_count++], de->d_name, sizeof(part_names[0]));
		}
	}
	closedir(d);

	/* sort drives and partition names before assigning so truncation (if > MAX_PARTITIONS_PER_DRIVE) is deterministic */
	for (i = 0; i < detected_drives_count; ++i) {
		for (j = i + 1; j < detected_drives_count; ++j) {
			if (name_cmp(detected_drives[j].name, detected_drives[i].name) < 0) {
				httpd_nvme_drive_t tmp = detected_drives[i];
				detected_drives[i] = detected_drives[j];
				detected_drives[j] = tmp;
			}
		}
	}
	for (i = 0; i < part_names_count; ++i) {
		for (j = i + 1; j < part_names_count; ++j) {
			if (name_cmp(part_names[j], part_names[i]) < 0) {
				char tmp[32];
				strlcpy(tmp, part_names[i], sizeof(tmp));
				strlcpy(part_names[i], part_names[j], sizeof(part_names[0]));
				strlcpy(part_names[j], tmp, sizeof(part_names[0]));
			}
		}
	}

	for (k = 0; k < part_names_count; ++k) {
		strlcpy(parent, part_names[k], sizeof(parent));
		if ((p = strrchr(parent, 'p')) == NULL)
			continue;
		*p = '\0';

		for (idx = -1, i = 0; i < detected_drives_count; ++i) {
			if (strcmp(detected_drives[i].name, parent) == 0) {
				idx = i;
				break;
			}
		}
		if ((idx < 0) || (detected_drives[idx].partition_count >= MAX_PARTITIONS_PER_DRIVE))
			continue;

		httpd_nvme_partition_t *part = &detected_drives[idx].partitions[detected_drives[idx].partition_count++];
		memset(part, 0, sizeof(*part));
		strlcpy(part->name, part_names[k], sizeof(part->name));
		part->blocks = get_partition_blocks(part_names[k]);
		strlcpy(part->fstype, "Unknown", sizeof(part->fstype));
	}

	load_blkid();
}

static void web_js_string(const char *s)
{
	char *e = js_string(s);

	web_printf("'%s'", e ? e : "");
	free(e);
}

/* nvme_disks = { 'nvme0n1': { blocks: N, partitions: [ [name, label, blocks, fstype, uuid, partlabel], ... ] }, ... }; */
void asp_nvme_disks(int argc, char **argv)
{
	int i, j;
	(void)argc;
	(void)argv;

	discover_nvme_hardware();

	web_puts("\nnvme_disks = {\n");
	for (i = 0; i < detected_drives_count; i++) {
		httpd_nvme_drive_t *drv = &detected_drives[i];

		if (i > 0)
			web_puts(",\n");
		web_puts("\t");
		web_js_string(drv->name);
		web_printf(": { blocks: %llu, partitions: [", drv->blocks);

		for (j = 0; j < drv->partition_count; j++) {
			httpd_nvme_partition_t *part = &drv->partitions[j];

			if (j > 0)
				web_puts(", ");
			web_puts("[");
			web_js_string(part->name);
			web_puts(", ");
			web_js_string(part->label);
			web_printf(", %llu, ", part->blocks);
			web_js_string(part->fstype);
			web_puts(", ");
			web_js_string(part->uuid);
			web_puts(", ");
			web_js_string(part->partlabel);
			web_puts("]");
		}
		web_puts("] }");
	}
	web_puts("\n};\n");

	/* filesystems that can be created, and whether partitioning is possible (sgdisk) */
	init_nvme_tools();
	web_puts("nvme_fs = [");
	for (i = 0, j = 0; nvme_fs_tools[i].fs; i++) {
		if (!(nvme_fs_mask & (1U << i)))
			continue;
		web_printf("%s'%s'", j++ ? ", " : "", nvme_fs_tools[i].fs);
	}
	web_printf("];\nnvme_can_partition = %d;\n", nvme_has_sgdisk ? 1 : 0);
}

/* Check whether NVME_BUSY_FILE belongs to a live background nvme process; clean up stale locks if it died. */
static int nvme_is_busy(void)
{
	struct stat st;
	char buf[32];
	int pid = 0;

	if (stat(NVME_BUSY_FILE, &st) != 0)
		return 0;

	if (f_read_string(NVME_BUSY_FILE, buf, sizeof(buf)) > 0)
		pid = atoi(buf);

	if (pid > 0) {
		if ((kill(pid, 0) != 0) && (errno == ESRCH)) {
			if (!f_exists(NVME_BUSY_FILE))
				return 0;
			unlink(NVME_BUSY_FILE);
			if (!f_exists(NVME_ERR_FILE))
				f_write_string(NVME_ERR_FILE, "The operation terminated unexpectedly.", 0, 0600);
			return 0;
		}
	}
	else if ((time(NULL) - st.st_mtime) > 5) {
		unlink(NVME_BUSY_FILE);
		return 0;
	}

	return 1;
}

/* nvmecmd.cgi
 *   cmd=status                                        -> report whether a format/partition job is running
 *   cmd=format&dev=/dev/nvme0n1p1&fs=ext4&label=X     -> format a partition (runs in the background)
 *   cmd=partition&dev=/dev/nvme0n1&size=1024&label=X  -> create + format a partition (background)
 *   cmd=delete&dev=/dev/nvme0n1p1                     -> delete a partition (background)
 * Replies with: nvme = { busy: 0|1, ok: 0|1, err: '...' }; */
void wo_nvmecommand(char *url)
{
	const char *cmd = webcgi_safeget("cmd", "");
	const char *dev = webcgi_safeget("dev", "");
	const char *fs = webcgi_safeget("fs", "ext4");
	const char *label = webcgi_safeget("label", "");
	const char *size = webcgi_safeget("size", "");
	const char *err = NULL;
	char err_buf[256] = {0};
	char *argv[6];
	char *e;
	long mb;
	int pid, busy, lock_fd;
	(void)url;

	if (strcmp(cmd, "status") == 0) {
		/* nothing to start */
	}
	else if ((strcmp(cmd, "format") == 0) || (strcmp(cmd, "partition") == 0) || (strcmp(cmd, "delete") == 0)) {
		int is_fmt = (strcmp(cmd, "format") == 0);
		int is_del = (strcmp(cmd, "delete") == 0);

		if (nvme_is_busy())
			err = "Another NVMe operation is already running.";
		else if (!((is_fmt || is_del) ? nvme_valid_partition(dev) : nvme_valid_disk(dev)))
			err = "Invalid device.";
		else if (!is_del && !nvme_valid_label(label))
			err = "Invalid volume label (maximum 16 characters of A-Z a-z 0-9 _ . -).";
		else if (is_fmt) {
			if (!nvme_fs_available(fs))
				err = "Unsupported filesystem, or its mkfs tool is not installed.";
			else if ((strcmp(fs, "vfat") == 0) && (strlen(label) > 11))
				err = "A FAT volume label is limited to 11 characters.";
		}
		else if (init_nvme_tools(), !nvme_has_sgdisk)
			err = "sgdisk is not installed.";
		else if (!is_del) { /* Check sizes and mkfs.ext4 for partition creation only */
			if (!nvme_fs_available("ext4"))
				err = "mkfs.ext4 is not installed.";
			else {
				mb = strtol(size, &e, 10);
				if ((*size == '\0') || (*e != '\0') || (mb <= 0) || (mb > 100000000L))
					err = "Invalid partition size.";
			}
		}

		if (!err) {
			unlink(NVME_ERR_FILE);
			/* atomically mark busy before starting so that a second request cannot slip in */
			lock_fd = open(NVME_BUSY_FILE, O_WRONLY | O_CREAT | O_EXCL, 0600);
			if (lock_fd < 0)
				err = "Another NVMe operation is already running.";
			else {
				close(lock_fd);

				argv[0] = "nvme";
				argv[1] = (char *)cmd;
				argv[2] = (char *)dev;
				if (is_del)
					argv[3] = NULL;
				else {
					argv[3] = (char *)(is_fmt ? fs : size);
					argv[4] = (char *)label;
					argv[5] = NULL;
				}

				if (_eval(argv, NULL, 0, &pid) != 0) {
					unlink(NVME_BUSY_FILE);
					err = "Failed to start the operation.";
				}
			}
		}
	}
	else
		err = "Unknown command.";

	busy = nvme_is_busy();
	if (!busy && !err && f_exists(NVME_ERR_FILE)) {
		if (f_read_string(NVME_ERR_FILE, err_buf, sizeof(err_buf)) > 0) {
			err_buf[strcspn(err_buf, "\r\n")] = '\0';
			if (err_buf[0] != '\0')
				err = err_buf;
		}
		unlink(NVME_ERR_FILE);
	}

	web_printf("\nnvme = { busy: %d, ok: %d, err: ", busy, (err == NULL) ? 1 : 0);
	web_js_string(err ? err : "");
	web_puts(" };\n");
}
