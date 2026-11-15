// Browser test of main/webui.html against the device mock (tools/webui_mock.py).
//   NODE_PATH=<dir containing playwright> node tools/webui_test.js
// Starts the mock itself, drives the page in headless Chromium, writes screenshots to a temp dir.
const { chromium } = require('playwright');
const fs = require('fs');
const os = require('os');
const path = require('path');
const { spawn } = require('child_process');

const SP = fs.mkdtempSync(path.join(os.tmpdir(), 'webui-test-'));
fs.mkdirSync(SP + '/shots');
const PORT = 8099;
const mock = spawn('python3', [path.join(__dirname, 'webui_mock.py'), String(PORT)], { stdio: 'ignore' });

// A minimal file that passes the device's header check (ESP image, ESP32-S3, app descriptor).
function fakeApp(withDisplay) {
  const b = Buffer.alloc(6000, 0x11);
  b[0] = 0xE9; b[1] = 3; b[12] = 9; b[13] = 0; b.writeUInt32LE(0xABCD5432, 32);
  if (withDisplay) Buffer.from('co5300 display driver').copy(b, 3000);
  return b;
}
fs.writeFileSync(SP + '/app_with_display.bin', fakeApp(true));
fs.writeFileSync(SP + '/app_no_display.bin', fakeApp(false));
let problems = [];
const ok = (c, m) => { if (!c) { problems.push(m); console.log('FAIL:', m); } else console.log('ok  :', m); };
(async () => {
  const b = await chromium.launch();
  const ctx = await b.newContext({ viewport: { width: 400, height: 900 }, deviceScaleFactor: 1 });
  const page = await ctx.newPage();
  page.on('pageerror', e => problems.push('pageerror ' + e.message));
  page.on('console', m => { if (m.type() === 'error' && !m.text().includes('Failed to load resource')) problems.push('console ' + m.text()); });
  let dialogs = [];
  page.on('dialog', d => { dialogs.push(d.message()); d.accept(); });
  await new Promise(r => setTimeout(r, 800));
  await page.goto('http://127.0.0.1:' + PORT + '/');
  await page.waitForSelector('#apps .row');
  const names = async () => page.$$eval('#apps .row .name', e => e.map(x => x.textContent));
  const msg = async () => page.textContent('#msg');
  ok(JSON.stringify(await names()) === JSON.stringify(['PrintSphere', 'Capsule Radar']), 'two installed apps listed in order');
  ok(await page.$$eval('#prev i', e => e.length) === 2, 'preview shows two pills');
  await page.screenshot({ path: SP + '/shots/1_initial.png', fullPage: true });

  // reorder: move PrintSphere down
  await page.click('#apps .row:nth-child(1) button[title="Move down"]');
  await page.waitForTimeout(400);
  ok(JSON.stringify(await names()) === JSON.stringify(['Capsule Radar', 'PrintSphere']), 'move down reorders the list');
  ok(JSON.stringify(await page.$$eval('#prev i', e => e.map(x => x.textContent))) === JSON.stringify(['Capsule Radar', 'PrintSphere']), 'preview follows the order');
  ok(await page.$eval('#apps .row:nth-child(1) button[title="Move up"]', e => e.disabled), 'first row cannot move up');

  // rename
  await page.click('#apps .row:nth-child(2) button[title="Rename"]');
  await page.fill('#apps input:not([type=color])', 'Print Sphere Pro');
  await page.click('#apps button.pri');
  await page.waitForTimeout(400);
  ok((await names())[1] === 'Print Sphere Pro', 'rename works');

  // upload a real app image into the free slot
  await page.setInputFiles('#file', SP + '/app_with_display.bin');
  await page.fill('#nname', 'My Uploaded');
  ok((await page.textContent('#go')).trim() === 'Upload', 'free slot selected: plain Upload button');
  await page.click('#go');
  await page.waitForFunction(() => document.querySelectorAll('#apps .row').length === 3, null, { timeout: 8000 });
  ok(JSON.stringify(await names()) === JSON.stringify(['Capsule Radar', 'Print Sphere Pro', 'My Uploaded']), 'uploaded app appears last');
  ok((await msg()).includes('Installed My Uploaded') && !(await msg()).includes('CO5300'), 'success message shown, no warning for an image with a display driver');
  await page.screenshot({ path: SP + '/shots/2_three_apps.png', fullPage: true });

  // replace flow needs confirmation
  dialogs = [];
  await page.setInputFiles('#file', SP + '/app_no_display.bin');
  await page.selectOption('#slot', '0');
  ok((await page.textContent('#go')).includes('replace'), 'occupied slot: button says replace');
  await page.click('#go');
  await page.waitForTimeout(800);
  ok(dialogs.some(d => d.includes('Replace')), 'replace asks for confirmation');

  // bad files
  fs.writeFileSync(SP + '/notes.bin', Buffer.alloc(200, 0x41));
  await page.setInputFiles('#file', SP + '/notes.bin');
  await page.click('#go');
  await page.waitForTimeout(800);
  ok((await msg()).includes('not an ESP32'), 'garbage file rejected with a clear message');
  const boot = Buffer.alloc(200); boot[0] = 0xE9; boot[1] = 3; boot[12] = 9; boot.writeUInt32LE(0x50505050, 32);
  fs.writeFileSync(SP + '/boot.bin', boot);
  await page.setInputFiles('#file', SP + '/boot.bin');
  await page.click('#go');
  await page.waitForTimeout(800);
  ok((await msg()).includes('app-only'), 'bootloader/full image rejected with a clear message');
  await page.screenshot({ path: SP + '/shots/3_error.png', fullPage: true });

  // delete (third row = last in order)
  dialogs = [];
  await page.click('#apps .row:nth-child(3) button[title="Remove"]');
  await page.waitForTimeout(500);
  ok((await names()).length === 2 && dialogs.some(d => d.includes('Remove')), 'delete asks, then removes');

  // GitHub tab: preset fill, install job with progress banner
  await page.click('#t2');
  await page.click('#presets button:nth-child(3)');
  ok(await page.inputValue('#repo') === 'skiiirt1983/LyricFlash' && await page.inputValue('#prefix') === 'lyricflash-ota.bin', 'preset fills the form');
  ok((await page.textContent('#go')).includes('GitHub'), 'button says Install from GitHub');
  await page.click('#go');
  await page.waitForFunction(() => document.querySelector('#banner').style.display === 'block', null, { timeout: 4000 });
  await page.screenshot({ path: SP + '/shots/4_job.png', fullPage: true });
  await page.waitForFunction(() => document.querySelectorAll('#apps .row').length === 3, null, { timeout: 12000 });
  ok(true, 'GitHub install job completed and the app appeared');
  await page.waitForFunction(() => document.querySelector('#banner').style.display === 'none', null, { timeout: 6000 });

  // bad repo
  await page.fill('#repo', 'nonsense');
  await page.selectOption('#slot', '0');
  await page.click('#go');
  await page.waitForTimeout(500);
  ok((await msg()).includes('owner/name'), 'bad repository name rejected');

  // Wi-Fi
  ok((await page.textContent('#wst')).includes('BaseCamp-Setup network is on'), 'before saving: setup network shown as on');
  await page.click('#scan');
  await page.waitForFunction(() => document.querySelectorAll('#nets option').length > 1, null, { timeout: 8000 });
  await page.selectOption('#nets', 'HomeNet');
  ok(await page.inputValue('#ssid') === 'HomeNet', 'picking a scanned network fills the name');
  await page.fill('#pass', 'short');
  await page.click('#wsave');
  await page.waitForTimeout(400);
  ok((await msg()).includes('8 characters'), 'short password rejected');
  await page.fill('#pass', 'longenough1');
  await page.click('#wsave');
  await page.waitForTimeout(500);
  const w = await page.textContent('#wst');
  ok(w.includes('HomeNet') && w.includes('Connected'), 'Wi-Fi saved and shown as connected');
  ok(w.includes('http://192.168.1.57') && w.includes('BaseCamp-Setup network is off'), 'after saving: setup network shown as off, home address shown');
  ok((await msg()).includes('switches off'), 'save message explains the setup network goes away');
  await page.screenshot({ path: SP + '/shots/5_wifi.png', fullPage: true });

  // XSS: a hostile app name is rendered as text, not markup
  await page.evaluate(() => fetch('/api/rename', { method: 'POST', headers: { 'X-Basecamp': '1' }, body: JSON.stringify({ slot: 0, name: '<img src=x onerror=window.__x=1>' }) }));
  await page.waitForTimeout(3500);
  ok(await page.evaluate(() => window.__x === undefined) && (await page.$$('#apps img')).length === 0, 'hostile app name is not interpreted as HTML');

  // remove everything -> empty state
  for (let k = 0; k < 3; k++) {
    if (!(await page.$$('#apps .row')).length) break;
    await page.click('#apps .row:nth-child(1) button[title="Remove"]');
    await page.waitForTimeout(500);
  }
  ok(await page.isVisible('#none'), 'empty state message when no apps');
  ok(await page.$$eval('#prev i', e => e.length) === 0, 'preview is empty');
  await page.screenshot({ path: SP + '/shots/6_empty.png', fullPage: true });

  // CSRF-style request without the custom header is refused
  const r = await page.evaluate(async () => (await fetch('/api/delete', { method: 'POST', body: JSON.stringify({ slot: 0 }) })).status);
  ok(r === 403, 'POST without X-Basecamp header is refused (' + r + ')');

  await b.close();
  mock.kill();
  console.log('screenshots in', SP + '/shots');
  console.log(problems.length ? 'PROBLEMS:\n' + problems.join('\n') : 'ALL WEB UI CHECKS PASSED');
  process.exit(problems.length ? 1 : 0);
})();
