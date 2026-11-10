<!DOCTYPE html>
<!--
	Tomato GUI
	Copyright (C) 2006-2010 Jonathan Zarate
	http://www.polarcloud.com/tomato/

	Shell Web GUI
	Copyright (C) 2023 Lance Fredrickson
	lancethepants@gmail.com

	For use with Tomato Firmware only.
	No part of this file may be used without permission.
-->
<html lang="en-GB">
<head>
<meta http-equiv="content-type" content="text/html;charset=utf-8">
<meta name="robots" content="noindex,nofollow">
<title>[<% ident(); %>] Tools: System Commands</title>
<link rel="stylesheet" type="text/css" href="tomato.css?rel=<% version(); %>">
<% css(); %>
<script src="tomato.js?rel=<% version(); %>"></script>

<style>
.mobax-wrap {
	display: flex;
	flex-direction: row;
	align-items: stretch;
	width: 100%;
	height: 560px;
	border: 1px solid #444;
	border-radius: 4px;
	overflow: hidden;
	background: #111;
	box-shadow: 0 2px 6px rgba(0, 0, 0, 0.3);
	margin-bottom: 10px;
}
.mobax-sidebar {
	position: relative;
	width: 330px;
	min-width: 260px;
	max-width: 450px;
	background: #1a1e22;
	color: #e0e0e0;
	display: flex;
	flex-direction: column;
	border-right: 1px solid #333;
	transition: width 0.15s ease, min-width 0.15s ease;
}
.mobax-sidebar.collapsed {
	display: none !important;
}
.mobax-toggle {
	width: 16px;
	background: #22262b;
	cursor: pointer;
	display: flex;
	align-items: center;
	justify-content: center;
	color: #888;
	user-select: none;
	border-right: 1px solid #333;
	font-size: 10px;
}
.mobax-toggle:hover {
	background: #2b82d9;
	color: #fff;
}
.mobax-main {
	flex: 1 1 auto;
	display: flex;
	flex-direction: column;
	background: #000;
	min-width: 0;
}
#terminal {
	width: 100%;
	height: 100%;
	border: none;
	background: #000;
}
.mobax-bar {
	display: flex;
	align-items: center;
	gap: 4px;
	padding: 6px 8px;
	background: #21262d;
	border-bottom: 1px solid #30363d;
}
.mobax-bar input[type="text"] {
	flex: 1 1 auto;
	min-width: 0;
	background: #0d1117;
	color: #c9d1d9;
	border: 1px solid #30363d;
	border-radius: 3px;
	padding: 3px 6px;
	font-family: monospace;
	font-size: 11px;
}
.mobax-btn {
	background: #30363d;
	color: #c9d1d9;
	border: 1px solid #484f58;
	border-radius: 3px;
	padding: 2px 7px;
	cursor: pointer;
	font-size: 11px;
	line-height: 18px;
	white-space: nowrap;
}
.mobax-btn:hover {
	background: #2b82d9;
	color: #fff;
	border-color: #2b82d9;
}
.mobax-filelist-wrap {
	flex: 1 1 auto;
	overflow-y: auto;
	background: #0d1117;
	position: relative;
}
.mobax-table {
	width: 100%;
	border-collapse: collapse;
	font-size: 11px;
	font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, monospace;
}
.mobax-table th {
	position: sticky;
	top: 0;
	background: #161b22;
	color: #8b949e;
	text-align: left;
	padding: 4px 6px;
	border-bottom: 1px solid #30363d;
	font-weight: 600;
	z-index: 1;
}
.mobax-table td {
	padding: 3px 6px;
	border-bottom: 1px solid rgba(255, 255, 255, 0.05);
	color: #c9d1d9;
	white-space: nowrap;
	overflow: hidden;
	text-overflow: ellipsis;
	max-width: 160px;
}
.mobax-row:hover {
	background: rgba(56, 139, 253, 0.15);
}
.mobax-folder {
	color: #e3b341;
	font-weight: 500;
	cursor: pointer;
}
.mobax-file {
	color: #c9d1d9;
	cursor: default;
}
.mobax-actions {
	text-align: right;
	width: 70px;
	overflow: visible !important;
	text-overflow: clip !important;
}
.mobax-action-btn {
	color: #8b949e;
	cursor: pointer;
	text-decoration: none;
	padding: 0 3px;
	font-size: 11px;
}
.mobax-action-btn:hover {
	color: #58a6ff;
}
.mobax-action-get {
	color: #3fff5a !important;
	font-weight: bold;
}
.mobax-action-get:hover {
	color: #9dffad !important;
	text-decoration: underline;
}
.mobax-action-del:hover {
	color: #f85149;
}
.mobax-restrict {
	display: none;
	padding: 4px 8px;
	background: #2a1214;
	border-bottom: 1px solid #5a1d22;
	color: #ff4d4d;
	font-size: 11px;
	font-weight: bold;
	cursor: pointer;
	user-select: none;
}
.mobax-restrict:hover {
	background: #3a1619;
	text-decoration: underline;
}
.mobax-popup {
	display: none;
	position: absolute;
	left: 8px;
	right: 8px;
	top: 70px;
	z-index: 10;
	background: #161b22;
	border: 1px solid #ff4d4d;
	border-radius: 4px;
	box-shadow: 0 4px 12px rgba(0, 0, 0, 0.6);
	padding: 8px 10px;
	font-size: 11px;
	color: #c9d1d9;
	max-height: 260px;
	overflow-y: auto;
}
.mobax-popup-title {
	color: #ff4d4d;
	font-weight: bold;
	margin-bottom: 5px;
}
.mobax-popup-path {
	display: block;
	padding: 2px 4px;
	font-family: monospace;
	color: #58a6ff;
	cursor: pointer;
	border-radius: 2px;
}
.mobax-popup-path:hover {
	background: rgba(56, 139, 253, 0.2);
}
.mobax-popup-hint {
	margin-top: 5px;
	color: #8b949e;
	font-size: 10px;
}
.mobax-progress {
	height: 3px;
	width: 0%;
	background: #2ea043;
	transition: width 0.1s;
}
.mobax-status-bar {
	padding: 4px 8px;
	background: #161b22;
	border-top: 1px solid #282e35;
	font-size: 10px;
	color: #8b949e;
	display: flex;
	justify-content: space-between;
}
.mobax-dragover {
	border: 2px dashed #58a6ff !important;
	background: rgba(56, 139, 253, 0.15) !important;
}
</style>

