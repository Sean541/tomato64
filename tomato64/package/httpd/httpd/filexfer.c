/*
 * filexfer.c - Web GUI File Transfer & Visual Directory Browser handler for Tomato64
 *
 * Provides visual directory listings, streaming file uploads, downloads,
 * directory creation, and file deletion for tools-shell.asp (MobaXterm-style SCP panel).
 *
 * Honors NVRAM variable 'restrict_wf':
 *   - If restrict_wf == "" (empty): file transfer can access any directory.
 *   - If restrict_wf is set to a path or comma-separated list of paths:
 *     file transfer can only access those paths and subdirectories under them.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <libgen.h>
#include <limits.h>

#include "tomato.h"
#include "httpd.h"

#define MAX_FILE_SIZE  (64UL * 1024UL * 1024UL)  /* 64 MB maximum transfer limit */

static char g_upload_path[256];
static unsigned long g_uploaded_bytes = 0;
static const char *g_upload_error = NULL;

/* Check if test_path is under allowed_prefix */
static int path_under_prefix(const char *test_path, const char *prefix)
{
	size_t plen = strlen(prefix);

	if (strcmp(prefix, "/") == 0)
		return 1;

	if (strcmp(test_path, prefix) == 0)
		return 1;

	if ((strncmp(test_path, prefix, plen) == 0) && (test_path[plen] == '/'))
		return 1;

	return 0;
}

/*
 * Check if target_path is permitted according to NVRAM variable 'restrict_wf'.
 * If restrict_wf == "" or unset, all paths are permitted (returns 1).
 * If restrict_wf has comma-separated paths, target_path must be inside one of them.
 */
static int path_is_allowed(const char *target_path)
{
	const char *restrict_wf = nvram_safe_get("restrict_wf");
	char *list, *p, *item;
	char resolved[PATH_MAX];
	int match = 0;

	/* If restrict_wf is empty or not set, access to any directory is allowed */
	if (!restrict_wf || !*restrict_wf)
		return 1;

	if (!target_path || (target_path[0] != '/'))
		return 0;

	/* Reject basic directory traversal */
	if (strstr(target_path, "..") != NULL)
		return 0;

	list = strdup(restrict_wf);
	if (!list)
		return 0;

	p = list;
	while ((item = strsep(&p, ",")) != NULL) {
		size_t len;

		while ((*item == ' ') || (*item == '\t'))
			item++;

		len = strlen(item);
		while ((len > 0) && ((item[len - 1] == ' ') || (item[len - 1] == '\t')))
			item[--len] = '\0';

		while ((len > 1) && (item[len - 1] == '/'))
			item[--len] = '\0';

		if (len == 0)
			continue;

		if (path_under_prefix(target_path, item)) {
			match = 1;
			break;
		}
	}
	free(list);

	if (!match)
		return 0;

	/* Verify realpath to prevent symlink traversal escaping allowed prefixes */
	if (realpath(target_path, resolved) != NULL) {
		list = strdup(restrict_wf);
		if (!list)
			return 0;

		p = list;
		match = 0;
		while ((item = strsep(&p, ",")) != NULL) {
			size_t len;

			while ((*item == ' ') || (*item == '\t'))
				item++;

			len = strlen(item);
			while ((len > 0) && ((item[len - 1] == ' ') || (item[len - 1] == '\t')))
				item[--len] = '\0';

			while ((len > 1) && (item[len - 1] == '/'))
				item[--len] = '\0';

			if (len == 0)
				continue;

			if (path_under_prefix(resolved, item)) {
				match = 1;
				break;
			}
		}
		free(list);
		if (!match)
			return 0;
	}

	return 1;
}

/* Sanitize target filename: extract basename, strip illegal characters, reject path traversal */
static int sanitize_basename(char *dst, const char *src, size_t dst_sz)
{
	const char *base;

	if ((src == NULL) || (*src == '\0'))
		return -1;

	/* Reject traversal patterns */
	if (strstr(src, "..") != NULL)
		return -1;

	base = strrchr(src, '/');
	base = base ? base + 1 : src;

	if (*base == '\0')
		return -1;

	if (strchr(base, '\\') != NULL)
		return -1;

	strlcpy(dst, base, dst_sz);
	return 0;
}

