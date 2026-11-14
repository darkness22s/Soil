const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');

const root = path.resolve(__dirname, '..');
const read = (p) => fs.readFileSync(path.join(root, p), 'utf8');

test('onboarding never provisions unsolicited VMs or agents', () => {
  const setup = read('app/Soil/ui/onboarding.js');
  assert.match(setup, /onboardingComplete:\s*true/);
  assert.match(setup, /window\.location\.href\s*=\s*'app\.html'/);
  assert.doesNotMatch(setup, /postJson\(['"]\/api\/(?:vms|apps|agents)['"]/);
});

test('new VMs start only after explicit user action; 2GiB is the default', () => {
  const js = read('app/Soil/ui/app-ui.js');
  const html = read('app/Soil/ui/app.html');
  assert.match(js, /var mkType="lab"/);
  assert.match(js, /size:"nano"/);
  assert.match(js, /method:"DELETE"/);
  const creation = js.slice(js.indexOf('mkDone.addEventListener'), js.indexOf('var tour='));
  assert.doesNotMatch(creation, /\/start["']/);
  assert.match(html, /id="mkRepoWrap" hidden/);
  assert.match(html, /id="githubBtn"/);
});

test('GitHub can be connected after setup, reconnected, and disconnected', () => {
  const html = read('app/Soil/ui/app.html');
  const js = read('app/Soil/ui/github-ui.js');
  assert.match(html, /src="github-ui.js"/);
  assert.match(js, /\/api\/github\/connect/);
  assert.match(js, /\/api\/github\/config/);
  assert.match(js, /\/api\/github\/disconnect/);
  assert.match(js, /soil:github-connected/);
});

test('VM startup avoids full-RAM autosnapshots and survives UI input drops', () => {
  const manager = read('app/Soil/VmManager.cs');
  const main = read('app/Soil/MainForm.cs');
  const ipc = read('engine/src/ipc.c');
  const vmhost = read('engine/src/vmhost.c');
  const launch = manager.slice(manager.indexOf('private async Task LaunchAsync'), manager.indexOf('private async Task MonitorExitAsync'));
  assert.doesNotMatch(launch, /--snapshot-out|--restore/);
  assert.doesNotMatch(main, /IdleAll\(\)|ResumeAll\(\)/);
  assert.match(ipc, /input client disconnected; accepting a new connection/);
  assert.doesNotMatch(ipc, /input client disconnected - stopping VM/);
  assert.doesNotMatch(vmhost, /C:\\Users\\Lenovo|VhWriteFbBmp/);
});