<script>

//      <% nvram('restrict_wf'); %>        // http_id

var currentDir = '/tmp';
var sidebarVisible = true;

function getAllowedRoots() {
	var r = (nvram.restrict_wf || '').trim();
	if (!r) return [];
	return r.split(',').map(function(s) {
		var p = s.trim();
		while (p.length > 1 && p.endsWith('/')) {
			p = p.slice(0, -1);
		}
		return p;
	}).filter(function(p) { return p.length > 0; });
}

function isPathRestrictedRoot(dir) {
	var roots = getAllowedRoots();
	if (!roots.length) return false;
	for (var i = 0; i < roots.length; i++) {
		if (dir === roots[i]) return true;
	}
	return false;
}

/* Delegated click handling: paths live in data-path attributes (raw, HTML-escaped)
 * so they are never embedded in inline JS and are URL-encoded exactly once, by the callers. */
function onFmClick(ev) {
	var el = ev.target.closest ? ev.target.closest('[data-act]') : null;
	if (!el) return;

	var p = el.getAttribute('data-path');
	var a = el.getAttribute('data-act');

	if (a === 'open') loadDir(p);
	else if (a === 'download') downloadFile(p);
	else if (a === 'delete') deleteItem(p, el.getAttribute('data-dir') === '1');
}

