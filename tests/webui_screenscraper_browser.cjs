/* ScreenScraper's sign-in on the Download Media tab in Chromium (tests/
 * test_scraper_screenscraper.py starts the server and a fake ScreenScraper): clicking the
 * source opens the form, a wrong password is refused in it, the right one shows a green
 * check on the chip, the account panel shows the quota, and signing out removes the check.
 * The password never reaches an answer of the console. */
const assert = require('node:assert/strict');
const { chromium } = require(process.env.PLAYWRIGHT_PATH || 'playwright');
(async () => {
  const browser = await chromium.launch({ headless: true, executablePath: process.env.CHROMIUM_PATH || '/usr/bin/chromium' });
  try {
    const page = await browser.newPage();
    const errors = []; page.on('pageerror', e => errors.push(e.message));
    const answers = [];
    page.on('response', async r => { if (r.url().includes('/api/')) answers.push(await r.text().catch(() => '')); });
    await page.route('https://api.github.com/**', route => route.fulfill({ json: [] }));
    await page.goto(process.env.WEBUI_TEST_URL + '/#media');
    await page.waitForFunction(() => document.querySelectorAll('.kind-tile').length === 10);
    const chip = page.locator('#scrape-sources .chip', { hasText: 'ScreenScraper' });
    assert.equal(await chip.isDisabled(), false);
    assert.match(await chip.innerText(), /Sign in/);
    assert.equal(await chip.locator('.signed-check').count(), 0);
    // A click opens the form; the source is not chosen until signed in.
    await chip.click();
    const dialog = page.locator('#sign-in');
    await dialog.waitFor({ state: 'visible' });
    assert.equal(await chip.getAttribute('aria-checked'), 'false');
    assert.equal(await page.locator('#sign-in-user').evaluate(n => n === document.activeElement), true);
    assert.equal(await page.locator('#sign-in-password').getAttribute('type'), 'password');
    await page.locator('#sign-in-user').fill(process.env.SS_USER);
    await page.locator('#sign-in-password').fill('wrong-password');
    await page.locator('#sign-in-submit').click();
    await page.waitForFunction(() => /did not accept/.test(document.querySelector('#sign-in-error').textContent));
    assert.equal(await chip.locator('.signed-check').count(), 0);
    await page.locator('#sign-in-password').fill(process.env.SS_PASSWORD);
    await page.locator('#sign-in-submit').click();
    await page.waitForFunction(() => !document.querySelector('#sign-in-account').hidden);
    assert.match(await page.locator('#sign-in-who').innerText(), new RegExp(`Signed in as ${process.env.SS_USER}`));
    assert.match(await page.locator('#sign-in-quota').innerText(), /12 of 20,000/);
    assert.equal(await page.locator('#sign-in-password').inputValue(), '', 'the password is cleared from the page');
    await page.locator('#sign-in-submit').click(); // Done
    await dialog.waitFor({ state: 'hidden' });
    // The green check, the source chosen, its own media and the language offered; PC mode off.
    assert.equal(await chip.locator('.signed-check').count(), 1);
    assert.equal(await chip.getAttribute('aria-checked'), 'true');
    assert.equal(await page.locator('.kind-tile:disabled').count(), 0);
    assert.equal(await page.locator('#scrape-language-pick').isVisible(), true);
    assert.equal(await page.locator('input[name="scrape-method"][value="pc"]').isDisabled(), true);
    assert.match(await page.locator('#scrape-account-text').innerText(), /Signed in to ScreenScraper/);
    // Still signed in after a reload (kept on the console, not in the page).
    await page.reload();
    await page.waitForFunction(() => document.querySelectorAll('.kind-tile').length === 10);
    assert.equal(await chip.locator('.signed-check').count(), 1);
    // Signed in, a click just chooses the source (no form).
    await chip.click();
    assert.equal(await chip.getAttribute('aria-checked'), 'true');
    assert.equal(await dialog.isVisible(), false);
    // Manage, then sign out: the check goes, libretro is chosen again.
    await page.locator('#scrape-account-manage').click();
    await dialog.waitFor({ state: 'visible' });
    await page.locator('#sign-in-forget').click();
    await dialog.waitFor({ state: 'hidden' });
    assert.equal(await chip.locator('.signed-check').count(), 0);
    assert.equal(await page.locator('#scrape-sources .chip', { hasText: 'libretro' }).getAttribute('aria-checked'), 'true');
    assert.equal(await page.locator('input[name="scrape-method"][value="pc"]').isDisabled(), false);
    for (const text of answers) assert.ok(!text.includes(process.env.SS_PASSWORD), 'an answer held the password');
    assert.deepEqual(errors, []);
    console.log('screenscraper sign-in browser test: ok');
  } finally { await browser.close(); }
})().catch(error => { console.error(error); process.exit(1); });
