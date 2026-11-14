#!/usr/bin/env node
import { spawn } from 'node:child_process';
import { existsSync, mkdirSync, readFileSync, writeFileSync, rmSync, chmodSync, openSync, closeSync, statSync, readdirSync, createReadStream } from 'node:fs';
import net from 'node:net';
import os from 'node:os';
import path from 'node:path';
import crypto from 'node:crypto';
import { fileURLToPath } from 'node:url';

const repoRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..', '..');
const requiredImages = ['vmlinuz64', 'core2.gz', 'soil.squashfs'];
const defaultImagesDir = path.join(repoRoot, 'app', 'images');
const defaultStateDir = path.join(repoRoot, '.soil-ci');
const sshHost = 'tc@127.0.0.1';

function parseArgs(argv) {
  const options = {};
  const rest = [];
  let command = 'help';
  let i = 0;
  if (argv.length && !argv[0].startsWith('-')) {
    command = argv[0];
    i = 1;
  }
  for (; i < argv.length; i += 1) {
    const arg = argv[i];
    if (arg === '--') {
      rest.push(...argv.slice(i + 1));
      break;
    }
    if (!arg.startsWith('--')) {
      rest.push(arg);
      continue;
    }
    const body = arg.slice(2);
    const eq = body.indexOf('=');
    if (eq !== -1) {
      options[body.slice(0, eq)] = body.slice(eq + 1);
      continue;
    }
    if (['keep', 'json', 'help', 'quiet', 'clean'].includes(body)) {
      options[body] = true;
      continue;
    }
    const next = argv[i + 1];
    if (next === undefined || next.startsWith('--')) {
      options[body] = true;
      continue;
    }
    options[body] = next;
    i += 1;
  }
  return { command, options, rest };
}

function readOption(options, name, envName, fallback) {
  if (options[name] !== undefined) return String(options[name]);
  if (envName && process.env[envName] !== undefined && process.env[envName] !== '') return process.env[envName];
  return fallback;
}

function numberOption(options, name, envName, fallback) {
  const value = Number(readOption(options, name, envName, fallback));
  if (!Number.isFinite(value) || value <= 0) throw new Error(`Invalid --${name} value.`);
  return Math.trunc(value);
}

function resolvePath(value) {
  return path.resolve(value);
}

function defaultQemu() {
  const windowsQemu = 'C:\\Program Files\\qemu\\qemu-system-x86_64.exe';
  if (process.platform === 'win32' && existsSync(windowsQemu)) return windowsQemu;
  return 'qemu-system-x86_64';
}

function makeConfig(options) {
  return {
    imagesDir: resolvePath(readOption(options, 'images-dir', 'SOIL_IMAGES_DIR', defaultImagesDir)),
    imagesUrl: readOption(options, 'images-url', 'SOIL_IMAGES_URL', ''),
    imagesTarball: readOption(options, 'images-tarball', 'SOIL_IMAGES_TARBALL', ''),
    imagesToken: readOption(options, 'images-token', 'SOIL_IMAGES_TOKEN', ''),
    sshKey: readOption(options, 'ssh-key', 'SOIL_SSH_KEY', ''),
    sshPrivateKey: readOption(options, 'ssh-private-key', 'SOIL_SSH_PRIVATE_KEY', ''),
    qemu: readOption(options, 'qemu', 'SOIL_QEMU', defaultQemu()),
    ssh: readOption(options, 'ssh', 'SOIL_SSH', 'ssh'),
    curl: readOption(options, 'curl', 'SOIL_CURL', 'curl'),
    tar: readOption(options, 'tar', 'SOIL_TAR', 'tar'),
    stateDir: resolvePath(readOption(options, 'state-dir', 'SOIL_CI_STATE_DIR', defaultStateDir)),
    port: readOption(options, 'port', 'SOIL_SSH_PORT', ''),
    bootTimeoutMs: numberOption(options, 'boot-timeout', 'SOIL_BOOT_TIMEOUT_MS', 300000),
    memoryMb: numberOption(options, 'memory', 'SOIL_VM_MEMORY', 1024),
    cpus: numberOption(options, 'cpus', 'SOIL_VM_CPUS', 2),
    accel: readOption(options, 'accel', 'SOIL_QEMU_ACCEL', ''),
    keep: Boolean(options.keep),
  };
}