function updateRestrictBanner() {
	var roots = getAllowedRoots();
	var banner = E('mobax-restrict');
	var popup = E('mobax-restrict-popup');
	if (!banner || !popup) return;

	if (!roots.length) {
		banner.style.display = 'none';
		popup.style.display = 'none';
		popup.innerHTML = '';
		return;
	}

	var h = '<div class="mobax-popup-title">Allowed paths</div>';
	for (var i = 0; i < roots.length; i++)
		h += '<span class="mobax-popup-path" data-act="open" data-path="' + escapeHTML(roots[i]) + '">' + escapeHTML(roots[i]) + '</span>';
	h += '<div class="mobax-popup-hint">File access is limited to these folders and their subfolders (restrict_wf). Click a path to open it.</div>';
	popup.innerHTML = h;
	banner.style.display = 'block';
}

function toggleRestrictPopup(ev) {
	if (ev) ev.stopPropagation();
	var popup = E('mobax-restrict-popup');
	popup.style.display = (popup.style.display === 'block') ? 'none' : 'block';
}

function closeRestrictPopup() {
	var popup = E('mobax-restrict-popup');
	if (popup) popup.style.display = 'none';
}

function loadIframe() {
	E('mobax-filelist-body').addEventListener('click', onFmClick);
	E('mobax-restrict-popup').addEventListener('click', function(ev) {
		onFmClick(ev);
		closeRestrictPopup();
	});
	document.addEventListener('click', function(ev) {
		if (!ev.target.closest || !ev.target.closest('#mobax-restrict-popup'))
			closeRestrictPopup();
	});
	document.addEventListener('keydown', function(ev) {
		if (ev.key === 'Escape') closeRestrictPopup();
	});
	updateRestrictBanner();
	E('mobax-filelist-body').addEventListener('dblclick', function(ev) {
		var el = ev.target.closest ? ev.target.closest('[data-dblact="download"]') : null;
		if (el) downloadFile(el.getAttribute('data-path'));
	});
	document.getElementById("terminal").src = location.protocol + '//' + location.hostname + ':' + location.port + '/console';

	var roots = getAllowedRoots();
	if (roots.length > 0) {
		currentDir = roots[0];
	} else {
		currentDir = '/tmp';
	}

	loadDir(currentDir);
}

function toggleSidebar() {
	sidebarVisible = !sidebarVisible;
	var sidebar = E('mobax-sidebar');
	var toggleBtn = E('mobax-toggle-icon');
	if (sidebarVisible) {
		sidebar.classList.remove('collapsed');
		toggleBtn.innerHTML = '&#9664;';
	} else {
		sidebar.classList.add('collapsed');
		toggleBtn.innerHTML = '&#9654;';
	}
}

function loadDir(dir) {
	if (!dir) dir = '/';
	/* Normalize trailing slash */
	if (dir.length > 1 && dir.endsWith('/')) {
		dir = dir.slice(0, -1);
	}
	currentDir = dir;
	E('mobax-path-input').value = currentDir;
	E('mobax-filelist-body').innerHTML = '<tr><td colspan="3" style="text-align:center;color:#888;padding:20px;">Loading...</td></tr>';

	var xhr = new XMLHttpRequest();
	var url = 'filexfer.cgi?action=list&dir=' + encodeURIComponent(currentDir) + '&_http_id=' + nvram.http_id;

	xhr.onload = function() {
		if (xhr.status == 200) {
			try {
				var res = JSON.parse(xhr.responseText);
				if (res.status === 'ok') {
					if (typeof res.restrict_wf !== 'undefined') {
						nvram.restrict_wf = res.restrict_wf;
						updateRestrictBanner();
					}
					renderFileList(res.entries || []);
				} else {
					E('mobax-filelist-body').innerHTML = '<tr><td colspan="3" style="color:#f85149;padding:10px;">' + escapeHTML(res.error || 'Error loading directory') + '</td></tr>';
				}
			} catch (e) {
				E('mobax-filelist-body').innerHTML = '<tr><td colspan="3" style="color:#f85149;padding:10px;">Invalid response</td></tr>';
			}
		} else {
			try {
				var res = JSON.parse(xhr.responseText);
				E('mobax-filelist-body').innerHTML = '<tr><td colspan="3" style="color:#f85149;padding:10px;">' + escapeHTML(res.error || 'Access denied') + '</td></tr>';
			} catch(e) {
				E('mobax-filelist-body').innerHTML = '<tr><td colspan="3" style="color:#f85149;padding:10px;">Access denied (' + xhr.status + ')</td></tr>';
			}
		}
	};

	xhr.onerror = function() {
		E('mobax-filelist-body').innerHTML = '<tr><td colspan="3" style="color:#f85149;padding:10px;">Network error</td></tr>';
	};

	xhr.open('GET', url, true);
	xhr.send();
}

