/*
 *
 * Tomato Firmware
 * NVMe Storage Support
 *
 */


#include "rc.h"

#include <sys/mount.h>
#include <sys/statvfs.h>
#include <mntent.h>

#ifndef MNT_DETACH
#define MNT_DETACH		0x00000002
#endif

/* needed by logmsg() */
#define LOGMSG_DISABLE		DISABLE_SYSLOG_OSM
#define LOGMSG_NVDEBUG		"nvme_debug"

#define NVME_MAX_MOUNTS		16
#define MAX_BLKID_CACHE		64
#define NVME_BUSY_FILE		"/tmp/nvme_busy"
#define NVME_ERR_FILE		"/tmp/nvme_err"
#define NVME_PREV_MOUNTS_FILE	"/tmp/nvme_prev_mounts"
#define NVME_DEVICE_WAIT_SECS	5 /* total time to wait for missing device nodes at boot */

typedef struct {
	char device[64];
	char mount_path[128];
	char uuid[40];
	int mount_at_boot;
} rc_nvme_mount_t;

typedef struct {
	char dev[64];
	char fstype[16];
	char uuid[40];
} rc_blkid_entry_t;

static rc_nvme_mount_t rc_nvme_mounts[NVME_MAX_MOUNTS];
static int rc_nvme_mounts_count = 0;
static rc_blkid_entry_t blkid_cache[MAX_BLKID_CACHE];
static int blkid_cache_count = 0;
static int blkid_cache_loaded = 0;

/* Find the value of KEY="value" in a blkid output line (ignores PARTUUID=, PARTLABEL= ...) */
static int nvme_blkid_attr(const char *line, const char *key, char *out, size_t len)
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
		return 0;

	p += strlen(pat);
	if ((end = strchr(p, '"')) == NULL)
		return 0;

	snprintf(out, len, "%.*s", (int)(end - p), p);
	return 1;
}

static void load_blkid_cache(void)
{
	DIR *d;
	struct dirent *de;
	FILE *pipe_fp;
	char cmd[2048], path[64], line[512], dev[64];
	int has_parts = 0;

	if (blkid_cache_loaded)
		return;
	blkid_cache_loaded = 1;
	blkid_cache_count = 0;

	if ((d = opendir("/sys/class/block")) == NULL)
		return;

	strlcpy(cmd, "blkid", sizeof(cmd));
	while ((de = readdir(d)) != NULL) {
		if (strncmp(de->d_name, "nvme", 4) != 0)
			continue;
		snprintf(path, sizeof(path), "/dev/%s", de->d_name);
		if (nvme_valid_partition(path)) {
			strlcat(cmd, " ", sizeof(cmd));
			strlcat(cmd, path, sizeof(cmd));
			has_parts = 1;
		}
	}
	closedir(d);

	if (!has_parts || ((pipe_fp = popen(cmd, "r")) == NULL))
		return;

	while ((fgets(line, sizeof(line), pipe_fp) != NULL) && (blkid_cache_count < MAX_BLKID_CACHE)) {
		if ((sscanf(line, "%63[^:]:", dev) != 1) || !nvme_valid_partition(dev))
			continue;

		rc_blkid_entry_t *e = &blkid_cache[blkid_cache_count++];
		strlcpy(e->dev, dev, sizeof(e->dev));
		nvme_blkid_attr(line, "TYPE", e->fstype, sizeof(e->fstype));
		nvme_blkid_attr(line, "UUID", e->uuid, sizeof(e->uuid));
	}
	pclose(pipe_fp);
}

