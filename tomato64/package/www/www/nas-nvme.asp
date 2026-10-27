<!DOCTYPE html>
<html lang="en-GB">
<head>
<meta http-equiv="content-type" content="text/html;charset=utf-8">
<meta name="robots" content="noindex,nofollow">
<title>[<% ident(); %>] NAS: NVMe Storage</title>
<link rel="stylesheet" type="text/css" href="tomato.css?rel=<% version(); %>">
<% css(); %>
<link rel="stylesheet" type="text/css" href="nvme.css?rel=<% version(); %>">
<script src="tomato.js?rel=<% version(); %>"></script>
<script src="nvme_format.js?rel=<% version(); %>"></script>
<script src="nvme_partition.js?rel=<% version(); %>"></script>
<script>
//	<% nvram("http_id,nvme_mounts"); %>
var cprefix = 'nas_nvme';
var nvme_disks, nvme_fs, nvme_can_partition;
<% nvme_disks(); %>
var nvmeGrids = [];
var nvmeUuid = {};
var saved_mounts = {};
var saved_by_uuid = {};
var present_uuids = {};
var mountPathRe = /^[A-Za-z0-9_.\-\/]+$/;
function mountSuffix(p) {
	var s = ('' + p).trim();
	if (s == '/opt') return 'opt';
	return s.replace(/^\/mnt\//, '');
}
(function() {
	for (var d in nvme_disks) {
		if (!nvme_disks.hasOwnProperty(d)) continue;
		var parts = nvme_disks[d].partitions || [];
		for (var j = 0; j < parts.length; ++j) {
			if (parts[j][4]) present_uuids[parts[j][4]] = 1;
		}
	}
	var list = nvram.nvme_mounts ? nvram.nvme_mounts.split('<') : [];
	for (var i = 0; i < list.length; ++i) {
		var t = list[i].split('>');
		if (t[0] == '') continue;
		var e = {
			path: mountSuffix(t[1] || ''),
			boot: (t[2] == '1') ? 1 : 0,
			uuid: t[3] || ''
		};
		saved_mounts[t[0].replace(/^\/dev\//, '')] = e;
		if (e.uuid != '') saved_by_uuid[e.uuid] = e;
	}
	try {
		var rawDraft = sessionStorage.getItem('nas_nvme_draft');
		if (rawDraft) {
			sessionStorage.removeItem('nas_nvme_draft');
			var draft = JSON.parse(rawDraft);
			for (var k in draft) {
				if (!draft.hasOwnProperty(k)) continue;
				var prev = saved_mounts[k];
				var de = {
					path: mountSuffix(draft[k].path || ''),
					boot: draft[k].boot ? 1 : 0,
					uuid: prev ? (prev.uuid || '') : ''
				};
				saved_mounts[k] = de;
				if (de.uuid != '') saved_by_uuid[de.uuid] = de;
			}
		}
	} catch (ex) {}
})();

function nvmeSaveDraft(excludeDev) {
	try {
		var draft = {};
		for (var i = 0; i < nvmeGrids.length; ++i) {
			var g = nvmeGrids[i];
			var editDev = (g.isEditing() && g.source) ? ('' + g.source.getRowData()[0]) : '';
			var rows = g.getAllData();
			for (var j = 0; j < rows.length; ++j) {
				if (rows[j][6] == 1) continue;
				var dev = '' + (rows[j][0] || '');
				var path = rows[j][3] || '';
				var boot = (rows[j][4] == 1) ? 1 : 0;
				if (dev == editDev && g.editor) {
					var f = fields.getAll(g.editor);
					if (f && f[0] && g.verifyFields(g.editor, true)) {
						path = f[0].value.trim();
						boot = (f[1] && f[1].checked) ? 1 : 0;
					}
				}
				if (excludeDev && dev == excludeDev) continue;
				draft[dev.replace(/^\/dev\//, '')] = {
					path: path,
					boot: boot
				};
			}
		}
		sessionStorage.setItem('nas_nvme_draft', JSON.stringify(draft));
	} catch (ex) {}
}

function nvmeClearDraft() {
	try {
		sessionStorage.removeItem('nas_nvme_draft');
	} catch (ex) {}
}

function verifyFields(focused, quiet) {
	return 1;
}

function nvmeSetupGrid(g, grid_id, partitions) {
	g.init(grid_id, '', 50, [
		{
			type: 'display'
		},
		{
			type: 'display'
		},
		{
			type: 'display'
		},
		{
			type: 'text',
			maxlen: 20,
			edit: '1',
			prefix: '<span class="grid-prefix">/mnt/<\/span>'
		},
		{
			type: 'checkbox'
		}
]);
	g.canDelete = false;
	g.headerSet(['Partition', 'Labels ( P / V )', 'Size / FS Type', 'Mount Path', 'Auto Mount']);
	g.createControls = function(which, rowIndex) {
		var controls = TomatoGrid.prototype.createControls.call(this, which, rowIndex);
		if (which == 'edit') {
			var cell = controls.querySelector('td:last-child') || controls.lastElementChild;
			if (cell && !cell.querySelector('.btn-formatting')) {
				var dev = this.source ? this.source.getRowData()[0] : '';
				var d = this.source ? this.source.getRowData() : null;
				var is_foreign = (d && d[6] == 1);
				var nativeDel = cell.querySelector('input[value="Delete"]');
				if (nativeDel) nativeDel.style.display = 'none';
				var bFmt = document.createElement('input');
				bFmt.type = 'button';
				bFmt.value = 'Format\nPartition';
				bFmt.className = 'btn btn-danger btn-formatting btn-action-left';
				if (is_foreign) {
					bFmt.disabled = true;
					bFmt.title = 'Partition is mounted by the system (' + (d[5] || '') + ')';
				}
				else if (nvme_fs && nvme_fs.length) bFmt.onclick = function() {
					triggerFormat(dev);
				};
				else {
					bFmt.disabled = true;
					bFmt.title = 'No mkfs tools installed';
				}
				// Hardware Delete Button
				var bDel = document.createElement('input');
				bDel.type = 'button';
				bDel.value = 'Delete\nPartition';
				bDel.className = 'btn btn-danger btn-formatting btn-action-left';
				if (is_foreign) {
					bDel.disabled = true;
					bDel.title = 'Partition is mounted by the system (' + (d[5] || '') + ')';
				}
				else if (nvme_can_partition) bDel.onclick = function() {
					triggerDelete(dev);
				};
				else {
					bDel.disabled = true;
					bDel.title = 'sgdisk is not installed';
				}
				cell.insertBefore(bFmt, cell.firstChild);
				cell.insertBefore(bDel, bFmt);
			}
		}
		return controls;
	};
	g.verifyFields = function(row, quiet) {
		var o = this.source ? this.source.getRowData() : null;
		if (o && o[6] == 1) return 1;
		var f = fields.getAll(row);
		if (!f || f.length < 2) return 1;
		var boot = f[1].checked;
		var s = f[0].value.trim();
		if (!quiet) f[0].value = s;
		var pfx = f[0].parentNode ? f[0].parentNode.querySelector('.grid-prefix') : null;
		if (pfx) pfx.textContent = (s == 'opt') ? '/' : '/mnt/';
		if (!boot) {
			ferror.clear(f[0]);
			return 1;
		}
		if (s == '') {
			ferror.set(f[0], 'Mount path is required when Auto Mount is enabled.', quiet);
			return 0;
		}
		if (s.length > 20 || !mountPathRe.test(s)) {
			ferror.set(f[0], 'Invalid mount path. Only the characters "A-Z a-z 0-9 _ . - /" are allowed (maximum 20 characters).', quiet);
			return 0;
		}
		if (s.charAt(0) == '/' || s.charAt(s.length - 1) == '/') {
			ferror.set(f[0], 'Please enter a folder suffix without leading or trailing slash.', quiet);
			return 0;
		}
		if (s.indexOf('..') != -1 || s.indexOf('//') != -1 || /(^|\/)\.(\/|$)/.test(s) || s.indexOf('opt/') == 0) {
			ferror.set(f[0], 'Invalid mount path.', quiet);
			return 0;
		}
		for (var i = 0; i < nvmeGrids.length; ++i) {
			var gOther = nvmeGrids[i];
			var d = gOther.getAllData();
			for (var j = 0; j < d.length; ++j) {
				if (d[j][6] == 1) continue;
				if (d[j][3] == s) {
					ferror.set(f[0], 'Duplicate mount path: ' + ((s == 'opt') ? '/opt' : '/mnt/' + s), quiet);
					return 0;
				}
			}
			if (gOther !== this && gOther.isEditing()) {
				var ef = gOther.editor ? fields.getAll(gOther.editor) : null;
				var es = (ef && ef[0]) ? ef[0].value.trim() : (gOther.source ? (gOther.source.getRowData()[3] || '') : '');
				if (es != '' && es == s) {
					ferror.set(f[0], 'Duplicate mount path: ' + ((s == 'opt') ? '/opt' : '/mnt/' + s), quiet);
					return 0;
				}
			}
		}
		ferror.clear(f[0]);
		return 1;
	};
	g.edit = function(row) {
		for (var i = 0; i < nvmeGrids.length; ++i) {
			if (nvmeGrids[i].isEditing()) return;
		}
		TomatoGrid.prototype.edit.call(this, row);
		if (this.editor) {
			var d = this.source ? this.source.getRowData() : null;
			var f = fields.getAll(this.editor);
			if (d && d[6] == 1) {
				if (f && f[0]) {
					f[0].value = d[5] || d[3] || '';
					f[0].disabled = true;
					f[0].style.color = '#888';
					f[0].style.backgroundColor = '#e9ecef';
					f[0].style.cursor = 'not-allowed';
					var pfx = f[0].parentNode ? f[0].parentNode.querySelector('.grid-prefix') : null;
					if (pfx) pfx.style.display = 'none';
				}
				if (f && f[1]) {
					f[1].checked = false;
					f[1].disabled = true;
					f[1].style.cursor = 'not-allowed';
				}
			} else if (f && f[0] && f[1]) {
				var pfx = f[0].parentNode ? f[0].parentNode.querySelector('.grid-prefix') : null;
				if (pfx) {
					pfx.style.display = '';
					pfx.textContent = (f[0].value.trim() == 'opt') ? '/' : '/mnt/';
				}
				f[0].oninput = function() {
					if (pfx) pfx.textContent = (f[0].value.trim() == 'opt') ? '/' : '/mnt/';
				};
				var updateMountState = function() {
					if (f[1].checked) {
						f[0].disabled = false;
						f[0].style.color = '';
						f[0].style.backgroundColor = '';
						f[0].style.cursor = '';
						f[0].focus();
					} else {
						f[0].disabled = true;
						f[0].value = '';
						f[0].style.color = '#888';
						f[0].style.backgroundColor = '#e9ecef';
						f[0].style.cursor = 'not-allowed';
						ferror.clear(f[0]);
						if (pfx) pfx.textContent = '/mnt/';
					}
				};
				f[1].onclick = updateMountState;
				f[1].onchange = updateMountState;

				if (!f[1].checked) {
					f[0].disabled = true;
					f[0].value = '';
					f[0].style.color = '#888';
					f[0].style.backgroundColor = '#e9ecef';
					f[0].style.cursor = 'not-allowed';
					f[1].focus();
				} else {
					f[0].disabled = false;
					f[0].style.color = '';
					f[0].style.backgroundColor = '';
					f[0].style.cursor = '';
				}
			}
		}
	};
	g.dataToFieldValues = function(data) {
		return [data[0], data[1], data[2], data[3], (data[4] == 1) ? 'checked' : ''];
	};
	g.fieldValuesToData = function(row) {
		var o = this.source ? this.source.getRowData() : [];
		if (o && o[6] == 1) {
			return [o[0] || '', o[1] || '', o[2] || '', o[3] || '', 0, o[5] || '', 1];
		}
		var f = fields.getAll(row);
		var b = (f[1] && f[1].checked) ? 1 : 0;
		var p = (b && f[0]) ? f[0].value.trim() : '';
		return [o[0] || '', o[1] || '', o[2] || '', p, (p != '' ? b : 0), o[5] || '', 0];
	};
	g.dataToView = function(data) {
		var p = data[3] || '';
		var is_foreign = (data[6] == 1);
		var mpText;
		if (is_foreign) {
			mpText = '<span class="text-muted" style="color:#888;">' + escapeHTML(data[5] || p) + ' <small><i>(system)<\/i><\/small><\/span>';
		} else if (p != '') {
			mpText = (p == 'opt') ? '/opt' : '/mnt/' + escapeHTML(p);
		} else {
			mpText = '';
		}
		var bootText;
		if (is_foreign) {
			bootText = '<span class="text-muted" style="color:#888;">Disabled<\/span>';
		} else {
			bootText = (data[4] == 1) ? '<b>Enabled<\/b>' : 'Disabled';
		}
		return [
			escapeHTML('' + (data[0] || '')),
			escapeHTML('' + (data[1] || 'none / none')),
			escapeHTML('' + (data[2] || '')),
			mpText,
			bootText
		];
	};
	g.recolor = function() {};
	for (var k = 0; k < partitions.length; ++k) {
		var part = partitions[k];
		var dev_node = '/dev/' + part[0];
		var uuid = part[4] || '';
		var fstype = part[3] || 'Unknown';
		var active_mp = part[6] || '';
		nvmeUuid[dev_node] = uuid;
		var byDev = saved_mounts[part[0]];
		var saved = ((uuid != '') ? saved_by_uuid[uuid] : null) || ((byDev && (!byDev.uuid || !present_uuids[byDev.uuid])) ? byDev : null) || {
			path: '',
			boot: 0
		};
		var expected_mp = (saved.path != '') ? ((saved.path == 'opt') ? '/opt' : ('/mnt/' + saved.path)) : '';
		var is_our_mount = (active_mp != '' && saved.boot == 1 && (active_mp == expected_mp || (saved.path == 'opt' && active_mp == '/mnt/opt')));
		var is_foreign = (active_mp != '' && !is_our_mount) ? 1 : 0;
		g.insertData(-1, [
			dev_node,
			((part[5] && part[5] != '') ? part[5] : 'none') + ' / ' + (part[1] != '' ? part[1] : 'none'),
			scaleSize((part[2] || 0) * 1024).replace(/<[^>]*>/g, '') + ' / ' + fstype.toUpperCase(),
			is_foreign ? active_mp : saved.path,
			is_foreign ? 0 : (saved.boot ? 1 : 0),
			active_mp,
			is_foreign
		]);
	}
	delete g.recolor;
	g.recolor();
}

function nvmeAddDisk(root, disk_name, info) {
	var partitions = info.partitions || [];
	var allocated = 0;
	for (var j = 0; j < partitions.length; ++j) allocated += partitions[j][2] || 0;
	var total = Math.max(info.blocks || 0, allocated);
	var free = total - allocated;
	var title = document.createElement('div');
	title.className = 'section-title';
	title.innerHTML = 'Disk Drive: /dev/' + escapeHTML(disk_name) + ' ';
	var btn = document.createElement('input');
	btn.type = 'button';
	btn.className = 'btn disk-header-btn';
	btn.value = 'Add Partition';
	var tMB = Math.floor(total / 1024),
		aMB = Math.floor(allocated / 1024),
		fMB = Math.floor(free / 1024);
	if (nvme_can_partition && nvme_fs && nvme_fs.length && partitions.length < 16) btn.onclick = function() {
		triggerPartition(disk_name, tMB, aMB, fMB);
	};
	else {
		btn.disabled = true;
		btn.title = !nvme_can_partition ? 'sgdisk is not installed' : (partitions.length >= 16 ? 'Maximum of 16 partitions reached' : 'No mkfs tools installed');
	}
	title.appendChild(btn);
	var sub = document.createElement('div');
	sub.className = 'disk-info-subtitle';
	sub.innerHTML = 'Space: Total - ' + scaleSize(total * 1024) + ' / Allocated - ' + scaleSize(allocated * 1024) + ' / Free - ' + scaleSize(free * 1024);
	title.appendChild(sub);
	var section = document.createElement('div');
	section.className = 'section';
	var gd = document.createElement('div');
	gd.className = 'tomato-grid';
	gd.id = 'grid-' + disk_name;
	section.appendChild(gd);
	var frag = document.createDocumentFragment();
	frag.appendChild(title);
	frag.appendChild(section);
	root.appendChild(frag);
	var g = new TomatoGrid();
	nvmeSetupGrid(g, gd.id, partitions);
	nvmeGrids.push(g);
}

function save_pre() {
	for (var i = 0; i < nvmeGrids.length; ++i) {
		if (nvmeGrids[i].isEditing()) {
			nvmeGrids[i].onOK();
			if (nvmeGrids[i].isEditing()) return 0;
		}
	}
	return 1;
}

function save(nomsg) {
	if (!save_pre()) return;
	var fom = E('t_fom');
	var r = [];
	var seen = {};
	for (var i = 0; i < nvmeGrids.length; ++i) {
		var g = nvmeGrids[i];
		var data = g.getAllData();
		for (var j = 0; j < data.length; ++j) {
			if (data[j][6] == 1) continue;
			var dev = ('' + data[j][0]).trim();
			var sfx = data[j][3] || '';
			var boot = (data[j][4] == 1) ? 1 : 0;
			if (!boot || sfx == '') {
				data[j][3] = '';
				data[j][4] = 0;
				continue;
			}
			if (dev == '') continue;
			if (seen[sfx]) {
				alert('Duplicate mount path: ' + ((sfx == 'opt') ? '/opt' : '/mnt/' + sfx));
				return;
			}
			seen[sfx] = 1;
			var mp = (sfx == 'opt') ? '/opt' : '/mnt/' + sfx;
			r.push(dev + '>' + mp + '>1>' + (nvmeUuid[dev] || '') + '<');
		}

		var rows = g.tb ? g.tb.rows : [];
		for (var k = 0; k < rows.length; ++k) {
			var row = rows[k];
			if (row._data && row._data[6] == 1) continue;
			if (row._data && (row._data[4] == 0 || !row._data[3])) {
				row._data[3] = '';
				row._data[4] = 0;
				var view = g.dataToView(row._data);
				for (var c = 0; c < row.cells.length; ++c) {
					row.cells[c].innerHTML = view[c];
				}
			}
		}
	}
	if (r.length > 16) {
		alert('A maximum of 16 mounts is supported.');
		return;
	}
	fom.nvme_mounts.value = r.join('');
	if (fom.nvme_mounts.value.length > 2048) {
		alert('The mount list is too long.');
		return;
	}
	fom._nofootermsg.value = (nomsg ? 1 : 0);
	form.submit(fom, 1);
}

function earlyInit() {
	var root = E('dynamic-grids-root');
	var count = 0;
	var names = {
		ext4: 'EXT4 (Recommended)',
		ext3: 'EXT3',
		btrfs: 'Btrfs',
		vfat: 'FAT32 (vfat)',
		ntfs: 'NTFS'
	};
	var sel = E('modal-fs-select');
	var partSel = E('modal-part-fs-select');
	var fs_list = nvme_fs || [];
	if (fs_list.length > 0) {
		for (var i = 0; i < fs_list.length; ++i) {
			var optVal = fs_list[i];
			var optText = names[optVal] || optVal;
			if (sel) sel.appendChild(new Option(optText, optVal));
			if (partSel) partSel.appendChild(new Option(optText, optVal));
		}
	}
	for (var disk_name in nvme_disks) {
		if (!nvme_disks.hasOwnProperty(disk_name)) continue;
		nvmeAddDisk(root, disk_name, nvme_disks[disk_name]);
		++count;
	}
	if (count == 0) root.innerHTML = '<div class="section"><i>No NVMe drives detected.<\/i><\/div>';
	insOvl();
}

function init() {
	restoreVisibility(cprefix, 'notes');
}
</script>
</head>
<body onload="init()">
<form id="t_fom" method="post" action="tomato.cgi">
<input type="hidden" name="_nextpage" value="nas-nvme.asp">
<input type="hidden" name="_service" value="nvme-start">
<input type="hidden" name="nvme_mounts" value="">
<input type="hidden" name="_nofootermsg">
<table id="container">
<tr><td colspan="2" id="header">
<div class="title"><a href="/">Tomato64</a></div>
<div class="version">Version <% version(); %> on <% nv("t_model_name"); %><span class="blinking bl2"><script><% anonupdate(); %> anon_update()</script>&nbsp;</span></div>
</td></tr>
<tr id="body"><td id="navi"><script>navi();</script></td>
<td id="content">
<div id="ident"><% ident(); %> | <script>wikiLink();</script></div>
<div id="dynamic-grids-root"></div>
<script>writeToggleSectionTitle('Notes', 'notes');</script>
<div class="section" id="sesdiv_notes" style="display:none">
<ul>
<li><b>Mount Path</b> - Folder created under <i>/mnt</i>. Use letters, digits, <i>_ . -</i> and <i>/</i> only; each path must be unique. Entering: &nbsp;<i><b>opt</b></i>&nbsp; :as the mount path will mount the partition to <i>/opt</i> ( or bind-mount it from <i>/mnt/opt</i> to <i>/opt</i> if the root filesystem is read-only ). Auto Mount must be enabled first to edit the Mount Path field.</li>
<br>
<li><b>Auto Mount</b> - Mount the partition when the router starts or when settings are saved. Enabling Auto Mount unlocks the Mount Path field. Disabling Auto Mount ( or saving an entry without a Mount Path ) will unmount the partition and clean up its mount directory under <i>/mnt</i>.</li>
<br>
<li><b>System / External Mounts</b> - If a partition is currently mounted by other means ( system, scripts etc ) rather than by this GUI, you will not be able to make any changes to it here. Disable any auto mounting for the drive, unmount it, then reload this page to access the options.</li>
<br>
<li><b>Add Partition</b> - Creates a new GPT partition in the free space of the drive and formats it with the selected filesystem.</li>
<br>
<li><b>Delete/Format</b> - Available when editing a row. <b>All data on the partition is permanently erased. Once you click ok in the popup warning, there is no going back. CANCEL BUTTONS OUTSIDE THE POPUP WILL NOT REVERT THE CHANGE.</b></li>
</ul>
</div>
<script>writeFooter();</script>
</td></tr>
</table>
</form>
<!-- Modal Overlay for Partition Formatting -->
<div id="format-modal-overlay">
<div id="format-modal-box">
<div class="section-title">Format NVMe Partition</div>
<div class="section">
<div class="field-row">
<label>Target:</label>
<span id="modal-target-dev" class="modal-code-text"></span>
</div>
<div class="field-row">
<label>Filesystem:</label>
<select id="modal-fs-select"></select>
</div>
<div class="field-row">
<label>Volume Label:</label>
<input type="text" id="modal-label-input" maxlength="16" placeholder="Optional label">
</div>
<div class="buttons">
<input type="button" class="btn btn-danger" value="Format Partition" onclick="confirmFormat()">
<input type="button" class="btn" value="Cancel" onclick="closeFormatModal()">
</div>
</div>
</div>
</div>
<!-- Modal Overlay for Partition Creation -->
<div id="partition-modal-overlay">
<div id="partition-modal-box">
<div class="section-title">Create New NVMe Partition</div>
<div class="section">
<div class="field-row">
<label>Target Drive:</label>
<span id="modal-part-target-disk" class="modal-code-text"></span>
</div>
<div class="field-row">
<label>Space Status:</label>
<span id="modal-part-space-info" class="modal-space-info"></span>
</div>
<div class="field-row">
<label for="modal-part-size-input">Size (MB):</label>
<input type="text" id="modal-part-size-input" maxlength="10" placeholder="e.g. 10240 for 10GB">
</div>
<div class="field-row">
<label for="modal-part-fs-select">Filesystem:</label>
<select id="modal-part-fs-select"></select>
</div>
<div class="field-row">
<label for="modal-part-label-input">Volume Label:</label>
<input type="text" id="modal-part-label-input" maxlength="16" placeholder="Optional label">
</div>
<div class="buttons">
<input type="button" class="btn btn-danger" value="Create Partition" onclick="confirmPartition()">
<input type="button" class="btn" value="Cancel" onclick="closePartitionModal()">
</div>
</div>
</div>
</div>
<!-- Modal Overlay for Partition Deletion -->
<div id="delete-modal-overlay">
<div id="delete-modal-box">
<div class="section-title">Delete NVMe Partition</div>
<div class="section">
<div class="field-row">
<label>Target:</label>
<span id="modal-delete-target" class="modal-code-text"></span>
</div>
<div style="margin-bottom: 12px; color: #cc0000; font-weight: bold;">
WARNING: This will permanently delete the partition and ALL data on it. This action cannot be undone.
</div>
<div class="field-row" style="margin-bottom: 8px;">
<input type="checkbox" id="modal-delete-check" onclick="verifyDeleteInput()" style="margin-right: 6px;">
<label for="modal-delete-check" style="width: auto; font-weight: normal;">I understand that data will be destroyed.</label>
</div>
<div class="field-row">
<label style="width: auto; margin-right: 8px;">Type 'DELETE' to confirm:</label>
<input type="text" id="modal-delete-input" maxlength="6" oninput="verifyDeleteInput()" style="width: 80px;" autocomplete="off">
</div>
<div class="buttons">
<input type="button" id="btn-modal-delete" class="btn btn-danger" value="Delete Partition" onclick="confirmDelete()" disabled>
<input type="button" class="btn" value="Cancel" onclick="closeDeleteModal()">
</div>
</div>
</div>
</div>
<script>earlyInit();</script>
</body>
</html>