const config = makeConfig(parseArgs(process.argv.slice(2)).options);
const stateFile = path.join(config.stateDir, 'state.json');
const serialLog = path.join(config.stateDir, 'serial.log');
const qemuLog = path.join(config.stateDir, 'qemu.log');
const qemuErrLog = path.join(config.stateDir, 'qemu.err.log');

function log(message) {
  process.stdout.write(`${message}\n`);
}

function fail(message) {
  throw new Error(message);
}

function isAlive(pid) {
  if (!pid) return false;
  try {
    process.kill(pid, 0);
    return true;
  } catch {
    return false;
  }
}

function readState() {
  if (!existsSync(stateFile)) fail('No Soil CI VM state found. Run "node tools/soil-ci/soil-ci.mjs start" first.');
  return JSON.parse(readFileSync(stateFile, 'utf8'));
}

function writeState(state) {
  mkdirSync(config.stateDir, { recursive: true });
  writeFileSync(stateFile, `${JSON.stringify(state, null, 2)}\n`);
}

function publicState(state) {
  return {
    status: state.status,
    pid: state.pid,
    sshPort: state.sshPort,
    guest: state.guest,
    imagesDir: state.imagesDir,
    toolCount: state.toolCount,
    serialLog: state.serialLog,
    qemuLog: state.qemuLog,
  };
}

function runProcess(command, args, options = {}) {
  return new Promise((resolve) => {
    const child = spawn(command, args, {
      cwd: options.cwd || repoRoot,
      env: options.env || process.env,
      stdio: options.stdio || ['pipe', 'pipe', 'pipe'],
      windowsHide: true,
    });
    let stdout = '';
    let stderr = '';
    let settled = false;
    const maxBytes = options.maxBytes || 16 * 1024 * 1024;
    const timer = options.timeoutMs
      ? setTimeout(() => {
          if (!settled) {
            child.kill();
            finish(null, stdout, `${stderr}\nTimed out after ${options.timeoutMs}ms`);
          }
        }, options.timeoutMs)
      : null;
    function finish(code, out, err) {
      if (settled) return;
      settled = true;
      if (timer) clearTimeout(timer);
      resolve({ code, stdout: out, stderr: err });
    }
    child.stdout?.on('data', (chunk) => {
      if (stdout.length < maxBytes) stdout += chunk.toString();
    });
    child.stderr?.on('data', (chunk) => {
      if (stderr.length < maxBytes) stderr += chunk.toString();
    });
    child.on('error', (error) => finish(null, stdout, `${stderr}${error.message}`));
    child.on('close', (code) => finish(code, stdout, stderr));
    if (!options.stdio && child.stdin) {
      if (options.input) child.stdin.end(options.input);
      else child.stdin.end();
    }
  });
}

async function requireSuccess(result, description) {
  if (result.code !== 0) {
    fail(`${description} failed (exit ${result.code}).\n${result.stdout}\n${result.stderr}`.trim());
  }
  return result;
}

function sshCommonArgs(keyPath, port) {
  const knownHosts = process.platform === 'win32' ? 'NUL' : '/dev/null';
  return [
    '-i', keyPath,
    '-p', String(port),
    '-o', 'StrictHostKeyChecking=no',
    '-o', `UserKnownHostsFile=${knownHosts}`,
    '-o', 'BatchMode=yes',
    '-o', 'ConnectTimeout=8',
    '-o', 'LogLevel=ERROR',
    '-o', 'ServerAliveInterval=15',
  ];
}

async function sshScript(keyPath, port, script, timeoutMs = 90000) {
  const args = [...sshCommonArgs(keyPath, port), '-T', sshHost, 'sh', '-s'];
  return runProcess(config.ssh, args, { input: script, timeoutMs });
}

