// PetKit T4 stock calibration: NVS decoding and display, shared by the guide and the extractor page.
// Port of tools/extract_stock_calibration.py; see docs/stock-calibration.md.
'use strict';

const PAGE_SIZE = 4096;
const ENTRY_COUNT = 126;
const VARIABLE_TYPES = new Set([0x21, 0x41, 0x42]);
const PARTITION = 'user_calibrt', NAMESPACE = 'T4', KEY = 'data';

const hex = n => '0x' + n.toString(16);
const ascii = s => new TextEncoder().encode(s);
const sameBytes = (a, b) => a.length === b.length && a.every((v, i) => v === b[i]);
const allFF = bytes => bytes.every(v => v === 0xFF);

const CRC_TABLE = (() => {
	const table = new Uint32Array(256);
	for (let n = 0; n < 256; n++) {
		let c = n;
		for (let k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320 ^ (c >>> 1) : c >>> 1;
		table[n] = c >>> 0;
	}
	return table;
})();

// Same as Python zlib.crc32(data, 0xFFFFFFFF), as used by ESP-IDF NVS.
function crc32(...parts) {
	let crc = 0;  // zlib starts from ~0xFFFFFFFF
	for (const data of parts)
		for (const byte of data) crc = CRC_TABLE[(crc ^ byte) & 0xFF] ^ (crc >>> 8);
	return ~crc >>> 0;
}

// Compact MD5 (RFC 1321), only for the partition table checksum.
function md5(data) {
	const s = [7, 12, 17, 22, 5, 9, 14, 20, 4, 11, 16, 23, 6, 10, 15, 21];
	const K = Array.from({ length: 64 }, (_, i) => Math.floor(Math.abs(Math.sin(i + 1)) * 2 ** 32) >>> 0);
	const len = data.length, padded = new Uint8Array(((len + 8) >> 6) + 1 << 6);
	padded.set(data);
	padded[len] = 0x80;
	const view = new DataView(padded.buffer);
	view.setUint32(padded.length - 8, (len * 8) >>> 0, true);
	view.setUint32(padded.length - 4, Math.floor(len / 0x20000000), true);
	let a0 = 0x67452301, b0 = 0xEFCDAB89, c0 = 0x98BADCFE, d0 = 0x10325476;
	for (let off = 0; off < padded.length; off += 64) {
		let a = a0, b = b0, c = c0, d = d0;
		for (let i = 0; i < 64; i++) {
			let f, g;
			if (i < 16) { f = (b & c) | (~b & d); g = i; }
			else if (i < 32) { f = (d & b) | (~d & c); g = (5 * i + 1) % 16; }
			else if (i < 48) { f = b ^ c ^ d; g = (3 * i + 5) % 16; }
			else { f = c ^ (b | ~d); g = (7 * i) % 16; }
			f = (f + a + K[i] + view.getUint32(off + g * 4, true)) >>> 0;
			a = d; d = c; c = b;
			const r = s[(i >> 4) * 4 + (i & 3)];
			b = (b + ((f << r) | (f >>> (32 - r)))) >>> 0;
		}
		a0 = (a0 + a) >>> 0; b0 = (b0 + b) >>> 0; c0 = (c0 + c) >>> 0; d0 = (d0 + d) >>> 0;
	}
	const out = new Uint8Array(16), outView = new DataView(out.buffer);
	[a0, b0, c0, d0].forEach((v, i) => outView.setUint32(i * 4, v, true));
	return out;
}

function findPartition(flash, view, label) {
	const wanted = ascii(label);
	const matches = [];
	for (let base = 0; base + 32 <= flash.length; base += PAGE_SIZE) {
		if (flash[base] !== 0xAA || flash[base + 1] !== 0x50) continue;
		let candidate = null, terminated = false;
		for (let pos = base; pos < Math.min(base + 0xC00, flash.length); pos += 32) {
			const raw = flash.subarray(pos, pos + 32);
			if (raw.length !== 32) break;
			if (allFF(raw)) { terminated = true; break; }
			if (raw[0] === 0xEB && raw[1] === 0xEB) {
				if (!sameBytes(md5(flash.subarray(base, pos)), raw.subarray(16, 32)))
					throw new Error(`Partition table MD5 mismatch at ${hex(base)}`);
				terminated = true;
				break;
			}
			if (view.getUint16(pos, true) !== 0x50AA) break;
			const name = raw.subarray(12, 28);
			const end = name.indexOf(0);
			if (sameBytes(end < 0 ? name : name.subarray(0, end), wanted)) {
				if (candidate) throw new Error(`Duplicate partition label: ${label}`);
				if (raw[2] !== 1 || raw[3] !== 2) throw new Error(`${label} is not an NVS data partition`);
				candidate = { tableOffset: base, offset: view.getUint32(pos + 4, true),
				              size: view.getUint32(pos + 8, true), flags: view.getUint32(pos + 28, true) };
			}
		}
		if (terminated && candidate) matches.push(candidate);
	}
	if (matches.length !== 1)
		throw new Error(`Expected one partition table containing ${label}; found ${matches.length}. Supply a full dump.`);
	const part = matches[0];
	if (part.flags & 1) throw new Error('Encrypted partition is unsupported; supply a decrypted dump');
	if (part.offset % PAGE_SIZE || part.size <= 0 || part.size % PAGE_SIZE)
		throw new Error('Invalid NVS partition alignment or size');
	if (part.offset + part.size > flash.length)
		throw new Error('Truncated dump: calibration partition is not fully present');
	return part;
}

const entryState = (page, i) => Number((page.bitmap >> BigInt(i * 2)) & 3n);

function readEntries(flash, view, part) {
	const pages = [];
	for (let address = part.offset; address < part.offset + part.size; address += PAGE_SIZE) {
		const bytes = flash.subarray(address, address + PAGE_SIZE);
		if (allFF(bytes)) continue;
		const state = view.getUint32(address, true), sequence = view.getUint32(address + 4, true);
		if (![0xFFFFFFFE, 0xFFFFFFFC, 0xFFFFFFF8].includes(state))
			throw new Error(`Unsupported/corrupt NVS page state at ${hex(address)}`);
		if (bytes[8] !== 0xFF && bytes[8] !== 0xFE)
			throw new Error(`Unsupported NVS page version at ${hex(address)}`);
		if (crc32(bytes.subarray(4, 28)) !== view.getUint32(address + 28, true))
			throw new Error(`NVS page CRC mismatch at ${hex(address)}`);
		let bitmap = 0n;
		for (let i = 31; i >= 0; i--) bitmap = (bitmap << 8n) | BigInt(bytes[32 + i]);
		pages.push({ sequence, address, bytes, bitmap });
	}
	if (!pages.length) throw new Error('No initialized NVS pages; calibration is absent or encrypted');
	const age = (newer, older) => (newer - older) >>> 0;
	const newest = pages.filter(p => pages.every(o => age(p.sequence, o.sequence) < 2 ** 31));
	if (newest.length !== 1 || new Set(pages.map(p => p.sequence)).size !== pages.length)
		throw new Error('Ambiguous NVS page sequence numbers');
	pages.sort((x, y) => age(newest[0].sequence, y.sequence) - age(newest[0].sequence, x.sequence));

	const entries = [];
	pages.forEach((page, rank) => {
		let index = 0;
		while (index < ENTRY_COUNT) {
			if (entryState(page, index) !== 2) { index++; continue; }
			const pos = 64 + index * 32, at = page.address + pos;
			const raw = page.bytes.subarray(pos, pos + 32);
			const [ns, kind, span, chunk] = raw;
			if (crc32(raw.subarray(0, 4), raw.subarray(8)) !== view.getUint32(at + 4, true))
				throw new Error(`NVS entry CRC mismatch at ${hex(at)}`);
			if (span < 1 || index + span > ENTRY_COUNT) throw new Error(`Invalid NVS entry span at ${hex(at)}`);
			const keyField = raw.subarray(8, 24), nul = keyField.indexOf(0);
			if (nul <= 0) throw new Error(`Invalid NVS key at ${hex(at)}`);
			let payload = new Uint8Array(0);
			if (VARIABLE_TYPES.has(kind)) {
				const size = view.getUint16(at + 24, true);
				if (span !== 1 + Math.floor((size + 31) / 32)) throw new Error('NVS variable-data size/span mismatch');
				for (let j = index + 1; j < index + span; j++)
					if (entryState(page, j) !== 2) throw new Error('Incomplete NVS variable-data entry');
				payload = page.bytes.subarray(pos + 32, pos + 32 + size);
				if (crc32(payload) !== view.getUint32(at + 28, true))
					throw new Error(`NVS blob CRC mismatch at ${hex(at)}`);
			} else if (span !== 1) {
				throw new Error('Unsupported multi-entry NVS type');
			}
			entries.push({ order: [rank, index], offset: at, namespace: ns, kind, chunk,
			               key: keyField.subarray(0, nul), value: raw.subarray(24, 32), payload });
			index += span;
		}
	});
	return entries;
}

const newer = (x, y) => x.order[0] - y.order[0] || x.order[1] - y.order[1];
const latest = list => list.reduce((best, e) => newer(e, best) > 0 ? e : best);

function readBlob(entries, namespace, key) {
	const namespaces = new Set(entries
		.filter(e => e.namespace === 0 && e.kind === 1 && sameBytes(e.key, ascii(namespace)))
		.map(e => e.value[0]));
	if (namespaces.size !== 1) throw new Error(`Missing or ambiguous NVS namespace: ${namespace}`);
	const [ns] = namespaces;
	if (!(ns > 0 && ns < 255)) throw new Error('Invalid NVS namespace index');
	const records = entries.filter(e => e.namespace === ns && sameBytes(e.key, ascii(key)));
	// Chunk data is not committed until its matching blob index exists.
	const committed = records.filter(e => e.kind !== 0x42);
	if (!committed.length) throw new Error(`No committed calibration blob: ${namespace}/${key}`);
	const head = latest(committed);
	if (head.kind !== 0x41 && head.kind !== 0x48) throw new Error('Calibration key has an unsupported NVS data type');
	if (head.kind === 0x41) return { payload: head.payload, records: [head] };
	const headView = new DataView(head.value.buffer, head.value.byteOffset, 8);
	const size = headView.getUint32(0, true), count = head.value[4], start = head.value[5];
	if (![0, 0x80].includes(start) || count < 1 || count > 127 || start + count > 0xFF)
		throw new Error('Unsupported NVS blob index layout');
	const selected = [];
	for (let chunk = start; chunk < start + count; chunk++) {
		const candidates = records.filter(e => e.kind === 0x42 && e.chunk === chunk);
		if (!candidates.length) throw new Error(`Missing committed NVS blob chunk: ${chunk}`);
		if (candidates.some(e => !sameBytes(e.payload, candidates[0].payload)))
			throw new Error('Conflicting live NVS blob chunks; refusing ambiguous recovery');
		selected.push(latest(candidates));
	}
	const payload = new Uint8Array(selected.reduce((n, e) => n + e.payload.length, 0));
	selected.reduce((at, e) => (payload.set(e.payload, at), at + e.payload.length), 0);
	if (payload.length !== size) throw new Error('NVS blob index size does not match its chunks');
	return { payload, records: [head, ...selected] };
}

function extract(buffer) {
	const flash = new Uint8Array(buffer), view = new DataView(buffer);
	const part = findPartition(flash, view, PARTITION);
	const { payload, records } = readBlob(readEntries(flash, view, part), NAMESPACE, KEY);
	if (payload.length !== 20)
		throw new Error(`Unknown PETKIT calibration schema: expected 20 bytes, got ${payload.length}`);
	const blob = new DataView(payload.buffer, payload.byteOffset, 20);
	const [zero, low, high, slopeLow, slopeHigh] = [0, 4, 8, 12, 16].map(o => blob.getInt32(o, true));
	if (!(0 < high && high < low && low < zero && zero <= 0xFFFFFF && slopeLow > 0 && slopeHigh > 0))
		throw new Error('Calibration values do not match the known PETKIT T4 schema');
	return {
		source_size: flash.length, partition_table_offset: hex(part.tableOffset),
		partition: PARTITION, partition_offset: hex(part.offset),
		namespace: NAMESPACE, key: KEY, blob_size: payload.length,
		record_offsets: records.map(e => hex(e.offset)), crc_verified: true,
		calibration: {
			zero_raw: zero, low_reference_raw: low, high_reference_raw: high,
			slope_low_raw_per_gram: slopeLow, slope_high_raw_per_gram: slopeHigh,
		},
	};
}


const escape = s => String(s).replace(/[&<>"]/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' })[c]);
const fmt = n => n.toLocaleString('en-US');

// secrets.yaml keys, one per calibration field.
const FIELDS = [
	['zero_raw', 'weight_stock_zero_raw', 'raw ADC value at 0 g (bare load cells)'],
	['low_reference_raw', 'weight_stock_low_raw', 'raw value at the low reference'],
	['high_reference_raw', 'weight_stock_high_raw', 'raw value at the high reference'],
	['slope_low_raw_per_gram', 'weight_stock_slope_low', 'raw units per gram, first range'],
	['slope_high_raw_per_gram', 'weight_stock_slope_high', 'raw units per gram, second range'],
];

function secretsYaml(c) {
	return '# PetKit T4 factory weight calibration (tools/stock-calibration.html)\n'
		+ FIELDS.map(([field, secret]) => `${secret}: ${c[field]}`).join('\n') + '\n';
}

// navigator.clipboard needs a secure context; fall back to a hidden textarea otherwise.
async function copy(text) {
	try {
		await navigator.clipboard.writeText(text);
	} catch {
		const area = Object.assign(document.createElement('textarea'), { value: text });
		area.style.position = 'fixed';
		area.style.opacity = '0';
		document.body.append(area);
		area.select();
		const ok = document.execCommand('copy');
		area.remove();
		if (!ok) throw new Error('copy refused');
	}
}

function copyButton(label, text) {
	const button = Object.assign(document.createElement('button'), { textContent: label, type: 'button' });
	button.onclick = () => copy(text).then(
		() => { button.textContent = 'Copied'; setTimeout(() => { button.textContent = label; }, 1500); },
		() => { button.textContent = 'Copy failed'; });
	return button;
}

// Renders an extract() result into container. In the install guide (guide: true) the secrets.yaml
// lines are added to step 5 automatically: they only show at the bottom, for the command line.
function renderCalibration(container, name, data, { guide = false } = {}) {
	const c = data.calibration;
	const lowGrams = Math.trunc((c.zero_raw - c.low_reference_raw) / c.slope_low_raw_per_gram);
	const highGrams = Math.trunc((c.low_reference_raw - c.high_reference_raw) / c.slope_high_raw_per_gram);
	const yaml = secretsYaml(c), json = JSON.stringify(data, null, 2);
	const yamlCard = guide ? `
		<div class="card">
			<h2>Using the command line?</h2>
			<p class="note">Add these lines to your own <code>secrets.yaml</code>. With the form of step 5, there is nothing to copy.</p>
			<pre>${escape(yaml)}</pre>
			<div class="actions yaml-actions"></div>
		</div>` : `
		<div class="card">
			<h2>Paste into <code>secrets.yaml</code></h2>
			<pre>${escape(yaml)}</pre>
			<div class="actions yaml-actions"></div>
		</div>`;
	const copyColumn = guide ? '' : '<th></th>';
	container.innerHTML = `
		<div class="card">
			<p class="ok">Calibration found, every CRC verified</p>
			${guide ? '<p class="note">Already added to the <code>secrets.yaml</code> of <a href="#secrets">step 5</a>.</p>' : ''}
		</div>
		${guide ? '' : yamlCard}
		<div class="card">
			<h2>Values</h2>
			<table>
				<tr><th>Field</th><th class="num">Value</th>${copyColumn}</tr>
				${FIELDS.map(([field, , meaning]) => `<tr><td><code>${field}</code><br><span class="note">${meaning}</span></td>`
					+ `<td class="num">${c[field]}</td>` + (guide ? '' : `<td class="copy" data-value="${c[field]}"></td>`) + '</tr>').join('')}
			</table>
			<p class="note" style="margin-top:12px">Stock two-range conversion: first range ${fmt(lowGrams)} g, second range ${fmt(highGrams)} g more. A lower raw value means more weight.</p>
		</div>
		<div class="card">
			<table class="meta">
				<tr><td>File</td><td>${escape(name)} (${fmt(data.source_size)} bytes)</td></tr>
				<tr><td>Partition table</td><td>${data.partition_table_offset}</td></tr>
				<tr><td>Partition</td><td>${data.partition} at ${data.partition_offset}</td></tr>
				<tr><td>Records</td><td>${data.record_offsets.join(', ')}</td></tr>
			</table>
			<details>
				<summary>Full JSON</summary>
				<pre>${escape(json)}</pre>
				<div class="actions json-actions"></div>
			</details>
		</div>
		${guide ? yamlCard : ''}`;
	container.querySelector('.yaml-actions').append(copyButton('Copy for secrets.yaml', yaml));
	container.querySelector('.json-actions').append(copyButton('Copy JSON', json));
	container.querySelectorAll('td.copy').forEach(cell => cell.append(copyButton('Copy', cell.dataset.value)));
}

function renderCalibrationError(container, name, error) {
	container.innerHTML = `<div class="card error"><strong>Extraction failed</strong><br>${escape(name)}: ${escape(error.message)}</div>`;
}
