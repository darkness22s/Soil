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
