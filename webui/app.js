/* RetroArch PS5 WebUI. No external runtime or browser storage of console credentials. */
'use strict';
const $ = (selector, root = document) => root.querySelector(selector);
const $$ = (selector, root = document) => [...root.querySelectorAll(selector)];
const repo = 'mihawk-99/PS5_RetroArch';
let token = '', connected = false, freeBytes = null, uploadLimit = 64 * 1024 ** 3;
let currentPath = '', entries = [], folderRequest = 0, settingsValues = {};
let sessionRequest = null, sending = false, nextTransfer = 0, frontend = 'retroarch';
const transfers = [];
function element(tag, text, className) {
  const node = document.createElement(tag);
  if (text !== undefined) node.textContent = text;
  if (className) node.className = className;
  return node;
}
function icon(kind) {
  const selectors = { folder: '.library-panel .section-icon', file: '.dropzone .icon', check: '.release-icon' };
  const node = $(selectors[kind] || selectors.file).cloneNode(true);
  node.setAttribute('class', 'icon');
  return node;
}
function bytes(n) {
  if (n === null || !Number.isFinite(n)) return 'Unknown';
  const units = ['B', 'KiB', 'MiB', 'GiB', 'TiB'];
  let i = 0;
  while (n >= 1024 && i < units.length - 1) { n /= 1024; i++; }
  return `${n.toLocaleString(undefined, { maximumFractionDigits: i ? 1 : 0 })} ${units[i]}`;
}
function announce(message, failure = false) {
  const node = $('#announcement'); node.textContent = message;
  node.classList.toggle('error', failure);
}
async function api(path, options = {}) {
  const headers = new Headers(options.headers || {});
  if (options.method && options.method !== 'GET') headers.set('X-RetroArch-Token', token);
  const response = await fetch(path, { ...options, headers, cache: 'no-store', signal: options.signal || AbortSignal.timeout(15000) });
  const data = await response.json();
  if (!response.ok) throw new Error(data.error || `Request failed (${response.status}). Try again.`);
  return data;
}
function setConnection(ok) {
  connected = ok;
  $('#connection').classList.toggle('offline', !ok);
  // The WebUI stays up while the title changes frontend (src/webui_link.h): say which one shows.
  const showing = { retroarch: 'RetroArch is running', picker: 'Frontend picker is open', 'es-de': 'EmulationStation is open', title: 'PS5 RetroArch is starting' }[frontend] || 'PS5 RetroArch is closed';
  $('#connection span:last-child').textContent = ok ? showing : 'Console disconnected';
  $('#connection-notice').hidden = ok;
  for (const id of ['destination', 'dropzone', 'browse-files', 'upload-here', 'folder-name', 'quick-volume', 'quick-rumble', 'quick-frontend', 'settings-fields', 'save-settings']) {
    $('#' + id).disabled = !ok || (['settings-fields', 'save-settings'].includes(id) && !editorRevision) || (['quick-volume', 'quick-rumble', 'quick-frontend', 'settings-fields', 'save-settings'].includes(id) && !Object.keys(settingsValues).length);
  }
  $('#folder-form button').disabled = !ok;
  updateButton();
  $('#install-update').disabled = !ok;
}
async function reconnect() {
  if (sessionRequest) return sessionRequest;
  sessionRequest = (async () => {
    try {
      const state = await api('/api/status');
      if (token && token !== state.token && updateState === 'installing') { location.reload(); return false; }
      const recovered = !connected || token !== state.token;
      token = state.token; freeBytes = state.freeBytes; uploadLimit = state.uploadLimit; frontend = state.frontend ?? 'retroarch';
      setConnection(true);
      $('#storage-info').textContent = freeBytes === null ? 'Games and files stored on your PS5' : `${bytes(freeBytes)} free on the console`;
      if (recovered) await Promise.all([loadLibrary(), loadSettings(), loadContent(currentPath), loadAlerts(), loadUpdate()]);
      return true;
    } catch (error) { setConnection(false); return false; }
    finally { sessionRequest = null; }
  })();
  return sessionRequest;
}
function pageFromHash() {
  const name = location.hash.slice(1).split('?')[0];
  return ['content', 'games', 'transfers', 'settings'].includes(name) ? name : 'overview';
}
function navigate() {
  const page = pageFromHash();
  $$('.page').forEach(node => { node.hidden = node.id !== page; });
  $$('[data-page]').forEach(node => { if (node.dataset.page === page) node.setAttribute('aria-current', 'page'); else node.removeAttribute('aria-current'); });
  $('#page-title').textContent = page[0].toUpperCase() + page.slice(1);
  if (page === 'content') loadContent(currentPath);
  if (page === 'games') { loadGames(); loadJob(); }
  $('#main').focus({ preventScroll: true });
}
function openFolder(path) {
  ++folderRequest; // Ignore a previous folder response while the route changes.
  currentPath = path;
  if (location.hash !== '#content') location.hash = 'content';
  else loadContent(path);
}
async function loadLibrary() {
  const target = $('#library-folders');
  try {
    const data = await api('/api/content');
    const folders = data.entries.filter(e => e.directory);
    target.replaceChildren();
    const select = $('#destination'), selected = select.value;
    select.replaceChildren(new Option('Content folder', ''));
    for (const entry of folders) {
      select.add(new Option(entry.name, entry.name));
      if (target.childElementCount < 3) {
        const row = element('a', undefined, 'folder-row'); row.href = '#content';
        row.append(icon('folder'), element('strong', entry.name), element('span', 'Open content'));
        row.addEventListener('click', event => { event.preventDefault(); openFolder(entry.name); });
        target.append(row);
      }
    }
    if ([...select.options].some(o => o.value === selected)) select.value = selected;
    if (!folders.length) {
      const box = element('div', undefined, 'list-message');
      box.append(element('p', 'Your content starts here. Upload a file or create a folder.'));
      const link = element('a', 'Open content'); link.href = '#content'; box.append(link); target.append(box);
    } else if (folders.length > 3) {
      const more = element('a', `View all ${folders.length} folders`, 'list-message'); more.href = '#content';
      more.addEventListener('click', event => { event.preventDefault(); openFolder(''); }); target.append(more);
    }
  } catch (error) { target.replaceChildren(element('p', 'Library unavailable. Reconnect or open Content to retry.', 'list-message')); }
}
function renderBreadcrumbs() {
  const nav = $('#breadcrumbs'); nav.replaceChildren();
  const parts = currentPath ? currentPath.split('/') : [];
  ['', ...parts].forEach((part, index) => {
    const path = parts.slice(0, index).join('/');
    const button = element('button', index ? part : 'Content', 'text-button');
    button.type = 'button'; button.disabled = index === parts.length;
    button.addEventListener('click', () => openFolder(path)); nav.append(button);
  });
}
async function loadContent(path) {
  const request = ++folderRequest;
  $('#content-error').hidden = true;
  $('#content-list').setAttribute('aria-busy', 'true');
  try {
    const data = await api('/api/content?path=' + encodeURIComponent(path));
    if (request !== folderRequest) return;
    currentPath = data.path; entries = data.entries; renderBreadcrumbs();
    $('#file-search').value = '';
    $('#folder-caption').textContent = data.truncated ? 'Showing the first 10,000 items. Organize files into smaller folders to see more.' : `${entries.length} ${entries.length === 1 ? 'item' : 'items'}`;
    renderContent();
  } catch (error) {
    if (request !== folderRequest) return;
    $('#content-error').textContent = error.message; $('#content-error').hidden = false;
    $('#content-list').replaceChildren();
  } finally { if (request === folderRequest) $('#content-list').removeAttribute('aria-busy'); }
}
function renderContent() {
  const target = $('#content-list'); target.replaceChildren();
  const search = $('#file-search').value.toLocaleLowerCase();
  for (const entry of entries.filter(e => e.name.toLocaleLowerCase().includes(search))) {
    const row = element('div', undefined, 'content-row');
    const path = [currentPath, entry.name].filter(Boolean).join('/');
    const name = element(entry.directory ? 'button' : 'a', entry.name, 'file-name');
    if (entry.directory) { name.type = 'button'; name.addEventListener('click', () => openFolder(path)); }
    else {
      name.href = '/api/download?path=' + encodeURIComponent(path); name.download = entry.name;
      name.addEventListener('click', () => recordDownload(entry.name));
    }
    row.append(icon(entry.directory ? 'folder' : 'file'), name, element('span', entry.directory ? 'Folder' : bytes(entry.size), 'file-size'));
    if (!entry.directory) {
      const link = element('a', 'Download'); link.href = name.href; link.download = entry.name;
      link.addEventListener('click', () => recordDownload(entry.name)); row.append(link);
    }
    target.append(row);
  }
  if (!target.childElementCount) target.append(element('p', search ? 'No matching files. Try another name.' : 'This folder is empty. Upload files here to get started.', 'list-message'));
}
function validName(name) { return !!name && name[0] !== '.' && !/[\x00-\x1f\x7f\\/:]/.test(name) && new TextEncoder().encode(name).length <= 255; }
$('#folder-form').addEventListener('submit', async event => {
  event.preventDefault(); const input = $('#folder-name'), name = input.value.trim();
  if (!validName(name)) { announce('Use a folder name without slashes, a leading dot, or control characters.', true); return; }
  const button = $('#folder-form button'); button.disabled = true;
  try {
    await api('/api/folder?path=' + encodeURIComponent([currentPath, name].filter(Boolean).join('/')), { method: 'POST' });
    input.value = ''; announce(`Created ${name}.`); await Promise.all([loadLibrary(), loadContent(currentPath)]);
  } catch (error) { announce(error.message, true); }
  finally { button.disabled = !connected; }
});
function drawTransfers() {
  for (const selector of ['#recent-transfers', '#all-transfers']) {
    const target = $(selector);
    const shown = selector === '#recent-transfers' ? transfers.slice(-3).reverse() : [...transfers].reverse();
    const visible = new Set(shown.map(transfer => String(transfer.id)));
    for (const child of [...target.children]) if (!visible.has(child.dataset.transfer)) child.remove();
    if (!shown.length) { const empty = element('div', undefined, 'empty-state'); empty.append(icon('file'), element('p', 'Your transfers will appear here.')); target.append(empty); }
    for (const [index, transfer] of shown.entries()) {
      let row = target.querySelector(`[data-transfer="${transfer.id}"]`);
      if (!row) {
        row = element('div', undefined, 'transfer-row'); row.dataset.transfer = transfer.id;
        const details = element('div', undefined, 'transfer-details');
        details.append(element('strong', transfer.name), element('span', '', 'muted')); row.append(details);
        target.insertBefore(row, target.children[index] || null);
      }
      const details = row.firstElementChild, message = details.children[1];
      message.textContent = transfer.message; message.className = transfer.state === 'failed' ? 'inline-error' : 'muted';
      if (['queued', 'uploading'].includes(transfer.state)) {
        let progress = details.querySelector('progress');
        if (!progress) {
          progress = document.createElement('progress'); progress.max = 100;
          progress.setAttribute('aria-label', `Upload progress for ${transfer.name}`); details.append(progress);
          const cancel = element('button', 'Cancel', 'secondary');
          cancel.addEventListener('click', () => cancelTransfer(transfer));
          row.append(cancel);
        }
        progress.value = transfer.percent || 0;
      } else { details.querySelector('progress')?.remove(); row.querySelector('button')?.remove(); }
    }
  }
}
function recordDownload(name) {
  transfers.push({ id: ++nextTransfer, name, state: 'download', message: 'Download requested · check your browser’s download manager' }); drawTransfers();
}
function queueFiles(files, destination) {
  if (!connected) { announce('Reconnect to the console before uploading files.', true); return; }
  for (const file of files) {
    if (!validName(file.name) || file.size > uploadLimit) { announce(`${file.name}: choose a valid filename and a file no larger than ${bytes(uploadLimit)}.`, true); continue; }
    transfers.push({ id: ++nextTransfer, name: file.name, file, path: [destination, file.name].filter(Boolean).join('/'), state: 'queued', message: `Queued · ${bytes(file.size)}`, percent: 0 });
  }
  drawTransfers(); sendNext();
}
// The transfer engine (src/webui_transfer.h on the console): six lanes run at once.
// Small files travel many to a request (a batch), large files as parts on every free
// lane (an upload session), the rest one to a request. Every file is renamed into
// place on the console only once whole.
const LANES = 6, SMALL = 4 * 1024 ** 2, LARGE = 64 * 1024 ** 2, PART = 32 * 1024 ** 2;
const BATCH_FILES = 256, BATCH_BYTES = 16 * 1024 ** 2, PART_TRIES = 3;
let jobs = [], opening = 0; // sessions being opened: their parts are about to queue
function cancelTransfer(transfer) {
  if (!['queued', 'uploading'].includes(transfer.state)) return;
  transfer.state = 'cancelled'; transfer.message = 'Cancelled';
  for (const xhr of transfer.requests || []) xhr.abort();
  if (transfer.session) api('/api/upload/session?id=' + encodeURIComponent(transfer.session), { method: 'DELETE' }).catch(() => {});
  transfer.file = null; drawTransfers();
}
function showProgress(transfer, sent) {
  transfer.sent = Math.min(sent, transfer.file?.size ?? sent);
  const size = transfer.file?.size || 0, percent = size ? Math.floor(transfer.sent / size * 100) : 100;
  if (percent === transfer.percent && transfer.state === 'uploading') return;
  transfer.percent = percent; transfer.state = 'uploading';
  transfer.message = percent >= 100 ? 'Finishing on the console…' : `${percent}% · ${bytes(transfer.sent)} of ${bytes(size)}`;
  const now = Date.now(); if (now - (transfer.drawn || 0) > 150 || percent >= 100) { transfer.drawn = now; drawTransfers(); }
}
function finish(transfer, state, message) {
  if (transfer.state === 'cancelled') return;
  transfer.state = state; transfer.message = message; transfer.file = null; transfer.requests = [];
  transfer.session = null; drawTransfers();
}
// One request on a lane: resolves with {status, body}, or rejects when the connection fails.
function send(method, url, body, transfers, onProgress) {
  return new Promise((resolve, reject) => {
    const xhr = new XMLHttpRequest();
    for (const transfer of transfers) (transfer.requests ||= []).push(xhr);
    const done = () => { for (const transfer of transfers) transfer.requests = (transfer.requests || []).filter(x => x !== xhr); };
    xhr.open(method, url); xhr.setRequestHeader('X-RetroArch-Token', token);
    if (onProgress) xhr.upload.onprogress = event => onProgress(event.loaded);
    xhr.onload = () => { done(); let parsed = {}; try { parsed = JSON.parse(xhr.responseText); } catch {} resolve({ status: xhr.status, body: parsed }); };
    xhr.onerror = () => { done(); reject(new Error('Connection lost. Reconnect, then choose this file again.')); };
    xhr.onabort = () => { done(); reject(new Error('Cancelled')); };
    xhr.send(body);
  });
}
// The batch stream: per file a u16 name length, the name (UTF-8), a u64 size and the
// file itself, then a zero length. A Blob of the files' own Blobs: nothing is copied.
function batchBody(transfers) {
  const parts = [], encoder = new TextEncoder();
  for (const transfer of transfers) {
    const name = encoder.encode(transfer.path), head = new Uint8Array(2 + name.length + 8), view = new DataView(head.buffer);
    view.setUint16(0, name.length, true); head.set(name, 2); view.setBigUint64(2 + name.length, BigInt(transfer.file.size), true);
    parts.push(head, transfer.file);
  }
  parts.push(new Uint8Array(2));
  return new Blob(parts);
}
async function runBatch(transfers) {
  transfers = transfers.filter(transfer => transfer.state === 'queued');
  if (!transfers.length) return;
  const encoder = new TextEncoder(), starts = []; let at = 0;
  for (const transfer of transfers) { at += 2 + encoder.encode(transfer.path).length + 8; starts.push(at); at += transfer.file.size; }
  for (const transfer of transfers) showProgress(transfer, 0);
  try {
    const { status, body: result } = await send('PUT', '/api/upload/batch', batchBody(transfers), transfers,
      loaded => transfers.forEach((transfer, i) => showProgress(transfer, loaded - starts[i])));
    const failed = new Map((result.failed || []).map(entry => [entry.name, entry.error]));
    const skipped = new Set(result.skippedNames || []);
    for (const transfer of transfers) {
      if (transfer.state !== 'uploading') continue;
      if (skipped.has(transfer.path)) finish(transfer, 'failed', 'A file with this name already exists. Rename your file first.');
      else if (failed.has(transfer.path)) finish(transfer, 'failed', `Upload failed (${failed.get(transfer.path)}). Choose the file again to retry.`);
      else if (status === 200) finish(transfer, 'complete', `Uploaded · ${bytes(transfer.file.size)}`);
      else finish(transfer, 'failed', result.error || result.problem || 'Upload failed. Choose the file again to retry.');
    }
  } catch (error) {
    for (const transfer of transfers) if (transfer.state === 'uploading') finish(transfer, 'failed', error.message);
    if (error.message !== 'Cancelled') setConnection(false);
  }
}
async function runSingle(transfer) {
  if (transfer.state !== 'queued') return;
  showProgress(transfer, 0);
  try {
    const { status, body } = await send('PUT', '/api/upload?path=' + encodeURIComponent(transfer.path), transfer.file, [transfer], loaded => showProgress(transfer, loaded));
    if (status === 201) finish(transfer, 'complete', `Uploaded · ${bytes(transfer.file.size)}`);
    else finish(transfer, 'failed', body.error || 'Upload failed. Choose the file again to retry.');
  } catch (error) { finish(transfer, error.message === 'Cancelled' ? 'cancelled' : 'failed', error.message); if (error.message !== 'Cancelled') setConnection(false); }
}
// A large file: a session, then its parts as jobs any lane takes. The part that
// completes the file commits it, on its own lane: no lane ever waits on another.
async function runLarge(transfer) {
  if (transfer.state !== 'queued') return;
  showProgress(transfer, 0);
  let opened;
  opening++;
  try { opened = await send('POST', `/api/upload/session?path=${encodeURIComponent(transfer.path)}&size=${transfer.file.size}`, null, [transfer]); }
  catch (error) { finish(transfer, 'failed', error.message); return; }
  finally { opening--; }
  if (opened.status !== 201) { finish(transfer, 'failed', opened.body.error || 'Upload failed. Choose the file again to retry.'); return; }
  transfer.session = opened.body.id;
  const size = transfer.file.size, part = Math.max(4 * 1024 ** 2, Math.min(opened.body.partSize || PART, Math.ceil(size / LANES)));
  const sent = new Map(), parts = [];
  for (let start = 0; start < size; start += part) parts.push([start, Math.min(start + part, size)]);
  let left = parts.length, failure = null;
  const abandon = message => {
    if (failure) return;
    failure = message;
    api('/api/upload/session?id=' + encodeURIComponent(transfer.session), { method: 'DELETE' }).catch(() => {});
    if (message !== 'Cancelled') finish(transfer, 'failed', message);
  };
  const commit = async () => {
    try {
      const { status, body } = await send('POST', '/api/upload/commit?id=' + encodeURIComponent(transfer.session), null, [transfer]);
      if (status === 201) finish(transfer, 'complete', `Uploaded · ${bytes(size)}`);
      else finish(transfer, 'failed', body.error || 'Upload failed. Choose the file again to retry.');
    } catch (error) { finish(transfer, 'failed', error.message); }
  };
  const partJob = ([start, end]) => async () => {
    for (let attempt = 1; !failure; attempt++) {
      if (transfer.state !== 'uploading') { abandon('Cancelled'); return; }
      try {
        const { status, body } = await send('PUT', `/api/upload/part?id=${encodeURIComponent(transfer.session)}&offset=${start}`, transfer.file.slice(start, end), [transfer],
          loaded => { sent.set(start, loaded); showProgress(transfer, [...sent.values()].reduce((a, b) => a + b, 0)); });
        if (status === 200) { sent.set(start, end - start); break; }
        if (attempt >= PART_TRIES || status === 404 || status === 416) { abandon(body.error || 'A part could not be written.'); return; }
      } catch (error) {
        if (error.message === 'Cancelled') { abandon('Cancelled'); return; }
        if (attempt >= PART_TRIES) { abandon(error.message); return; }
      }
    }
    if (!failure && --left === 0) await commit();
  };
  // First in the queue, so every free lane joins this file at once.
  jobs.unshift(...parts.map(partJob));
}
// Turns the queued transfers into jobs: batches of small files, sessions, single files.
function planJobs() {
  let batch = [], batchBytes = 0;
  const flush = () => { if (batch.length) { const group = batch; jobs.push(() => runBatch(group)); } batch = []; batchBytes = 0; };
  for (const transfer of transfers) {
    if (transfer.state !== 'queued' || transfer.planned) continue;
    transfer.planned = true;
    const size = transfer.file.size;
    if (size < SMALL) {
      if (batch.length >= BATCH_FILES || batchBytes + size > BATCH_BYTES) flush();
      batch.push(transfer); batchBytes += size;
    } else if (size >= LARGE) jobs.push(() => runLarge(transfer));
    else jobs.push(() => runSingle(transfer));
  }
  flush();
}
async function sendNext() {
  planJobs();
  if (sending) return;
  sending = true;
  try {
    if (!connected && !await reconnect()) {
      for (const transfer of transfers) if (transfer.state === 'queued') finish(transfer, 'failed', 'Console disconnected. Reconnect, then choose this file again.');
      jobs = []; return;
    }
    // Each lane runs one job at a time until none is left; a job is one request (or a
    // part and its file's commit), so no lane waits on another lane's work.
    const lane = async () => {
      for (;;) {
        planJobs();
        const job = jobs.shift();
        if (job) await job();
        else if (opening) await new Promise(resolve => setTimeout(resolve, 25));
        else return;
      }
    };
    await Promise.all(Array.from({ length: LANES }, lane));
  } finally { sending = false; await Promise.all([loadLibrary(), loadContent(currentPath), reconnect()]); }
}
let pickerDestination = '';
function pickFiles(destination) { pickerDestination = destination; $('#file-input').click(); }
$('#browse-files').addEventListener('click', () => pickFiles($('#destination').value));
$('#dropzone').addEventListener('click', () => pickFiles($('#destination').value));
$('#upload-here').addEventListener('click', () => pickFiles(currentPath));
$('#file-input').addEventListener('change', event => { queueFiles([...event.target.files], pickerDestination); event.target.value = ''; });
for (const name of ['dragenter', 'dragover']) $('#dropzone').addEventListener(name, event => { event.preventDefault(); $('#dropzone').classList.add('dragging'); });
for (const name of ['dragleave', 'drop']) $('#dropzone').addEventListener(name, event => { event.preventDefault(); $('#dropzone').classList.remove('dragging'); });
$('#dropzone').addEventListener('drop', event => queueFiles([...event.dataTransfer.files], $('#destination').value));
window.addEventListener('dragover', event => event.preventDefault());
window.addEventListener('drop', event => event.preventDefault());
window.addEventListener('beforeunload', event => { if (Object.keys(editorDraft).length || transfers.some(t => ['queued', 'uploading'].includes(t.state))) { event.preventDefault(); event.returnValue = ''; } });
$('#clear-transfers').addEventListener('click', () => { for (let i = transfers.length - 1; i >= 0; i--) if (!['queued', 'uploading'].includes(transfers[i].state)) transfers.splice(i, 1); drawTransfers(); });
function updateQuick() {
  for (const [id, key, unit] of [['quick-volume', 'audio_volume', ' dB'], ['quick-rumble', 'input_rumble_gain', '%']]) {
    $('#' + id).value = settingsValues[key]; $(`output[for="${id}"]`).textContent = Number(settingsValues[key]) + unit;
  }
  // The frontend the title opens on: the picker every time, or one straight away.
  if (settingsValues.frontend_start) $('#quick-frontend').value = settingsValues.frontend_start;
}
let editorSettings = [], editorValues = {}, editorDraft = {}, editorRevision = '', editorPage = 0, editorRequest = 0;
let editorProfile = '', editorKind = 'core-options', editorMode = 'guided', editorCategory = '';
let editorMetadata = {}, editorCategories = [], metadataUnavailable = false;
const pageSize = 40;
function settingTitle(key) {
  const known = { audio_volume: 'Audio volume', input_rumble_gain: 'Rumble strength', video_smooth: 'Smooth image scaling', video_vsync: 'Vertical sync', menu_driver: 'Console menu' };
  return known[key] || key.replace(/[_-]/g, ' ').replace(/\b\w/g, c => c.toUpperCase()).replace(/\b(ppsspp|snes|nes|gba|gpu|cpu|msaa|fps|vsync|xmb|rgui)\b/gi, word => word.toUpperCase());
}
function editorUrl() { return '/api/config?scope=' + (editorProfile ? editorKind : 'global') + '&core=' + encodeURIComponent(editorProfile); }
function markEdits() {
  const count = Object.keys(editorDraft).length;
  $('#settings-result').textContent = count ? `${count} unsaved ${count === 1 ? 'change' : 'changes'}` : '';
  $('#refresh-settings').textContent = count ? 'Discard changes & refresh' : 'Refresh settings';
}
function settingGuide(setting) {
  if (!editorProfile || editorKind === 'core-settings') return globalGuide[setting.key];
  const meta = editorMetadata[setting.key];
  if (!meta) return null;
  return { ...meta, category: meta.category || 'general', control: meta.choices?.length ? 'select' : 'text', description: coreHelp(setting.key) || meta.description || 'This core provides the available choices but no explanation for this option. Keep its current value unless you know the change you need.' };
}
function choiceLabel(value, label) {
  if (['enabled', 'true'].includes(label)) return 'On';
  if (['disabled', 'Disabled', 'false'].includes(label)) return 'Off';
  return label || value;
}
function renderSettings() {
  const guided = editorMode === 'guided', query = $('#settings-search').value.trim().toLocaleLowerCase();
  const available = editorSettings.filter(s => !guided || settingGuide(s));
  if (guided) {
    const order = Object.keys(!editorProfile || editorKind === 'core-settings' ? globalGuide : editorMetadata);
    const rank = new Map(order.map((key, index) => [key, index]));
    available.sort((a, b) => rank.get(a.key) - rank.get(b.key));
  }
  const categories = (!editorProfile || editorKind === 'core-settings' ? globalCategories : [...editorCategories, { key: 'general', label: 'General', description: 'Options supplied by this core.' }])
    .filter(c => available.some(s => settingGuide(s)?.category === c.key));
  if (!categories.some(c => c.key === editorCategory)) editorCategory = categories[0]?.key || '';
  const nav = $('#settings-categories'); nav.replaceChildren(); nav.hidden = !guided || !categories.length;
  for (const category of categories) {
    const button = element('button', category.label); button.type = 'button'; button.dataset.category = category.key;
    button.setAttribute('aria-pressed', String(category.key === editorCategory && !query));
    button.addEventListener('click', () => { editorCategory = category.key; editorPage = 0; $('#settings-search').value = ''; renderSettings(); [...nav.children].find(n => n.dataset.category === category.key)?.focus(); }); nav.append(button);
  }
  const active = categories.find(c => c.key === editorCategory);
  $('#settings-category-title').textContent = guided ? (query ? 'Search results' : active?.label || 'Guided settings') : 'All settings';
  $('#settings-category-help').textContent = guided ? (query ? 'Matching settings from every category.' : active?.description || 'No option catalog is available for this core.') : 'Technical names and exact saved values. Use this view for settings not covered by the guide.';
  const uncovered = editorSettings.length - available.length;
  $('#settings-coverage').textContent = !guided ? '' : metadataUnavailable ? 'Core guidance could not be loaded. Refresh to try again, or use Advanced.' : uncovered ? `${uncovered} additional ${uncovered === 1 ? 'setting is' : 'settings are'} available in Advanced.` : '';
  $('#settings-coverage').hidden = !$('#settings-coverage').textContent;
  const matched = available.filter(s => {
    const meta = settingGuide(s);
    return (query ? `${meta?.label || settingTitle(s.key)} ${s.key} ${meta?.description || ''}`.toLocaleLowerCase().includes(query) : !guided || meta?.category === editorCategory);
  });
  const pages = Math.max(1, Math.ceil(matched.length / pageSize)); editorPage = Math.min(editorPage, pages - 1);
  const fields = $('#settings-fields'); fields.replaceChildren();
  $('#settings-count').textContent = `${matched.length} settings${query ? ' matching your search' : ''}`;
  for (const setting of matched.slice(editorPage * pageSize, (editorPage + 1) * pageSize)) {
    const meta = settingGuide(setting), value = editorDraft[setting.key] ?? setting.value;
    const row = element('div', undefined, 'setting-row'), details = element('div', undefined, 'setting-details');
    const label = element('label', guided ? meta.label : settingTitle(setting.key), 'setting-label');
    const id = 'setting-' + setting.key; label.htmlFor = id;
    if (!guided) label.append(element('small', setting.key));
    const description = element('p', meta?.description || 'No description is available for this technical setting. Keep its value unless you know the configuration change you need.', 'setting-description');
    description.id = id + '-help'; details.append(label, description);
    let input, control = element('div', undefined, 'setting-control'), output;
    const choices = guided ? meta.choices : setting.key === 'menu_driver' ? [['xmb', 'XMB'], ['rgui', 'RGUI']] : null;
    if (choices?.length) {
      input = document.createElement('select');
      for (const [value, text] of choices) input.add(new Option(choiceLabel(value, text), value));
      if (![...input.options].some(o => o.value === value)) input.add(new Option(`Current: ${value || '(empty)'}`, value));
      input.value = value;
    } else {
      input = document.createElement('input');
      input.type = guided && ['volume', 'rumble'].includes(meta.control) ? 'range' : (guided && meta.control === 'toggle') || setting.kind === 'bool' ? 'checkbox' : setting.kind;
      if (input.type === 'checkbox') input.checked = value === 'true';
      else { input.value = value; input.maxLength = 4096; if (setting.kind === 'number') { input.step = 'any'; input.required = true; } }
      if (input.type === 'range') { input.min = meta.control === 'volume' ? -80 : 0; input.max = meta.control === 'volume' ? 12 : 100; input.step = meta.control === 'volume' ? '0.1' : '1'; input.value = value; }
      if (['checkbox', 'range'].includes(input.type)) {
        output = element('output'); output.htmlFor = id;
        const updateOutput = () => { output.textContent = input.type === 'checkbox' ? (input.checked ? 'On' : 'Off') : input.value + (meta?.control === 'volume' ? ' dB' : '%'); };
        updateOutput(); input.addEventListener('input', updateOutput);
      }
    }
    if (setting.key === 'audio_volume') { input.min = -80; input.max = 12; }
    if (setting.key === 'input_rumble_gain') { input.min = 0; input.max = 100; }
    input.id = id; input.name = setting.key; input.setAttribute('aria-describedby', description.id);
    input.addEventListener('input', () => {
      const updated = input.type === 'checkbox' ? String(input.checked) : input.value;
      if (updated === editorValues[setting.key]) delete editorDraft[setting.key]; else editorDraft[setting.key] = updated;
      row.classList.toggle('setting-edited', Object.hasOwn(editorDraft, setting.key)); markEdits();
    });
    row.classList.toggle('setting-edited', Object.hasOwn(editorDraft, setting.key));
    control.append(input); if (output) control.append(output); row.append(details, control); fields.append(row);
  }
  if (!matched.length) fields.append(element('p', query ? 'No matching settings. Try another search or switch to Advanced.' : guided ? 'No guided options are available. Refresh to try again, or use Advanced for saved values.' : 'No options are available for this profile.', 'list-message'));
  $('#settings-page').textContent = `Page ${editorPage + 1} of ${pages}`;
  $('.settings-paging').hidden = pages <= 1;
  $('#settings-previous').disabled = editorPage === 0; $('#settings-next').disabled = editorPage >= pages - 1;
}
async function loadEditor() {
  const request = ++editorRequest; editorRevision = '';
  for (const id of ['settings-profile', 'settings-kind', 'refresh-settings']) $('#' + id).disabled = true;
  $('#settings-fields').disabled = true; $('#save-settings').disabled = true;
  $('#settings-result').textContent = 'Loading settings…';
  try {
    const [data, metadata] = await Promise.all([api(editorUrl()), editorProfile && editorKind === 'core-options' ? api('/api/core-metadata?core=' + encodeURIComponent(editorProfile)).catch(() => null) : Promise.resolve({ categories: [], settings: [] })]);
    if (request !== editorRequest) return;
    metadataUnavailable = !metadata;
    // The previous running title may serve new assets before its next restart.
    const catalogs = [metadata?.bundled || {}, metadata?.runtime || metadata || {}];
    editorMetadata = Object.fromEntries(catalogs.flatMap(m => m.settings || []).map(s => [s.key, s]));
    editorCategories = [...new Map(catalogs.flatMap(m => m.categories || []).map(c => [c.key, c])).values()];
    editorSettings = data.settings; editorValues = Object.fromEntries(data.settings.map(s => [s.key, s.value]));
    editorRevision = data.revision; editorDraft = {}; editorPage = 0;
    $('#settings-heading').textContent = editorProfile ? `${editorProfile} · ${editorKind === 'core-options' ? 'Core options' : 'RetroArch overrides'}` : 'Global RetroArch settings';
    $('#settings-help').textContent = editorProfile
      ? 'Saved changes apply when you restart RetroArch. Game-specific settings may take priority. Core options control the emulator; RetroArch overrides change shared preferences for just this core.'
      : 'Start with everyday preferences in Guided, or switch to Advanced for the full configuration. Saved changes apply when you restart RetroArch. Core and game preferences may take priority.';
    renderSettings(); markEdits();
  } catch (error) { if (request === editorRequest) { editorRevision = ''; $('#settings-fields').replaceChildren(); $('#settings-result').textContent = error.message; } }
  finally { if (request === editorRequest) { for (const id of ['settings-profile', 'settings-kind', 'refresh-settings']) $('#' + id).disabled = false; $('#settings-fields').disabled = !connected || !editorRevision; $('#save-settings').disabled = !connected || !editorRevision; } }
}
async function loadSettings() {
  try {
    const [data, profiles] = await Promise.all([api('/api/settings'), api('/api/cores')]);
    settingsValues = Object.fromEntries(data.settings.map(s => [s.key, s.value]));
    const select = $('#settings-profile'); select.replaceChildren(new Option('Global RetroArch', ''));
    for (const name of profiles.cores) select.add(new Option(name, name)); select.value = editorProfile;
    setConnection(connected); updateQuick(); if (!Object.keys(editorDraft).length) await loadEditor();
  } catch (error) { $('#settings-result').textContent = 'Settings unavailable. Reconnect to try again.'; }
}
async function saveSettings(changes) {
  const body = Object.entries(changes).map(([key, value]) => `${key}=${value}`).join('\n');
  if (!body) return;
  await api('/api/settings', { method: 'POST', body, headers: { 'Content-Type': 'text/plain' } });
  Object.assign(settingsValues, changes); updateQuick();
  if (!editorProfile && !Object.keys(editorDraft).length) await loadEditor();
  announce('Settings saved. They will apply the next time you open RetroArch.');
}
$('#settings-form').addEventListener('submit', async event => {
  event.preventDefault(); if (!Object.keys(editorDraft).length) { $('#settings-result').textContent = 'No changes to save.'; return; }
  $('#save-settings').disabled = true; $('#settings-fields').disabled = true;
  for (const id of ['settings-profile', 'settings-kind', 'refresh-settings']) $('#' + id).disabled = true;
  try {
    const body = Object.entries(editorDraft).map(([key, value]) => `${key}=${value}`).join('\n');
    await api(editorUrl(), { method: 'POST', body, headers: { 'Content-Type': 'text/plain', 'X-RetroArch-Revision': editorRevision } });
    if (!editorProfile) { Object.assign(settingsValues, editorDraft); updateQuick(); }
    await loadEditor(); $('#settings-result').textContent = 'Saved for next launch.'; announce('Settings saved. Restart RetroArch to apply them.');
  } catch (error) { $('#settings-result').textContent = error.message; announce(error.message, true); }
  finally { for (const id of ['settings-profile', 'settings-kind', 'refresh-settings']) $('#' + id).disabled = false; $('#save-settings').disabled = !connected || !editorRevision; $('#settings-fields').disabled = !connected || !editorRevision; }
});
function changeProfile() {
  if (Object.keys(editorDraft).length) { $('#settings-profile').value = editorProfile; $('#settings-kind').value = editorKind; announce('Save your changes or discard them with Refresh before switching profiles.', true); return; }
  editorProfile = $('#settings-profile').value; editorKind = $('#settings-kind').value;
  $('#settings-kind-label').hidden = !editorProfile; $('#settings-search').value = ''; loadEditor();
}
for (const mode of ['guided', 'advanced']) $('#settings-' + mode).addEventListener('click', () => {
  editorMode = mode; editorPage = 0;
  for (const name of ['guided', 'advanced']) $('#settings-' + name).setAttribute('aria-pressed', String(name === mode));
  $('#settings-mode-help').textContent = mode === 'guided' ? 'Clear explanations and ready-to-use choices.' : 'Full configuration with technical names and exact values.';
  renderSettings();
});
$('#settings-profile').addEventListener('change', changeProfile); $('#settings-kind').addEventListener('change', changeProfile);
$('#refresh-settings').addEventListener('click', () => { editorDraft = {}; loadSettings(); });
$('#settings-search').addEventListener('input', () => { editorPage = 0; renderSettings(); });
$('#settings-previous').addEventListener('click', () => { --editorPage; renderSettings(); });
$('#settings-next').addEventListener('click', () => { ++editorPage; renderSettings(); });
for (const [id, key, unit] of [['quick-volume', 'audio_volume', ' dB'], ['quick-rumble', 'input_rumble_gain', '%']]) {
  $('#' + id).addEventListener('input', event => { $(`output[for="${id}"]`).textContent = event.target.value + unit; });
  $('#' + id).addEventListener('change', async event => {
    const input = event.target; input.disabled = true;
    try { await saveSettings({ [key]: input.value }); const full = $('#setting-' + key); if (full) full.value = settingsValues[key]; }
    catch (error) { announce(error.message, true); updateQuick(); }
    finally { input.disabled = !connected; }
  });
}
try { if (localStorage.getItem('retroarch-theme') === 'dark') { document.documentElement.dataset.theme = 'dark'; $('#theme').value = 'dark'; } } catch { /* Browser storage may be disabled. */ }
$('#quick-frontend').addEventListener('change', async event => {
  const input = event.target; input.disabled = true;
  try { await saveSettings({ frontend_start: input.value }); }
  catch (error) { announce(error.message, true); updateQuick(); }
  finally { input.disabled = !connected; }
});
$('#theme').addEventListener('change', event => { document.documentElement.dataset.theme = event.target.value; try { localStorage.setItem('retroarch-theme', event.target.value); } catch { /* Theme still works for this visit. */ } });
// Numeric identifiers and prerelease ordering follow SemVer; alphas are published releases too.
function compareVersions(left, right) {
  const parse = value => /^v?(\d+)\.(\d+)\.(\d+)(?:-([0-9A-Za-z.-]+))?(?:\+[0-9A-Za-z.-]+)?$/.exec(value);
  const a = parse(left), b = parse(right); if (!a || !b) return null;
  for (let i = 1; i <= 3; i++) { if (BigInt(a[i]) !== BigInt(b[i])) return BigInt(a[i]) > BigInt(b[i]) ? 1 : -1; }
  if (!a[4] || !b[4]) return a[4] === b[4] ? 0 : !a[4] ? 1 : -1;
  const aa = a[4].split('.'), bb = b[4].split('.');
  for (let i = 0; i < Math.max(aa.length, bb.length); i++) {
    if (aa[i] === bb[i]) continue;
    if (aa[i] === undefined || bb[i] === undefined) return aa[i] === undefined ? -1 : 1;
    const an = /^\d+$/.test(aa[i]), bn = /^\d+$/.test(bb[i]);
    if (an && bn) { if (BigInt(aa[i]) !== BigInt(bb[i])) return BigInt(aa[i]) > BigInt(bb[i]) ? 1 : -1; else continue; }
    if (an !== bn) return an ? -1 : 1;
    return aa[i] > bb[i] ? 1 : -1;
  }
  return 0;
}
let installed = null, latestUpdate = null, updateState = 'idle', updatePolling = false;
function updateButton() {
  const busy = ['downloading', 'verifying', 'ready', 'installing'].includes(updateState);
  $('#download-update').disabled = !connected || !latestUpdate || busy;
  $('#download-update').textContent = busy ? 'Update in progress' : installed && !installed.release ? 'Install latest release' : 'Update RetroArch';
}
function renderUpdate(data) {
  updateState = data.state;
  $('#update-panel').hidden = data.state === 'idle' && $('#update-error').hidden;
  $('#update-tag').textContent = data.tag || '';
  $('#update-development-note').hidden = !installed || Boolean(installed.release);
  const downloading = data.state === 'downloading';
  $('#update-message').textContent = data.message + (downloading && data.received ? ` ${bytes(data.received)} received.` : '');
  $('#update-progress').hidden = !['downloading', 'verifying'].includes(data.state);
  $('#install-update').hidden = data.state !== 'ready';
  $('#install-update').disabled = !connected;
  updateButton();
}
async function loadUpdate() {
  if (updatePolling) return;
  updatePolling = true;
  try { renderUpdate(await api('/api/update')); }
  catch (error) {
    if (updateState === 'installing') {
      $('#update-message').textContent = 'PS5 RetroArch has closed for installation. Reopen it from your launcher when installation finishes, then check the version above.';
    } else if (updateState !== 'idle') {
      $('#update-message').textContent = 'Update status is unavailable. Reconnect to check its progress before retrying.';
    }
  } finally { updatePolling = false; }
}
async function loadAlerts() {
  const button = $('#refresh-alerts'); button.disabled = true;
  try {
    const data = await api('/api/alerts'), list = $('#alerts-list'); list.replaceChildren();
    const groups = new Map();
    for (const alert of data.alerts) {
      if (!groups.has(alert.core)) { const group = element('section', undefined, 'alert-group'); group.append(element('h3', alert.core)); groups.set(alert.core, group); list.append(group); }
      const row = element('div', undefined, 'alert-item');
      row.append(element('strong', alert.title));
      if (alert.path) row.append(element('code', alert.path));
      else row.append(element('p', alert.message));
      groups.get(alert.core).append(row);
    }
    const count = data.alerts.length;
    $('#alerts-count').textContent = count; $('#alerts-count').hidden = !count;
    $('#alerts-summary').textContent = count ? `${count} ${count === 1 ? 'item needs' : 'items need'} attention across your installed cores.` : 'No missing required BIOS or system files found.';
    $('#alerts-details').hidden = !count;
    $('.alerts-panel').dataset.state = count ? 'warning' : 'clear';
  } catch (error) { $('#alerts-summary').textContent = 'Couldn’t check required files. Reconnect and try again.'; $('#alerts-details').hidden = true; $('#alerts-count').hidden = true; }
  finally { button.disabled = false; }
}
$('#refresh-alerts').addEventListener('click', loadAlerts);
$('#download-update').addEventListener('click', async () => {
  if (!latestUpdate) return;
  $('#download-update').disabled = true; $('#update-error').hidden = true;
  try { renderUpdate(await api(`/api/update/download?tag=${encodeURIComponent(latestUpdate)}`, { method: 'POST' })); }
  catch (error) { $('#update-panel').hidden = false; $('#update-error').textContent = error.message; $('#update-error').hidden = false; updateButton(); }
});
$('#install-update').addEventListener('click', async () => {
  $('#install-update').disabled = true; $('#update-error').hidden = true;
  renderUpdate({ state: 'installing', tag: $('#update-tag').textContent, message: 'Requesting installation…' });
  try { renderUpdate(await api('/api/update/install', { method: 'POST' })); }
  catch (error) { $('#update-error').textContent = error.message + ' Rechecking installation status…'; $('#update-error').hidden = false; await loadUpdate(); }
});
setInterval(() => { if (!document.hidden && connected) loadUpdate(); }, 2000);
async function checkRelease() {
  const button = $('#check-release'), bar = $('.release-bar'); button.disabled = true;
  latestUpdate = null; updateButton();
  $('#release-summary').textContent = 'Checking releases…'; bar.dataset.state = 'checking';
  try {
    { const response = await fetch('/version.json', { cache: 'no-store', signal: AbortSignal.timeout(10000) }); if (!response.ok) throw new Error(); installed = await response.json(); }
    $('#release-title').textContent = installed.release || 'Development build';
    const response = await fetch(`https://api.github.com/repos/${repo}/releases?per_page=30`, { headers: { Accept: 'application/vnd.github+json' }, signal: AbortSignal.timeout(12000) });
    if (!response.ok) throw new Error();
    const releases = await response.json(); if (!Array.isArray(releases)) throw new Error();
    const published = releases.filter(r => !r.draft && r.published_at && typeof r.tag_name === 'string');
    published.sort((a, b) => { const order = compareVersions(a.tag_name, b.tag_name); return order === null ? Date.parse(b.published_at) - Date.parse(a.published_at) : -order; });
    const latest = published[0];
    if (!latest) { $('#release-summary').textContent = 'No published releases yet'; $('#show-notes').hidden = true; bar.dataset.state = 'unknown'; return; }
    const comparison = installed.release ? compareVersions(installed.release, latest.tag_name) : null;
    const assetNames = new Set((latest.assets || []).map(asset => asset.name));
    const archive = `PS5_RetroArch-${latest.tag_name}.zip`;
    if ((comparison === null || comparison < 0) && assetNames.has(archive) && assetNames.has(archive + '.sha256')) latestUpdate = latest.tag_name;
    bar.dataset.state = comparison === null ? 'development' : comparison < 0 ? 'update' : 'current';
    $('#release-summary').textContent = comparison === null ? `Latest release: ${latest.tag_name}` : comparison < 0 ? `Update available · ${latest.tag_name}` : 'You’re up to date';
    $('.release-icon').innerHTML = comparison !== null && comparison >= 0 ? '<circle cx="12" cy="12" r="10"/><path d="m7 12 3 3 7-7"/>' : '<circle cx="12" cy="12" r="10"/><path d="M12 11v6 M12 7v.1"/>';
    $('#notes-title').textContent = `What’s new in ${latest.tag_name}`;
    $('#release-date').textContent = new Date(latest.published_at).toLocaleDateString(undefined, { dateStyle: 'long' });
    $('#release-notes').textContent = latest.body || 'No release notes were provided for this version.';
    $('#release-link').href = `https://github.com/${repo}/releases/tag/${encodeURIComponent(latest.tag_name)}`;
    $('#show-notes').hidden = false;
  } catch { bar.dataset.state = 'unknown'; $('#release-summary').textContent = 'Couldn’t check updates. Try again.'; }
  finally { button.disabled = false; updateButton(); }
}
function showNotes(show) { $('#release-details').hidden = !show; $('#show-notes').setAttribute('aria-expanded', String(show)); if (show) $('#release-details').scrollIntoView({ behavior: 'smooth', block: 'start' }); }
$('#show-notes').addEventListener('click', () => showNotes($('#release-details').hidden));
$('#close-notes').addEventListener('click', () => { showNotes(false); $('#show-notes').focus(); });
$('#check-release').addEventListener('click', checkRelease);
$('#reconnect').addEventListener('click', reconnect);
$('#refresh-content').addEventListener('click', () => loadContent(currentPath));
$('#file-search').addEventListener('input', renderContent);
window.addEventListener('hashchange', navigate);
setInterval(() => { if (!document.hidden) reconnect(); }, 10000);
navigate(); drawTransfers(); reconnect(); checkRelease();