/* Incoming upload stream handler */
void wi_filexfer(char *url, int len, char *boundary)
{
	char *action, *dir_param, *name_param;
	char filename[128];
	char fullpath[256];
	char buf[8192];
	struct statvfs svfs;
	struct stat st;
	FILE *f = NULL;
	int remaining, n;

	g_upload_path[0] = '\0';
	g_uploaded_bytes = 0;
	g_upload_error = NULL;

	/* Validate CSRF token */
	check_id(url);

	action = webcgi_safeget("action", "");
	if (strcmp(action, "upload") != 0) {
		web_eat(len);
		return;
	}

	dir_param = webcgi_safeget("dir", "/tmp");
	name_param = webcgi_safeget("name", "");

	/* Reject relative paths and path traversal */
	if ((dir_param[0] != '/') || (strstr(dir_param, "..") != NULL)) {
		g_upload_error = "Invalid target directory path";
		goto ERROR;
	}

	/* Enforce restrict_wf */
	if (!path_is_allowed(dir_param)) {
		g_upload_error = "Access denied: directory restricted by restrict_wf";
		goto ERROR;
	}

	/* Verify target directory exists and is a directory */
	if ((stat(dir_param, &st) != 0) || !S_ISDIR(st.st_mode)) {
		g_upload_error = "Target directory does not exist";
		goto ERROR;
	}

	/* Sanitize filename */
	if (sanitize_basename(filename, name_param, sizeof(filename)) != 0) {
		g_upload_error = "Invalid filename";
		goto ERROR;
	}

	/* Build destination path */
	if (strcmp(dir_param, "/") == 0)
		snprintf(fullpath, sizeof(fullpath), "/%s", filename);
	else
		snprintf(fullpath, sizeof(fullpath), "%s/%s", dir_param, filename);

	/* Check upload size constraint */
	if ((len <= 0) || ((unsigned long)len > MAX_FILE_SIZE)) {
		g_upload_error = "File size exceeds allowed limit (64 MB)";
		goto ERROR;
	}

	/* Check available disk space on target filesystem */
	if (statvfs(dir_param, &svfs) == 0) {
		unsigned long long free_bytes = (unsigned long long)svfs.f_bavail * svfs.f_frsize;
		if (free_bytes < (unsigned long long)len) {
			g_upload_error = "Insufficient storage space on target filesystem";
			goto ERROR;
		}
	}

	/* Open destination file (create or truncate) */
	f = fopen(fullpath, "wb");
	if (f == NULL) {
		g_upload_error = "Unable to create file in target directory";
		goto ERROR;
	}

	/* Stream incoming HTTP payload directly to disk in chunks */
	remaining = len;
	while (remaining > 0) {
		n = web_read(buf, MIN((int)sizeof(buf), remaining));
		if (n <= 0) {
			g_upload_error = "Connection lost during upload";
			goto ERROR;
		}
		if (fwrite(buf, 1, n, f) != (size_t)n) {
			g_upload_error = "Error writing file to disk";
			goto ERROR;
		}
		remaining -= n;
		g_uploaded_bytes += n;
	}

	fclose(f);
	f = NULL;

	/* Set permissions: executable for scripts/binaries */
	chmod(fullpath, 0755);
	strlcpy(g_upload_path, fullpath, sizeof(g_upload_path));
	return;

ERROR:
	if (f != NULL) {
		fclose(f);
		unlink(fullpath);
	}
	web_eat(len);
}