async function sshCommand(keyPath, port, command, args, timeoutMs = 60000) {
  const remote = [command, ...args].join(' ');
  const sshArgs = [...sshCommonArgs(keyPath, port), '-T', sshHost, remote];
  return runProcess(config.ssh, sshArgs, { timeoutMs });
}

function sha256(filePath) {
  return new Promise((resolve, reject) => {
    const hash = crypto.createHash('sha256');
    const stream = createReadStream(filePath);
    stream.on('error', reject);
    stream.on('data', (chunk) => hash.update(chunk));
    stream.on('end', () => resolve(hash.digest('hex')));
  });
}

function findFile(root) {
  const stack = [root];
  while (stack.length) {
    const dir = stack.pop();
    let entries;
    try {
      entries = readdirSync(dir, { withFileTypes: true });
    } catch {
      continue;
    }
    for (const entry of entries) {
      if (entry.isFile() && requiredImages.includes(entry.name)) return dir;
      if (entry.isDirectory() && !entry.isSymbolicLink()) stack.push(path.join(dir, entry.name));
    }
  }
  return null;
}

async function validateImages(dir) {
  for (const name of requiredImages) {
    const file = path.join(dir, name);
    if (!existsSync(file) || statSync(file).size < 1024) fail(`Guest image is missing or empty: ${file}`);
  }
  log(`Images: ${dir}`);
  for (const name of requiredImages) {
    const file = path.join(dir, name);
    log(`  ${name} ${statSync(file).size} bytes sha256=${await sha256(file)}`);
  }
  return dir;
}

async function prepareImages() {
  if (requiredImages.every((name) => existsSync(path.join(config.imagesDir, name)))) return validateImages(config.imagesDir);

  mkdirSync(config.stateDir, { recursive: true });
  const archive = path.join(config.stateDir, 'images.tar.gz');
  let source = config.imagesTarball;

  if (!source && config.imagesUrl) {
    const headerArgs = config.imagesToken ? ['-H', `Authorization: Bearer ${config.imagesToken}`] : [];
    const download = await runProcess(config.curl, [
      '--fail', '--location', '--retry', '3', '--silent', '--show-error',
      ...headerArgs, '--output', archive, config.imagesUrl,
    ], { timeoutMs: 600000 });
    if (download.code !== 0) fail(`Guest image download failed.\n${download.stderr}`);
    source = archive;
  }

  if (!source) {
    fail('Guest images were not found. Set SOIL_IMAGES_DIR, SOIL_IMAGES_TARBALL, or SOIL_IMAGES_URL.');
  }
  if (!existsSync(source)) fail(`Guest image archive was not found: ${source}`);

  const extractDir = path.join(config.stateDir, 'images');
  rmSync(extractDir, { recursive: true, force: true });
  mkdirSync(extractDir, { recursive: true });
  const extract = await runProcess(config.tar, ['-xzf', source, '-C', extractDir], { timeoutMs: 300000 });
  if (extract.code !== 0) fail(`Guest image archive extraction failed.\n${extract.stderr}`);

  const found = findFile(extractDir);
  if (!found) fail(`The guest image archive did not contain ${requiredImages.join(', ')}.`);
  return validateImages(found);
}

function resolveSshKey() {
  if (config.sshPrivateKey) {
    const keyPath = path.join(config.stateDir, 'id_ed25519');
    mkdirSync(config.stateDir, { recursive: true });
    writeFileSync(keyPath, `${config.sshPrivateKey.replace(/\r\n/g, '\n').trim()}\n`, { mode: 0o600 });
    if (process.platform !== 'win32') chmodSync(keyPath, 0o600);
    return keyPath;
  }
  const candidates = [config.sshKey, path.join(repoRoot, 'soil-keys', 'id_ed25519'), path.join(os.homedir(), '.soil', 'keys', 'ssh', 'id_ed25519')].filter(Boolean);
  for (const candidate of candidates) {
    if (existsSync(candidate)) {
      if (process.platform !== 'win32') chmodSync(candidate, 0o600);
      return candidate;
    }
  }
  fail('SSH private key was not found. Set SOIL_SSH_KEY or SOIL_SSH_PRIVATE_KEY.');
}

