const { chromium } = require('playwright');
(async () => {
  const b = await chromium.launch();
  const items = process.argv.slice(2);
  for (const it of items) {
    const [name, w, h] = it.split(':');
    const p = await b.newPage({ viewport: { width: +w, height: +h }, deviceScaleFactor: 1 });
    await p.goto('file://' + process.cwd() + '/' + name + '.html');
    await p.waitForTimeout(300);
    await p.screenshot({ path: name + '.png' });
    await p.close();
  }
  await b.close();
})();