// The games and their media (src/scraper.h): the shared library every frontend reads.
const KIND_NAMES = { cover: 'Cover', screenshot: 'Screenshot', title: 'Title', logo: 'Logo', video: 'Video' };
let library = { systems: [] }, selectedGames = new Map(), shownGames = 300, scraperSettings = null, jobTimer = null, jobShown = null;
function gameName(game) { return game.name || game.label; }
// What tells versions of one game apart: the (region), (Rev 1), (Proto), [a1], (1992)
// tags of its label or, failing that, of its file name. Two cards with one title are
// two versions, and their tags say which.
function gameTags(game) {
  const tags = [], text = /[(\[]/.test(game.label) ? game.label : game.key;
  for (const match of text.matchAll(/\(([^)]*)\)|\[([^\]]*)\]/g)) tags.push((match[1] ?? match[2]).trim());
  return tags.filter(Boolean);
}
function visibleGames() {
  const system = $('#games-system').value, words = $('#games-search').value.trim().toLowerCase(), missing = $('#games-missing').checked;
  const out = [];
  for (const s of library.systems) {
    if (system && s.id !== system) continue;
    for (const game of s.games) {
      if (words && !gameName(game).toLowerCase().includes(words) && !game.label.toLowerCase().includes(words)) continue;
      if (missing && game.media.includes('cover') && game.media.includes('screenshot')) continue;
      out.push({ system: s, game });
    }
  }
  return out;
}
function mediaUrl(system, game, kind) {
  return `/api/library/media?system=${encodeURIComponent(system)}&game=${encodeURIComponent(game.key)}&kind=${kind}&v=${encodeURIComponent(game.scraped || '')}`;
}
function drawGames() {
  const grid = $('#games-grid'), list = visibleGames();
  grid.replaceChildren();
  const total = library.systems.reduce((n, s) => n + s.games.length, 0);
  const withCovers = library.systems.reduce((n, s) => n + s.games.filter(g => g.media.includes('cover')).length, 0);
  $('#games-summary').textContent = `${total} games in ${library.systems.length} systems · ${withCovers} with covers · shared by RetroArch, EmulationStation and every frontend`;
  if (!list.length) { grid.append(element('p', total ? 'No game matches.' : 'No games yet. Add content or scan it in RetroArch, then refresh.', 'list-message')); }
  for (const { system, game } of list.slice(0, shownGames)) {
    const card = element('article', undefined, 'game-card'), cover = element('div', undefined, 'game-cover'), info = element('div', undefined, 'game-info');
    const pick = document.createElement('input'); pick.type = 'checkbox'; pick.className = 'game-select';
    pick.setAttribute('aria-label', `Select ${gameName(game)}`); pick.checked = selectedGames.has(game.path);
    card.setAttribute('aria-selected', pick.checked);
    pick.addEventListener('change', () => { if (pick.checked) selectedGames.set(game.path, system.id); else selectedGames.delete(game.path); card.setAttribute('aria-selected', pick.checked); drawSelection(); });
    if (game.media.includes('cover')) {
      const img = document.createElement('img'); img.loading = 'lazy'; img.alt = `${gameName(game)} cover`; img.src = mediaUrl(system.id, game, 'cover');
      img.addEventListener('error', () => img.replaceWith(icon('file'))); cover.append(img);
    } else cover.append(icon('file'));
    const badges = element('div', undefined, 'media-badges');
    for (const kind of game.media) badges.append(element('span', KIND_NAMES[kind] || kind));
    const tags = element('div', undefined, 'version-tags');
    for (const tag of gameTags(game)) tags.append(element('span', tag));
    card.title = game.path.split('/').pop();
    info.append(element('strong', gameName(game)), tags, element('small', system.name), badges);
    card.append(pick, cover, info); grid.append(card);
  }
  if (list.length > shownGames) {
    const more = element('button', `Show ${Math.min(300, list.length - shownGames)} more of ${list.length - shownGames}`, 'secondary games-more');
    more.addEventListener('click', () => { shownGames += 300; drawGames(); }); grid.append(more);
  }
  drawSelection();
}
function drawSelection() {
  $('#games-selection').textContent = selectedGames.size ? `${selectedGames.size} game${selectedGames.size === 1 ? '' : 's'} selected` : 'Select games to scrape them, or scrape whole systems.';
  $('#scrape-scope-selected').textContent = `Selected games (${selectedGames.size})`;
}
async function loadGames() {
  try {
    library = await api('/api/library', { signal: AbortSignal.timeout(60000) });
    const select = $('#games-system'), chosen = select.value;
    select.replaceChildren(new Option('All systems', ''));
    for (const s of library.systems) select.add(new Option(`${s.name} (${s.games.length})`, s.id));
    if ([...select.options].some(o => o.value === chosen)) select.value = chosen;
    drawGames();
  } catch (error) { $('#games-grid').replaceChildren(element('p', error.message, 'list-message inline-error')); }
}
for (const id of ['#games-system', '#games-missing']) $(id).addEventListener('change', () => { shownGames = 300; drawGames(); });
$('#games-search').addEventListener('input', () => { shownGames = 300; drawGames(); });
$('#refresh-games').addEventListener('click', loadGames);

