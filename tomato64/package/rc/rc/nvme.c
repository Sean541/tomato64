#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <bcmnvram.h>
#include <shutils.h>
#include <shared.h>
#include <tomato_profile.h>
#include <tomato_config.h>
#include <sys/mount.h>
#include <ctype.h>
#include "rc.h"

#define LOGMSG_DISABLE	DISABLE_SYSLOG_OSM
#define LOGMSG_NVDEBUG	"nvme_debug"

#define MAX_DETECTED_DRIVES 4
#define MAX_PARTITIONS_PER_DRIVE 8

#define APPEND_CONFIG_ENTRY(dest, node, path, boot, uuid) do { \
    char entry_buf[256]; \
    snprintf(entry_buf, sizeof(entry_buf), "%s>%s>%s>%s<", (node), (path), (boot), (uuid)); \
    strlcat((dest), entry_buf, sizeof(dest)); \
} while(0)

typedef struct {
    char name[32];
	char uuid[40];
    unsigned long blocks;
    char fstype[16];
} rc_nvme_partition_t;

typedef struct {
    char name[32];
    unsigned long blocks;
    rc_nvme_partition_t partitions[MAX_PARTITIONS_PER_DRIVE];
    int partition_count;
} rc_nvme_drive_t;

rc_nvme_drive_t detected_drives[MAX_DETECTED_DRIVES];
int detected_drives_count = 0;

static void get_partition_metadata(const char *dev_node, char *out_uuid, size_t uuid_len, char *out_type, size_t type_len) {
    FILE *pipe_fp;
    char cmd[128];
    char res_buf[256];

    out_uuid[0] = '\0';
    strlcpy(out_type, "auto", type_len);

    snprintf(cmd, sizeof(cmd), "blkid %s", dev_node);
    pipe_fp = popen(cmd, "r");
    if (!pipe_fp) return;

    if (fgets(res_buf, sizeof(res_buf), pipe_fp) != NULL) {
        char *uuid_attr = strstr(res_buf, "UUID=\"");
        if (uuid_attr) {
            uuid_attr += 6;
            char *end_quote = strchr(uuid_attr, '"');
            if (end_quote) {
                *end_quote = '\0';
                strlcpy(out_uuid, uuid_attr, uuid_len);
            }
        }
        char *type_attr = strstr(res_buf, "TYPE=\"");
        if (type_attr) {
            type_attr += 6;
            char *end_quote = strchr(type_attr, '"');
            if (end_quote) {
                *end_quote = '\0';
                strlcpy(out_type, type_attr, type_len);
            }
        }
    }
    pclose(pipe_fp);
}

/* Helper function to find active device node for a given UUID */
static void get_device_by_uuid(const char *uuid_str, char *out_node, size_t out_len) {
    FILE *pipe_fp;
    char cmd[128];

    out_node[0] = '\0';
    if (!uuid_str || uuid_str[0] == '\0') return;

    snprintf(cmd, sizeof(cmd), "blkid -U %s", uuid_str);
    pipe_fp = popen(cmd, "r");
    if (!pipe_fp) return;

    if (fgets(out_node, out_len, pipe_fp) != NULL) {
        out_node[strcspn(out_node, "\r\n")] = '\0';
    }
    pclose(pipe_fp);
}

/* Helper function to securely query partition block boundaries from sysfs */
unsigned long get_partition_blocks(const char *part_name) {
    char sys_path[128];
    char size_buf[32];
    unsigned long sectors = 0;

    snprintf(sys_path, sizeof(sys_path), "/sys/class/block/%s/size", part_name);
    if (f_read_string(sys_path, size_buf, sizeof(size_buf)) > 0) {
        sectors = strtoul(size_buf, NULL, 10);
    }
    return sectors / 2;
}

