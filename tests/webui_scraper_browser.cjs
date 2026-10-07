/* The Download Media tab in Chromium (tests/test_scraper.py starts the server and a fake
 * libretro source): the method is asked first and remembered, the library is scraped on
 * the PS5 with live stats, the ambiguous game is resolved from the tab, and covers show
 * on the Games tab. */
const assert = require('node:assert/strict');
const { chromium } = require(process.env.PLAYWRIGHT_PATH || 'playwright');
(async () => {
  const browser = await chromium.launch({ headless: true, executablePath: process.env.CHROMIUM_PATH || '/usr/bin/chromium' });
  try {
    const page = await browser.newPage();
    const errors = []; page.on('pageerror', e => errors.push(e.message));
    await page.route('https://api.github.com/**', route => route.fulfill({ json: [] }));
    // Opened straight on the tab (the state it reads is declared before it runs).
    await page.goto(process.env.WEBUI_TEST_URL + '/#media');
    await page.waitForFunction(() => document.querySelectorAll('.kind-tile').length === 10);
    await page.goto(process.env.WEBUI_TEST_URL + '/#games');
    await page.waitForFunction(() => document.querySelectorAll('.game-card').length === 5);
    await page.locator('#scrape-open').click();
    await page.waitForFunction(() => location.hash === '#media' && document.querySelectorAll('.kind-tile').length === 10);
    assert.match(await page.locator('#scrape-method-hint').innerText(), /This PC → PS5: this PC downloads, then transfers to the PS5/);
    assert.equal(await page.locator('#scrape-start').isDisabled(), true, 'no start until a method is chosen');
    // The source's kinds can be chosen; the others say they are coming.
    assert.equal(await page.locator('.kind-tile[aria-pressed="true"]').count(), 3);
    assert.equal(await page.locator('.kind-tile:disabled').count(), 6);
    await page.locator('.kind-tile', { hasText: 'Logo' }).click();
    assert.equal(await page.locator('.kind-tile[aria-pressed="true"]').count(), 4);
    await page.locator('#kinds-recommended').click();
    assert.equal(await page.locator('.kind-tile[aria-pressed="true"]').count(), 3);
    await page.locator('.segmented label:has(input[value="ps5"])').click();
    assert.match(await page.locator('#scrape-summary').innerText(), /3 kinds of media · 5 games · libretro thumbnails · downloaded on the PS5/);
    // What happens to media already there: said plainly, missing-only by default.
    assert.match(await page.locator('#scrape-summary').innerText(), /only what’s missing$/);
    assert.match(await page.locator('#scrape-existing-hint').innerText(), /Only media your games don’t have yet is downloaded/);
    await page.locator('label:has(#scrape-overwrite)').click();
    assert.match(await page.locator('#scrape-existing-hint').innerText(), /downloaded again and replaces the files/);
    assert.match(await page.locator('#scrape-summary').innerText(), /replacing what you have$/);
    await page.locator('label:has(#scrape-keep)').click();
    await page.locator('#scrape-start').click();
    await page.waitForFunction(() => /finished/.test(document.querySelector('#job-title').textContent), null, { timeout: 60000 });
    await page.waitForFunction(() => document.querySelector('#stat-done').textContent === '2' && document.querySelector('#stat-attention').textContent === '2');
    assert.equal(await page.locator('#stat-partial').innerText(), '1');
    // The ambiguous game: its region's candidate offered first, chosen from the tab.
    await page.locator('#job-problems-box summary').click();
    const row = page.locator('.problem-row', { hasText: 'Chrono Trigger Special Edition' });
    assert.equal(await row.locator('select').inputValue(), 'Chrono Trigger (USA)');
    await row.getByRole('button', { name: 'Use this match' }).click();
    await page.waitForFunction(() => document.querySelector('#stat-attention').textContent === '1', null, { timeout: 60000 });
    // The recap: per kind, then each game's media, the missing ones and why.
    await page.waitForFunction(() => !document.querySelector('#job-recap').hidden && /4 new/.test(document.querySelector('.recap-kind')?.textContent || ''), null, { timeout: 30000 });
    assert.match(await page.locator('.recap-kind', { hasText: 'Title screen' }).innerText(), /3 new[\s\S]*0 kept[\s\S]*2 missing/);
    const metroid = page.locator('.recap-game', { hasText: 'Super Metroid' });
    assert.equal(await metroid.locator('.kind-pill.got').count(), 2);
    assert.equal(await metroid.locator('.kind-pill.missed').innerText(), 'Title screen');
    await page.locator('#recap-filter .chip', { hasText: 'Not found' }).click();
    assert.equal(await page.locator('.recap-game').count(), 1);
    assert.match(await page.locator('.recap-game').innerText(), /Totally Unknown Homebrew[\s\S]*No match at libretro/);
    await page.locator('#recap-filter .chip', { hasText: 'All games' }).click();
    await page.locator('.recap-kind', { hasText: 'Title screen' }).click();
    await page.locator('#recap-search').fill('chrono');
    assert.equal(await page.locator('.recap-game').count(), 2);
    assert.equal(await page.locator('.recap-game .kind-pill').count(), 2, 'one kind shown when one is picked');
    await page.goto(process.env.WEBUI_TEST_URL + '/#games');
    await page.waitForFunction(() => document.querySelectorAll('.game-cover img').length === 4);
    // A card opens the game: one row a kind of media, missing ones greyed, each replaceable.
    await page.locator('.game-card', { hasText: 'Donkey Kong Country 2' }).locator('.game-cover').click();
    const sheet = page.locator('#game-sheet');
    await sheet.waitFor({ state: 'visible' });
    await page.waitForFunction(() => document.querySelectorAll('.media-row-item').length === 10);
    assert.equal(await page.locator('.media-row-item:not(.missing)').count(), 3);
    assert.equal(await page.locator('.media-row-item.missing').count(), 7);
    assert.equal(await page.locator('.media-row-item[data-kind="cover"] .preview img').count(), 1);
    assert.match(await page.locator('#sheet-media-count').innerText(), /3 of 10 kinds/);
    const fanart = page.locator('.media-row-item[data-kind="fanart"]');
    assert.equal(await fanart.locator('button').innerText(), 'Add…');
    // A type no frontend shows is refused on the page; a PNG is sent and shown at once.
    await fanart.locator('input[type=file]').setInputFiles({ name: 'art.gif', mimeType: 'image/gif', buffer: Buffer.from('GIF89a') });
    assert.match(await fanart.locator('.state').innerText(), /takes PNG, JPG, JPEG files/);
    const png = Buffer.from('iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mNk+M9QDwADhgGAWjR9awAAAABJRU5ErkJggg==', 'base64');
    await fanart.locator('input[type=file]').setInputFiles({ name: 'my fan art.png', mimeType: 'image/png', buffer: png });
    await page.waitForFunction(() => !document.querySelector('.media-row-item[data-kind="fanart"]').classList.contains('missing'));
    assert.match(await page.locator('.media-row-item[data-kind="fanart"] .state').innerText(), /PNG · .* · your file, kept when downloading again/);
    assert.match(await page.locator('#sheet-media-count').innerText(), /4 of 10 kinds/);
    await page.keyboard.press('Escape');
    await sheet.waitFor({ state: 'hidden' });
    assert.match(await page.locator('.game-card', { hasText: 'Donkey Kong Country 2' }).locator('.media-badges').innerText(), /Fan art/);
    // The checkbox still selects without opening.
    await page.locator('.game-card', { hasText: 'Donkey Kong Country 2' }).locator('.game-select').click();
    assert.equal(await sheet.isVisible(), false);
    await page.locator('.game-card', { hasText: 'Donkey Kong Country 2' }).locator('.game-select').click();
    // The method is remembered on the console.
    await page.goto(process.env.WEBUI_TEST_URL + '/#media');
    await page.waitForFunction(() => document.querySelector('input[name="scrape-method"][value="ps5"]').checked);
    assert.match(await page.locator('#scrape-method-hint').innerText(), /downloads everything itself/);
    await page.setViewportSize({ width: 390, height: 844 });
    assert.equal(await page.evaluate(() => document.documentElement.scrollWidth <= window.innerWidth + 1), true, 'no sideways scroll on a phone');
    assert.deepEqual(errors, []);
    console.log('PASS: method asked then remembered, media tiles, system scraped on the PS5 with live stats, ambiguous game resolved, recap by kind and game, covers shown, the game overlay with its media rows and an upload, phone width');
  } finally { await browser.close(); }
})().catch(error => { console.error(error); process.exit(1); });