// The scrape panel: the download method first, remembered on the console.
const METHOD_NAMES = { pc: 'Download through this PC and transfer to PS5', ps5: 'Download directly on PS5' };
let scrapeSource = 'libretro', scrapeKinds = new Set();
const RECOMMENDED = ['cover', 'screenshot', 'title'];
function chosenSource() { return scraperSettings.sources.find(s => s.id === scrapeSource) || scraperSettings.sources[0]; }
function drawSources() {
  const box = $('#scrape-sources'); box.replaceChildren();
  for (const source of scraperSettings.sources) {
    const card = element('label', undefined, 'source-card'), radio = document.createElement('input');
    radio.type = 'radio'; radio.name = 'scrape-source'; radio.value = source.id; radio.checked = source.id === scrapeSource; radio.disabled = !source.available;
    radio.addEventListener('change', () => { scrapeSource = source.id; drawKinds(); });
    const text = element('span'), title = element('strong', source.name);
    if (!source.available) title.append(element('em', 'Coming', 'soon-tag'));
    if (source.account) title.append(element('em', 'Account needed', 'account-tag'));
    text.append(title, element('small', source.description)); card.append(radio, text);
    if (!source.available) card.classList.add('unavailable');
    box.append(card);
  }
}
function drawKinds() {
  const source = chosenSource(), box = $('#scrape-kinds'); box.replaceChildren();
  for (const kind of scraperSettings.catalog) {
    const has = source.kinds.includes(kind.id), card = element('label', undefined, 'kind-card'), box2 = document.createElement('input');
    box2.type = 'checkbox'; box2.value = kind.id; box2.checked = has && scrapeKinds.has(kind.id); box2.disabled = !has;
    box2.addEventListener('change', () => { if (box2.checked) scrapeKinds.add(kind.id); else scrapeKinds.delete(kind.id); drawSummary(); });
    const others = scraperSettings.sources.filter(s => s.id !== source.id && s.kinds.includes(kind.id)).map(s => s.name + (s.available ? '' : ' (coming)'));
    const text = element('span');
    text.append(element('strong', kind.name), element('small', kind.description),
      element('small', has ? `From ${source.name}` : `Not from ${source.name}` + (others.length ? ` · from ${others.join(', ')}` : ''), has ? 'kind-source' : 'kind-source missing'));
    if (!has) card.classList.add('unavailable');
    card.append(box2, text); box.append(card);
  }
  drawSummary();
}
function setKinds(ids) { scrapeKinds = new Set(ids.filter(id => chosenSource().kinds.includes(id))); drawKinds(); }
function scopeGames() {
  if ($('input[name="scrape-scope"]:checked').value === 'selected') return selectedGames.size;
  const system = $('#games-system').value;
  return library.systems.filter(s => !system || s.id === system).reduce((n, s) => n + s.games.length, 0);
}
function drawSummary() {
  const kinds = [...scrapeKinds].filter(id => chosenSource().kinds.includes(id)), games = scopeGames();
  const names = scraperSettings.catalog.filter(k => kinds.includes(k.id)).map(k => k.name.toLowerCase());
  const system = $('#games-system').value, systemName = library.systems.find(s => s.id === system)?.name;
  $('#scrape-scope-selected').textContent = `Selected games (${selectedGames.size})`;
  $('#scrape-scope-systems').textContent = systemName ? `Every game of ${systemName}` : `Every game of all ${library.systems.length} systems`;
  $('#scrape-summary').textContent = !kinds.length ? 'Choose at least one media type in step 3.' : !games ? 'Choose games in step 4: select some on the page, or scrape whole systems.'
    : `Ready: ${names.join(', ')} for ${games} game${games === 1 ? '' : 's'} from ${chosenSource().name}${$('#scrape-overwrite').checked ? ', replacing what is there' : ', keeping what is already on the PS5'}.`;
}
async function openScrape() {
  $('#scrape-panel').hidden = false;
  try { scraperSettings = await api('/api/scraper/settings'); } catch (error) { announce(error.message, true); return; }
  for (const radio of $$('input[name="scrape-method"]')) radio.checked = radio.value === scraperSettings.mode;
  $('#scrape-method-saved').textContent = scraperSettings.mode ? `Last used on this console: ${METHOD_NAMES[scraperSettings.mode]}. You can change it.` : 'Choose one to continue. Your choice is remembered on this console.';
  scrapeSource = scraperSettings.sources.some(s => s.id === scraperSettings.source && s.available) ? scraperSettings.source : 'libretro';
  scrapeKinds = new Set(scraperSettings.kinds);
  $('#scrape-region').value = scraperSettings.region;
  $('#scrape-language').value = scraperSettings.language;
  if (!selectedGames.size) $('input[name="scrape-scope"][value="systems"]').checked = true;
  drawSources(); drawKinds();
  $('#scrape-panel').scrollIntoView({ block: 'start' });
}
$('#kinds-recommended').addEventListener('click', () => setKinds(RECOMMENDED));
$('#kinds-all').addEventListener('click', () => setKinds(chosenSource().kinds));
$('#kinds-none').addEventListener('click', () => setKinds([]));
for (const node of $$('input[name="scrape-scope"]')) node.addEventListener('change', drawSummary);
$('#scrape-overwrite').addEventListener('change', drawSummary);
$('#scrape-open').addEventListener('click', openScrape);
$('#scrape-close').addEventListener('click', () => { $('#scrape-panel').hidden = true; });
$('#scrape-start').addEventListener('click', async () => {
  const method = $('input[name="scrape-method"]:checked')?.value;
  if (!method) { $('#scrape-result').textContent = 'Choose where the media should be downloaded first.'; $('.scrape-method').scrollIntoView({ block: 'center' }); return; }
  const kinds = [...scrapeKinds].filter(id => chosenSource().kinds.includes(id));
  if (!kinds.length) { $('#scrape-result').textContent = 'Choose at least one media type in step 3.'; return; }
  const scope = $('input[name="scrape-scope"]:checked').value;
  let lines = [];
  if (scope === 'selected') lines = [...selectedGames].map(([path, system]) => `${system}\t${path}`);
  else { const system = $('#games-system').value; lines = library.systems.filter(s => !system || s.id === system).map(s => `${s.id}\t`); }
  if (!lines.length) { $('#scrape-result').textContent = 'Select games first, or choose every game of the systems shown.'; return; }
  const query = new URLSearchParams({ mode: method, source: scrapeSource, kinds: kinds.join(','), region: $('#scrape-region').value, language: $('#scrape-language').value, overwrite: $('#scrape-overwrite').checked ? '1' : '0' });
  $('#scrape-start').disabled = true; $('#scrape-result').textContent = 'Starting…';
  try {
    await api('/api/scraper/settings?' + query, { method: 'POST' });
    const started = await api('/api/scraper/start?' + query, { method: 'POST', body: lines.join('\n'), headers: { 'Content-Type': 'text/plain' }, signal: AbortSignal.timeout(60000) });
    $('#scrape-result').textContent = ''; $('#scrape-panel').hidden = true; drawJob(started.job); pollJob();
  } catch (error) { $('#scrape-result').textContent = error.message; }
  finally { $('#scrape-start').disabled = false; }
});