function renderFileList(entries) {
	/* Sort: directories first, then alphabetical */
	entries.sort(function(a, b) {
		if (a.is_dir !== b.is_dir) return b.is_dir - a.is_dir;
		return a.name.localeCompare(b.name, undefined, { sensitivity: 'base' });
	});

	var html = '';

	/* Show parent directory row only if not at root and not at a restricted root boundary */
	if (currentDir !== '/' && !isPathRestrictedRoot(currentDir)) {
		html += '<tr class="mobax-row" onclick="navUp()">' +
		        '<td class="mobax-folder">&#128193; ..</td>' +
		        '<td style="color:#888;">[Up]</td>' +
		        '<td></td></tr>';
	}

	var fileCount = 0;
	var totalSize = 0;

	for (var i = 0; i < entries.length; i++) {
		var item = entries[i];
		var itemPath = (currentDir === '/' ? '' : currentDir) + '/' + item.name;

		if (item.is_dir) {
			html += '<tr class="mobax-row">' +
			        '<td class="mobax-folder" data-act="open" data-path="' + escapeHTML(itemPath) + '" title="' + escapeHTML(item.name) + '">&#128193; ' + escapeHTML(item.name) + '</td>' +
			        '<td style="color:#666;">&mdash;</td>' +
			        '<td class="mobax-actions">' +
			        '<a class="mobax-action-btn mobax-action-del" title="Delete folder" data-act="delete" data-dir="1" data-path="' + escapeHTML(itemPath) + '">&#10006;</a>' +
			        '</td></tr>';
		} else {
			fileCount++;
			totalSize += item.size;
			html += '<tr class="mobax-row">' +
			        '<td class="mobax-file" data-dblact="download" data-path="' + escapeHTML(itemPath) + '" title="Double-click to download: ' + escapeHTML(item.name) + '">&#128196; ' + escapeHTML(item.name) + '</td>' +
			        '<td style="color:#888;">' + scaleSize(item.size) + '</td>' +
			        '<td class="mobax-actions">' +
			        '<a class="mobax-action-btn mobax-action-get" title="Download" data-act="download" data-path="' + escapeHTML(itemPath) + '">&#8595; Get</a>' +
			        '<a class="mobax-action-btn mobax-action-del" title="Delete" data-act="delete" data-dir="0" data-path="' + escapeHTML(itemPath) + '">&#10006;</a>' +
			        '</td></tr>';
		}
	}

	if (entries.length === 0 && currentDir === '/') {
		html = '<tr><td colspan="3" style="text-align:center;color:#666;padding:20px;">Directory is empty</td></tr>';
	}

	E('mobax-filelist-body').innerHTML = html;
	var lockBadge = getAllowedRoots().length > 0 ? ' &bull; <span style="color:#f0ad4e;" title="Paths restricted by restrict_wf">&#128274; Restricted</span>' : '';
	E('mobax-status-text').innerHTML = fileCount + ' file(s) &bull; ' + scaleSize(totalSize) + lockBadge;
}

function navUp() {
	if (currentDir === '/' || isPathRestrictedRoot(currentDir)) return;
	var parts = currentDir.split('/');
	parts.pop();
	var parentDir = parts.join('/');
	if (!parentDir) parentDir = '/';
	loadDir(parentDir);
}

