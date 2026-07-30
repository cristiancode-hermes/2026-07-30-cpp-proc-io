import puppeteer from 'puppeteer';
import { mkdirSync } from 'fs';
import { join, dirname } from 'path';
import { fileURLToPath } from 'url';

const __dirname = dirname(fileURLToPath(import.meta.url));

async function captureProcIO() {
  const htmlPath = '/opt/data/cpp-projects/2026-07-30-cpp-proc-io/screenshot.html';
  const assetsDir = '/opt/data/portafolio/public/assets';
  mkdirSync(assetsDir, { recursive: true });

  const browser = await puppeteer.launch({
    executablePath: '/opt/data/.agent-browser/browsers/chromium/chrome-linux/chrome',
    args: ['--no-sandbox', '--disable-setuid-sandbox', '--disable-dev-shm-usage'],
  });

  try {
    const page = await browser.newPage();

    // Desktop capture
    await page.setViewport({ width: 1440, height: 900 });
    await page.goto('file://' + htmlPath, { waitUntil: 'networkidle0', timeout: 15000 });
    await page.waitForSelector('.terminal', { timeout: 5000 });
    // Wait a bit for fonts to render
    await new Promise(r => setTimeout(r, 1000));

    // Clip to just the terminal element for desktop
    const terminalEl = await page.$('.terminal');
    const box = await terminalEl.boundingBox();
    
    // Capture full page (desktop)
    await page.screenshot({
      path: join(assetsDir, 'proc-io.png'),
      clip: {
        x: Math.max(0, box.x - 40),
        y: Math.max(0, box.y - 20),
        width: Math.min(box.width + 80, 1440),
        height: Math.min(box.height + 40, 900),
      },
    });

    // Mobile capture — show just the terminal centered
    await page.setViewport({ width: 390, height: 844 });
    await page.goto('file://' + htmlPath, { waitUntil: 'networkidle0', timeout: 15000 });
    await page.waitForSelector('.terminal', { timeout: 5000 });
    await new Promise(r => setTimeout(r, 1000));

    const terminalEl2 = await page.$('.terminal');
    const box2 = await terminalEl2.boundingBox();

    await page.screenshot({
      path: join(assetsDir, 'proc-io-m.png'),
      clip: {
        x: Math.max(0, box2.x - 20),
        y: Math.max(0, box2.y - 10),
        width: Math.min(box2.width + 40, 390),
        height: Math.min(box2.height + 20, 844),
      },
    });

    console.log('Screenshots captured successfully');
    console.log(`Desktop: ${join(assetsDir, 'proc-io.png')}`);
    console.log(`Mobile: ${join(assetsDir, 'proc-io-m.png')}`);
  } finally {
    await browser.close();
  }
}

captureProcIO().catch(err => {
  console.error('Capture failed:', err);
  process.exit(1);
});