void discover_nvme_hardware(void) {
    FILE *pipe_fp;
    char line[256];
    char dev_path[64], fstype[16];
    int i;

    detected_drives_count = 0;

    pipe_fp = popen("blkid", "r");
    if (!pipe_fp) return;

    while (fgets(line, sizeof(line), pipe_fp) && detected_drives_count < MAX_DETECTED_DRIVES) {
        if (sscanf(line, "%63[^:]:", dev_path) == 1) {
            if (strncmp(dev_path, "/dev/nvme", 9) == 0) {
                char *part_name = dev_path + 5;
                char *p_ptr = strchr(part_name, 'p');
                
                if (p_ptr != NULL && isdigit((unsigned char)*(p_ptr + 1))) {
                    size_t drive_name_len = p_ptr - part_name;
                    char parent_drive[32];
                    
                    if (drive_name_len >= sizeof(parent_drive)) drive_name_len = sizeof(parent_drive) - 1;
                    snprintf(parent_drive, sizeof(parent_drive), "%.*s", (int)drive_name_len, part_name);

                    int drive_idx = -1;
                    for (i = 0; i < detected_drives_count; i++) {
                        if (strcmp(detected_drives[i].name, parent_drive) == 0) {
                            drive_idx = i;
                            break;
                        }
                    }

                    if (drive_idx == -1 && detected_drives_count < MAX_DETECTED_DRIVES) {
                        drive_idx = detected_drives_count;
                        strlcpy(detected_drives[drive_idx].name, parent_drive, sizeof(detected_drives[drive_idx].name));
                        detected_drives[drive_idx].partition_count = 0;
                        detected_drives[drive_idx].blocks = get_partition_blocks(parent_drive);
                        detected_drives_count++;
                    }

                    if (drive_idx != -1) {
                        rc_nvme_drive_t *drive = &detected_drives[drive_idx];
                        if (drive->partition_count < MAX_PARTITIONS_PER_DRIVE) {
                            int p_idx = drive->partition_count;
                            
                            strlcpy(drive->partitions[p_idx].name, part_name, sizeof(drive->partitions[p_idx].name));
                            drive->partitions[p_idx].blocks = get_partition_blocks(part_name);
							
							char uuid[40] = "";
							char *uuid_attr = strstr(line, "UUID=\"");
							if (uuid_attr) {
								uuid_attr += 6;
								char *end_quote = strchr(uuid_attr, '"');
								if (end_quote) {
									*end_quote = '\0';
									strlcpy(uuid, uuid_attr, sizeof(uuid));
								}
							}
							strlcpy(drive->partitions[p_idx].uuid, uuid, sizeof(drive->partitions[p_idx].uuid));

                            strlcpy(fstype, "Unknown", sizeof(fstype));
                            char *type_attr = strstr(line, "TYPE=\"");
                            if (type_attr) {
                                type_attr += 6;
                                char *end_quote = strchr(type_attr, '"');
                                if (end_quote) {
                                    *end_quote = '\0';
                                    strlcpy(fstype, type_attr, sizeof(fstype));
                                }
                            }
                            strlcpy(drive->partitions[p_idx].fstype, fstype, sizeof(drive->partitions[p_idx].fstype));
                            drive->partition_count++;
                        }
                    }
                }
            }
        }
    }
    pclose(pipe_fp);
}