/* dev_node must be a validated NVMe partition path */
static void get_partition_metadata(const char *dev_node, char *out_type, size_t type_len)
{
	FILE *pipe_fp;
	char cmd[128];
	char res_buf[512];
	int i;

	strlcpy(out_type, "auto", type_len);

	load_blkid_cache();
	for (i = 0; i < blkid_cache_count; ++i) {
		if (strcmp(blkid_cache[i].dev, dev_node) == 0) {
			if (blkid_cache[i].fstype[0] != '\0')
				strlcpy(out_type, blkid_cache[i].fstype, type_len);
			return;
		}
	}

	snprintf(cmd, sizeof(cmd), "blkid %s", dev_node);
	if ((pipe_fp = popen(cmd, "r")) == NULL)
		return;

	if (fgets(res_buf, sizeof(res_buf), pipe_fp) != NULL) {
		char type[16];
		if (nvme_blkid_attr(res_buf, "TYPE", type, sizeof(type)) && (type[0] != '\0'))
			strlcpy(out_type, type, type_len);
	}
	pclose(pipe_fp);
}

/* The partition numbering of /dev/nvme* can change; find the partition that currently has this UUID. */
static int nvme_find_by_uuid(const char *uuid, char *out, size_t len)
{
	int i;

	load_blkid_cache();
	for (i = 0; i < blkid_cache_count; ++i) {
		if ((blkid_cache[i].uuid[0] != '\0') && (strcmp(blkid_cache[i].uuid, uuid) == 0)) {
			strlcpy(out, blkid_cache[i].dev, len);
			return 1;
		}
	}
	return 0;
}

static int nvme_uuid_mismatch(const char *dev_node, const char *expected_uuid)
{
	int i;

	load_blkid_cache();
	for (i = 0; i < blkid_cache_count; ++i) {
		if (strcmp(blkid_cache[i].dev, dev_node) == 0)
			return ((blkid_cache[i].uuid[0] != '\0') && (strcmp(blkid_cache[i].uuid, expected_uuid) != 0));
	}
	return 0;
}

/* skip a run of digits; returns 0 if there is none */
static int nvme_skip_num(const char **p)
{
	if (!isdigit((unsigned char)**p))
		return 0;
	while (isdigit((unsigned char)**p))
		(*p)++;
	return 1;
}

/* "/dev/nvme<N>n<M>" */
int nvme_valid_disk(const char *s)
{
	if (!s || (strncmp(s, "/dev/nvme", 9) != 0))
		return 0;
	s += 9;
	if (!nvme_skip_num(&s) || (*s++ != 'n') || !nvme_skip_num(&s))
		return 0;
	return (*s == '\0');
}

/* "/dev/nvme<N>n<M>p<K>" */
int nvme_valid_partition(const char *s)
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

/* optional volume label: up to 16 chars of [A-Za-z0-9_.-] */
int nvme_valid_label(const char *s)
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

/* optional filesystem UUID: up to 36 chars of [A-Za-z0-9-] (an empty string is valid) */
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