function hasKvm() {
  try {
    return existsSync('/dev/kvm') && (statSync('/dev/kvm').mode & 0o666) !== 0;
  } catch {
    return false;
  }
}

function acceleration() {
  if (config.accel) return config.accel;
  if (process.platform === 'linux' && hasKvm()) return 'kvm';
  return 'tcg';
}

function qemuAccelerationArgs(accel) {
  if (accel === 'kvm') return ['-accel', 'kvm', '-cpu', 'host'];
  if (accel === 'whpx') return ['-accel', 'whpx', '-cpu', 'max'];
  if (accel === 'tcg') return ['-accel', 'tcg,thread=multi', '-cpu', 'max'];
  return ['-accel', accel];
}

function qemuArgs(imagesDir, port) {
  const accel = acceleration();
  if (accel === 'kvm' && !hasKvm()) fail('/dev/kvm is unavailable or not writable. Use a runner with nested virtualization.');
  return [
    '-machine', 'pc',
    ...qemuAccelerationArgs(accel),
    '-smp', String(config.cpus),
    '-m', String(config.memoryMb),
    '-kernel', path.join(imagesDir, 'vmlinuz64'),
    '-initrd', path.join(imagesDir, 'core2.gz'),
    '-append', 'console=ttyS0 quiet loglevel=3 acpi=off noapic nolapic mitigations=off nozswap panic=-1',
    '-drive', `file=${path.join(imagesDir, 'soil.squashfs')},format=raw,if=virtio,readonly=on`,
    '-vga', 'std',
    '-netdev', `user,id=net0,hostfwd=tcp:127.0.0.1:${port}-:22`,
    '-device', 'ne2k_pci,netdev=net0',
    '-display', 'none',
    '-monitor', 'none',
    '-serial', `file:${serialLog}`,
    '-rtc', 'base=utc',
    '-no-reboot',
    '-name', 'soil-ci',
  ];
}

function allocatePort() {
  return new Promise((resolve, reject) => {
    const server = net.createServer();
    server.unref();
    server.on('error', reject);
    server.listen(0, '127.0.0.1', () => {
      const port = server.address().port;
      server.close(() => resolve(port));
    });
  });
}

function waitForPort(port, timeoutMs, isRunning) {
  return new Promise((resolve, reject) => {
    const deadline = Date.now() + timeoutMs;
    function attempt() {
      if (!isRunning()) return reject(new Error('QEMU exited before the guest SSH port became reachable.'));
      const socket = net.connect({ host: '127.0.0.1', port });
      socket.setTimeout(2000);
      socket.once('connect', () => {
        socket.destroy();
        resolve();
      });
      socket.once('timeout', () => {
        socket.destroy();
        retry();
      });
      socket.once('error', () => {
        socket.destroy();
        retry();
      });
    }
    function retry() {
      if (Date.now() >= deadline) return reject(new Error(`Guest SSH port 127.0.0.1:${port} did not open in time.`));
      setTimeout(attempt, 1000);
    }
    attempt();
  });
}

async function waitForSsh(keyPath, port, timeoutMs) {
  const deadline = Date.now() + timeoutMs;
  let last = '';
  while (Date.now() < deadline) {
    const result = await sshCommand(keyPath, port, 'true', [], 10000);
    if (result.code === 0) return;
    last = result.stderr || result.stdout;
    await new Promise((resolve) => setTimeout(resolve, 1000));
  }
  fail(`Guest SSH did not authenticate in time.${last ? `\n${last.trim()}` : ''}`);
}

