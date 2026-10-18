<!DOCTYPE html>
<html lang="en-GB">
<head>
<meta http-equiv="content-type" content="text/html;charset=utf-8">
<meta name="robots" content="noindex,nofollow">
<title>[<% ident(); %>] NAS: NVMe Storage</title>
<link rel="stylesheet" type="text/css" href="tomato.css?rel=<% version(); %>">
<% css(); %>
<script src="isup.jsx?_http_id=<% nv(http_id); %>"></script>
<script src="isup.js?rel=<% version(); %>"></script>
<script src="tomato.js?rel=<% version(); %>"></script>
<script>

//	<% nvram("http_id,nvme_mounts,nvme_disks"); %>

var nvme_mounts_data = nvram['nvme_mounts'] ? nvram['nvme_mounts'].split('<') : [];
var nvme_disks = nvram['nvme_disks'] || '';

var disk_grids = [];
var saved_mounts = {};

for (var i = 0; i < nvme_mounts_data.length; ++i) {
    var t = nvme_mounts_data[i].split('>');
    if (t.length > 0 && t[0] !== "") {
        var clean_key = t[0].replace(/^\/dev\//, '');
        saved_mounts[clean_key] = { 
            path: t[1] || "", 
            boot: (t.length >= 3 && !isNaN(parseInt(t[2], 10))) ? parseInt(t[2], 10) : 0 
        };
    }
}

function verifyFields(focused, quiet) {
    return 1;
}

function initGrids() {
    var grid_container = E('dynamic-grids-root');
    grid_container.innerHTML = '';
    var disk_count = 0;

    for (var disk_name in nvme_disks) {
        if (!nvme_disks.hasOwnProperty(disk_name)) continue;
        disk_count++;

        var partitions = nvme_disks[disk_name];

        var wrapper = document.createElement('div');
        wrapper.className = 'section-title';
        wrapper.innerHTML = 'Disk Drive: /dev/' + disk_name;
        grid_container.appendChild(wrapper);

        var section = document.createElement('div');
        section.className = 'section';
        var grid_div = document.createElement('div');
        grid_div.className = 'tomato-grid';
        var grid_id = 'grid-' + disk_name;
        grid_div.id = grid_id;
        section.appendChild(grid_div);
        grid_container.appendChild(section);

        var g = new TomatoGrid();
        g.init(grid_id, '', 50, [
            { type: 'display' },
            { type: 'display' },
            { type: 'text', maxlen: 20, edit: '1', prefix: '<span class="grid-prefix" style="opacity:0.7; padding-right:2px;">/mnt/</span>' },
            { type: 'checkbox' }
        ]);

        g.headerSet(['Partition', 'Size / Filesystem', 'Mount Path', 'Mount at Boot']);

        g.verifyFields = function(row, quiet) {
            var f = fields.getAll(row);
            if (!f || f.length < 1) return 1;

            var pathInput = f[0];
            var suffix = pathInput.value.trim();

            if (suffix === '') return 1; // Allow empty mount paths

            if (!v_nodelim(pathInput, quiet, 'Mount Suffix', 1)) return 0;

            if (suffix.charAt(0) === '/' || suffix.charAt(suffix.length - 1) === '/') {
                ferror.set(pathInput, 'Please enter a folder suffix without trailing or leading slashes (e.g., storage1)', quiet);
                return 0;
            }
            ferror.clear(pathInput);
            return 1;
        };
		
		g.fieldValues = function(row) {
            var f = fields.getAll(row);
            // Retrieve the exact partition name stored safely in row metadata
            var partitionName = row._nvme_partition || "";
            var sizeText = row._nvme_size || "";
            var pathValue = f[0] ? f[0].value.trim() : '';
            var bootValue = f[1] ? (f[1].checked ? 1 : 0) : 0;

            return [partitionName, sizeText, pathValue, bootValue];
        };

        g.dataToView = function(data) {
            var display_path = (data && data[2]) ? String(data[2]).trim() : '';
            if (display_path !== '') {
                if (display_path.substr(0, 5) !== '/mnt/') {
                    display_path = '/mnt/' + display_path;
                }
            }

            var visual_size = ('' + (data[1] || '')).replace(/(KB|MB|GB|TB)/i, "<small>$1</small>");

            return [
                escapeHTML('' + (data[0] || '')), 
                visual_size, 
                escapeHTML('' + display_path), 
                (data[3] == '1' || data[3] === true) ? '<strong>Enabled</strong>' : '<span style="opacity: 0.6;">Disabled</span>' 
            ];
        };

        g.fieldValuesToData = function(row) {
            var f = fields.getAll(row);
            var origData = this.source ? this.source.getRowData() : ['', ''];
            
            var partitionName = origData[0] || '';
            var sizeText = origData[1] || '';
            var pathValue = f[0] ? f[0].value.trim() : '';
            var bootValue = f[1] ? (f[1].checked ? 1 : 0) : 0;

            return [partitionName, sizeText, pathValue, bootValue];
        };

        for (var j = 0; j < partitions.length; j++) {
            var p_name = partitions[j][0];
            var p_blocks = partitions[j][1];
            var p_fs = (partitions[j][2] || 'Unknown').toUpperCase();
            var dev_path = '/dev/' + p_name;
            
            var raw_size = (scaleSize(p_blocks * 1024) + ' / ' + p_fs).replace(/<\/?\s*[^>]+(>|$)/g, "");

            var initial_path = '';
            var initial_boot = 0;

            if (saved_mounts[p_name]) {
                initial_path = saved_mounts[p_name].path;
                initial_boot = saved_mounts[p_name].boot;
            } else if (saved_mounts[dev_path]) {
                initial_path = saved_mounts[dev_path].path;
                initial_boot = saved_mounts[dev_path].boot;
            }

            if (initial_path.substr(0, 5) === '/mnt/') {
                initial_path = initial_path.substr(5);
            }

            // insertData directly returns the row tr element safely
            g.insertData(-1, [dev_path, raw_size, initial_path, initial_boot]);
        }
        disk_grids.push(g);
    }

    if (disk_count === 0) {
        grid_container.innerHTML = '<div class="section-title">No NVMe Drives Detected</div><div class="section"><small>The system kernel could not identify any storage device configurations at this time.</small></div>';
    }
}

function save() {
    var cfg = "";

    for (var i = 0; i < disk_grids.length; i++) {
        var g = disk_grids[i];
        if (g.isEditing()) return;
        
        if (g.newEditor) {
            g.resetNewEditor();
        }

        var data = g.getAllData();
        for (var j = 0; j < data.length; j++) {
            var row = data[j];
            var path_val = (row[2] || "").trim();

            if (path_val !== "") {
                if (path_val.indexOf('<') !== -1 || path_val.indexOf('>') !== -1) {
                    alert("Error: Mount paths cannot contain delimiter tags '<' or '>'");
                    return;
                }

                var full_absolute_path = (path_val.substr(0, 5) === '/mnt/') ? path_val : ('/mnt/' + path_val);
                cfg += row[0] + ">" + full_absolute_path + ">" + (row[3] ? "1" : "0") + "<";
            }
        }
    }

    var fom = E('t_form');
    form.addId(fom); // Attaches the secure CSRF _http_id token
    
    // Assign the serialized string to the hidden form field
    fom.nvme_mounts.value = cfg;
    
    // Set the service target
    fom._service.value = 'nvme-restart';
    
    // Submits via standard Tomato form handling (triggers backend save & service restart)
    form.submit(fom, 1);
}

function init() {
    initGrids();
}
</script>
</head>
<body onload='init();'>
<form id='t_form' method='post' action='tomato.cgi'>
<input type='hidden' name='nvme_mounts' value=''>
<input type='hidden' name='_service' value='nvme-restart'>
<table id="container">
<tr><td colspan="2" id="header">
	<div class="title"><a href="/">Tomato64</a></div>
	<div class="version">Version <% version(); %> on <% nv("t_model_name"); %><span class="blinking bl2"><script><% anonupdate(); %> anon_update()</script>&nbsp;</span></div>
</td></tr>
<tr id="body"><td id="navi"><script>navi()</script></td>
<td id="content">
<div id="ident"><% ident(); %> | <script>wikiLink();</script></div>
<div id="dynamic-grids-root"></div>
<script>writeFooter();</script>
</td></tr>
</table>
</form>
<script>insOvl()</script>
</body>
</html>
