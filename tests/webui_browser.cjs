/* Run against an isolated native webui_server_main fixture. No production console writes. */
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const { chromium } = require(process.env.PLAYWRIGHT_PATH || 'playwright');
const base = process.env.WEBUI_TEST_URL || 'http://127.0.0.1:6770';
const out = path.resolve('.impeccable/review');
const release = tag => ({ tag_name: tag, published_at: '2026-10-01T13:43:33Z', draft: false, prerelease: true, body: '## What changed\nMore reliable emulation.\n<script>window.notesExecuted=true</script>' });
(async () => {
  const browser = await chromium.launch({ headless: true, executablePath: process.env.CHROMIUM_PATH || '/usr/bin/chromium' });
  try {
    const context = await browser.newContext({ viewport: { width: 1504, height: 1046 }, reducedMotion: 'reduce' });
    const page = await context.newPage();
    const errors = []; page.on('pageerror', e => errors.push(e.message));
    let releases = [release('v0.5.7-alpha.5')], fail = false;
    await page.route('https://api.github.com/**', route => fail ? route.abort() : route.fulfill({ json: releases }));
    await page.goto(base);
    await page.waitForFunction(() => document.querySelector('#release-summary').textContent.includes('up to date'));
    await page.waitForFunction(() => !document.querySelector('#browse-files').disabled);
    assert.equal(await page.locator('.folder-row').count(), 3);
    assert.equal(await page.locator('#connection').innerText(), 'RetroArch is running');
    for (const [latest, state] of [['v0.5.7-alpha.6', 'update'], ['v0.5.7-alpha.10', 'update'], ['v0.5.7-alpha.4', 'current'], ['v0.5.7', 'update'], ['v0.5.6', 'current'], ['unversioned', 'development']]) {
      releases = [release(latest)]; await page.locator('#check-release').click();
      await page.waitForFunction(expected => document.querySelector('.release-bar').dataset.state === expected && !document.querySelector('#check-release').disabled, state);
    }
    releases = []; await page.locator('#check-release').click(); await page.waitForFunction(() => document.querySelector('#release-summary').textContent.includes('No published'));
    fail = true; await page.locator('#check-release').click(); await page.waitForFunction(() => document.querySelector('#release-summary').textContent.includes('Couldn’t'));
    fail = false; releases = [release('v0.5.7-alpha.5')]; await page.locator('#check-release').click(); await page.waitForFunction(() => document.querySelector('.release-bar').dataset.state === 'current');
    await page.locator('#show-notes').click(); assert.match(await page.locator('#release-notes').innerText(), /<script>/); assert.equal(await page.evaluate(() => window.notesExecuted), undefined);
    await page.locator('#close-notes').click();
    const filename = `webui-browser-${Date.now()}.bin`, payload = Buffer.alloc(128 * 1024, 75);
    await page.locator('#destination').selectOption('PSP');
    const [firstPicker] = await Promise.all([page.waitForEvent('filechooser'), page.locator('#browse-files').click()]);
    await firstPicker.setFiles({ name: filename, mimeType: 'application/octet-stream', buffer: payload });
    await page.waitForFunction(name => [...document.querySelectorAll('#recent-transfers .transfer-row')].some(n => n.textContent.includes(name) && n.textContent.includes('Uploaded')), filename);
    await page.locator('.folder-row').filter({ hasText: 'PSP' }).click();
    const fileRow = page.locator('.content-row').filter({ hasText: filename }); await fileRow.waitFor();
    const [download] = await Promise.all([page.waitForEvent('download'), fileRow.getByText('Download', { exact: true }).click()]);
    const downloaded = await download.path(); assert.deepEqual(fs.readFileSync(downloaded), payload);
    await page.locator('[data-page="overview"]').click();
    const [secondPicker] = await Promise.all([page.waitForEvent('filechooser'), page.locator('#browse-files').click()]);
    await secondPicker.setFiles({ name: filename, mimeType: 'application/octet-stream', buffer: Buffer.from('replace') });
    await page.waitForFunction(() => document.querySelector('#recent-transfers').textContent.includes('already exists'));
    // Real throttled upload: progress must not replace the focused Cancel control.
    const cdp = await context.newCDPSession(page);
    await cdp.send('Network.enable');
    await cdp.send('Network.emulateNetworkConditions', { offline: false, latency: 0, downloadThroughput: -1, uploadThroughput: 64 * 1024 });
    const cancelledName = `cancel-${Date.now()}.bin`;
    const [cancelPicker] = await Promise.all([page.waitForEvent('filechooser'), page.locator('#browse-files').click()]);
    await cancelPicker.setFiles({ name: cancelledName, mimeType: 'application/octet-stream', buffer: Buffer.alloc(1024 * 1024, 42) });
    const cancelRow = page.locator('#recent-transfers .transfer-row').filter({ hasText: cancelledName });
    const cancelButton = cancelRow.getByRole('button', { name: 'Cancel' });
    await cancelButton.focus();
    await page.waitForFunction(name => [...document.querySelectorAll('#recent-transfers .transfer-row')].some(row => row.textContent.includes(name) && row.querySelector('progress')?.value >= 10), cancelledName);
    assert.equal(await cancelButton.evaluate(button => document.activeElement === button), true, 'Cancel stays focused across progress updates');
    await page.keyboard.press('Enter');
    await page.waitForFunction(name => [...document.querySelectorAll('#recent-transfers .transfer-row')].some(row => row.textContent.includes(name) && row.textContent.includes('Cancelled')), cancelledName);
    await cdp.send('Network.emulateNetworkConditions', { offline: false, latency: 0, downloadThroughput: -1, uploadThroughput: -1 });
    await cdp.detach();
    const cancelledListing = await page.evaluate(async () => (await (await fetch('/api/content?path=PSP')).json()).entries);
    assert.equal(cancelledListing.some(entry => entry.name === cancelledName), false, 'Cancelled upload is not published');
    await page.locator('[data-page="settings"]').click();
    await page.locator('#setting-audio_volume').fill('-12'); await page.locator('#save-settings').click();
    await page.waitForFunction(() => document.querySelector('#settings-result').textContent.includes('Saved'));
    const settings = await page.evaluate(async () => (await (await fetch('/api/settings')).json()).settings);
    assert.equal(settings.find(s => s.key === 'audio_volume').value, '-12');
    await page.locator('[data-page="content"]').click();
    await page.locator('#folder-name').fill('browser-created-' + Date.now()); await page.locator('#folder-form button').click();
    await page.waitForFunction(() => document.querySelector('#announcement').textContent.startsWith('Created'));
    // A full reload proves that settings come from the server, not optimistic browser state.
    await page.reload(); await page.waitForFunction(() => document.querySelector('#quick-volume').value === '-12');
    await page.locator('[data-page="overview"]').click();
    await page.evaluate(() => { document.querySelector('#announcement').textContent = ''; });
    await page.locator('[data-page="transfers"]').click(); await page.locator('#clear-transfers').click(); await page.locator('[data-page="overview"]').click();
    await page.evaluate(() => document.fonts.ready);
    fs.mkdirSync(out, { recursive: true });
    // One bounded visual round across the supported sizes and representative pages.
    for (const [width, height, name] of [[1504, 1046, 'comp-size'], [1440, 1001, 'desktop'], [1280, 960, 'desktop-1280'], [390, 844, 'mobile']]) {
      await page.setViewportSize({ width, height });
      await page.screenshot({ path: path.join(out, name + '.png'), fullPage: true });
      assert.equal(await page.evaluate(() => document.documentElement.scrollWidth <= window.innerWidth), true, `${name} horizontal overflow`);
    }
    await page.locator('[data-page="content"]').click(); await page.locator('#content-list .content-row').first().waitFor();
    await page.screenshot({ path: path.join(out, 'mobile-content.png'), fullPage: true });
    await page.locator('[data-page="settings"]').click(); await page.screenshot({ path: path.join(out, 'mobile-settings.png'), fullPage: true });
    await page.locator('[data-page="overview"]').click(); await page.locator('#theme').selectOption('dark');
    await page.screenshot({ path: path.join(out, 'mobile-dark.png'), fullPage: true });
    await page.reload(); assert.equal(await page.locator('html').getAttribute('data-theme'), 'dark');
    await page.locator('#theme').selectOption('light');
    assert.deepEqual(errors, [], 'Browser JavaScript errors');
    console.log('PASS: release ordering/current/update/unknown/offline/empty, safe notes, real upload/download, duplicate refusal, keyboard cancellation during upload, settings persistence, folder creation, navigation, mobile overflow and saved theme.');
  } finally { await browser.close(); }
})().catch(error => { console.error(error); process.exitCode = 1; });