/* "/opt" or "/mnt/<suffix>": no '..', no trailing '/', limited character set */
static int nvme_valid_mount_path(const char *s)
{
	const char *p;

	if (!s)
		return 0;
	if (strcmp(s, "/opt") == 0)
		return 1;
	if ((strncmp(s, "/mnt/", 5) != 0) || (s[5] == '\0') || (strcmp(s, "/mnt/opt") == 0) || (strncmp(s, "/mnt/opt/", 9) == 0))
		return 0;
	if (strlen(s) >= sizeof(((rc_nvme_mount_t *)0)->mount_path))
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

int nvme_have_tool(const char *name)
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

static int root_is_readonly(void)
{
	struct statvfs sfs;

	if (statvfs("/", &sfs) == 0)
		return ((sfs.f_flag & ST_RDONLY) != 0);
	return (access("/", W_OK) != 0);
}

static int is_path_mounted(const char *dir)
{
	FILE *fp;
	struct mntent *mnt;
	int mounted = 0;

	if ((fp = setmntent("/proc/mounts", "r")) == NULL)
		return 0;
	while ((mnt = getmntent(fp)) != NULL) {
		if (strcmp(mnt->mnt_dir, dir) == 0) {
			mounted = 1;
			break;
		}
	}
	endmntent(fp);
	return mounted;
}

static int is_dev_mounted_at(const char *dev, const char *dir)
{
	FILE *fp;
	struct mntent *mnt;
	int mounted = 0;

	if ((fp = setmntent("/proc/mounts", "r")) == NULL)
		return 0;
	while ((mnt = getmntent(fp)) != NULL) {
		if ((strcmp(mnt->mnt_fsname, dev) == 0) && (strcmp(mnt->mnt_dir, dir) == 0)) {
			mounted = 1;
			break;
		}
	}
	endmntent(fp);
	return mounted;
}

/* Unmount any active mountpoints of dev other than keep1 / keep2 (in reverse /proc/mounts order so bind mounts unmount first).
 * Returns 0 on success, -1 if an old mountpoint was busy and could not be unmounted. */
static int nvme_unmount_dev_except(const char *dev, const char *keep1, const char *keep2)
{
	FILE *fp;
	struct mntent *mnt;
	char dirs[NVME_MAX_MOUNTS * 2][128];
	int dcount = 0, i, rc = 0;

	if ((fp = setmntent("/proc/mounts", "r")) == NULL)
		return 0;
	while (((mnt = getmntent(fp)) != NULL) && (dcount < (int)(sizeof(dirs) / sizeof(dirs[0])))) {
		if (strcmp(mnt->mnt_fsname, dev) != 0)
			continue;
		if ((keep1 && (strcmp(mnt->mnt_dir, keep1) == 0)) ||
		    (keep2 && (strcmp(mnt->mnt_dir, keep2) == 0)))
			continue;
		strlcpy(dirs[dcount++], mnt->mnt_dir, sizeof(dirs[0]));
	}
	endmntent(fp);

	for (i = dcount - 1; i >= 0; --i) {
		if ((umount(dirs[i]) != 0) && (umount2(dirs[i], MNT_FORCE) != 0) && (umount2(dirs[i], MNT_DETACH) != 0)) {
			logmsg(LOG_ERR, "nvme: %s is busy on %s, cannot unmount", dev, dirs[i]);
			rc = -1;
			break;
		}
		if (strncmp(dirs[i], "/mnt/", 5) == 0)
			rmdir(dirs[i]);
	}
	return rc;
}

int nvme_unmount_partition(const char *dev)
{
	return nvme_unmount_dev_except(dev, NULL, NULL);
}

/* If dir is currently mounted by an NVMe partition other than keep_dev (e.g. a partition whose mount
 * path was cleared or reassigned), unmount that NVMe partition so the new partition can use dir. */
static void nvme_unmount_nvme_at_path(const char *dir, const char *keep_dev)
{
	FILE *fp;
	struct mntent *mnt;
	char other_dev[64] = {0};

	if ((fp = setmntent("/proc/mounts", "r")) == NULL)
		return;
	while ((mnt = getmntent(fp)) != NULL) {
		if ((strcmp(mnt->mnt_dir, dir) == 0) &&
		    nvme_valid_partition(mnt->mnt_fsname) &&
		    (!keep_dev || (strcmp(mnt->mnt_fsname, keep_dev) != 0))) {
			strlcpy(other_dev, mnt->mnt_fsname, sizeof(other_dev));
			break;
		}
	}
	endmntent(fp);

	if (other_dev[0] != '\0')
		nvme_unmount_partition(other_dev);
}

/* Match a saved nvme_mounts record (rec_dev, rec_uuid) against a partition (dev, uuid).
 * Prefer UUID comparison whenever both UUIDs are present so device-node renumbering does not
 * overwrite or delete a different partition's mount configuration. */
int nvme_record_matches(const char *rec_dev, const char *rec_uuid, const char *dev, const char *uuid)
{
	if (uuid && (uuid[0] != '\0') && rec_uuid && (rec_uuid[0] != '\0'))
		return (strcmp(rec_uuid, uuid) == 0);

	if (rec_uuid && (rec_uuid[0] != '\0')) {
		char resolved[64];
		blkid_cache_loaded = 0;
		if (nvme_find_by_uuid(rec_uuid, resolved, sizeof(resolved)))
			return 0;
	}

	return (rec_dev && dev && (strcmp(rec_dev, dev) == 0));
}

void nvme_get_partition_uuid(const char *dev_node, char *out_uuid, size_t uuid_len)
{
	FILE *pipe_fp;
	char cmd[128], line[512];

	out_uuid[0] = '\0';
	if (!nvme_valid_partition(dev_node))
		return;

	snprintf(cmd, sizeof(cmd), "blkid %s", dev_node);
	if ((pipe_fp = popen(cmd, "r")) == NULL)
		return;

	if (fgets(line, sizeof(line), pipe_fp) != NULL)
		nvme_blkid_attr(line, "UUID", out_uuid, uuid_len);

	pclose(pipe_fp);
}

void nvme_set_error(const char *msg)
{
	if (!msg || (*msg == '\0')) {
		unlink(NVME_ERR_FILE);
		return;
	}
	f_write_string(NVME_ERR_FILE, msg, 0, 0600);
}

/* record format: <device>><mount path>><mount at boot>[><uuid>]< (repeated); invalid records are skipped */
static int parse_nvme_mounts_string(const char *data, rc_nvme_mount_t *out_mounts, int max_mounts)
{
	char *buf, *ptr, *rec, *dev, *path, *boot, *uuid;
	int count = 0, i, dup;

	if (!data || (*data == '\0') || !out_mounts || (max_mounts <= 0))
		return 0;

	/* never modify the nvram string in place */
	if ((buf = strdup(data)) == NULL)
		return 0;

	ptr = buf;
	while ((count < max_mounts) && ((rec = strsep(&ptr, "<")) != NULL)) {
		if (*rec == '\0')
			continue;

		dev = strsep(&rec, ">");
		path = strsep(&rec, ">");
		boot = strsep(&rec, ">");
		uuid = strsep(&rec, ">");
		if (!uuid)
			uuid = "";

		if (!dev || !path || !boot || !nvme_valid_partition(dev) || !nvme_valid_mount_path(path) || !nvme_valid_uuid(uuid)) {
			logmsg(LOG_WARNING, "nvme: ignoring invalid mount entry");
			continue;
		}

		for (dup = 0, i = 0; i < count; ++i) {
			if ((strcmp(out_mounts[i].device, dev) == 0) || (strcmp(out_mounts[i].mount_path, path) == 0)) {
				dup = 1;
				break;
			}
		}
		if (dup) {
			logmsg(LOG_WARNING, "nvme: ignoring duplicate mount entry (%s -> %s)", dev, path);
			continue;
		}

		strlcpy(out_mounts[count].device, dev, sizeof(out_mounts[0].device));
		strlcpy(out_mounts[count].mount_path, path, sizeof(out_mounts[0].mount_path));
		strlcpy(out_mounts[count].uuid, uuid, sizeof(out_mounts[0].uuid));
		out_mounts[count].mount_at_boot = (*boot == '1');
		count++;
	}

	free(buf);
	return count;
}

static void parse_rc_nvme_mounts(const char *data)
{
	rc_nvme_mounts_count = parse_nvme_mounts_string(data, rc_nvme_mounts, NVME_MAX_MOUNTS);
}

static void nvme_set_busy(int on)
{
	if (on) {
		char pid_s[16];
		snprintf(pid_s, sizeof(pid_s), "%d\n", (int)getpid());
		f_write_string(NVME_BUSY_FILE, pid_s, 0, 0600);
	}
	else
		unlink(NVME_BUSY_FILE);
}

int nvme_main(int argc, char *argv[])
{
	int rc;
	int i, waited = 0, is_stop;
	char resolved_devs[NVME_MAX_MOUNTS][64];
	int entry_valid[NVME_MAX_MOUNTS];

	/* nvme format <partition> <fs> [label] / nvme partition <disk> <size_mb> [label] (started by nvmecmd.cgi) */
	if ((argc >= 4) && (strcmp(argv[1], "format") == 0)) {
		unlink(NVME_ERR_FILE);
		nvme_set_busy(1);
		rc = nvme_execute_format(argv[2], argv[3], (argc >= 5) ? argv[4] : "");
		if ((rc != 0) && !f_exists(NVME_ERR_FILE))
			nvme_set_error("Failed to format partition.");
		nvme_set_busy(0);
		return rc;
	}
	if ((argc >= 4) && (strcmp(argv[1], "partition") == 0)) {
		const char *fs = (argc >= 5) ? argv[4] : "ext4";
		const char *label = (argc >= 6) ? argv[5] : "";
		unlink(NVME_ERR_FILE);
		nvme_set_busy(1);
		rc = nvme_execute_partition(argv[2], argv[3], fs, label);
		if ((rc != 0) && !f_exists(NVME_ERR_FILE))
			nvme_set_error("Failed to create partition.");
		nvme_set_busy(0);
		return rc;
	}
	if ((argc >= 3) && (strcmp(argv[1], "delete") == 0)) {
		unlink(NVME_ERR_FILE);
		nvme_set_busy(1);
		rc = nvme_execute_delete(argv[2]);
		if ((rc != 0) && !f_exists(NVME_ERR_FILE))
			nvme_set_error("Failed to delete partition.");
		nvme_set_busy(0);
		return rc;
	}

	parse_rc_nvme_mounts(nvram_safe_get("nvme_mounts"));
	is_stop = ((argc >= 2) && (strcmp(argv[1], "stop") == 0));
	blkid_cache_loaded = 0;

	if (is_stop) {
		FILE *fp;
		struct mntent *mnt;

		unlink(NVME_PREV_MOUNTS_FILE);
		sync();
		if ((fp = setmntent("/proc/mounts", "r")) != NULL) {
			char dirs[NVME_MAX_MOUNTS * 2][128];
			int dcount = 0;
			while (((mnt = getmntent(fp)) != NULL) && (dcount < (int)(sizeof(dirs) / sizeof(dirs[0])))) {
				if (nvme_valid_partition(mnt->mnt_fsname))
					strlcpy(dirs[dcount++], mnt->mnt_dir, sizeof(dirs[0]));
			}
			endmntent(fp);
			for (i = dcount - 1; i >= 0; i--) {
				if ((umount(dirs[i]) != 0) && (umount2(dirs[i], MNT_FORCE) != 0) && (umount2(dirs[i], MNT_DETACH) != 0))
					logmsg(LOG_DEBUG, "nvme: %s was not unmounted (not mounted or busy)", dirs[i]);
				else if (strncmp(dirs[i], "/mnt/", 5) == 0)
					rmdir(dirs[i]);
			}
		}
		return 0;
	}

	{
		rc_nvme_mount_t prev_mounts[NVME_MAX_MOUNTS];
		int prev_count = 0;
		char prev_data[2048];

		if (f_read_string(NVME_PREV_MOUNTS_FILE, prev_data, sizeof(prev_data)) > 0)
			prev_count = parse_nvme_mounts_string(prev_data, prev_mounts, NVME_MAX_MOUNTS);

		for (i = 0; i < prev_count; i++) {
			char prev_dev[64];
			int found = 0;
			int j;

			strlcpy(prev_dev, prev_mounts[i].device, sizeof(prev_dev));
			if (prev_mounts[i].uuid[0] != '\0') {
				char resolved[64];
				if (nvme_find_by_uuid(prev_mounts[i].uuid, resolved, sizeof(resolved)))
					strlcpy(prev_dev, resolved, sizeof(prev_dev));
			}

			for (j = 0; j < rc_nvme_mounts_count; j++) {
				if (nvme_record_matches(rc_nvme_mounts[j].device, rc_nvme_mounts[j].uuid,
				                        prev_dev, prev_mounts[i].uuid)) {
					found = 1;
					if (!rc_nvme_mounts[j].mount_at_boot) {
						logmsg(LOG_INFO, "nvme: auto mount disabled for %s, unmounting", prev_dev);
						nvme_unmount_partition(prev_dev);
						if (strncmp(prev_mounts[i].mount_path, "/mnt/", 5) == 0 && !is_path_mounted(prev_mounts[i].mount_path))
							rmdir(prev_mounts[i].mount_path);
						if (strcmp(prev_mounts[i].mount_path, "/opt") == 0 && !is_path_mounted("/mnt/opt"))
							rmdir("/mnt/opt");
					}
					break;
				}
			}

			if (!found) {
				logmsg(LOG_INFO, "nvme: mount path cleared for %s, unmounting", prev_dev);
				nvme_unmount_partition(prev_dev);
				if (strncmp(prev_mounts[i].mount_path, "/mnt/", 5) == 0 && !is_path_mounted(prev_mounts[i].mount_path))
					rmdir(prev_mounts[i].mount_path);
				if (strcmp(prev_mounts[i].mount_path, "/opt") == 0 && !is_path_mounted("/mnt/opt"))
					rmdir("/mnt/opt");
			}
		}
	}

	/* First pass: resolve device nodes by UUID and unmount any old/changed mountpoints across all
	 * configured entries before mounting, so swapping or reassigning paths between partitions works. */
	for (i = 0; i < rc_nvme_mounts_count; i++) {
		char partition[64];
		const char *mount_path = rc_nvme_mounts[i].mount_path;
		const char *target_dir;
		int is_opt, ro_root, has_uuid, found_uuid = 0;

		entry_valid[i] = 0;
		if (!rc_nvme_mounts[i].mount_at_boot)
			continue;
		strlcpy(partition, rc_nvme_mounts[i].device, sizeof(partition));
		is_opt = (strcmp(mount_path, "/opt") == 0);
		ro_root = is_opt ? root_is_readonly() : 0;
		target_dir = (is_opt && ro_root) ? "/mnt/opt" : mount_path;
		has_uuid = (rc_nvme_mounts[i].uuid[0] != '\0');

		/* device numbering can change between boots: prefer the filesystem UUID when it is known */
		if (has_uuid) {
			char resolved[64];
			if (nvme_find_by_uuid(rc_nvme_mounts[i].uuid, resolved, sizeof(resolved))) {
				found_uuid = 1;
				if (strcmp(resolved, partition) != 0) {
					logmsg(LOG_INFO, "nvme: UUID %s is now %s (was %s)", rc_nvme_mounts[i].uuid, resolved, partition);
					strlcpy(partition, resolved, sizeof(partition));
				}
			}
		}

		/* the device node may not exist yet right after boot; wait a little (once for all mounts) */
		while (rc_nvme_mounts[i].mount_at_boot && (has_uuid ? !found_uuid : !f_exists(partition)) && (waited < NVME_DEVICE_WAIT_SECS)) {
			sleep(1);
			waited++;
			blkid_cache_loaded = 0;
			if (has_uuid) {
				char resolved[64];
				if (nvme_find_by_uuid(rc_nvme_mounts[i].uuid, resolved, sizeof(resolved))) {
					found_uuid = 1;
					if (strcmp(resolved, partition) != 0) {
						logmsg(LOG_INFO, "nvme: UUID %s is now %s (was %s)", rc_nvme_mounts[i].uuid, resolved, partition);
						strlcpy(partition, resolved, sizeof(partition));
					}
				}
			}
		}

		strlcpy(resolved_devs[i], partition, sizeof(resolved_devs[0]));

		if (!f_exists(partition)) {
			if (rc_nvme_mounts[i].mount_at_boot)
				logmsg(LOG_ERR, "nvme: %s not found, cannot mount on %s", partition, mount_path);
			continue;
		}
		if (has_uuid && !found_uuid && nvme_uuid_mismatch(partition, rc_nvme_mounts[i].uuid)) {
			if (rc_nvme_mounts[i].mount_at_boot)
				logmsg(LOG_ERR, "nvme: UUID %s not found (%s has a different UUID), cannot mount on %s",
				       rc_nvme_mounts[i].uuid, partition, mount_path);
			continue;
		}

		/* If the user changed this partition's mount path, unmount its previous mountpoint(s) */
		if (nvme_unmount_dev_except(partition, target_dir, (is_opt && ro_root) ? "/opt" : NULL) != 0)
			continue;

		entry_valid[i] = 1;
	}

	/* Second pass: mount all enabled entries. */
	for (i = 0; i < rc_nvme_mounts_count; i++) {
		char partition[64], mount_path[128], fstype[16];
		const char *target_dir;
		int is_opt, ro_root;

		if (!rc_nvme_mounts[i].mount_at_boot || !entry_valid[i])
			continue;

		strlcpy(partition, resolved_devs[i], sizeof(partition));
		strlcpy(mount_path, rc_nvme_mounts[i].mount_path, sizeof(mount_path));
		is_opt = (strcmp(mount_path, "/opt") == 0);
		ro_root = is_opt ? root_is_readonly() : 0;
		target_dir = (is_opt && ro_root) ? "/mnt/opt" : mount_path;

		if (!is_dev_mounted_at(partition, target_dir)) {
			/* If another NVMe partition whose mount path was cleared was occupying this target path, unmount it first */
			if (is_opt)
				nvme_unmount_nvme_at_path("/opt", partition);
			nvme_unmount_nvme_at_path(target_dir, partition);

			if (is_opt && !is_dev_mounted_at(partition, "/opt") && is_path_mounted("/opt")) {
				logmsg(LOG_ERR, "nvme: /opt is already mounted, cannot mount %s", partition);
				continue;
			}
			if (is_path_mounted(target_dir)) {
				logmsg(LOG_ERR, "nvme: %s is already mounted, cannot mount %s", target_dir, partition);
				continue;
			}

			get_partition_metadata(partition, fstype, sizeof(fstype));
			if (strcmp(fstype, "auto") == 0) {
				logmsg(LOG_ERR, "nvme: no filesystem found on %s, cannot mount on %s", partition, target_dir);
				continue;
			}

			mkdir_if_none(target_dir);

			/* Tomato64 mounts NTFS with the in-kernel ntfs3 driver (as in usb.c); ntfs-3g is not guaranteed to be installed.
			 * blkid reports the type as "ntfs". */
			if (strcmp(fstype, "ntfs") == 0)
				strlcpy(fstype, "ntfs3", sizeof(fstype));

			rc = eval("mount", "-t", fstype, "-o", "noatime,nodiratime", partition, (char *)target_dir, NULL);
			if (rc != 0) {
				logmsg(LOG_ERR, "nvme: failed to mount %s on %s (%s, exit code %d)", partition, target_dir, fstype, rc);
				if (strncmp(target_dir, "/mnt/", 5) == 0)
					rmdir(target_dir);
				continue;
			}
			logmsg(LOG_INFO, "nvme: mounted %s on %s", partition, target_dir);
		}

		if (is_opt && ro_root && !is_dev_mounted_at(partition, "/opt")) {
			nvme_unmount_nvme_at_path("/opt", partition);
			if (is_path_mounted("/opt")) {
				logmsg(LOG_ERR, "nvme: /opt is already mounted, cannot bind mount %s", partition);
				continue;
			}
			rc = eval("mount", "-o", "bind", "/mnt/opt", "/opt", NULL);
			if (rc != 0) {
				logmsg(LOG_ERR, "nvme: failed to bind mount /mnt/opt on /opt (exit code %d)", rc);
				if ((umount("/mnt/opt") == 0) || (umount2("/mnt/opt", MNT_FORCE) == 0))
					rmdir("/mnt/opt");
			}
			else
				logmsg(LOG_INFO, "nvme: bind mounted /mnt/opt on /opt for %s", partition);
		}
	}

	f_write_string(NVME_PREV_MOUNTS_FILE, nvram_safe_get("nvme_mounts"), 0, 0600);

	return 0;
}

void start_nvme(void)
{
	char *argv[] = { "nvme", "start", NULL };
	logmsg(LOG_INFO, "starting nvme storage...");
	nvme_main(2, argv);
}

void stop_nvme(void)
{
	char *argv[] = { "nvme", "stop", NULL };
	logmsg(LOG_INFO, "stopping nvme storage...");
	nvme_main(2, argv);
}
