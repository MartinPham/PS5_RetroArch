/* The Games page in Chromium (tests/test_scraper.py starts the server and a fake libretro
 * source): the method is asked first and remembered, a whole system is scraped on the
 * PS5, the ambiguous game is resolved from the page, and covers show in the grid. */
const assert = require('node:assert/strict');
const { chromium } = require(process.env.PLAYWRIGHT_PATH || 'playwright');
(async () => {
  const browser = await chromium.launch({ headless: true, executablePath: process.env.CHROMIUM_PATH || '/usr/bin/chromium' });
  try {
    const page = await browser.newPage();
    const errors = []; page.on('pageerror', e => errors.push(e.message));
    await page.route('https://api.github.com/**', route => route.fulfill({ json: [] }));
    await page.goto(process.env.WEBUI_TEST_URL + '/#games');
    await page.waitForFunction(() => document.querySelectorAll('.game-card').length === 5);
    await page.locator('#scrape-open').click();
    await page.waitForFunction(() => document.querySelector('#scrape-method-saved').textContent.includes('Choose one'));
    assert.equal(await page.locator('input[name="scrape-method"]:checked').count(), 0, 'no method until the user chooses');
    await page.locator('#scrape-start').click();
    assert.match(await page.locator('#scrape-result').innerText(), /Choose where/);
    await page.locator('input[name="scrape-method"][value="ps5"]').check();
    await page.locator('#scrape-start').click();
    await page.waitForFunction(() => /Finished/.test(document.querySelector('#job-summary').textContent), null, { timeout: 60000 });
    assert.match(await page.locator('#job-summary').innerText(), /2 complete, 1 partly, 0 already had media, 2 need you/);
    // The ambiguous game: its region's candidate offered first, chosen from the page.
    const row = page.locator('.problem-row', { hasText: 'Chrono Trigger Special Edition' });
    assert.equal(await row.locator('select').inputValue(), 'Chrono Trigger (USA)');
    await row.getByRole('button', { name: 'Use this match' }).click();
    await page.waitForFunction(() => /Finished/.test(document.querySelector('#job-summary').textContent) && !document.querySelector('.problem-row')?.textContent.includes('Chrono'), null, { timeout: 60000 });
    await page.locator('#refresh-games').click();
    await page.waitForFunction(() => document.querySelectorAll('.game-cover img').length === 4);
    const loaded = await page.evaluate(() => Promise.all([...document.querySelectorAll('.game-cover img')].map(img => img.decode().then(() => true, () => img.complete && img.naturalWidth === 0 ? 'not an image' : true))));
    assert.equal(loaded.length, 4);
    // The method is remembered on the console: shown when the next job starts.
    await page.locator('#scrape-open').click();
    await page.waitForFunction(() => document.querySelector('#scrape-method-saved').textContent.includes('Download directly on PS5'));
    assert.equal(await page.locator('input[name="scrape-method"][value="ps5"]').isChecked(), true);
    assert.deepEqual(errors, []);
    console.log('PASS: method asked then remembered, system scraped on the PS5, ambiguous game resolved in the page, covers shown');
  } finally { await browser.close(); }
})().catch(error => { console.error(error); process.exit(1); });