const displayScript = `#!/bin/sh
pkill -f 'startx' 2>/dev/null || true
pkill -f 'Xorg :0' 2>/dev/null || true
pkill -f 'Xvfb' 2>/dev/null || true
sleep 1
sudo mkdir -p /etc/X11
sudo tee /etc/X11/xorg.conf >/dev/null <<'EOF'
Section "Device"
    Identifier "soil-vesa"
    Driver "vesa"
EndSection
Section "Screen"
    Identifier "soil-screen"
    Device "soil-vesa"
    DefaultDepth 24
    SubSection "Display"
        Depth 24
        Modes "1280x1024"
    EndSubSection
EndSection
EOF
sudo rm -f /dev/fb0
sudo Xorg :0 -config /etc/X11/xorg.conf -logfile /tmp/soil-xorg.log >/tmp/soil-xorg.out 2>&1 &
i=0
while [ "$i" -lt 30 ]; do
  if [ -S /tmp/.X11-unix/X0 ]; then break; fi
  i=$((i + 1))
  sleep 1
done
export DISPLAY=:0
export XDG_RUNTIME_DIR=/run/user/1001
if [ -S /run/user/1001/bus ]; then export DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1001/bus; fi
if ! xset q >/dev/null 2>&1; then
  echo XORG_FAILED
  tail -80 /tmp/soil-xorg.log 2>/dev/null || true
  tail -20 /tmp/soil-xorg.out 2>/dev/null || true
  exit 1
fi
feh --bg-fill /usr/local/share/soil/wallpaper.png >/tmp/soil-feh.log 2>&1 || true
pcmanfm --desktop >/tmp/soil-pcmanfm.log 2>&1 &
fluxbox >/tmp/soil-fluxbox.log 2>&1 &
sleep 2
ps w | grep -E '[p]cmanfm|[f]luxbox' || true
echo DISPLAY_READY=1
`;

async function configureDisplay(keyPath, port) {
  const result = await sshScript(keyPath, port, displayScript, 90000);
  if (result.code !== 0 || !result.stdout.includes('DISPLAY_READY=1')) {
    fail(`Guest display setup failed (exit ${result.code}).\n${result.stdout}\n${result.stderr}`.trim());
  }
}

function mcpProbeInput() {
  return [
    JSON.stringify({ jsonrpc: '2.0', id: 1, method: 'initialize', params: { protocolVersion: '2024-11-05', capabilities: {}, clientInfo: { name: 'soil-ci', version: '1' } } }),
    JSON.stringify({ jsonrpc: '2.0', method: 'notifications/initialized' }),
    JSON.stringify({ jsonrpc: '2.0', id: 2, method: 'tools/list' }),
  ].join('\n') + '\n';
}

async function probeMcp(keyPath, port) {
  const args = [...sshCommonArgs(keyPath, port), '-T', sshHost, '/opt/cua-driver/cua-driver-mcp'];
  const result = await runProcess(config.ssh, args, { input: mcpProbeInput(), timeoutMs: 120000, maxBytes: 32 * 1024 * 1024 });
  if (result.code !== 0) fail(`MCP probe failed (exit ${result.code}).\n${result.stderr}`);
  const responses = new Map();
  for (const line of result.stdout.split(/\r?\n/)) {
    if (!line.startsWith('{')) continue;
    try {
      const message = JSON.parse(line);
      if (message.id !== undefined) responses.set(message.id, message);
    } catch {
      continue;
    }
  }
  if (!responses.has(1) || responses.get(1).error) fail(`MCP initialize failed.\n${JSON.stringify(responses.get(1))}`);
  const list = responses.get(2);
  if (!list || list.error || !Array.isArray(list.result?.tools)) fail(`MCP tools/list failed.\n${JSON.stringify(list)}`);
  const names = list.result.tools.map((tool) => tool.name);
  if (!names.includes('list_windows')) fail(`MCP tools/list did not contain list_windows.\n${names.join(', ')}`);
  return names;
}