function refreshDir() {
	loadDir(currentDir);
}

function promptMkdir() {
	var name = prompt('Enter new folder name:');
	if (!name || !name.trim()) return;
	name = name.trim();

	var xhr = new XMLHttpRequest();
	var url = 'filexfer.cgi?action=mkdir&dir=' + encodeURIComponent(currentDir) + '&name=' + encodeURIComponent(name) + '&_http_id=' + nvram.http_id;

	xhr.onload = function() {
		try {
			var res = JSON.parse(xhr.responseText);
			if (res.status === 'ok') {
				loadDir(currentDir);
			} else {
				alert('Error: ' + (res.error || 'Failed to create folder'));
			}
		} catch (e) {
			alert('Server error creating folder');
		}
	};
	xhr.open('GET', url, true);
	xhr.send();
}

function deleteItem(path, isDir) {
	var msg = isDir ? 'Delete directory ' + path + '?' : 'Delete file ' + path + '?';
	if (!confirm(msg)) return;

	var xhr = new XMLHttpRequest();
	var url = 'filexfer.cgi?action=delete&path=' + encodeURIComponent(path) + '&_http_id=' + nvram.http_id;

	xhr.onload = function() {
		try {
			var res = JSON.parse(xhr.responseText);
			if (res.status === 'ok') {
				loadDir(currentDir);
			} else {
				alert('Error: ' + (res.error || 'Failed to delete'));
			}
		} catch (e) {
			alert('Server error deleting item');
		}
	};
	xhr.open('GET', url, true);
	xhr.send();
}

function downloadFile(path) {
	var url = 'filexfer.cgi?action=download&path=' + encodeURIComponent(path) + '&_http_id=' + nvram.http_id;
	var dlFrame = E('hidden_dl_frame');
	if (!dlFrame) {
		dlFrame = document.createElement('iframe');
		dlFrame.id = 'hidden_dl_frame';
		dlFrame.style.display = 'none';
		document.body.appendChild(dlFrame);
	}
	dlFrame.src = url;
}

function onFileSelect(input) {
	if (!input.files || !input.files.length) return;
	uploadFile(input.files[0]);
	input.value = '';
}

function uploadFile(file) {
	var progressBar = E('mobax-progress-bar');
	progressBar.style.width = '0%';

	var xhr = new XMLHttpRequest();
	var url = 'filexfer.cgi?action=upload&dir=' + encodeURIComponent(currentDir) + 
	          '&name=' + encodeURIComponent(file.name) + 
	          '&_http_id=' + nvram.http_id;

	xhr.upload.onprogress = function(e) {
		if (e.lengthComputable) {
			var pct = Math.round((e.loaded / e.total) * 100);
			progressBar.style.width = pct + '%';
		}
	};

	xhr.onload = function() {
		progressBar.style.width = '0%';
		try {
			var res = JSON.parse(xhr.responseText);
			if (xhr.status == 200 && res.status === 'ok') {
				loadDir(currentDir);
			} else {
				alert('Upload failed: ' + (res.error || xhr.responseText));
			}
		} catch (e) {
			alert('Server error during upload');
		}
	};

	xhr.onerror = function() {
		progressBar.style.width = '0%';
		alert('Network error during upload');
	};

	xhr.open('POST', url, true);
	xhr.setRequestHeader('Content-Type', 'application/octet-stream');
	xhr.send(file);
}

/* Drag and Drop Upload Support */
function onDragOver(e) {
	e.preventDefault();
	E('mobax-filelist-wrap').classList.add('mobax-dragover');
}

function onDragLeave(e) {
	e.preventDefault();
	E('mobax-filelist-wrap').classList.remove('mobax-dragover');
}

function onDrop(e) {
	e.preventDefault();
	E('mobax-filelist-wrap').classList.remove('mobax-dragover');
	if (e.dataTransfer && e.dataTransfer.files && e.dataTransfer.files.length) {
		uploadFile(e.dataTransfer.files[0]);
	}
}
</script>
</head>

