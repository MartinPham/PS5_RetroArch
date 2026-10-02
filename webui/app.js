/* RetroArch PS5 WebUI. No external runtime or browser storage of console credentials. */
'use strict';
const $ = (selector, root = document) => root.querySelector(selector);
const $$ = (selector, root = document) => [...root.querySelectorAll(selector)];
const repo = 'mihawk-99/PS5_RetroArch';
let token = '', connected = false, freeBytes = null, uploadLimit = 64 * 1024 ** 3;
let currentPath = '', entries = [], folderRequest = 0, settingsValues = {};
let sessionRequest = null, sending = false, nextTransfer = 0;
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
  $('#connection span:last-child').textContent = ok ? 'RetroArch is running' : 'Console disconnected';
  $('#connection-notice').hidden = ok;
  for (const id of ['destination', 'dropzone', 'browse-files', 'upload-here', 'folder-name', 'quick-volume', 'quick-rumble', 'settings-fields', 'save-settings']) {
    $('#' + id).disabled = !ok || (['settings-fields', 'save-settings'].includes(id) && !editorRevision) || (['quick-volume', 'quick-rumble', 'settings-fields', 'save-settings'].includes(id) && !Object.keys(settingsValues).length);
  }
  $('#folder-form button').disabled = !ok;
}
async function reconnect() {
  if (sessionRequest) return sessionRequest;
  sessionRequest = (async () => {
    try {
      const state = await api('/api/status');
      const recovered = !connected || token !== state.token;
      token = state.token; freeBytes = state.freeBytes; uploadLimit = state.uploadLimit;
      setConnection(true);
      $('#storage-info').textContent = freeBytes === null ? 'Games and files stored on your PS5' : `${bytes(freeBytes)} free on the console`;
      if (recovered) await Promise.all([loadLibrary(), loadSettings(), loadContent(currentPath)]);
      return true;
    } catch (error) { setConnection(false); return false; }
    finally { sessionRequest = null; }
  })();
  return sessionRequest;
}
function pageFromHash() {
  const name = location.hash.slice(1).split('?')[0];
  return ['content', 'transfers', 'settings'].includes(name) ? name : 'overview';
}
function navigate() {
  const page = pageFromHash();
  $$('.page').forEach(node => { node.hidden = node.id !== page; });
  $$('[data-page]').forEach(node => { if (node.dataset.page === page) node.setAttribute('aria-current', 'page'); else node.removeAttribute('aria-current'); });
  $('#page-title').textContent = page[0].toUpperCase() + page.slice(1);
  if (page === 'content') loadContent(currentPath);
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
          cancel.addEventListener('click', () => { transfer.state = 'cancelled'; transfer.message = 'Cancelled'; if (transfer.xhr) transfer.xhr.abort(); transfer.file = null; drawTransfers(); });
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
async function sendNext() {
  if (sending) return;
  sending = true;
  try {
    for (const transfer of transfers) {
      if (transfer.state !== 'queued') continue;
      if (!connected && !await reconnect()) { transfer.state = 'failed'; transfer.message = 'Console disconnected. Reconnect, then choose this file again.'; transfer.file = null; drawTransfers(); continue; }
      await new Promise(resolve => {
        const xhr = new XMLHttpRequest(); transfer.xhr = xhr; transfer.state = 'uploading';
        transfer.message = 'Uploading…'; drawTransfers();
        xhr.open('PUT', '/api/upload?path=' + encodeURIComponent(transfer.path));
        xhr.setRequestHeader('X-RetroArch-Token', token);
        let lastProgress = 0;
        xhr.upload.onprogress = event => {
          if (!event.lengthComputable) return;
          transfer.percent = Math.floor(event.loaded / event.total * 100);
          transfer.message = transfer.percent === 100 ? 'Finishing on the console…' : `${transfer.percent}% · ${bytes(event.loaded)} of ${bytes(event.total)}`;
          if (Date.now() - lastProgress > 150 || transfer.percent === 100) { lastProgress = Date.now(); drawTransfers(); }
        };
        xhr.onload = () => {
          if (xhr.status === 201) { transfer.state = 'complete'; transfer.message = `Uploaded · ${bytes(transfer.file.size)}`; }
          else { transfer.state = 'failed'; try { transfer.message = JSON.parse(xhr.responseText).error; } catch { transfer.message = 'Upload failed. Choose the file again to retry.'; } }
        };
        xhr.onerror = () => { transfer.state = 'failed'; transfer.message = 'Connection lost. Reconnect, then choose this file again.'; setConnection(false); };
        xhr.onabort = () => { transfer.state = 'cancelled'; transfer.message = 'Cancelled'; };
        xhr.onloadend = () => { transfer.file = null; transfer.xhr = null; drawTransfers(); resolve(); };
        xhr.send(transfer.file);
      });
    }
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
}
let editorSettings = [], editorValues = {}, editorDraft = {}, editorRevision = '', editorPage = 0, editorRequest = 0;
let editorProfile = '', editorKind = 'core-options';
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
function renderSettings() {
  const query = $('#settings-search').value.toLocaleLowerCase();
  const matched = editorSettings.filter(s => (settingTitle(s.key) + ' ' + s.key).toLocaleLowerCase().includes(query));
  const pages = Math.max(1, Math.ceil(matched.length / pageSize)); editorPage = Math.min(editorPage, pages - 1);
  const fields = $('#settings-fields'); fields.replaceChildren();
  $('#settings-count').textContent = `${matched.length} settings${query ? ' matching your search' : ''}`;
  for (const setting of matched.slice(editorPage * pageSize, (editorPage + 1) * pageSize)) {
    const value = editorDraft[setting.key] ?? setting.value;
    const row = element('div', undefined, 'setting-row'), label = element('label', settingTitle(setting.key), 'setting-label');
    const id = 'setting-' + setting.key; label.htmlFor = id; label.append(element('small', setting.key));
    let input;
    if (setting.key === 'menu_driver') { input = document.createElement('select'); for (const name of ['xmb', 'rgui']) input.add(new Option(name.toUpperCase(), name)); input.value = value; }
    else { input = document.createElement('input'); input.type = setting.kind === 'bool' ? 'checkbox' : setting.kind;
      if (setting.kind === 'bool') input.checked = value === 'true';
      else { input.value = value; input.maxLength = 4096; if (setting.kind === 'number') { input.step = 'any'; input.required = true; } }
    }
    if (setting.key === 'audio_volume') { input.min = -80; input.max = 12; }
    if (setting.key === 'input_rumble_gain') { input.min = 0; input.max = 100; }
    input.id = id; input.name = setting.key;
    input.addEventListener('input', () => {
      const updated = input.type === 'checkbox' ? String(input.checked) : input.value;
      if (updated === editorValues[setting.key]) delete editorDraft[setting.key]; else editorDraft[setting.key] = updated;
      markEdits();
    });
    row.append(label, input); fields.append(row);
  }
  if (!matched.length) fields.append(element('p', editorSettings.length ? 'No matching settings. Try another search.' : 'No saved core options yet. Open and close a game with this core, then refresh. You can also choose RetroArch overrides.', 'list-message'));
  $('#settings-page').textContent = `Page ${editorPage + 1} of ${pages}`;
  $('#settings-previous').disabled = editorPage === 0; $('#settings-next').disabled = editorPage >= pages - 1;
}
async function loadEditor() {
  const request = ++editorRequest; editorRevision = '';
  for (const id of ['settings-profile', 'settings-kind', 'refresh-settings']) $('#' + id).disabled = true;
  $('#settings-fields').disabled = true; $('#save-settings').disabled = true;
  $('#settings-result').textContent = 'Loading settings…';
  try {
    const data = await api(editorUrl()); if (request !== editorRequest) return;
    editorSettings = data.settings; editorValues = Object.fromEntries(data.settings.map(s => [s.key, s.value]));
    editorRevision = data.revision; editorDraft = {}; editorPage = 0;
    $('#settings-heading').textContent = editorProfile ? `${editorProfile} · ${editorKind === 'core-options' ? 'Core options' : 'RetroArch overrides'}` : 'Global RetroArch settings';
    $('#settings-help').textContent = editorProfile
      ? 'Changes apply when you restart RetroArch. Game and folder overrides can take priority. Core options use the values saved by the emulator; RetroArch overrides inherit global values until edited here.'
      : 'All settings from the console’s saved configuration are available here. Changes apply when you restart RetroArch. Core, game and folder overrides can take priority. Core profiles appear after you open and close content with that core.';
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
let installed = null;
async function checkRelease() {
  const button = $('#check-release'), bar = $('.release-bar'); button.disabled = true;
  $('#release-summary').textContent = 'Checking releases…'; bar.dataset.state = 'checking';
  try {
    if (!installed) { const response = await fetch('/version.json', { cache: 'no-store', signal: AbortSignal.timeout(10000) }); if (!response.ok) throw new Error(); installed = await response.json(); }
    $('#release-title').textContent = installed.release || 'Development build';
    const response = await fetch(`https://api.github.com/repos/${repo}/releases?per_page=30`, { headers: { Accept: 'application/vnd.github+json' }, signal: AbortSignal.timeout(12000) });
    if (!response.ok) throw new Error();
    const releases = await response.json(); if (!Array.isArray(releases)) throw new Error();
    const published = releases.filter(r => !r.draft && r.published_at && typeof r.tag_name === 'string');
    published.sort((a, b) => { const order = compareVersions(a.tag_name, b.tag_name); return order === null ? Date.parse(b.published_at) - Date.parse(a.published_at) : -order; });
    const latest = published[0];
    if (!latest) { $('#release-summary').textContent = 'No published releases yet'; $('#show-notes').hidden = true; bar.dataset.state = 'unknown'; return; }
    const comparison = installed.release ? compareVersions(installed.release, latest.tag_name) : null;
    bar.dataset.state = comparison === null ? 'development' : comparison < 0 ? 'update' : 'current';
    $('#release-summary').textContent = comparison === null ? `Latest release: ${latest.tag_name}` : comparison < 0 ? `Update available · ${latest.tag_name}` : 'You’re up to date';
    $('.release-icon').innerHTML = comparison !== null && comparison >= 0 ? '<circle cx="12" cy="12" r="10"/><path d="m7 12 3 3 7-7"/>' : '<circle cx="12" cy="12" r="10"/><path d="M12 11v6 M12 7v.1"/>';
    $('#notes-title').textContent = `What’s new in ${latest.tag_name}`;
    $('#release-date').textContent = new Date(latest.published_at).toLocaleDateString(undefined, { dateStyle: 'long' });
    $('#release-notes').textContent = latest.body || 'No release notes were provided for this version.';
    $('#release-link').href = `https://github.com/${repo}/releases/tag/${encodeURIComponent(latest.tag_name)}`;
    $('#show-notes').hidden = false;
  } catch { bar.dataset.state = 'unknown'; $('#release-summary').textContent = 'Couldn’t check updates. Try again.'; }
  finally { button.disabled = false; }
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