/* Directory listing handler */
static void send_dir_listing(const char *dir)
{
	DIR *dp;
	struct dirent *ent;
	struct stat st;
	char item_path[512];
	int first = 1;

	/* Enforce restrict_wf */
	if (!path_is_allowed(dir)) {
		send_header(403, NULL, mime_javascript, 0);
		web_printf("{\"status\":\"error\",\"error\":\"Access denied: directory restricted by restrict_wf\"}");
		return;
	}

	dp = opendir(dir);
	if (dp == NULL) {
		send_header(404, NULL, mime_javascript, 0);
		web_printf("{\"status\":\"error\",\"error\":\"Cannot open directory\"}");
		return;
	}

	send_header(200, NULL, mime_javascript, 0);
	web_printf("{\"status\":\"ok\",\"dir\":\"%s\",\"restrict_wf\":\"", dir);
	web_putj(nvram_safe_get("restrict_wf"));
	web_puts("\",\"entries\":[");

	while ((ent = readdir(dp)) != NULL) {
		char esc_name[256];
		char *src, *dst;

		/* Skip current directory "." */
		if (strcmp(ent->d_name, ".") == 0)
			continue;

		/* If root directory "/", skip ".." */
		if ((strcmp(dir, "/") == 0) && (strcmp(ent->d_name, "..") == 0))
			continue;

		if (strcmp(dir, "/") == 0)
			snprintf(item_path, sizeof(item_path), "/%s", ent->d_name);
		else
			snprintf(item_path, sizeof(item_path), "%s/%s", dir, ent->d_name);

		memset(&st, 0, sizeof(st));
		if (stat(item_path, &st) != 0)
			continue;

		/* Escape JSON string characters */
		src = ent->d_name;
		dst = esc_name;
		while (*src && (dst < (esc_name + sizeof(esc_name) - 2))) {
			if ((*src == '"') || (*src == '\\'))
				*dst++ = '\\';
			*dst++ = *src++;
		}
		*dst = '\0';

		web_printf("%s{\"name\":\"%s\",\"is_dir\":%d,\"size\":%lu,\"mtime\":%lu,\"mode\":%u}",
		           first ? "" : ",",
		           esc_name,
		           S_ISDIR(st.st_mode) ? 1 : 0,
		           (unsigned long)st.st_size,
		           (unsigned long)st.st_mtime,
		           (unsigned int)(st.st_mode & 0777));
		first = 0;
	}

	closedir(dp);
	web_puts("]}");
}

/* Delete file or empty folder */
static void handle_delete(const char *path)
{
	struct stat st;

	if (!*path || (path[0] != '/') || (strstr(path, "..") != NULL)) {
		send_header(400, NULL, mime_javascript, 0);
		web_printf("{\"status\":\"error\",\"error\":\"Invalid file path\"}");
		return;
	}

	/* Enforce restrict_wf */
	if (!path_is_allowed(path)) {
		send_header(403, NULL, mime_javascript, 0);
		web_printf("{\"status\":\"error\",\"error\":\"Access denied: path restricted by restrict_wf\"}");
		return;
	}

	/* Never allow deleting root or top-level essential system directories */
	if ((strcmp(path, "/") == 0) || (strcmp(path, "/bin") == 0) || (strcmp(path, "/sbin") == 0) ||
	    (strcmp(path, "/etc") == 0) || (strcmp(path, "/lib") == 0) || (strcmp(path, "/usr") == 0) ||
	    (strcmp(path, "/dev") == 0) || (strcmp(path, "/proc") == 0) || (strcmp(path, "/sys") == 0)) {
		send_header(403, NULL, mime_javascript, 0);
		web_printf("{\"status\":\"error\",\"error\":\"Cannot delete system directory\"}");
		return;
	}

	if (stat(path, &st) != 0) {
		send_header(404, NULL, mime_javascript, 0);
		web_printf("{\"status\":\"error\",\"error\":\"File not found\"}");
		return;
	}

	if (S_ISDIR(st.st_mode)) {
		if (rmdir(path) != 0) {
			send_header(400, NULL, mime_javascript, 0);
			web_printf("{\"status\":\"error\",\"error\":\"Cannot remove directory (ensure it is empty)\"}");
			return;
		}
	}
	else {
		if (unlink(path) != 0) {
			send_header(400, NULL, mime_javascript, 0);
			web_printf("{\"status\":\"error\",\"error\":\"Failed to delete file\"}");
			return;
		}
	}

	send_header(200, NULL, mime_javascript, 0);
	web_printf("{\"status\":\"ok\"}");
}