<body onLoad="loadIframe()">
<table id="container">
<tr><td colspan="2" id="header">
	<div class="title"><a href="/">Tomato64</a></div>
	<div class="version">Version <% version(); %> on <% nv("t_model_name"); %><span class="blinking bl2"><script><% anonupdate(); %> anon_update()</script>&nbsp;</span></div>
</td></tr>
<tr id="body"><td id="navi"><script>navi()</script></td>
<td id="content">
<div id="ident"><% ident(); %> | <script>wikiLink();</script></div>

<div class="section-title">Execute System Commands &amp; File Manager</div>
<div class="section">
	<!-- Split Pane Container (MobaXterm style) -->
	<div class="mobax-wrap">
		<!-- Left Pane: Visual Directory Browser (SCP) -->
		<div id="mobax-sidebar" class="mobax-sidebar">
			<!-- Address / Navigation Bar -->
			<div class="mobax-bar">
				<button class="mobax-btn" onclick="navUp()" title="Go to parent directory">&#9650; Up</button>
				<input type="text" id="mobax-path-input" value="/tmp" onkeydown="if(event.key==='Enter')loadDir(this.value)">
				<button class="mobax-btn" onclick="refreshDir()" title="Refresh directory">&#8635;</button>
				<button class="mobax-btn" onclick="promptMkdir()" title="Create folder">+&#128193;</button>
				<label class="mobax-btn" title="Upload file to this folder" style="display:inline-block; margin-bottom:0;">
					&#8593; Upload
					<input type="file" id="mobax-file-input" style="display:none;" onchange="onFileSelect(this)">
				</label>
			</div>

			<!-- Upload Progress Bar -->
			<div id="mobax-progress-bar" class="mobax-progress"></div>

			<!-- Restricted paths banner (shown only when restrict_wf is set) -->
			<div id="mobax-restrict" class="mobax-restrict" onclick="toggleRestrictPopup(event)">&#128274; Restricted paths: click for list</div>
			<div id="mobax-restrict-popup" class="mobax-popup"></div>

			<!-- File List Table -->
			<div id="mobax-filelist-wrap" class="mobax-filelist-wrap" ondragover="onDragOver(event)" ondragleave="onDragLeave(event)" ondrop="onDrop(event)">
				<table class="mobax-table">
					<thead>
						<tr>
							<th>Name</th>
							<th style="width: 60px;">Size</th>
							<th class="mobax-actions">Act</th>
						</tr>
					</thead>
					<tbody id="mobax-filelist-body">
						<tr><td colspan="3" style="text-align:center;color:#888;padding:20px;">Loading...</td></tr>
					</tbody>
				</table>
			</div>

			<!-- Bottom Status Bar -->
			<div class="mobax-status-bar">
				<span id="mobax-status-text">0 files</span>
				<span><i>Drag &amp; drop to upload &bull; dbl-click file to download</i></span>
			</div>
		</div>

		<!-- Vertical Toggle Bar between Panes -->
		<div class="mobax-toggle" onclick="toggleSidebar()" title="Toggle SCP Browser Sidebar">
			<span id="mobax-toggle-icon">&#9664;</span>
		</div>

		<!-- Right Pane: Terminal Window -->
		<div class="mobax-main">
			<iframe id="terminal" title="Terminal"></iframe>
		</div>
	</div>

	<!-- Controls below terminal -->
	<div>
		<input type="button" value="Open in New Tab" class="new_window" onclick="window.open(location.protocol+'//'+location.hostname+':'+location.port+'/console')">
		&nbsp;
		<input type="button" value="Open Legacy System Commands" onclick="window.open(location.protocol+'//'+location.hostname+':'+location.port+'/tools-shell-legacy.asp','_self')">
	</div>
</div>

</td></tr>
</table>
<script>insOvl()</script>
</body>
</html>
