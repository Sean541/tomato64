#include "tomato.h"
#include "shutils.h"
#include <ctype.h>
#include <sys/sysinfo.h>

#define MAX_DETECTED_DRIVES 4
#define MAX_PARTITIONS_PER_DRIVE 8

typedef struct {
    char name[32];
    unsigned long blocks;
    char fstype[16];
} httpd_nvme_partition_t;

typedef struct {
    char name[32];
    unsigned long blocks;
    httpd_nvme_partition_t partitions[MAX_PARTITIONS_PER_DRIVE];
    int partition_count;
} httpd_nvme_drive_t;

httpd_nvme_drive_t detected_drives[MAX_DETECTED_DRIVES];
nvme_mount_t nvme_mounts[16];
int detected_drives_count = 0;
int nvme_mounts_count = 0;

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
    char line[384];
    char dev_path[64], fstype[32];
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
                        httpd_nvme_drive_t *drive = &detected_drives[drive_idx];
                        if (drive->partition_count < MAX_PARTITIONS_PER_DRIVE) {
                            int p_idx = drive->partition_count;
                            
                            strlcpy(drive->partitions[p_idx].name, part_name, sizeof(drive->partitions[p_idx].name));
                            drive->partitions[p_idx].blocks = get_partition_blocks(part_name);

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

int parse_nvme_mounts(const char *data) {
    if (!data || strlen(data) == 0) {
        nvme_mounts_count = 0;
        return 0;
    }

    nvme_mounts_count = 0;
    const char *ptr = data;
    const char *end;

    while (nvme_mounts_count < 16 && (end = strchr(ptr, '<')) != NULL) {
        const char *start = ptr;
        const char *dev_end = strchr(ptr, '>');
        if (dev_end && dev_end < end) {
            const char *path_end = strchr(dev_end + 1, '>');
            if (path_end && path_end < end) {
                size_t dev_len = dev_end - start;
                if (dev_len >= sizeof(nvme_mounts[0].device)) dev_len = sizeof(nvme_mounts[0].device) - 1;
                snprintf(nvme_mounts[nvme_mounts_count].device, sizeof(nvme_mounts[0].device), "%.*s", (int)dev_len, start);

                const char *path_start = dev_end + 1;
                size_t path_len = path_end - path_start;
                if (path_len >= sizeof(nvme_mounts[0].mount_path)) path_len = sizeof(nvme_mounts[0].mount_path) - 1;
                snprintf(nvme_mounts[nvme_mounts_count].mount_path, sizeof(nvme_mounts[0].mount_path), "%.*s", (int)path_len, path_start);

                nvme_mounts[nvme_mounts_count].mount_at_boot = (strncmp(path_end + 1, "1", 1) == 0);
                nvme_mounts_count++;
            }
        }
        ptr = end + 1;
    }
    return 0;
}

int load_nvme_mounts(void) {
    char *data = nvram_safe_get("nvme_mounts");
    return parse_nvme_mounts(data);
}

void asp_nvme_mounts(int argc, char **argv) {
    int i;
    int first = 1;
    load_nvme_mounts();

    for (i = 0; i < nvme_mounts_count; i++) {
        if (!first) web_puts(", ");
        char *dev_esc = js_string(nvme_mounts[i].device);
        char *path_esc = js_string(nvme_mounts[i].mount_path);
        if (dev_esc && path_esc) {
            web_printf("\"%s>%s>%d\"", dev_esc, path_esc, nvme_mounts[i].mount_at_boot);
            first = 0;
        }
        free(dev_esc);
        free(path_esc);
    }
}

void asp_nvme_disks(int argc, char **argv) {
    int i, j;
    int first_disk = 1;
    discover_nvme_hardware();

    web_puts("\nnvme_disks = {\n");
    for (i = 0; i < detected_drives_count; i++) {
        if (!first_disk) web_puts(",\n");
        char *disk_esc = js_string(detected_drives[i].name);
        if (disk_esc) {
            web_printf("\t'%s': [", disk_esc);
            free(disk_esc);
        }

        int first_partition = 1;
        for (j = 0; j < detected_drives[i].partition_count; j++) {
            if (!first_partition) web_puts(", ");
            char *part_esc = js_string(detected_drives[i].partitions[j].name);
            char *fs_esc = js_string(detected_drives[i].partitions[j].fstype);
            
            if (part_esc && fs_esc) {
                web_printf("['%s', %lu, '%s']", part_esc, detected_drives[i].partitions[j].blocks, fs_esc);
            }
            if (part_esc) free(part_esc);
            if (fs_esc) free(fs_esc);
            first_partition = 0;
        }
        web_puts("]");
        first_disk = 0;
    }
    web_puts("\n};\n");
}