/* Create new folder */
static void handle_mkdir(const char *dir, const char *name)
{
	char dirname[128];
	char fullpath[256];

	if (!dir || (dir[0] != '/') || (strstr(dir, "..") != NULL)) {
		send_header(400, NULL, mime_javascript, 0);
		web_printf("{\"status\":\"error\",\"error\":\"Invalid directory path\"}");
		return;
	}

	/* Enforce restrict_wf */
	if (!path_is_allowed(dir)) {
		send_header(403, NULL, mime_javascript, 0);
		web_printf("{\"status\":\"error\",\"error\":\"Access denied: directory restricted by restrict_wf\"}");
		return;
	}

	if (sanitize_basename(dirname, name, sizeof(dirname)) != 0) {
		send_header(400, NULL, mime_javascript, 0);
		web_printf("{\"status\":\"error\",\"error\":\"Invalid folder name\"}");
		return;
	}

	if (strcmp(dir, "/") == 0)
		snprintf(fullpath, sizeof(fullpath), "/%s", dirname);
	else
		snprintf(fullpath, sizeof(fullpath), "%s/%s", dir, dirname);

	if (mkdir(fullpath, 0755) != 0) {
		send_header(400, NULL, mime_javascript, 0);
		web_printf("{\"status\":\"error\",\"error\":\"Failed to create directory\"}");
		return;
	}

	send_header(200, NULL, mime_javascript, 0);
	web_printf("{\"status\":\"ok\",\"path\":\"%s\"}", fullpath);
}

/* Outgoing request dispatcher */
void wo_filexfer(char *url)
{
	char *action = webcgi_safeget("action", "");

	check_id(url);

	if (strcmp(action, "upload") == 0) {
		/* Return JSON status response for the upload */
		send_header(g_upload_error ? 400 : 200, NULL, mime_javascript, 0);
		if (g_upload_error == NULL) {
			web_printf("{\"status\":\"ok\",\"path\":\"%s\",\"size\":%lu}", g_upload_path, g_uploaded_bytes);
		}
		else {
			web_printf("{\"status\":\"error\",\"error\":\"%s\"}", g_upload_error);
		}
		return;
	}

	if (strcmp(action, "list") == 0) {
		char *dir = webcgi_safeget("dir", "/tmp");
		if ((dir[0] != '/') || (strstr(dir, "..") != NULL)) {
			send_header(400, NULL, mime_javascript, 0);
			web_printf("{\"status\":\"error\",\"error\":\"Invalid directory path\"}");
			return;
		}
		send_dir_listing(dir);
		return;
	}

	if (strcmp(action, "delete") == 0) {
		char *path = webcgi_safeget("path", "");
		handle_delete(path);
		return;
	}

	if (strcmp(action, "mkdir") == 0) {
		char *dir = webcgi_safeget("dir", "/tmp");
		char *name = webcgi_safeget("name", "");
		handle_mkdir(dir, name);
		return;
	}

	if (strcmp(action, "download") == 0) {
		char *path;
		char hdr[256];
		char fname[128];
		struct stat st;
		int fd;

		path = webcgi_safeget("path", "");

		/* Validate path */
		if ((*path != '/') || (strstr(path, "..") != NULL)) {
			send_error(400, NULL, "Invalid file path");
			return;
		}

		/* Enforce restrict_wf */
		if (!path_is_allowed(path)) {
			send_error(403, NULL, "Access denied: path restricted by restrict_wf");
			return;
		}

		if (stat(path, &st) != 0) {
			send_error(404, NULL, "File not found");
			return;
		}

		/* Download only regular files */
		if (!S_ISREG(st.st_mode)) {
			send_error(403, NULL, "Only regular files can be downloaded");
			return;
		}

		if ((unsigned long)st.st_size > MAX_FILE_SIZE) {
			send_error(400, NULL, "File exceeds maximum download size (64 MB)");
			return;
		}

		if (sanitize_basename(fname, path, sizeof(fname)) != 0)
			strlcpy(fname, "download.bin", sizeof(fname));

		/* Send attachment headers */
		snprintf(hdr, sizeof(hdr),
		         "Content-Disposition: attachment; filename=\"%s\"\r\nContent-Length: %ld",
		         fname, (long)st.st_size);

		send_header(200, hdr, mime_octetstream, 0);

		/* Stream file to client in chunks */
		fd = open(path, O_RDONLY | O_NONBLOCK);
		if (fd >= 0) {
			char buf[8192];
			ssize_t nr;

			while ((nr = read(fd, buf, sizeof(buf))) > 0) {
				if (web_write(buf, (int)nr) < 0)
					break;
			}
			close(fd);
		}
		return;
	}

	send_error(400, NULL, "Bad request");
}