async function start() {
  const imagesDir = await prepareImages();
  const keyPath = resolveSshKey();
  const qemuVersion = await runProcess(config.qemu, ['--version'], { timeoutMs: 10000 });
  if (qemuVersion.code !== 0) fail(`QEMU was not found at "${config.qemu}".\n${qemuVersion.stderr}`);

  if (existsSync(stateFile)) {
    const previous = JSON.parse(readFileSync(stateFile, 'utf8'));
    if (isAlive(previous.pid)) fail(`A Soil CI VM is already running with PID ${previous.pid}.`);
  }

  mkdirSync(config.stateDir, { recursive: true });
  rmSync(serialLog, { force: true });
  rmSync(qemuLog, { force: true });
  rmSync(qemuErrLog, { force: true });
  const port = config.port ? Number(config.port) : await allocatePort();
  if (!Number.isInteger(port) || port <= 0 || port > 65535) fail('Invalid SSH port.');
  const args = qemuArgs(imagesDir, port);
  const outFd = openSync(qemuLog, 'a');
  const errFd = openSync(qemuErrLog, 'a');
  const child = spawn(config.qemu, args, {
    cwd: repoRoot,
    detached: true,
    stdio: ['ignore', outFd, errFd],
    windowsHide: true,
  });
  child.on('error', () => {});
  child.unref();
  closeSync(outFd);
  closeSync(errFd);
  if (!child.pid) fail('QEMU did not start.');

  const state = {
    status: 'booting',
    pid: child.pid,
    sshPort: port,
    guest: sshHost,
    imagesDir,
    sshKey: keyPath,
    serialLog,
    qemuLog,
    qemuErrLog,
    startedAt: new Date().toISOString(),
    toolCount: null,
  };
  writeState(state);
  log(`QEMU ${child.pid} booting; SSH forward 127.0.0.1:${port}; accelerator ${acceleration()}.`);

  try {
    await waitForPort(port, config.bootTimeoutMs, () => isAlive(child.pid));
    await waitForSsh(keyPath, port, Math.min(config.bootTimeoutMs, 180000));
    log('Guest SSH is ready.');
    await configureDisplay(keyPath, port);
    log('Guest display is ready.');
    const tools = await probeMcp(keyPath, port);
    state.status = 'ready';
    state.toolCount = tools.length;
    state.readyAt = new Date().toISOString();
    writeState(state);
    log(`MCP is ready with ${tools.length} tools.`);
    log(JSON.stringify(publicState(state), null, 2));
  } catch (error) {
    state.status = 'failed';
    state.error = error.message;
    writeState(state);
    await stop({ quiet: true });
    throw error;
  }
}

function killTree(pid, signal) {
  if (process.platform === 'win32') {
    const result = spawn('taskkill', ['/PID', String(pid), '/T', '/F'], { windowsHide: true, stdio: 'ignore' });
    result.unref();
    return;
  }
  try {
    process.kill(-pid, signal);
  } catch {
    try {
      process.kill(pid, signal);
    } catch {
      return;
    }
  }
}

async function waitForExit(pid, timeoutMs) {
  const deadline = Date.now() + timeoutMs;
  while (Date.now() < deadline) {
    if (!isAlive(pid)) return true;
    await new Promise((resolve) => setTimeout(resolve, 500));
  }
  return !isAlive(pid);
}

async function stop(options = {}) {
  if (!existsSync(stateFile)) {
    if (!options.quiet) log('No Soil CI VM state found.');
    return;
  }
  const state = JSON.parse(readFileSync(stateFile, 'utf8'));
  if (isAlive(state.pid)) {
    killTree(state.pid, 'SIGTERM');
    if (!(await waitForExit(state.pid, 10000))) {
      killTree(state.pid, 'SIGKILL');
      await waitForExit(state.pid, 5000);
    }
  }
  state.status = 'stopped';
  state.stoppedAt = new Date().toISOString();
  writeState(state);
  if (!options.quiet) log(`Stopped QEMU ${state.pid}.`);
  if (options.clean) rmSync(stateFile, { force: true });
}

async function status() {
  if (!existsSync(stateFile)) {
    log('No Soil CI VM state found.');
    return;
  }
  const state = JSON.parse(readFileSync(stateFile, 'utf8'));
  const running = isAlive(state.pid);
  log(JSON.stringify({ ...publicState(state), running }, null, 2));
  if (!running) process.exitCode = 1;
}

