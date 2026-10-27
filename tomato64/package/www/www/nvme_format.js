var currentFormatDev = '';
var nvmeThemeBg = null;

function nvmeApplyModalTheme(modalBox) {
	if (!modalBox) return;
	if (nvmeThemeBg === null) {
		nvmeThemeBg = '';
		var sampleEl = document.querySelector('.tomato-grid td') || document.querySelector('input') || document.querySelector('.section');
		if (sampleEl) {
			var computedBg = window.getComputedStyle(sampleEl).backgroundColor;
			if (computedBg && computedBg !== 'rgba(0, 0, 0, 0)' && computedBg !== 'transparent')
				nvmeThemeBg = computedBg;
		}
	}
	if (nvmeThemeBg)
		modalBox.style.backgroundColor = nvmeThemeBg;
}

function triggerFormat(dev_path) {
	currentFormatDev = dev_path;
	E('modal-target-dev').innerText = dev_path;
	var sel = E('modal-fs-select');
	sel.value = 'ext4';
	if (sel.selectedIndex < 0 && sel.options.length > 0)
		sel.selectedIndex = 0;
	E('modal-label-input').value = '';

	nvmeApplyModalTheme(E('format-modal-box'));

	E('format-modal-overlay').style.display = 'flex';
}

function closeFormatModal() {
	E('format-modal-overlay').style.display = 'none';
	currentFormatDev = '';
}

function confirmFormat() {
	var fs_choice = E('modal-fs-select').value;
	var drive_label = E('modal-label-input').value.trim();
	var dev = currentFormatDev;

	if (drive_label !== '' && !/^[A-Za-z0-9_.\-]{1,16}$/.test(drive_label)) {
		alert('Invalid volume label. Use up to 16 characters of A-Z a-z 0-9 _ . -');
		return;
	}

	if (fs_choice === 'vfat' && drive_label.length > 11) {
		alert('A FAT volume label is limited to 11 characters.');
		return;
	}

	if (confirm('FINAL WARNING: Proceed with formatting ' + dev + ' using ' + fs_choice.toUpperCase() + '? All existing data will be permanently lost.')) {
		if (typeof nvmeSaveDraft === 'function') nvmeSaveDraft();
		closeFormatModal();
		nvmeRun('Formatting ' + dev + '...', 'cmd=format&dev=' + escapeCGI(dev) + '&fs=' + escapeCGI(fs_choice) + '&label=' + escapeCGI(drive_label));
	}
}

var nvmeXob = null;

function nvmeMsg(text) {
	var m = E('footer-msg');
	if (!m) return;
	m.innerHTML = escapeHTML(text);
	m.style.display = 'inline';
}

function nvmePoll() {
	if (nvmeXob) return;
	nvmeXob = new XmlHttp();
	nvmeXob.onCompleted = function(text, xml) {
		var nvme = null;
		nvmeXob = null;
		try { eval(text); } catch (ex) { nvme = null; }
		if (nvme && nvme.busy) {
			setTimeout(nvmePoll, 2000);
		}
		else if (nvme && !nvme.ok) {
			if (typeof nvmeClearDraft === 'function') nvmeClearDraft();
			nvmeMsg(nvme.err || 'The operation failed.');
			var sb = E('save-button'), cb = E('cancel-button');
			if (sb) sb.disabled = 0;
			if (cb) cb.disabled = 0;
		}
		else {
			nvmeMsg('Done. Reloading...');
			setTimeout(reloadPage, 300);
		}
	};
	nvmeXob.onError = function() {
		nvmeXob = null;
		setTimeout(nvmePoll, 3000);
	};
	nvmeXob.post('nvmecmd.cgi', 'cmd=status');
}

function nvmeRun(msg, params) {
	var sb = E('save-button'), cb = E('cancel-button');

	if (nvmeXob) return;
	if ((nvmeXob = new XmlHttp()) == null) {
		alert('This operation requires AJAX support.');
		return;
	}

	if (sb) sb.disabled = 1;
	if (cb) cb.disabled = 1;
	nvmeMsg(msg + ' (this can take a while)');

	nvmeXob.onCompleted = function(text, xml) {
		var nvme = null;
		nvmeXob = null;
		try { eval(text); } catch (ex) { nvme = null; }
		if (!nvme || !nvme.ok) {
			if (typeof nvmeClearDraft === 'function') nvmeClearDraft();
			nvmeMsg((nvme && nvme.err) ? nvme.err : 'The request failed.');
			if (sb) sb.disabled = 0;
			if (cb) cb.disabled = 0;
			return;
		}
		setTimeout(nvmePoll, 500);
	};
	nvmeXob.onError = function() {
		nvmeXob = null;
		if (typeof nvmeClearDraft === 'function') nvmeClearDraft();
		nvmeMsg('The request failed.');
		if (sb) sb.disabled = 0;
		if (cb) cb.disabled = 0;
	};
	nvmeXob.post('nvmecmd.cgi', params);
}