// The job: polled while this page is open; the console keeps it whether or not it is.
function count(job, ...states) { return states.reduce((n, s) => n + (job.counts[s] || 0), 0); }
function drawJob(job) {
  const panel = $('#scrape-job');
  if (!job) { panel.hidden = true; return; }
  panel.hidden = false; jobShown = job.id;
  const total = job.total, settled = count(job, 'done', 'partial', 'skipped', 'unmatched', 'ambiguous', 'failed');
  const identified = total - count(job, 'pending', 'working');
  const running = job.state === 'running';
  const stateText = { running: 'Running', cancelled: 'Cancelled', done: 'Finished', interrupted: 'Interrupted' }[job.state] || job.state;
  $('#job-summary').textContent = `${stateText} · ${METHOD_NAMES[job.mode]} · ${settled} of ${total} games settled · ${count(job, 'done')} complete, ${count(job, 'partial')} partly, ${count(job, 'skipped')} already had media, ${count(job, 'unmatched', 'ambiguous')} need you, ${count(job, 'failed')} failed`;
  $('#job-identified').value = total ? identified / total * 100 : 0; $('#job-identified-text').textContent = `${identified} of ${total} games`;
  const wanted = job.downloaded.files + job.tasks.queued + job.tasks.leased;
  $('#job-downloaded').value = job.mode === 'pc' ? (wanted ? job.downloaded.files / wanted * 100 : 0) : (total ? settled / total * 100 : 0);
  $('#job-downloaded-text').textContent = `${job.downloaded.files} files · ${bytes(job.downloaded.bytes)}`;
  $('#job-transferred-row').hidden = job.mode !== 'pc';
  if (job.mode === 'pc') {
    $('#job-transferred').value = job.downloaded.files ? job.transferred.files / Math.max(job.downloaded.files, 1) * 100 : 0;
    $('#job-transferred-text').textContent = `${job.transferred.files} files · ${bytes(job.transferred.bytes)}`;
  }
  $('#job-helper').hidden = job.mode !== 'pc' || !running;
  $('#job-helper-command').textContent = `python3 ps5-media-helper.py http://${location.host} ${job.id}`;
  $('#job-cancel').hidden = !running;
  $('#job-resume').hidden = running || job.state === 'done' && !count(job, 'pending', 'failed');
  const problems = $('#job-problems'); problems.replaceChildren();
  for (const p of job.problems) {
    const row = element('div', undefined, 'problem-row'), text = element('div');
    text.append(element('strong', p.label), element('p', p.message, p.state === 'failed' ? 'inline-error' : 'muted'));
    const actions = element('div', undefined, 'problem-actions');
    const resolve = async (action, value) => { try { drawJob((await api(`/api/scraper/resolve?id=${job.id}&item=${p.item}&action=${action}&value=${encodeURIComponent(value || '')}`, { method: 'POST', signal: AbortSignal.timeout(60000) })).job); pollJob(); } catch (error) { announce(error.message, true); } };
    if (p.candidates.length) {
      const pick = document.createElement('select'); pick.setAttribute('aria-label', `Match for ${p.label}`);
      for (const c of p.candidates) pick.add(new Option(c, c));
      const use = element('button', 'Use this match', 'primary compact'); use.addEventListener('click', () => resolve('choose', pick.value));
      actions.append(pick, use);
    }
    const search = document.createElement('input'); search.type = 'search'; search.placeholder = 'Search by name'; search.value = p.label; search.setAttribute('aria-label', `Search for ${p.label}`);
    const find = element('button', 'Search', 'secondary'); find.addEventListener('click', () => resolve('search', search.value));
    const skip = element('button', 'Skip', 'secondary'); skip.addEventListener('click', () => resolve('skip'));
    actions.append(search, find, skip); row.append(text, actions); problems.append(row);
  }
  if (!running && jobTimer) { clearInterval(jobTimer); jobTimer = null; loadGames(); }
}
async function loadJob() {
  try { const { job } = await api('/api/scraper/job'); drawJob(job); if (job && job.state === 'running') pollJob(); } catch {}
}
function pollJob() {
  if (jobTimer) return;
  jobTimer = setInterval(async () => {
    if (document.hidden) return;
    try { const { job } = await api('/api/scraper/job' + (jobShown ? '?id=' + jobShown : '')); drawJob(job); } catch {}
  }, 1500);
}
$('#job-cancel').addEventListener('click', async () => { try { await api('/api/scraper/cancel?id=' + jobShown, { method: 'POST' }); loadJob(); } catch (error) { announce(error.message, true); } });
$('#job-resume').addEventListener('click', async () => { try { drawJob((await api('/api/scraper/resume?id=' + jobShown, { method: 'POST' })).job); pollJob(); } catch (error) { announce(error.message, true); } });
$('#job-helper-copy').addEventListener('click', async () => { try { await navigator.clipboard.writeText($('#job-helper-command').textContent); announce('Command copied.'); } catch { announce('Select the command and copy it.'); } });