async function execute(rest) {
  if (!rest.length) fail('Usage: soil-ci exec -- <command> [args...]');
  const state = readState();
  if (!isAlive(state.pid)) fail(`QEMU ${state.pid} is not running.`);
  const remote = rest.map((part) => `'${part.replace(/'/g, `'\\''`)}'`).join(' ');
  const args = [...sshCommonArgs(state.sshKey, state.sshPort), '-T', sshHost, remote];
  const child = spawn(config.ssh, args, { stdio: 'inherit', windowsHide: true });
  const code = await new Promise((resolve) => child.on('exit', (exitCode) => resolve(exitCode ?? 1)));
  process.exitCode = code;
}

async function mcp() {
  const state = readState();
  if (!isAlive(state.pid)) fail(`QEMU ${state.pid} is not running.`);
  const args = [...sshCommonArgs(state.sshKey, state.sshPort), '-T', sshHost, '/opt/cua-driver/cua-driver-mcp'];
  const child = spawn(config.ssh, args, { stdio: ['pipe', 'pipe', 'pipe'], windowsHide: true });
  process.stdin.pipe(child.stdin);
  child.stdout.pipe(process.stdout);
  child.stderr.pipe(process.stderr);
  const shutdown = () => {
    child.kill();
  };
  process.once('SIGINT', shutdown);
  process.once('SIGTERM', shutdown);
  const code = await new Promise((resolve) => child.on('exit', (exitCode) => resolve(exitCode ?? 1)));
  process.exitCode = code;
}

async function smoke(options) {
  try {
    await start();
    if (!options.keep) await stop({ quiet: true });
  } catch (error) {
    await stop({ quiet: true });
    throw error;
  }
}

async function doctor() {
  const key = (() => {
    try {
      return resolveSshKey();
    } catch (error) {
      return `missing (${error.message})`;
    }
  })();
  const qemu = await runProcess(config.qemu, ['--version'], { timeoutMs: 10000 });
  log(JSON.stringify({
    platform: process.platform,
    imagesDir: config.imagesDir,
    imagesPresent: requiredImages.every((name) => existsSync(path.join(config.imagesDir, name))),
    sshKey: key,
    qemu: qemu.code === 0 ? qemu.stdout.split('\n')[0] : `missing (${qemu.stderr.trim()})`,
    accelerator: acceleration(),
    kvm: process.platform === 'linux' ? hasKvm() : false,
    stateDir: config.stateDir,
  }, null, 2));
}

function help() {
  log(`Usage: node tools/soil-ci/soil-ci.mjs <command> [options]

Commands:
  start       Boot the guest, wait for SSH and X, verify MCP, leave it running
  stop        Stop the running guest
  status      Show guest state
  exec -- CMD Run a command in the guest over SSH
  mcp         Proxy MCP stdio to the guest cua-driver
  smoke       Start, verify, and stop the guest
  doctor      Show local prerequisites

Options:
  --images-dir DIR        Directory containing vmlinuz64, core2.gz, soil.squashfs
  --images-url URL        Tar.gz URL for the guest images (SOIL_IMAGES_URL)
  --images-tarball FILE   Local tar.gz containing the guest images
  --ssh-key FILE          SSH private key authorized by the guest
  --ssh-private-key TEXT  Inline SSH private key
  --port PORT             SSH forward port (default: random)
  --boot-timeout MS       Boot timeout (default: 300000)
  --memory MB             Guest memory (default: 1024)
  --cpus N                Guest vCPUs (default: 2)
  --accel NAME            kvm, tcg, or whpx (default: kvm on Linux, tcg elsewhere)
  --state-dir DIR         State and logs directory (default: .soil-ci)
  --keep                  Keep the guest running after smoke
`);
}

async function main() {
  const parsed = parseArgs(process.argv.slice(2));
  if (parsed.options.help || parsed.command === 'help') return help();
  if (parsed.command === 'start') return start();
  if (parsed.command === 'stop') return stop({ clean: Boolean(parsed.options.clean) });
  if (parsed.command === 'status') return status();
  if (parsed.command === 'exec') return execute(parsed.rest);
  if (parsed.command === 'mcp') return mcp();
  if (parsed.command === 'smoke') return smoke({ keep: Boolean(parsed.options.keep) });
  if (parsed.command === 'doctor') return doctor();
  fail(`Unknown command "${parsed.command}". Run with --help.`);
}

main().catch((error) => {
  process.stderr.write(`soil-ci: ${error.message}\n`);
  process.exitCode = 1;
});
