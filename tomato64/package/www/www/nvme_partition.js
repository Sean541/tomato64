var currentPartitionDisk = '';
var currentPartitionFreeMB = 0;
var currentPartitionTotalMB = 0;
var currentDeleteDev = '';

function triggerPartition(diskName, totalMB, allocatedMB, freeMB) {
	currentPartitionDisk = diskName;
	currentPartitionFreeMB = freeMB;
	currentPartitionTotalMB = totalMB;

	E('modal-part-target-disk').innerText = '/dev/' + diskName;
	E('modal-part-space-info').innerHTML = 'Total: ' + scaleSize(totalMB * 1024 * 1024) +
		' / Allocated: ' + scaleSize(allocatedMB * 1024 * 1024) +
		' / Free: ' + scaleSize(freeMB * 1024 * 1024);
	E('modal-part-size-input').value = '';
	E('modal-part-label-input').value = '';

	nvmeApplyModalTheme(E('partition-modal-box'));

	E('partition-modal-overlay').style.display = 'flex';
}

function closePartitionModal() {
	E('partition-modal-overlay').style.display = 'none';
	currentPartitionDisk = '';
	currentPartitionFreeMB = 0;
	currentPartitionTotalMB = 0;
}

function confirmPartition() {
	var sizeInputStr = E('modal-part-size-input').value.trim();
	var requestedMB = parseInt(sizeInputStr, 10);
	var partitionLabel = E('modal-part-label-input').value.trim();

	if (partitionLabel !== '' && !/^[A-Za-z0-9_.\-]{1,16}$/.test(partitionLabel)) {
		alert('Invalid volume label. Use up to 16 characters of A-Z a-z 0-9 _ . -');
		return;
	}

	if (!/^\d+$/.test(sizeInputStr) || isNaN(requestedMB) || requestedMB <= 0) {
		alert('Please enter a valid partition size in MB.');
		return;
	}

	if (requestedMB > currentPartitionFreeMB) {
		alert('Error: Requested partition size (' + requestedMB + ' MB) exceeds available unallocated free space (' + currentPartitionFreeMB + ' MB).');
		return;
	}

	var postAllocatedMB = currentPartitionTotalMB - currentPartitionFreeMB + requestedMB;
	var postFreeMB = currentPartitionTotalMB - postAllocatedMB;
	if (postFreeMB < 0) postFreeMB = 0;

	var warningMsg = 'WARNING: You are about to partition /dev/' + currentPartitionDisk + '.\n\n' +
		'Please ensure you know what you are doing as permanent data loss can occur.\n\n' +
		'Requested Partition Size: ' + requestedMB + ' MB\n' +
		'Partition Label: ' + (partitionLabel !== '' ? partitionLabel : '(None)') + '\n' +
		'Projected Drive Space After Partitioning:\n' +
		' - Total Capacity: ' + scaleSize(currentPartitionTotalMB * 1024 * 1024).replace(/<[^>]*>/g, '') + '\n' +
		' - Projected Allocated: ' + scaleSize(postAllocatedMB * 1024 * 1024).replace(/<[^>]*>/g, '') + '\n' +
		' - Projected Free: ' + scaleSize(postFreeMB * 1024 * 1024).replace(/<[^>]*>/g, '') + '\n\n' +
		'Do you wish to proceed?';

	if (confirm(warningMsg)) {
		var disk = currentPartitionDisk;
		if (typeof nvmeSaveDraft === 'function') nvmeSaveDraft();
		closePartitionModal();
		nvmeRun('Partitioning /dev/' + disk + '...', 'cmd=partition&dev=' + escapeCGI('/dev/' + disk) + '&size=' + requestedMB + '&label=' + escapeCGI(partitionLabel));
	}
}

function triggerDelete(dev_path) {
	currentDeleteDev = dev_path;
	E('modal-delete-target').innerText = dev_path;
	E('modal-delete-check').checked = false;
	E('modal-delete-input').value = '';
	E('btn-modal-delete').disabled = true;

	nvmeApplyModalTheme(E('delete-modal-box'));

	E('delete-modal-overlay').style.display = 'flex';
}

function verifyDeleteInput() {
	var isChecked = E('modal-delete-check').checked;
	var isTyped = E('modal-delete-input').value.trim() === 'DELETE';

	E('btn-modal-delete').disabled = !(isChecked && isTyped);
}

function closeDeleteModal() {
	E('delete-modal-overlay').style.display = 'none';
	currentDeleteDev = '';
}

function confirmDelete() {
	if (E('btn-modal-delete').disabled) return;

	var dev = currentDeleteDev;
	if (typeof nvmeSaveDraft === 'function') nvmeSaveDraft(dev);
	closeDeleteModal();
	nvmeRun('Deleting partition ' + dev + '...', 'cmd=delete&dev=' + escapeCGI(dev));
}
