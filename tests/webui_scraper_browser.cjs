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
    await page.waitForFunction(() => !document.querySelector('#restart-notice').hidden);
    assert.match(await page.locator('#restart-notice').innerText(), /Close PS5 RetroArch on the console/);
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
    // Game details: offered, but not by libretro (pictures only).
    assert.equal(await page.locator('.details-tile').isDisabled(), true);
    assert.match(await page.locator('.details-tile').innerText(), /Game details[\s\S]*Not from libretro thumbnails/);
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
    await page.waitForFunction(() => {
      const covers = [...document.querySelectorAll('.game-cover img')];
      return covers.length === 4 && covers.every(img => img.complete && img.naturalWidth > 0);
    });
    // A card opens the game: one row a kind of media, missing ones greyed, each replaceable.
    await page.locator('.game-card', { hasText: 'Donkey Kong Country 2' }).locator('.game-cover').click();
    const sheet = page.locator('#game-sheet');
    await sheet.waitFor({ state: 'visible' });
    await page.waitForFunction(() => document.querySelectorAll('.media-row-item').length === 10);
    assert.equal(await page.locator('.media-row-item:not(.missing)').count(), 3);
    assert.equal(await page.locator('.media-row-item.missing').count(), 7);
    assert.equal(await page.locator('.media-row-item[data-kind="cover"]:not(.missing) .media-stage').count(), 1);
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
    // The cards' picture: any of four kinds, remembered; a game without it shows the kind's drawing.
    await page.locator('label:has(input[name="games-view"][value="box3d"])').click();
    await page.waitForFunction(() => document.querySelector('#games-grid').dataset.view === 'box3d');
    assert.equal(await page.locator('.game-cover.none').count(), 5);
    await page.reload();
    await page.waitForFunction(() => document.querySelectorAll('.game-card').length === 5);
    assert.equal(await page.locator('input[name="games-view"][value="box3d"]').isChecked(), true);
    await page.locator('label:has(input[name="games-view"][value="cover"])').click();
    await page.waitForFunction(() => {
      const covers = [...document.querySelectorAll('.game-cover img')];
      return covers.length === 4 && covers.every(img => img.complete && img.naturalWidth > 0);
    });
    // The checkbox still selects without opening.
    await page.locator('.game-card', { hasText: 'Donkey Kong Country 2' }).locator('.game-select').click();
    assert.equal(await sheet.isVisible(), false);
    await page.locator('.game-card', { hasText: 'Donkey Kong Country 2' }).locator('.game-select').click();
    // Sorting/filtering the complete library, across systems, before the 300-card limit.
    const game = (name, details = {}) => ({ name, label: name, key: name, path: `/app0/content/${name}.zip`, media: [], ...details });
    const library = { systems: [
      { id: 'snes', name: 'Super Nintendo', games: [game('Zelda', { released: '1991', rating: '0.9', genre: 'Adventure', developer: 'Nintendo', publisher: 'Nintendo' }), game('Alpha 10'), game('Éclair', { released: '1994-04-18', rating: '0.8' })] },
      { id: 'nes', name: 'Nintendo', games: [game('Alpha 2', { released: '1987-01', rating: '0', media: ['cover', 'screenshot'] }), game('1942', { released: '1985-12-11', rating: '0.5' }), game('!Unknown', { released: 'unknown', rating: 'unrated' })] }
    ] };
    const libraryRoute = route => route.fulfill({ json: library });
    await page.route('**/api/library', libraryRoute);
    await page.locator('#refresh-games').click();
    await page.waitForFunction(() => document.querySelectorAll('.game-card').length === 6);
    const titles = () => page.locator('.game-info strong').allTextContents();
    assert.deepEqual(await titles(), ['!Unknown', '1942', 'Alpha 2', 'Alpha 10', 'Éclair', 'Zelda']);
    await page.locator('.game-card', { hasText: 'Alpha 2' }).locator('.game-select').check();
    const letter = value => page.locator(`#games-letters button[data-letter="${value}"]`);
    await letter('A').click();
    assert.deepEqual(await titles(), ['Alpha 2', 'Alpha 10']);
    await page.locator('#games-direction').click();
    assert.deepEqual(await titles(), ['Alpha 10', 'Alpha 2']);
    assert.equal(await page.locator('.game-card', { hasText: 'Alpha 2' }).locator('.game-select').isChecked(), true);
    await page.locator('#games-system').selectOption('snes');
    assert.deepEqual(await titles(), ['Alpha 10']);
    await page.locator('#games-search').fill('2');
    assert.deepEqual(await titles(), []);
    assert.match(await page.locator('#games-grid').innerText(), /No game matches/);
    await page.locator('#games-search').fill('');
    await page.locator('#games-system').selectOption('');
    await page.locator('#games-missing').check();
    assert.deepEqual(await titles(), ['Alpha 10']);
    await page.locator('#games-missing').uncheck();
    await letter('E').click();
    assert.deepEqual(await titles(), ['Éclair']);
    await letter('#').click();
    assert.deepEqual(await titles(), ['1942', '!Unknown']);
    await letter('').click();
    await page.locator('#games-sort').selectOption('released');
    assert.deepEqual(await titles(), ['Éclair', 'Zelda', 'Alpha 2', '1942', '!Unknown', 'Alpha 10']);
    await page.locator('#games-direction').click();
    assert.deepEqual(await titles(), ['1942', 'Alpha 2', 'Zelda', 'Éclair', '!Unknown', 'Alpha 10']);
    await page.locator('#games-sort').selectOption('rating');
    assert.deepEqual(await titles(), ['Alpha 2', '1942', 'Éclair', 'Zelda', '!Unknown', 'Alpha 10']);
    await page.locator('#games-direction').click();
    assert.deepEqual(await titles(), ['Zelda', 'Éclair', '1942', 'Alpha 2', '!Unknown', 'Alpha 10']);
    for (const field of ['genre', 'developer', 'publisher']) {
      await page.locator('#games-sort').selectOption(field);
      assert.equal((await titles())[0], 'Zelda', `${field}: missing details stay last`);
    }
    await page.locator('#games-sort').selectOption('system');
    assert.deepEqual(await titles(), ['Alpha 10', 'Éclair', 'Zelda', '!Unknown', '1942', 'Alpha 2']);
    await page.reload();
    await page.waitForFunction(() => document.querySelectorAll('.game-card').length === 6);
    assert.equal(await page.locator('#games-sort').inputValue(), 'system');
    assert.match(await page.locator('#games-direction').innerText(), /Descending/);
    await page.locator('#games-sort').selectOption('name');
    await page.locator('#games-direction').click();
    if (process.env.WEBUI_GAMES_SCREENSHOTS) await page.screenshot({ path: process.env.WEBUI_GAMES_SCREENSHOTS + '-desktop.png', fullPage: true });
    await page.setViewportSize({ width: 390, height: 844 });
    assert.equal(await page.evaluate(() => document.documentElement.scrollWidth <= window.innerWidth + 1), true, 'Games has no sideways scroll on a phone');
    await letter('Z').focus(); await page.keyboard.press('Enter');
    assert.deepEqual(await titles(), ['Zelda']);
    if (process.env.WEBUI_GAMES_SCREENSHOTS) await page.screenshot({ path: process.env.WEBUI_GAMES_SCREENSHOTS + '-phone.png', fullPage: true });
    await letter('').click();
    library.systems[0].games.push(...Array.from({ length: 301 }, (_, i) => game(`Bulk ${i}`)));
    await page.locator('#refresh-games').click();
    await page.waitForFunction(() => document.querySelectorAll('.game-card').length === 300);
    await page.locator('#games-direction').click();
    assert.equal((await titles())[0], 'Zelda', 'sort before slicing the first 300');
    await page.locator('.games-more').click();
    assert.equal(await page.locator('.game-card').count(), 307);
    await letter('A').click(); await letter('').click();
    assert.equal(await page.locator('.game-card').count(), 300, 'filter resets pagination');
    assert.match(await page.locator('#games-results').innerText(), /307 of 307 games/);
    await page.unroute('**/api/library', libraryRoute);
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
