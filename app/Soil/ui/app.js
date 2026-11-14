(() => {
  'use strict';

  const $ = (s) => document.querySelector(s);
  const $$ = (s) => [...document.querySelectorAll(s)];
  const root = document.documentElement;
  const win = $('#win');
  const list = $('#list');
  const screen = $('#screen');
  const image = $('#vmscreen');
  const unavailText = $('#unavail-text');
  const appFailure = $('#app-failure');
  const appErrorLog = $('#app-error-log');
  const hint = $('#hint');
  const charts = $('#charts');
  const themeSw = $('#theme');
  const dlgApp = $('#dlg-app');
  const dlgAgent = $('#dlg-agent');
  const agentPanel = $('#agent');
  const agentState = $('#agent-state');
  const agentCommand = $('#agent-command');
  const agentLog = $('#agent-log');
  const agentCopy = $('#agent-copy');
  const agentTerminal = $('#agent-terminal');
  const appRepo = $('#app-repo');
  const appNote = $('#app-note');
  const ghLogin = $('#gh-login');
  const ghState = $('#gh-state');
  const ghConnect = $('#gh-connect');
  const tabMachines = $('#tab-machines');
  const tabFiles = $('#tab-files');
  const machineNav = $('#machine-nav');
  const filesNav = $('#files-nav');
  const filesView = $('#files');
  const places = $('#places');
  const crumbs = $('#crumbs');
  const fileListEl = $('#file-list');
  const fileUp = $('#file-up');
  const previewEmpty = $('#preview-empty');
  const previewCard = $('#preview-card');
  const previewKind = $('#preview-kind');
  const previewName = $('#preview-name');
  const previewMeta = $('#preview-meta');
  const previewText = $('#preview-text');
  const previewImage = $('#preview-image');
  const reduce = matchMedia('(prefers-reduced-motion: reduce)').matches;

  const LABEL = { running: 'Running', stopped: 'Stopped', starting: 'Starting', stopping: 'Stopping', idle: 'Idle' };
  const APP_LABEL = {
    queued: 'Queued',
    fetching: 'Downloading',
    detecting: 'Inspecting',
    installing: 'Installing',
    packaging: 'Packaging',
    starting: 'Starting',
    ready: 'Ready',
    failed: 'Setup failed',
    stopped: 'Stopped'
  };
  const AGENT_LABEL = {
    queued: 'Queued',
    starting: 'Starting tunnel',
    preparing: 'Preparing tools',
    ready: 'Tools ready',
    failed: 'Setup failed',
    stopped: 'Stopped'
  };
  const state = {
    vms: [],
    selected: null,
    elements: new Map(),
    streamId: null,
    buttons: 0,
    moveX: 0,
    moveY: 0,
    moveQueued: false,
    keys: new Set(),
    view: 'machines',
    filePath: '',
    fileParent: null,
    fileEntries: [],
    selectedFile: null,
    ghStatus: null,
    ghPoll: 0,
    ghRepos: []
  };
  const cards = {};
  let pendingDeleteId = null;
  let fileRequest = 0;
  let previewRequest = 0;

  const current = () => state.vms.find((vm) => vm.id === state.selected) || null;
  const currentLive = () => {
    const vm = current();
    return vm && (vm.status === 'running' || vm.status === 'starting');
  };
  const currentControllable = () => {
    const vm = current();
    return vm && vm.status === 'running';
  };
  const gb = (mb) => (mb >= 1024 ? (mb / 1024).toFixed(mb % 1024 ? 1 : 0) + ' GB' : Math.round(mb) + ' MB');

  function ago(seconds) {
    if (seconds < 60) return 'just now';
    const minutes = Math.floor(seconds / 60);
    if (minutes < 60) return minutes + 'm';
    const hours = Math.floor(minutes / 60);
    if (hours < 24) return hours + 'h ' + (minutes % 60) + 'm';
    return Math.floor(hours / 24) + 'd ' + (hours % 24) + 'h';
  }

  /* ---------------- charts ---------------- */
  for (const el of $$('.card')) {
    const plot = el.querySelector('.plot');
    const cols = [];
    for (let i = 0; i < 24; i++) {
      const col = document.createElement('div');
      col.className = 'col';
      col.innerHTML = '<div class="stk"><i class="g1"></i><i class="g2"></i><i class="g3"></i></div>';
      plot.append(col);
      cols.push(col);
    }
    plot.offsetWidth;
    const ring = document.createElement('i');
    ring.className = 'ring';
    const tip = document.createElement('div');
    tip.className = 'tip';
    plot.append(ring, tip);
    const card = { el, plot, cols, ring, tip, at: -1, values: null, describe: null };
    cards[el.dataset.k] = card;
    wirePlot(card);
  }

  function wirePlot(card) {
    const { plot, cols, ring, tip } = card;
    const place = (index) => {
      if (index === card.at) return;
      if (card.at >= 0) cols[card.at].classList.remove('on');
      cols[index].classList.add('on');
      card.at = index;
      const col = cols[index];
      const left = col.offsetLeft;
      const width = col.offsetWidth;
      ring.style.width = width + 6 + 'px';
      ring.style.setProperty('--x', left - 3 + 'px');
      const flip = left + width + 174 > plot.offsetWidth;
      tip.style.setProperty('--tx', (flip ? left - 174 : left + width + 8) + 'px');
      const value = card.values ? card.values[index] : null;
      tip.textContent = value && card.describe ? card.describe(value, index) : '';
    };
    const hit = (event) => {
      if (!card.values) return;
      const rect = plot.getBoundingClientRect();
      const index = Math.min(23, Math.max(0, Math.floor((event.clientX - rect.left) / ((rect.width + 3) / 24))));
      plot.toggleAttribute('data-on', true);
      place(index);
    };
    plot.addEventListener('pointermove', hit);
    plot.addEventListener('pointerdown', hit);
    plot.addEventListener('pointerleave', () => {
      plot.removeAttribute('data-on');
      if (card.at >= 0) cols[card.at].classList.remove('on');
      card.at = -1;
    });
  }

  function padSeries(series) {
    if (!Array.isArray(series) || series.length === 0) return null;
    const out = new Array(24).fill(null);
    const take = Math.min(24, series.length);
    for (let i = 0; i < take; i++) out[24 - take + i] = series[i];
    return out;
  }

  function setBars(card, values, segments) {
    card.cols.forEach((col, i) => {
      const value = values[i];
      const bars = col.querySelectorAll('.stk i');
      bars.forEach((bar, b) => {
        bar.style.height = value ? Math.max(0, segments(value)[b] || 0).toFixed(1) + 'px' : '0px';
      });
    });
  }

  function drawCharts(vm) {
    const running = vm && vm.status === 'running' && vm.series && vm.series.length;
    for (const kind of ['cpu', 'mem', 'net']) {
      const card = cards[kind];
      const big = card.el.querySelector('[data-big]');
      const sub = card.el.querySelector('[data-sub]');
      const data = running ? padSeries(vm.series) : null;
      card.values = data;
      if (!data) {
        card.el.setAttribute('data-off', '');
        big.textContent = '—';
        sub.textContent = '';
        setBars(card, new Array(24).fill(null), () => []);
        card.describe = null;
        continue;
      }
      card.el.removeAttribute('data-off');
      const last = data[23];
      if (kind === 'cpu') {
        const cpuMax = 100;
        const scale = 146 / cpuMax;
        setBars(card, data, (v) => [v.cpu * 0.72 * scale, v.cpu * 0.22 * scale, v.cpu * 0.06 * scale]);
        big.textContent = Math.round(last.cpu) + '%';
        sub.textContent = 'of 1 vCPU';
        card.describe = (v, i) =>
          `${i === 23 ? 'Now' : (23 - i) * 2 + 's ago'}\n${Math.round(v.cpu)}%\n` +
          `${Math.round(v.cpu * 0.72)} user, ${Math.round(v.cpu * 0.22)} system, ${Math.round(v.cpu * 0.06)} waiting`;
      } else if (kind === 'mem') {
        const max = vm.ramMb || 1024;
        const scale = 146 / max;
        setBars(card, data, (v) => [v.mem * scale, 0, 0]);
        big.textContent = gb(last.mem);
        sub.textContent = 'of ' + gb(vm.ramMb);
        card.describe = (v, i) =>
          `${i === 23 ? 'Now' : (23 - i) * 2 + 's ago'}\n${gb(v.mem)} used\nof ${gb(vm.ramMb)}`;
      } else {
        const max = Math.max(10, ...data.map((v) => v ? (v.in || 0) + (v.out || 0) : 0)) * 1.15;
        const scale = 146 / max;
        setBars(card, data, (v) => [(v.in || 0) * scale, (v.out || 0) * scale, 0]);
        big.textContent = ((last.in || 0) + (last.out || 0)).toFixed(1) + ' Mbps';
        sub.textContent = 'in and out';
        card.describe = (v, i) =>
          `${i === 23 ? 'Now' : (23 - i) * 2 + 's ago'}\n${((v.in || 0) + (v.out || 0)).toFixed(1)} Mbps\n` +
          `${(v.in || 0).toFixed(1)} in, ${(v.out || 0).toFixed(1)} out`;
      }
    }
  }

  function renderAgent(vm) {
    const isAgent = vm && vm.kind === 'agent';
    if (state.view !== 'machines' || !isAgent) {
      agentPanel.hidden = true;
      return;
    }
    agentPanel.hidden = false;
    const agent = vm.agent || {};
    const tunnel = agent.tunnel || null;
    const label = AGENT_LABEL[agent.status] || agent.status || 'Preparing';
    if (vm.status === 'stopped') {
      agentState.textContent = 'Stopped. Start the machine to reload the tools and tunnel.';
    } else if (vm.status === 'starting') {
      agentState.textContent = 'Booting the machine...';
    } else if (agent.status === 'failed') {
      agentState.textContent = agent.error || 'Agent setup failed.';
    } else if (agent.status === 'ready') {
      const claude = agent.claudeVersion ? 'Claude Code ' + agent.claudeVersion : 'Claude Code';
      const codex = agent.codexVersion ? 'Codex ' + agent.codexVersion : 'Codex';
      agentState.textContent = claude + ' and ' + codex + ' are ready.';
    } else {
      agentState.textContent = label + '...';
    }
    agentCommand.textContent = tunnel
      ? tunnel.command
      : 'Start the machine to open the SSH tunnel.';
    agentCopy.disabled = !tunnel;
    agentTerminal.disabled = !tunnel || agent.status !== 'ready';
    agentLog.textContent = agent.log || '';
    agentLog.hidden = !agent.log || agent.status === 'ready';
  }

  /* ---------------- list ---------------- */
  function paintItem(vm) {
    const el = state.elements.get(vm.id);
    if (!el) return;
    const busy = vm.status === 'starting' || vm.status === 'stopping';
    el.classList.toggle('sel', vm.id === state.selected);
    el.classList.toggle('busy', busy);
    el.dataset.status = vm.status;
    el.querySelector('.t').textContent = vm.name;
    let meta = `${LABEL[vm.status] || vm.status}, ${vm.cpu} CPU, ${gb(vm.ramMb)}`;
    if (vm.status === 'running' && vm.uptimeSec > 30) meta += ' / ' + ago(vm.uptimeSec);
    if (vm.status === 'idle') meta += ' / parked on disk';
    if (vm.status === 'stopped' && vm.error) meta = 'Stopped / ' + vm.error;
    if (vm.app) {
      const appState = APP_LABEL[vm.app.status] || vm.app.status;
      if (vm.app.status === 'failed') meta = 'App failed / ' + (vm.app.error || 'setup error');
      else if (vm.app.status !== 'ready') meta = 'App / ' + appState;
      else meta = 'App ready / ' + (LABEL[vm.status] || vm.status);
    } else if (vm.agent) {
      const agentState = AGENT_LABEL[vm.agent.status] || vm.agent.status;
      if (vm.status === 'stopped') meta = 'Agent / Stopped';
      else if (vm.agent.status === 'failed') meta = 'Agent failed / ' + (vm.agent.error || 'setup error');
      else if (vm.agent.status !== 'ready') meta = 'Agent / ' + agentState;
      else meta = 'Agent ready / ' + (LABEL[vm.status] || vm.status);
    }
    el.querySelector('.mt').textContent = meta;
    const sw = el.querySelector('.sw');
    const on = vm.status === 'running' || vm.status === 'starting';
    sw.setAttribute('aria-checked', String(on));
    sw.setAttribute('aria-disabled', String(busy));
    sw.setAttribute('aria-label', `Power for ${vm.name}`);
    el.querySelector('.del').setAttribute('aria-label', `Delete ${vm.name}`);
  }

  function makeItem(vm) {
    const empty = list.querySelector('.empty-list');
    if (empty) empty.remove();
    const el = document.createElement('div');
    el.className = 'item';
    el.innerHTML =
      '<button class="pick" type="button"><span class="dot"></span><span class="tx">' +
      '<span class="t"></span><span class="mt"></span></span></button>' +
      '<button class="sw" type="button" role="switch" aria-checked="false"><span class="k"></span></button>' +
      '<button class="del" type="button" aria-label="Delete machine" title="Delete machine">' +
      '<svg viewBox="0 0 16 16" aria-hidden="true"><path d="M2.5 4.5h11M6 4.5V3h4v1.5M4 4.5l.6 8.5h6.8l.6-8.5M6.7 7v3.8M9.3 7v3.8" fill="none" stroke="currentColor" stroke-width="1.4" stroke-linecap="round" stroke-linejoin="round"/></svg>' +
      '</button>';
    el.querySelector('.pick').addEventListener('click', () => {
      state.selected = vm.id;
      for (const item of state.vms) paintItem(item);
      renderSelected();
    });
    el.querySelector('.sw').addEventListener('click', (event) => {
      event.stopPropagation();
      const fresh = state.vms.find((item) => item.id === vm.id);
      if (fresh) togglePower(fresh);
    });
    el.querySelector('.del').addEventListener('click', (event) => {
      event.stopPropagation();
      const fresh = state.vms.find((item) => item.id === vm.id);
      if (fresh) askDelete(fresh);
    });
    state.elements.set(vm.id, el);
    list.append(el);
    return el;
  }

  function renderList() {
    for (const [id, el] of state.elements) {
      if (!state.vms.some((vm) => vm.id === id)) {
        el.remove();
        state.elements.delete(id);
      }
    }
    for (const vm of state.vms) {
      if (!state.elements.has(vm.id)) makeItem(vm).classList.add('new');
      paintItem(vm);
    }
    const empty = list.querySelector('.empty-list');
    if (!state.vms.length && !empty) {
      list.innerHTML = '<p class="empty-list">No machines yet. Create one with +.</p>';
    } else if (state.vms.length && empty) {
      empty.remove();
    }
  }

  /* ---------------- screen ---------------- */
  function contentScale() {
    const rect = screen.getBoundingClientRect();
    return Math.min(rect.width / 1360, rect.height / 768);
  }

  function setUnavailable(text, log) {
    unavailText.textContent = text;
    appFailure.hidden = !log;
    appErrorLog.textContent = log || '';
  }

  function updateScreen(vm) {
    if (!vm) {
      setUnavailable('Select a machine');
      screen.classList.remove('live', 'locked');
      stopStream();
      return;
    }

    if (vm.app && vm.app.status === 'failed') {
      stopStream();
      screen.classList.remove('live', 'locked');
      if (document.pointerLockElement === screen) document.exitPointerLock();
      setUnavailable(
        'App setup failed: ' + (vm.app.error || 'unknown error'),
        vm.app.log || ''
      );
      return;
    }

    if (vm.status === 'running' || vm.status === 'starting') {
      screen.classList.add('live');
      if (state.streamId !== vm.id) {
        stopStream();
        state.streamId = vm.id;
        image.src = `/api/vms/${encodeURIComponent(vm.id)}/stream.mjpg?t=${Date.now()}`;
      }
      hint.textContent = vm.status === 'starting'
        ? 'Starting the machine…'
        : 'Click the screen to capture the keyboard and mouse';
      if (vm.status !== 'running' && document.pointerLockElement === screen) {
        document.exitPointerLock();
      }
    } else {
      stopStream();
      screen.classList.remove('live', 'locked');
      if (document.pointerLockElement === screen) document.exitPointerLock();
      if (vm.app && vm.app.status !== 'ready' && vm.app.status !== 'stopped') {
        setUnavailable((APP_LABEL[vm.app.status] || 'Preparing') + ' app...');
      } else {
        setUnavailable(
          vm.status === 'stopping'
            ? 'Stopping...'
            : vm.status === 'idle'
              ? 'Idle — parked on disk; this machine uses no CPU or memory'
              : vm.error
                ? vm.error
                : 'Start this machine to see its screen'
        );
      }
    }
  }

  function stopStream() {
    if (state.streamId !== null) {
      state.streamId = null;
      image.removeAttribute('src');
    }
  }

  image.addEventListener('error', () => {
    const vm = current();
    if (!vm || !currentLive()) return;
    state.streamId = null;
    setTimeout(() => {
      const again = current();
      if (again && currentLive()) updateScreen(again);
    }, 1200);
  });

  function renderSelected() {
    const vm = current();
    for (const item of state.vms) paintItem(item);
    updateScreen(vm);
    drawCharts(vm);
    renderAgent(vm);
  }

  /* ---------------- files ---------------- */
  const PLACES = [
    { label: 'Soil home', path: '' },
    { label: 'Machines', path: 'machines' },
    { label: 'Apps', path: 'apps' },
    { label: 'Skills', path: 'skills' },
    { label: 'Cache', path: 'cache' },
    { label: 'Logs', path: 'logs' }
  ];
  const FOLDER_ICON =
    '<svg viewBox="0 0 16 16" aria-hidden="true"><path d="M1.75 4.25h4l1.2 1.5h7.3v6.5a1 1 0 0 1-1 1H2.75a1 1 0 0 1-1-1z" fill="none" stroke="currentColor" stroke-width="1.3" stroke-linejoin="round"/></svg>';
  const FILE_ICON =
    '<svg viewBox="0 0 16 16" aria-hidden="true"><path d="M4 1.75h5l3 3v9.5H4z" fill="none" stroke="currentColor" stroke-width="1.3" stroke-linejoin="round"/><path d="M9 1.75v3h3" fill="none" stroke="currentColor" stroke-width="1.3" stroke-linejoin="round"/></svg>';

  function fileQuery(path, extra) {
    let query = '?path=' + encodeURIComponent(path || '');
    return extra ? query + '&' + extra : query;
  }

  async function fileJson(url, options) {
    const response = await fetch(url, { cache: 'no-store', ...options });
    if (!response.ok) {
      let message = 'Request failed (' + response.status + ')';
      try {
        const body = await response.json();
        if (body && body.error) message = body.error;
      } catch (_) {}
      throw new Error(message);
    }
    const text = await response.text();
    return text ? JSON.parse(text) : null;
  }

  function formatSize(bytes) {
    if (typeof bytes !== 'number' || bytes < 0) return '—';
    if (bytes < 1024) return bytes + ' B';
    const units = ['KB', 'MB', 'GB', 'TB'];
    let value = bytes / 1024;
    let unit = 0;
    while (value >= 1024 && unit < units.length - 1) {
      value /= 1024;
      unit += 1;
    }
    return (value >= 10 ? Math.round(value) : value.toFixed(1)) + ' ' + units[unit];
  }

  function formatWhen(value) {
    const date = new Date(value);
    if (Number.isNaN(date.getTime())) return '';
    return date.toLocaleString([], { month: 'short', day: 'numeric', hour: '2-digit', minute: '2-digit' });
  }

  function renderPlaces() {
    places.replaceChildren();
    for (const place of PLACES) {
      const button = document.createElement('button');
      button.type = 'button';
      button.className = 'place';
      button.dataset.path = place.path;
      button.innerHTML = FOLDER_ICON;
      const label = document.createElement('span');
      label.textContent = place.label;
      button.append(label);
      button.addEventListener('click', () => loadFiles(place.path));
      places.append(button);
    }
  }

  function paintPlaces() {
    for (const button of places.querySelectorAll('.place')) {
      const path = button.dataset.path || '';
      button.classList.toggle('sel', path === state.filePath);
    }
  }

  function renderCrumbs() {
    crumbs.replaceChildren();
    const home = document.createElement('button');
    home.type = 'button';
    home.className = 'crumb';
    home.textContent = 'Soil home';
    home.addEventListener('click', () => loadFiles(''));
    crumbs.append(home);

    let accumulated = '';
    for (const segment of state.filePath ? state.filePath.split('/') : []) {
      accumulated = accumulated ? accumulated + '/' + segment : segment;
      const separator = document.createElement('span');
      separator.className = 'crumb-sep';
      separator.textContent = '/';
      crumbs.append(separator);
      const button = document.createElement('button');
      button.type = 'button';
      button.className = 'crumb';
      button.textContent = segment;
      const target = accumulated;
      button.addEventListener('click', () => loadFiles(target));
      crumbs.append(button);
    }
  }

  function renderFiles() {
    fileListEl.replaceChildren();
    if (!state.fileEntries.length) {
      const empty = document.createElement('p');
      empty.className = 'file-empty';
      empty.textContent = 'This folder is empty.';
      fileListEl.append(empty);
      return;
    }
    for (const entry of state.fileEntries) {
      const row = document.createElement('button');
      row.type = 'button';
      row.className = 'frow';
      row.dataset.path = entry.path;
      row.title = entry.path;
      row.innerHTML =
        (entry.kind === 'folder' ? FOLDER_ICON : FILE_ICON) +
        '<span class="fname"></span><span class="fsize"></span><span class="fdate"></span>';
      row.querySelector('.fname').textContent = entry.name;
      row.querySelector('.fsize').textContent = entry.kind === 'folder' ? '' : formatSize(entry.size);
      row.querySelector('.fdate').textContent = formatWhen(entry.modifiedUtc);
      row.addEventListener('click', () => {
        if (entry.kind === 'folder') {
          loadFiles(entry.path);
        } else {
          previewFile(entry, row);
        }
      });
      fileListEl.append(row);
    }
  }

  function clearPreview() {
    state.selectedFile = null;
    previewRequest += 1;
    previewCard.hidden = true;
    previewEmpty.hidden = false;
    previewText.hidden = true;
    previewText.textContent = '';
    previewImage.hidden = true;
    previewImage.removeAttribute('src');
  }

  async function previewFile(entry, row) {
    const ticket = ++previewRequest;
    for (const selected of fileListEl.querySelectorAll('.frow.sel')) selected.classList.remove('sel');
    row.classList.add('sel');
    state.selectedFile = entry;
    previewEmpty.hidden = true;
    previewCard.hidden = false;
    previewKind.textContent = entry.kind === 'link' ? 'Link' : 'File';
    previewName.textContent = entry.name;
    previewMeta.textContent = entry.path + ' · ' + formatSize(entry.size) + ' · ' + formatWhen(entry.modifiedUtc);
    previewText.hidden = true;
    previewText.textContent = '';
    previewImage.hidden = true;
    previewImage.removeAttribute('src');

    if (/\.(png|jpe?g|gif|webp|svg)$/i.test(entry.name)) {
      previewKind.textContent = 'Image';
      previewImage.src = '/api/files/raw' + fileQuery(entry.path, 't=' + Date.now());
      previewImage.hidden = false;
      return;
    }

    try {
      const data = await fileJson('/api/files/content' + fileQuery(entry.path));
      if (ticket !== previewRequest) return;
      if (data.kind === 'text') {
        previewKind.textContent = 'Text';
        previewText.textContent = data.content;
      } else {
        previewText.textContent = 'Binary file. Use Open to view it with the default app.';
      }
      previewText.hidden = false;
    } catch (error) {
      if (ticket !== previewRequest) return;
      previewText.textContent = error.message;
      previewText.hidden = false;
    }
  }

  previewImage.addEventListener('error', () => {
    previewImage.hidden = true;
    previewText.textContent = 'This image could not be shown. Use Open instead.';
    previewText.hidden = false;
  });

  async function loadFiles(path) {
    const ticket = ++fileRequest;
    state.view = 'files';
    try {
      const data = await fileJson('/api/files' + fileQuery(path || ''));
      if (ticket !== fileRequest) return;
      state.filePath = data.path || '';
      state.fileParent = data.parent;
      state.fileEntries = data.entries || [];
      renderCrumbs();
      renderFiles();
      paintPlaces();
      clearPreview();
      fileUp.disabled = data.parent === null || data.parent === undefined;
    } catch (error) {
      if (ticket !== fileRequest) return;
      fileListEl.replaceChildren();
      const message = document.createElement('p');
      message.className = 'file-empty';
      message.textContent = error.message;
      fileListEl.append(message);
    }
  }

  function setView(view) {
    state.view = view;
    const files = view === 'files';
    tabMachines.classList.toggle('active', !files);
    tabFiles.classList.toggle('active', files);
    machineNav.hidden = files;
    filesNav.hidden = !files;
    screen.hidden = files;
    charts.hidden = files;
    filesView.hidden = !files;
    if (files) {
      agentPanel.hidden = true;
      loadFiles(state.filePath);
    } else {
      renderAgent(current());
    }
  }

  async function openFile(path) {
    try {
      await fileJson('/api/files/open' + fileQuery(path), { method: 'POST' });
    } catch (error) {
      console.error(error);
    }
  }

  async function revealFile(path) {
    try {
      await fileJson('/api/files/reveal' + fileQuery(path), { method: 'POST' });
    } catch (error) {
      console.error(error);
    }
  }

  /* ---------------- GitHub apps ---------------- */
  function setGhStatus(status) {
    state.ghStatus = status;
    const configured = Boolean(status && status.configured);
    const connected = Boolean(status && status.connected);
    if (status && status.connecting) {
      ghLogin.textContent = 'GitHub';
      ghState.textContent = 'Waiting for GitHub...';
      ghConnect.textContent = 'Waiting';
      ghConnect.disabled = true;
    } else if (connected) {
      ghLogin.textContent = status.login || 'GitHub';
      ghState.textContent = 'Connected';
      ghConnect.textContent = 'Disconnect';
      ghConnect.disabled = false;
    } else {
      ghLogin.textContent = 'GitHub account';
      ghState.textContent = status && status.error
        ? status.error
        : configured ? 'Not connected' : 'Set the service URL in Settings';
      ghConnect.textContent = 'Connect';
      ghConnect.disabled = !configured;
    }
    appRepo.disabled = !connected;
    if (!connected) {
      if (appRepo.options.length === 0) {
        const option = document.createElement('option');
        option.value = '';
        optionsText(option, configured);
        appRepo.append(option);
      } else {
        optionsText(appRepo.options[0], configured);
      }
    }
  }

  function optionsText(option, configured) {
    option.textContent = configured
      ? 'Connect GitHub to list repositories'
      : 'GitHub service not configured';
  }

  async function refreshGhStatus() {
    try {
      const status = await fileJson('/api/github/status');
      setGhStatus(status);
      return status;
    } catch (error) {
      ghState.textContent = error.message;
      return null;
    }
  }

  async function loadAppRepos() {
    const status = state.ghStatus;
    if (!status || !status.connected) return;
    const ticket = ++state.ghPoll;
    appRepo.disabled = true;
    appRepo.replaceChildren();
    const loading = document.createElement('option');
    loading.value = '';
    loading.textContent = 'Loading repositories...';
    appRepo.append(loading);
    try {
      const data = await fileJson('/api/github/repos');
      if (ticket !== state.ghPoll) return;
      state.ghRepos = data.repos || [];
      appRepo.replaceChildren();
      const placeholder = document.createElement('option');
      placeholder.value = '';
      placeholder.textContent = state.ghRepos.length
        ? 'Choose a repository...'
        : 'No repositories found';
      appRepo.append(placeholder);
      for (const repo of state.ghRepos) {
        const option = document.createElement('option');
        option.value = repo.htmlUrl || ('https://github.com/' + repo.fullName);
        option.textContent = repo.fullName + (repo.private ? ' (private)' : '');
        option.dataset.owner = repo.owner || '';
        option.dataset.repo = repo.name || '';
        option.dataset.ref = repo.defaultBranch || '';
        appRepo.append(option);
      }
      appRepo.disabled = false;
    } catch (error) {
      appRepo.replaceChildren();
      const failed = document.createElement('option');
      failed.value = '';
      failed.textContent = error.message;
      appRepo.append(failed);
      appRepo.disabled = true;
    }
  }

  async function waitForGitHub() {
    const ticket = ++state.ghPoll;
    try {
      for (let i = 0; i < 200; i++) {
        await new Promise((resolve) => setTimeout(resolve, 1500));
        if (ticket !== state.ghPoll) return;
        const status = await fileJson('/api/github/status');
        setGhStatus(status);
        if (status.connected) {
          await loadAppRepos();
          return;
        }
        if (!status.connecting) return;
      }
    } catch (error) {
      ghState.textContent = error.message;
      ghConnect.disabled = false;
    }
  }

  async function connectGitHub() {
    if (state.ghStatus && state.ghStatus.connected) {
      if (!confirm('Disconnect GitHub from Soil?')) return;
      try {
        const status = await fileJson('/api/github/disconnect', { method: 'POST' });
        state.ghPoll += 1;
        setGhStatus(status);
        appRepo.replaceChildren();
      } catch (error) {
        ghState.textContent = error.message;
      }
      return;
    }

    ghConnect.disabled = true;
    ghConnect.textContent = 'Opening...';
    try {
      const status = await fileJson('/api/github/connect', { method: 'POST' });
      setGhStatus(status);
      if (status.connecting) waitForGitHub();
    } catch (error) {
      ghState.textContent = error.message;
      ghConnect.disabled = false;
      ghConnect.textContent = 'Connect';
    }
  }

  function repoNameFromUrl(value) {
    const match = String(value || '').match(/github\.com[/:]([^/]+)\/([^/#?]+)/i) ||
      String(value || '').match(/^([^/]+)\/([^/]+)$/);
    if (!match) return '';
    return match[2].replace(/\.git$/i, '');
  }

  async function openAppDialog() {
    $('#app-form').reset();
    $('#f-repo').value = '';
    $('#f-app-name').value = '';
    appNote.textContent = 'JavaScript apps need Node.js 20 or newer on this PC. Static apps work without it.';
    $('#app-progress').hidden = true;
    $('#app-progress-text').textContent = '';
    $('#app-log').textContent = '';
    $('#app-form').querySelector('button[type="submit"]').disabled = false;
    appRepo.replaceChildren();
    const option = document.createElement('option');
    option.value = '';
    option.textContent = 'Connect GitHub to list repositories';
    appRepo.append(option);
    appRepo.disabled = true;
    state.ghPoll += 1;
    dlgApp.showModal();
    const status = await refreshGhStatus();
    if (status && status.connected) await loadAppRepos();
  }

  async function createApp(event) {
    event.preventDefault();
    const urlInput = $('#f-repo').value.trim();
    const selected = appRepo.selectedOptions[0];
    const selectedUrl = selected && selected.value ? selected.value : '';
    const repoUrl = urlInput || selectedUrl;
    if (!repoUrl) {
      appNote.textContent = 'Choose a repository or paste a GitHub URL.';
      return;
    }

    let owner = null;
    let repo = null;
    let ref = null;
    if (!urlInput && selectedUrl) {
      owner = selected.dataset.owner || null;
      repo = selected.dataset.repo || null;
      ref = selected.dataset.ref || null;
    }

    let name = $('#f-app-name').value.trim();
    if (!name) name = repoNameFromUrl(repoUrl) || 'app';
    const size = new FormData($('#app-form')).get('app-size') || 'm';
    const submit = $('#app-form').querySelector('button[type="submit"]');
    submit.disabled = true;
    appNote.textContent = 'Creating the machine and fetching the repository...';
    $('#app-progress').hidden = false;
    $('#app-progress-text').textContent = 'Starting import';
    $('#app-log').textContent = '';

    try {
      const vm = await fileJson('/api/apps', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ name, repoUrl, owner, repo, ref, size })
      });
      dlgApp.close();
      merge(vm);
      state.selected = vm.id;
      renderSelected();
      await load();
      setTimeout(load, 1500);
      setTimeout(load, 3500);
    } catch (error) {
      appNote.textContent = error.message;
      $('#app-progress-text').textContent = 'Import failed';
      $('#app-log').textContent = error.message;
      submit.disabled = false;
    }
  }

  async function saveGitHubService() {
    const button = $('#gh-service-save');
    const stateLabel = $('#gh-service-state');
    button.disabled = true;
    stateLabel.textContent = 'Saving...';
    try {
      const status = await fileJson('/api/github/config', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ workerUrl: $('#gh-service').value.trim() })
      });
      setGhStatus(status);
      $('#gh-service').value = status.workerUrl || '';
      stateLabel.textContent = status.configured ? 'Saved' : 'Service URL cleared';
      if (!status.configured) appRepo.replaceChildren();
    } catch (error) {
      stateLabel.textContent = error.message;
    } finally {
      button.disabled = false;
    }
  }

  /* ---------------- actions ---------------- */
  async function parse(response) {
    if (!response.ok) throw new Error('Request failed (' + response.status + ')');
    return response.json();
  }

  function merge(vm) {
    const at = state.vms.findIndex((item) => item.id === vm.id);
    if (at >= 0) {
      state.vms[at] = vm;
    } else {
      state.vms.push(vm);
      const el = makeItem(vm);
      el.classList.add('new');
      setTimeout(() => el.classList.remove('new'), 650);
    }
    paintItem(vm);
    if (vm.id === state.selected) renderSelected();
  }

  async function load() {
    try {
      const vms = await parse(await fetch('/api/vms', { cache: 'no-store' }));
      state.vms = vms;
      if (!state.selected || !state.vms.some((vm) => vm.id === state.selected)) {
        state.selected = state.vms.length ? state.vms[0].id : null;
      }
      renderList();
      renderSelected();
    } catch (error) {
      setUnavailable('The local Soil service is not responding.');
    }
  }

  async function copyText(text) {
    if (navigator.clipboard?.writeText) {
      try {
        await navigator.clipboard.writeText(text);
        return true;
      } catch (error) {
      }
    }
    const area = document.createElement('textarea');
    area.value = text;
    area.style.position = 'fixed';
    area.style.opacity = '0';
    document.body.append(area);
    area.select();
    let copied = false;
    try {
      copied = document.execCommand('copy');
    } catch (error) {
    }
    area.remove();
    return copied;
  }

  async function copyAppLog() {
    const button = $('#copy-app-log');
    const log = appErrorLog.textContent || '';
    if (!log) return;
    const copied = await copyText(log);
    const previous = button.textContent;
    button.textContent = copied ? 'Copied' : 'Copy failed';
    setTimeout(() => { button.textContent = previous; }, 1600);
  }

  async function copyAgentCommand() {
    const vm = current();
    const command = vm && vm.agent && vm.agent.tunnel
      ? vm.agent.tunnel.command
      : '';
    if (!command) return;
    const copied = await copyText(command);
    const previous = agentCopy.textContent;
    agentCopy.textContent = copied ? 'Copied' : 'Copy failed';
    setTimeout(() => { agentCopy.textContent = previous; }, 1600);
  }

  async function openAgentTerminal() {
    const vm = current();
    if (!vm) return;
    const previous = agentTerminal.textContent;
    agentTerminal.disabled = true;
    agentTerminal.textContent = 'Opening...';
    try {
      await fileJson(`/api/vms/${encodeURIComponent(vm.id)}/terminal`, {
        method: 'POST'
      });
      agentTerminal.textContent = 'Opened';
    } catch (error) {
      agentState.textContent = error.message;
    } finally {
      setTimeout(() => {
        const now = current();
        agentTerminal.disabled = !now?.agent?.tunnel || now.agent.status !== 'ready';
        agentTerminal.textContent = previous;
      }, 1200);
    }
  }

  async function retryApp() {
    const vm = current();
    if (!vm) return;
    const button = $('#retry-app');
    const previous = button.textContent;
    button.disabled = true;
    button.textContent = 'Retrying...';
    try {
      const updated = await parse(await fetch(
        `/api/vms/${encodeURIComponent(vm.id)}/start`,
        { method: 'POST' }
      ));
      merge(updated);
      setTimeout(load, 500);
      setTimeout(load, 1600);
    } catch (error) {
      setUnavailable(
        'Could not retry the app: ' + error.message,
        appErrorLog.textContent || ''
      );
    } finally {
      setTimeout(() => {
        button.disabled = false;
        button.textContent = previous;
      }, 1600);
    }
  }

  async function togglePower(vm) {
    if (vm.status === 'starting' || vm.status === 'stopping') return;
    const live = vm.status === 'running';
    state.selected = vm.id;
    try {
      renderSelected();
    } catch (error) {
      console.error(error);
    }
    try {
      const updated = await parse(await fetch(
        `/api/vms/${encodeURIComponent(vm.id)}/${live ? 'stop' : 'start'}`,
        { method: 'POST' }
      ));
      merge(updated);
      setTimeout(load, 400);
      setTimeout(load, 1400);
    } catch (error) {
      setUnavailable('Could not change the power state: ' + error.message);
    }
  }

  function askDelete(vm) {
    pendingDeleteId = vm.id;
    $('#del-name').textContent = vm.name;
    $('#dlg-del').showModal();
  }

  async function deleteVm(event) {
    event.preventDefault();
    const id = pendingDeleteId;
    if (!id) return;
    $('#dlg-del').close();
    pendingDeleteId = null;
    try {
      await parse(await fetch(`/api/vms/${encodeURIComponent(id)}`, { method: 'DELETE' }));
      if (state.streamId === id) stopStream();
      state.elements.get(id)?.remove();
      state.elements.delete(id);
      state.vms = state.vms.filter((vm) => vm.id !== id);
      if (state.selected === id) {
        state.selected = state.vms.length ? state.vms[0].id : null;
      }
      renderList();
      renderSelected();
      await load();
    } catch (error) {
      setUnavailable('Could not delete the machine: ' + error.message);
    }
  }

  async function createVm(event) {
    event.preventDefault();
    const name = $('#f-name').value.trim();
    if (!name) return;
    const size = new FormData(event.target).get('size') || 'm';
    $('#dlg-new').close();
    try {
      const vm = await parse(await fetch('/api/vms', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ name: name.slice(0, 40), size })
      }));
      merge(vm);
      state.selected = vm.id;
      renderSelected();
      const started = await parse(await fetch(
        `/api/vms/${encodeURIComponent(vm.id)}/start`, { method: 'POST' }
      ));
      merge(started);
      setTimeout(load, 500);
    } catch (error) {
      setUnavailable('Could not create the machine: ' + error.message);
    }
  }

  function openAgentDialog() {
    $('#f-agent-name').value = 'agent-' + String(state.vms.length + 1).padStart(2, '0');
    dlgAgent.showModal();
    $('#f-agent-name').focus();
    $('#f-agent-name').select();
  }

  async function createAgent(event) {
    event.preventDefault();
    const name = $('#f-agent-name').value.trim();
    if (!name) return;
    dlgAgent.close();
    try {
      const vm = await fileJson('/api/agents', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ name: name.slice(0, 40) })
      });
      merge(vm);
      state.selected = vm.id;
      renderSelected();
      const started = await fileJson(
        `/api/vms/${encodeURIComponent(vm.id)}/start`, { method: 'POST' });
      merge(started);
      setTimeout(load, 700);
    } catch (error) {
      setUnavailable('Could not create the agent machine: ' + error.message);
    }
  }

  /* ---------------- input ---------------- */
  function sendInput(message) {
    if (!currentControllable()) return;
    fetch(`/api/vms/${encodeURIComponent(state.selected)}/input`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(message)
    }).catch(() => {});
  }

  function releaseAll() {
    for (const vk of state.keys) sendInput({ type: 'key', vk, down: false });
    state.keys.clear();
    state.buttons = 0;
    if (currentControllable()) sendInput({ type: 'release' });
  }

  function flushMove() {
    state.moveQueued = false;
    const dx = Math.round(state.moveX);
    const dy = Math.round(state.moveY);
    state.moveX -= dx;
    state.moveY -= dy;
    if (!dx && !dy) return;
    sendInput({ type: 'move', dx, dy, buttons: state.buttons });
  }

  function queueMove(dx, dy) {
    state.moveX += dx;
    state.moveY += dy;
    if (!state.moveQueued) {
      state.moveQueued = true;
      requestAnimationFrame(flushMove);
    }
  }

  screen.addEventListener('click', (event) => {
    if (event.target.closest('.expand')) return;
    if (event.target.closest('.app-failure')) return;
    if (!currentControllable()) return;
    if (document.pointerLockElement !== screen) {
      const request = screen.requestPointerLock?.();
      if (request && typeof request.catch === 'function') request.catch(() => {});
    }
  });

  screen.addEventListener('contextmenu', (event) => {
    if (currentControllable()) event.preventDefault();
  });

  document.addEventListener('pointerlockchange', () => {
    const locked = document.pointerLockElement === screen;
    screen.classList.toggle('locked', locked);
    if (!locked) releaseAll();
  });

  document.addEventListener('mousemove', (event) => {
    if (document.pointerLockElement !== screen || !currentControllable()) return;
    const scale = contentScale();
    if (scale <= 0) return;
    queueMove(event.movementX / scale, -event.movementY / scale);
  });

  document.addEventListener('mousedown', (event) => {
    if (document.pointerLockElement !== screen || !currentControllable()) return;
    const bit = event.button === 0 ? 1 : event.button === 2 ? 2 : event.button === 1 ? 4 : 0;
    if (!bit) return;
    state.buttons |= bit;
    sendInput({ type: 'button', buttons: state.buttons });
    event.preventDefault();
  });

  document.addEventListener('mouseup', (event) => {
    if (document.pointerLockElement !== screen) return;
    const bit = event.button === 0 ? 1 : event.button === 2 ? 2 : event.button === 1 ? 4 : 0;
    if (!bit) return;
    state.buttons &= ~bit;
    sendInput({ type: 'button', buttons: state.buttons });
    event.preventDefault();
  });

  document.addEventListener('wheel', (event) => {
    if (document.pointerLockElement === screen) event.preventDefault();
  }, { passive: false });

  document.addEventListener('keydown', (event) => {
    if (document.pointerLockElement !== screen || !currentControllable()) return;
    if (event.key === 'Escape') return;
    if (state.keys.has(event.keyCode)) {
      sendInput({ type: 'key', vk: event.keyCode, down: true });
    } else {
      state.keys.add(event.keyCode);
      sendInput({ type: 'key', vk: event.keyCode, down: true });
    }
    event.preventDefault();
  });

  document.addEventListener('keyup', (event) => {
    if (document.pointerLockElement !== screen) return;
    state.keys.delete(event.keyCode);
    sendInput({ type: 'key', vk: event.keyCode, down: false });
    event.preventDefault();
  });

  window.addEventListener('blur', releaseAll);

  /* ---------------- theme / expand ---------------- */
  function isDark() {
    return root.dataset.theme
      ? root.dataset.theme === 'dark'
      : matchMedia('(prefers-color-scheme: dark)').matches;
  }

  function setTheme(dark) {
    const apply = () => {
      root.dataset.theme = dark ? 'dark' : 'light';
      themeSw.setAttribute('aria-checked', String(dark));
      try { localStorage.setItem('soil-theme', root.dataset.theme); } catch (_) {}
    };
    if (!document.startViewTransition || reduce) {
      apply();
      return;
    }
    const rect = themeSw.getBoundingClientRect();
    const x = rect.left + rect.width / 2;
    const y = rect.top + rect.height / 2;
    const radius = Math.hypot(Math.max(x, innerWidth - x), Math.max(y, innerHeight - y));
    const transition = document.startViewTransition(apply);
    transition.ready
      .then(() => root.animate(
        { clipPath: [`circle(0px at ${x}px ${y}px)`, `circle(${radius}px at ${x}px ${y}px)`] },
        { duration: 650, easing: 'cubic-bezier(.22,1,.36,1)', pseudoElement: '::view-transition-new(root)' }
      ))
      .catch(() => {});
  }

  function toggleExpanded() {
    const flip = () => {
      win.classList.toggle('exp');
      screen.focus({ preventScroll: true });
    };
    if (!document.startViewTransition || reduce) {
      flip();
      return;
    }
    screen.style.viewTransitionName = 'screen';
    document.startViewTransition(flip).finished.finally(() => {
      screen.style.viewTransitionName = '';
    });
  }

  document.addEventListener('keydown', (event) => {
    if (event.key === 'Escape' && win.classList.contains('exp') && !document.pointerLockElement) {
      toggleExpanded();
    }
  });

  themeSw.addEventListener('click', () => setTheme(!isDark()));

  /* ---------------- dialogs ---------------- */
  $('#open-set').addEventListener('click', async () => {
    $('#dlg-set').showModal();
    try {
      const info = await fileJson('/api/info');
      $('#soil-home').textContent = info.home;
    } catch (_) {}
    try {
      const status = await fileJson('/api/github/status');
      $('#gh-service').value = status.workerUrl || '';
      $('#gh-service-state').textContent = status.configured
        ? (status.connected ? 'Connected as ' + (status.login || 'GitHub') : 'Configured')
        : 'Optional, for private repositories';
      setGhStatus(status);
    } catch (error) {
      $('#gh-service-state').textContent = error.message;
    }
  });
  $('#open-skill').addEventListener('click', async () => {
    const dialog = $('#dlg-skill');
    const content = $('#skill-content');
    const results = $('#skill-results');
    const button = $('#open-skill');
    dialog.showModal();
    button.disabled = true;
    button.querySelector('span').textContent = 'Installing…';
    content.textContent = 'Loading Soil commands…';
    results.textContent = 'Looking for AI assistants…';
    try {
      const [installResults, skillResponse] = await Promise.all([
        fileJson('/api/skill/install', { method: 'POST' }),
        fetch('/api/skill', { cache: 'no-store' })
      ]);
      if (!skillResponse.ok) throw new Error('The Soil skill could not be loaded.');
      content.textContent = await skillResponse.text();
      results.replaceChildren();
      for (const item of installResults) {
        const row = document.createElement('div');
        row.className = 'install-result ' + item.status;
        const title = document.createElement('strong');
        title.textContent = item.agent + ': ' + ({
          installed: 'installed',
          already_installed: 'already installed',
          conflict: 'existing skill left unchanged',
          not_detected: 'not detected',
          error: 'installation failed'
        }[item.status] || item.status);
        row.append(title);
        if (item.path) {
          const path = document.createElement('code');
          path.textContent = item.path;
          row.append(path);
        }
        if (item.message) {
          const message = document.createElement('span');
          message.textContent = item.message;
          row.append(message);
        }
        results.append(row);
      }
    } catch (error) {
      results.textContent = 'Could not install the Soil skill: ' + error.message;
      content.textContent = error.message;
    } finally {
      button.disabled = false;
      button.querySelector('span').textContent = 'Install Skill';
    }
  });
  $('#close-skill').addEventListener('click', () => $('#dlg-skill').close());
  $('#open-home').addEventListener('click', () => openFile(''));
  $('#close-set').addEventListener('click', () => $('#dlg-set').close());
  $('#add').addEventListener('click', () => {
    $('#f-name').value = 'soil-' + String(state.vms.length + 1).padStart(2, '0');
    $('#dlg-new').showModal();
    $('#f-name').focus();
    $('#f-name').select();
  });
  $('#add-app').addEventListener('click', openAppDialog);
  $('#add-agent').addEventListener('click', openAgentDialog);
  $('#cancel-new').addEventListener('click', () => $('#dlg-new').close());
  $('#cancel-app').addEventListener('click', () => dlgApp.close());
  $('#cancel-agent').addEventListener('click', () => dlgAgent.close());
  $('#new-form').addEventListener('submit', createVm);
  $('#app-form').addEventListener('submit', createApp);
  $('#agent-form').addEventListener('submit', createAgent);
  agentCopy.addEventListener('click', copyAgentCommand);
  agentTerminal.addEventListener('click', openAgentTerminal);
  $('#gh-connect').addEventListener('click', connectGitHub);
  $('#gh-service-save').addEventListener('click', saveGitHubService);
  appRepo.addEventListener('change', () => {
    const option = appRepo.selectedOptions[0];
    if (!option || !option.value) return;
    $('#f-repo').value = '';
    if (!$('#f-app-name').value.trim()) {
      const name = option.dataset.repo || repoNameFromUrl(option.value);
      if (name) $('#f-app-name').value = name;
    }
  });
  $('#f-repo').addEventListener('input', () => {
    if ($('#f-app-name').value.trim()) return;
    const name = repoNameFromUrl($('#f-repo').value);
    if (name) $('#f-app-name').value = name;
  });
  $('#cancel-del').addEventListener('click', () => $('#dlg-del').close());
  $('#del-form').addEventListener('submit', deleteVm);
  $('#dlg-del').addEventListener('close', () => { pendingDeleteId = null; });
  $('#expand').addEventListener('click', toggleExpanded);
  $('#copy-app-log').addEventListener('click', copyAppLog);
  $('#retry-app').addEventListener('click', retryApp);
  tabMachines.addEventListener('click', () => setView('machines'));
  tabFiles.addEventListener('click', () => setView('files'));
  fileUp.addEventListener('click', () => {
    if (state.fileParent !== null && state.fileParent !== undefined) loadFiles(state.fileParent);
  });
  $('#file-refresh').addEventListener('click', () => loadFiles(state.filePath));
  $('#file-open').addEventListener('click', () => openFile(state.filePath));
  $('#preview-open').addEventListener('click', () => {
    if (state.selectedFile) openFile(state.selectedFile.path);
  });
  $('#preview-reveal').addEventListener('click', () => {
    if (state.selectedFile) revealFile(state.selectedFile.path);
  });
  themeSw.setAttribute('aria-checked', String(isDark()));

  renderPlaces();
  setView('machines');
  load();
  setInterval(load, 2000);
})();
