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

test('GitHub dialog contains only Connect GitHub, Close, and the X control', () => {
  const html = read('app/Soil/ui/app.html');
  const dialog = html.match(/<dialog class="skill-dialog github-dialog"[\s\S]*?<\/dialog>/)?.[0];
  assert.ok(dialog);
  assert.deepEqual([...dialog.matchAll(/<button[^>]*id="([^"]+)"/g)].map(match => match[1]),
    ['githubCloseIcon', 'githubConnect', 'githubClose']);
  assert.doesNotMatch(dialog, /githubWorkerUrl|githubSave|githubDisconnect|authentication service URL|Save URL/);
  assert.match(html, /\.skill-dialog-kicker\{[^}]*text-transform:none/);
  assert.match(html, /<div class="skill-dialog-kicker">Integrations<\/div>/);
  assert.match(html, /<div class="skill-dialog-kicker">AI integrations<\/div>/);
});

test('GitHub sign-in uses the configured backend without exposing the service URL', () => {
  const html = read('app/Soil/ui/app.html');
  const js = read('app/Soil/ui/github-ui.js');
  const service = read('app/Soil/GitHubService.cs');
  assert.match(html, /src="github-ui.js"/);
  assert.match(js, /\/api\/github\/connect/);
  assert.match(js, /soil:github-connected/);
  assert.doesNotMatch(js, /githubWorkerUrl|githubSave|githubDisconnect|workerUrl|\/api\/github\/config/);
  assert.doesNotMatch(service.match(/public sealed record GitHubStatus\([\s\S]*?\);/)?.[0] || '', /WorkerUrl/);
  assert.match(service, /GitHub sign-in is not configured for this installation/);
});

test('VMs start on demand, restore single-use snapshots, and cleanly park when inactive', () => {
  const manager = read('app/Soil/VmManager.cs');
  const main = read('app/Soil/MainForm.cs');
  const paths = read('app/Soil/SoilPaths.cs');
  const ipc = read('engine/src/ipc.c');
  const vmhost = read('engine/src/vmhost.c');
  const snapshot = read('engine/src/snapshot.c');
  const whv = read('engine/src/winhv.c');
  const launch = manager.slice(manager.indexOf('private async Task LaunchAsync'), manager.indexOf('private async Task MonitorExitAsync'));
  assert.match(launch, /--snapshot-out/);
  assert.match(launch, /--restore/);
  assert.match(launch, /File\.Move\(snapshotPath, restorePath\)/);
  assert.match(paths, /MachineSnapshotPath/);
  assert.match(paths, /MachineRestorePath/);
  assert.match(manager, /runtime\.RestartAfterStop = true/);
  assert.match(manager, /runtime\.LastUiActivityUtc/);
  assert.match(manager, /DateTime\.UtcNow - runtime\.LastUiActivityUtc > TimeSpan\.FromSeconds\(90\)/);
  assert.match(main, /m_vmManager\.IdleAll\(\)/);
  assert.doesNotMatch(main, /m_vmManager\.ResumeAll\(\)/);
  assert.match(ipc, /fCleanExit = true/);
  assert.match(ipc, /input client disconnected; accepting a new connection/);
  assert.match(vmhost, /if \(pszSnapshotOut && pVm->fCleanExit\)/);
  assert.match(vmhost, /VhSnapLazyLoad\(pVm, uGpa\)/);
  assert.match(whv, /VhWhvMapRamChunk/);
  assert.match(snapshot, /VhWhvMapRamChunk\(pVm, uOff/);
  assert.doesNotMatch(vmhost, /C:\\Users\\Lenovo|VhWriteFbBmp/);
});

test('WHPX smoke test checks a real guest ready signal and an exited vmhost', () => {
  const smoke = read('engine/snapshot-smoke-test.ps1');
  assert.match(smoke, /SSH-2/);
  assert.match(smoke, /WaitForExit\(45000\)/);
  assert.match(smoke, /snap: restored in/);
  assert.match(smoke, /--restore/);
  assert.match(smoke, /--snapshot-out/);
});