int nvme_main(int argc, char *argv[]) {
    char cfg_buf[1024];
    char new_cfg[1024] = "";
    char *partition, *mount_path, *mount_at_boot, *uuid_str;
    char *cfg = nvram_safe_get("nvme_mounts");
    int is_stop = (argc >= 2 && strcmp(argv[1], "stop") == 0);
    int nvram_updated = 0;
    char fstype[16];

    if (!cfg || *cfg == '\0') {
        return 0;
    }

    strlcpy(cfg_buf, cfg, sizeof(cfg_buf));

    char *ptr = cfg_buf;
    while (ptr && *ptr != '\0') {
        partition = ptr;
        char *next_gt = strchr(ptr, '>');
        if (!next_gt) break;
        *next_gt = '\0'; 
        ptr = next_gt + 1;
        
        mount_path = ptr;
        next_gt = strchr(ptr, '>');
        if (!next_gt) break;
        *next_gt = '\0'; 
        ptr = next_gt + 1;
        
        mount_at_boot = ptr;
        next_gt = strchr(ptr, '>');
        char *next_lt = strchr(ptr, '<');
        
        uuid_str = "";
        if (next_gt && (!next_lt || next_gt < next_lt)) {
            *next_gt = '\0';
            ptr = next_gt + 1;
            uuid_str = ptr;
            
            next_lt = strchr(ptr, '<');
            if (!next_lt) break;
            *next_lt = '\0';
            ptr = next_lt + 1;
        } else {
            if (!next_lt) break;
            *next_lt = '\0';
            ptr = next_lt + 1;
        }

        if (strcmp(mount_path, "/") == 0 || strncmp(partition, "/dev/", 5) != 0) continue;

        if (is_stop) {
            if (umount(mount_path) != 0) {
                umount2(mount_path, MNT_FORCE); 
            }
        } else if (*mount_at_boot == '1') {
            
            char active_partition[64];
            strlcpy(active_partition, partition, sizeof(active_partition));

            if (uuid_str[0] != '\0') {
                char resolved_node[64];
                get_device_by_uuid(uuid_str, resolved_node, sizeof(resolved_node));
                if (resolved_node[0] != '\0' && strcmp(resolved_node, partition) != 0) {
                    logmsg(LOG_INFO, "NVMe: Device name change detected for mount %s. Updating from %s to %s", 
                           mount_path, partition, resolved_node);
                    strlcpy(active_partition, resolved_node, sizeof(active_partition));
                    nvram_updated = 1;
                }
            } else {
                char generated_uuid[64];
                get_partition_metadata(partition, generated_uuid, sizeof(generated_uuid), fstype, sizeof(fstype));
                if (generated_uuid[0] != '\0') {
                    uuid_str = generated_uuid;
                    nvram_updated = 1;
                    logmsg(LOG_INFO, "NVMe: Appended UUID %s to active mount %s", uuid_str, mount_path);
                }
            }

            if (findmntents(active_partition, 0, NULL, 0) != NULL) {
                logmsg(LOG_INFO, "NVMe: %s already mounted", active_partition);
                APPEND_CONFIG_ENTRY(new_cfg, active_partition, mount_path, mount_at_boot, uuid_str);
                continue;
            }
            
            mkdir_if_none(mount_path);
            
            if (uuid_str[0] != '\0' && strcmp(fstype, "auto") == 0) {
                char dummy_uuid[64];
                get_partition_metadata(active_partition, dummy_uuid, sizeof(dummy_uuid), fstype, sizeof(fstype));
            }

            if (strcmp(fstype, "ntfs") == 0 && f_exists("/usr/bin/ntfs-3g")) {
                int mount_ntfs = -1;
                if (uuid_str[0] != '\0') {
                    char ntfs_spec[128];
                    snprintf(ntfs_spec, sizeof(ntfs_spec), "UUID=%s", uuid_str);
                    mount_ntfs = eval("ntfs-3g", "-o", "noatime", ntfs_spec, mount_path);
                }
                if (mount_ntfs != 0) {
                    mount_ntfs = eval("ntfs-3g", "-o", "noatime", active_partition, mount_path);
                }

                if (mount_ntfs == 0)
                    logmsg(LOG_INFO, "NVMe: Successfully mounted NTFS partition on %s", mount_path);
                else
                    logmsg(LOG_ERR, "NVMe: Failed to mount NTFS partition on %s", mount_path);
                
                APPEND_CONFIG_ENTRY(new_cfg, active_partition, mount_path, mount_at_boot, uuid_str);
                continue; 
            }
      
            int mount_res = -1;
            if (uuid_str[0] != '\0') {
                char mount_spec[128];
                snprintf(mount_spec, sizeof(mount_spec), "UUID=%s", uuid_str);
                mount_res = eval("mount", mount_spec, mount_path, "-o", "noatime,nodiratime", "-t", fstype);
                if (mount_res == 0) {
                    logmsg(LOG_INFO, "NVMe: Mounted via stored UUID %s on %s", uuid_str, mount_path);
                }
            }

            if (mount_res != 0) {
                mount_res = eval("mount", active_partition, mount_path, "-o", "noatime,nodiratime", "-t", fstype);
                if (mount_res == 0) {
                    logmsg(LOG_INFO, "NVMe: Mounted via device name fallback %s on %s", active_partition, mount_path);
                } else {
                    logmsg(LOG_ERR, "NVMe: Failed to mount %s on %s as %s", active_partition, mount_path, fstype);
                }
            }

            APPEND_CONFIG_ENTRY(new_cfg, active_partition, mount_path, mount_at_boot, uuid_str);
        } else {
            APPEND_CONFIG_ENTRY(new_cfg, partition, mount_path, mount_at_boot, uuid_str);
        }
    }

    if (nvram_updated) {
        nvram_set("nvme_mounts", new_cfg);
        nvram_commit();
        logmsg(LOG_INFO, "NVMe: NVRAM nvme_mounts automatically updated and committed.");
    }

    return 0;
}
