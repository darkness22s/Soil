
  const reduce = matchMedia('(prefers-reduced-motion: reduce)').matches;

  const stage = document.querySelector('.stage');
  const pg0 = document.getElementById('pg0');
  const pg1 = document.getElementById('pg1');
  const pg2 = document.getElementById('pg2');
  const titles = Array.from(document.querySelectorAll('.title'));
  const mark = document.getElementById('mark');
  const btn0 = document.getElementById('btn0');
  const btn1 = document.getElementById('btn1');
  const bloom1 = document.getElementById('bloom1');
  const btn2 = document.getElementById('btn2');
  const bloom2 = document.getElementById('bloom2');
  const btn3 = document.getElementById('btn3');
  const bloom3 = document.getElementById('bloom3');
  const pg3 = document.getElementById('pg3');
  const agList = document.getElementById('agmList');
  const btn4 = document.getElementById('btn4');
  const bloom4 = document.getElementById('bloom4');
  const pg4 = document.getElementById('pg4');
  const dd = document.getElementById('dd');
  const ddt = document.getElementById('ddt');
  const ddLabel = document.getElementById('ddLabel');
  const ddMenu = document.getElementById('ddMenu');
  const statusEl = document.getElementById('status');

  function api(path, opts) {
    const init = opts || {};
    init.headers = Object.assign({ Accept: 'application/json' }, init.headers || {});
    return fetch(path, init).then(r => r.text().then(txt => {
      let data = null;
      if (txt) {
        try { data = JSON.parse(txt); } catch (_) { data = txt; }
      }
      if (!r.ok) {
        const msg = data && data.error ? data.error
          : (typeof data === 'string' ? data : ('Request failed (' + r.status + ')'));
        throw new Error(msg);
      }
      return data;
    }));
  }
  function postJson(path, body) {
    return api(path, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(body || {})
    });
  }

  const lerp = (a, b, t) => a + (b - a) * t;
  const clamp = (v, a, b) => Math.max(a, Math.min(b, v));

  const INK_LIGHT = [23, 21, 21];
  const INK_DARK = [245, 245, 243];
  let INK = INK_LIGHT.slice();
  const ACC = ['#4b7cff', '#49a2ff', '#e85b83', '#f2c34c'].map(h => [
    parseInt(h.slice(1, 3), 16), parseInt(h.slice(3, 5), 16), parseInt(h.slice(5, 7), 16)
  ]);

  const rootEl = document.documentElement;
  const tgl = document.getElementById('theme');

  function setInk() {
    INK = (rootEl.classList.contains('dark') ? INK_DARK : INK_LIGHT).slice();
  }

  api('/api/ui', { cache: 'no-store' }).then(st => {
    if (st && st.theme === 'dark') {
      rootEl.classList.add('dark');
      tgl.setAttribute('aria-pressed', 'true');
      setInk();
      if (base) buildGrid();
    }
  }).catch(() => {});

  titles.forEach(title => {
    const wordEls = Array.from(title.querySelectorAll('.w > span'));
    let k = 0;
    wordEls.forEach(w => {
      const txt = w.textContent;
      w.textContent = '';
      txt.split('').forEach(c => {
        const s = document.createElement('span');
        s.className = 'ch';
        s.textContent = c;
        s.style.setProperty('--k', k++);
        w.appendChild(s);
      });
    });
  });

  let chars = [];
  function bindChars() {
    const vis = document.querySelector('.page:not([hidden]) .title');
    chars = vis ? Array.from(vis.querySelectorAll('.ch')).map(el => ({ el, x: 0, y: 0, f: 0, v: 0 })) : [];
  }

  let cursor = null;
  let slotCX = 0, slotCY = 0;

  mark.addEventListener('click', e => e.preventDefault());

  const waves = [];
  const parts = [];
  let burstT0 = -1;

  const cv = document.getElementById('dots');
  const cx2 = cv.getContext('2d');
  let W = 0, H = 0, dpr = 1, base = null, grid = [];

  function buildGrid() {
    dpr = Math.min(devicePixelRatio || 1, 2);
    W = innerWidth; H = innerHeight;
    cv.width = W * dpr; cv.height = H * dpr;
    cv.style.width = W + 'px'; cv.style.height = H + 'px';
    cx2.setTransform(dpr, 0, 0, dpr, 0, 0);
    grid = [];
    const gap = 26, m = 18;
    for (let y = m; y < H; y += gap)
      for (let x = m; x < W; x += gap)
        grid.push({ x, y, s: (x * 7 + y * 13) % 4 });
    base = document.createElement('canvas');
    base.width = W * dpr; base.height = H * dpr;
    const b = base.getContext('2d');
    b.setTransform(dpr, 0, 0, dpr, 0, 0);
    b.fillStyle = `rgba(${INK[0]},${INK[1]},${INK[2]},.055)`;
    for (const d of grid) {
      b.beginPath();
      b.arc(d.x, d.y, 1.15, 0, 7);
      b.fill();
    }
  }

  function measure() {
    const r = document.querySelector('.page:not([hidden]) .mark-slot');
    if (r) {
      const rr = r.getBoundingClientRect();
      slotCX = rr.left + rr.width / 2;
      slotCY = rr.top + rr.height / 2;
    }
    for (const c of chars) {
      const cr = c.el.getBoundingClientRect();
      c.x = cr.left + cr.width / 2;
      c.y = cr.top + cr.height / 2;
    }
  }

  stage.addEventListener('pointermove', e => { cursor = e; });
  stage.addEventListener('pointerleave', () => { cursor = null; });
  stage.addEventListener('pointerdown', e => {
    if (e.target.closest('.connect-btn') || e.target.closest('#dd')) return;
    waves.push({ x: e.clientX, y: e.clientY, t: performance.now() });
  });

  function spawnParts(x, y) {
    for (let i = 0; i < 16; i++) {
      const a = Math.random() * Math.PI * 2;
      const sp = 2 + Math.random() * 2.6;
      parts.push({
        x, y,
        vx: Math.cos(a) * sp,
        vy: Math.sin(a) * sp - 1.2,
        c: i % 4,
        life: 1
      });
    }
  }

  function centerOf(el) {
    const r = el.getBoundingClientRect();
    return [r.left + r.width / 2, r.top + r.height / 2];
  }

  function bloomAt(btn, bloomEl, px, py) {
    const r = btn.getBoundingClientRect();
    bloomEl.style.setProperty('--bx', (px - r.left) + 'px');
    bloomEl.style.setProperty('--by', (py - r.top) + 'px');
    bloomEl.classList.remove('go');
    void bloomEl.offsetWidth;
    bloomEl.classList.add('go');
  }

  function runFlow(btn, bloomEl, doneText, hold, onHold, statusText, work) {
    if (btn.classList.contains('is-loading') || btn.classList.contains('is-done')) return;
    const isKb = flowX == null;
    const px = isKb ? centerOf(btn)[0] : flowX;
    const py = isKb ? centerOf(btn)[1] : flowY;
    flowX = null; flowY = null;
    bloomAt(btn, bloomEl, px, py);
    statusEl.textContent = statusText || (btn === btn1 ? 'Connecting to GitHub...' : 'Setting up your workspace...');
    btn.classList.add('is-loading');
    btn.querySelector('.done .txt').textContent = doneText;
    const minimum = new Promise(resolve => setTimeout(resolve, 1900));
    Promise.all([minimum, work ? work() : Promise.resolve()]).then(() => {
      btn.classList.remove('is-loading');
      btn.classList.add('is-done');
      statusEl.textContent = doneText === 'Connected' ? 'Connected to GitHub.' : 'Onboarding complete.';
      burstT0 = performance.now();
      spawnParts(px, py);
      waves.push({ x: px, y: py, t: performance.now() });
      waves.push({ x: slotCX, y: slotCY, t: performance.now() });
      if (hold > 0) setTimeout(onHold, hold);
    }).catch(error => {
      btn.classList.remove('is-loading');
      btn.classList.remove('is-done');
      statusEl.textContent = error && error.message ? error.message : 'Something went wrong. Try again.';
    });
  }

  function connectGithub() {
    function status() {
      return api('/api/github/status', { cache: 'no-store' });
    }
    return status().then(st => {
      if (st && st.connected) return st;
      if (!st || !st.configured) {
        const worker = window.prompt('GitHub service URL', '');
        if (!worker || !worker.trim()) throw new Error('A GitHub service URL is required.');
        return postJson('/api/github/config', { workerUrl: worker.trim() }).then(status);
      }
      return st;
    }).then(st => {
      if (st && st.connected) return st;
      return postJson('/api/github/connect', {}).then(() => {
        statusEl.textContent = 'Finish signing in with GitHub in your browser...';
        return new Promise((resolve, reject) => {
          const started = Date.now();
          const timer = setInterval(() => {
            status().then(s => {
              if (s && s.connected) {
                clearInterval(timer);
                resolve(s);
              } else if (s && s.error) {
                clearInterval(timer);
                reject(new Error(s.error));
              } else if (Date.now() - started > 180000) {
                clearInterval(timer);
                reject(new Error('GitHub connection timed out.'));
              }
            }).catch(() => {});
          }, 1000);
        });
      });
    });
  }

  let flowX = null, flowY = null;
  btn1.addEventListener('click', e => {
    flowX = e.clientX || null; flowY = e.clientY || null;
    if (flowX === 0 && flowY === 0) { flowX = null; flowY = null; }
    runFlow(
      btn1, bloom1, 'Connected', 1500,
      () => switchPage(purpose === 'agents' ? pg3 : pg4),
      'Connecting to GitHub...',
      connectGithub
    );
  });

  const useBtns = Array.from(document.querySelectorAll('.use-card'));
  const useCardsEl = document.getElementById('useCards');
  let purpose = 'testing';
  useBtns.forEach(c => {
    c.addEventListener('click', () => {
      purpose = c.dataset.id;
      useBtns.forEach(x => x.setAttribute('aria-checked', x === c ? 'true' : 'false'));
      const [px, py] = centerOf(c);
      waves.push({ x: px, y: py, t: performance.now() });
      statusEl.textContent = 'Selected ' + c.dataset.label + '.';
    });
  });
  useCardsEl.addEventListener('keydown', e => {
    let i = useBtns.findIndex(x => x.getAttribute('aria-checked') === 'true');
    if (i < 0) i = 0;
    if (e.key === 'ArrowDown' || e.key === 'ArrowRight') {
      e.preventDefault();
      i = (i + 1) % useBtns.length;
    } else if (e.key === 'ArrowUp' || e.key === 'ArrowLeft') {
      e.preventDefault();
      i = (i - 1 + useBtns.length) % useBtns.length;
    } else {
      return;
    }
    useBtns[i].click();
    useBtns[i].focus();
  });

  const AGENTS = [
    ['codex', 'Codex'],
    ['claude', 'Claude Code'],
    ['antigravity', 'Antigravity CLI'],
    ['sparky', 'Sparky']
  ];
  const agIcons = Array.from(document.querySelectorAll('#pg0 .lg'));
  const agmBtns = [];
  AGENTS.forEach((a, i) => {
    const row = document.createElement('button');
    row.type = 'button';
    row.className = 'agm-item';
    row.setAttribute('role', 'checkbox');
    row.setAttribute('aria-checked', 'true');
    row.dataset.id = a[0];
    row.style.setProperty('--j', i);
    const ic = agIcons[i];
    row.innerHTML =
      '<span class="uc-check"><svg viewBox="0 0 20 20" aria-hidden="true"><path d="M4 10.5l4 4 8-9"/></svg></span>' +
      '<span class="ag-ic">' + (ic ? ic.querySelector('svg, img').outerHTML : '') + '</span>' +
      '<span class="ag-nm">' + a[1] + '</span>';
    agList.appendChild(row);
    agmBtns.push(row);
    row.addEventListener('click', () => {
      const on = row.getAttribute('aria-checked') === 'true';
      row.setAttribute('aria-checked', on ? 'false' : 'true');
      const [px, py] = centerOf(row);
      waves.push({ x: px, y: py, t: performance.now() });
      const n = agmBtns.filter(x => x.getAttribute('aria-checked') === 'true').length;
      statusEl.textContent = n + (n === 1 ? ' agent selected.' : ' agents selected.');
    });
  });

  function switchToGithub() {
    statusEl.textContent = '';
    const [bx, by] = centerOf(btn0);
    const go = () => {
      pg0.hidden = true;
      pg1.hidden = false;
      bindChars();
      measure();
    };
    const done = () => {
      bindChars();
      measure();
      setTimeout(() => waves.push({ x: W / 2, y: H * .42, t: performance.now() }), 60);
    };
    const start = () => {
      if (document.startViewTransition && !reduce && !vtActive) {
        vtActive = true;
        rootEl.classList.remove('vt-in', 'vt-out', 'vt-page');
        rootEl.classList.add('vt-page');
        vtVars(bx, by);
        document.startViewTransition(go).finished.finally(() => {
          rootEl.classList.remove('vt-page');
          vtActive = false;
          done();
        });
      } else {
        go();
        done();
      }
    };
    start();
  }

  btn0.addEventListener('click', () => {
    postJson('/api/ui', { purpose: purpose }).catch(() => {});
    if (purpose === 'agents') switchToGithub();
    else straightCreate();
  });

  function straightCreate() {
    swapTo(pg4, pg0);
  }

  let vtActive = false;
  function vtVars(x, y) {
    rootEl.style.setProperty('--vt-x', x.toFixed(0) + 'px');
    rootEl.style.setProperty('--vt-y', y.toFixed(0) + 'px');
    rootEl.style.setProperty('--vt-r', Math.ceil(
      Math.max(
        Math.hypot(x, y), Math.hypot(W - x, y),
        Math.hypot(x, H - y), Math.hypot(W - x, H - y)
      ) + 8
    ) + 'px');
  }

  function swapTo(target, originEl) {
    statusEl.textContent = '';
    const [bx, by] = centerOf(originEl);
    const go = () => {
      originEl.hidden = true;
      target.hidden = false;
      target.classList.add('settled');
      bindChars();
      measure();
    };
    const start = () => {
      if (document.startViewTransition && !reduce && !vtActive) {
        vtActive = true;
        rootEl.classList.remove('vt-in', 'vt-out', 'vt-page');
        rootEl.classList.add('vt-page');
        vtVars(bx, by);
        document.startViewTransition(go).finished.finally(() => {
          rootEl.classList.remove('vt-page');
          vtActive = false;
          bindChars();
          measure();
          setTimeout(() => waves.push({ x: W / 2, y: H * .42, t: performance.now() }), 60);
        });
      } else {
        go();
        setTimeout(() => waves.push({ x: W / 2, y: H * .42, t: performance.now() }), 60);
      }
    };
    start();
  }

  function switchPage(next) {
    swapTo(next || pg2, pg1);
  }

  const REPOS = [
    ['sparky-ai-code/Sparky', 'main - updated 2d ago'],
    ['sparky-ai-code/sparky-website', 'main - updated 5d ago'],
    ['acme-co/platform', 'develop - updated 1h ago'],
    ['octocat/hello-world', 'main - updated 3w ago'],
    ['rust-core/agent-engine', 'trunk - updated 6h ago']
  ];
  const REPO_IC = '<svg class="di-ic" viewBox="0 0 16 16" aria-hidden="true" focusable="false"><path d="M2 2.5A2.5 2.5 0 0 1 4.5 0h8.75a.75.75 0 0 1 .75.75v12.5a.75.75 0 0 1-.75.75h-2.5a.75.75 0 0 1 0-1.5h1.75v-2h-8a1 1 0 0 0-.714 1.7.75.75 0 1 1-1.072 1.05A2.495 2.495 0 0 1 2 11.5Zm10.5-1h-8a1 1 0 0 0-1 1v6.708A2.486 2.486 0 0 1 4.5 9h8ZM5 12.25a.25.25 0 0 1 .25-.25h3.5a.25.25 0 0 1 .25.25v3.25a.25.25 0 0 1-.4.2l-1.45-1.087a.249.249 0 0 0-.3 0L5.4 15.7a.25.25 0 0 1-.4-.2Z"/></svg>';
  REPOS.forEach((r, i) => {
    const item = document.createElement('button');
    item.type = 'button';
    item.className = 'dd-item';
    item.setAttribute('role', 'option');
    item.setAttribute('aria-selected', 'false');
    item.style.setProperty('--j', i);
    item.innerHTML = `${REPO_IC}<span class="di-name">${r[0]}</span><span class="di-meta">${r[1]}</span>`;
    ddMenu.appendChild(item);
  });
  const items = Array.from(ddMenu.children);
  let chosen = null;

  function openMenu() {
    dd.classList.add('open');
    ddt.setAttribute('aria-expanded', 'true');
    setKb(-1);
  }
  function closeMenu() {
    dd.classList.remove('open');
    ddt.setAttribute('aria-expanded', 'false');
    setKb(-1);
  }
  let kb = -1;
  function setKb(i) {
    kb = i;
    items.forEach((it, j) => it.classList.toggle('kb-active', j === i));
  }

  ddt.addEventListener('click', () => {
    if (dd.classList.contains('open')) closeMenu();
    else openMenu();
  });
  document.addEventListener('pointerdown', e => {
    if (!dd.contains(e.target) && dd.classList.contains('open')) closeMenu();
  });
  dd.addEventListener('keydown', e => {
    if (e.key === 'Escape') { closeMenu(); ddt.focus(); return; }
    if (!dd.classList.contains('open')) return;
    if (e.key === 'ArrowDown') { e.preventDefault(); setKb(Math.min(items.length - 1, kb + 1)); }
    else if (e.key === 'ArrowUp') { e.preventDefault(); setKb(Math.max(0, kb - 1)); }
    else if (e.key === 'Enter' && kb >= 0) { e.preventDefault(); items[kb].click(); }
  });
  items.forEach(it => {
    it.addEventListener('mouseenter', () => setKb(items.indexOf(it)));
    it.addEventListener('click', () => {
      chosen = it.querySelector('.di-name').textContent.trim();
      items.forEach(x => x.setAttribute('aria-selected', x === it ? 'true' : 'false'));
      ddLabel.textContent = chosen;
      dd.classList.add('has');
      closeMenu();
      const [dx, dy] = centerOf(ddt);
      waves.push({ x: dx, y: dy, t: performance.now() });
      statusEl.textContent = `Selected ${chosen}.`;
    });
  });

  btn2.addEventListener('click', e => {
    if (!chosen) {
      ddt.classList.remove('shake');
      void ddt.offsetWidth;
      ddt.classList.add('shake');
      const [dx, dy] = centerOf(ddt);
      waves.push({ x: dx, y: dy, t: performance.now() });
      openMenu();
      statusEl.textContent = 'Pick a repository first.';
      return;
    }
    flowX = e.clientX || null; flowY = e.clientY || null;
    if (flowX === 0 && flowY === 0) { flowX = null; flowY = null; }
    let appId = null;
    const parts = chosen.split('/');
    runFlow(
      btn2, bloom2, 'All set', 1500,
      () => launchApp(btn2, appId, { package: 'tiny' }),
      'Creating your machine...',
      () => postJson('/api/apps', {
        name: parts[1] || 'app',
        repoUrl: 'https://github.com/' + chosen,
        owner: parts[0] || '',
        repo: parts[1] || '',
        ref: null,
        size: 'tiny'
      }).then(dto => { appId = dto.id; })
    );
  });

  btn3.addEventListener('click', e => {
    const picked = agmBtns.filter(x => x.getAttribute('aria-checked') === 'true').map(x => x.dataset.id);
    if (picked.length === 0) {
      agList.classList.remove('shake');
      void agList.offsetWidth;
      agList.classList.add('shake');
      const [gx, gy] = centerOf(agList);
      waves.push({ x: gx, y: gy, t: performance.now() });
      statusEl.textContent = 'Pick at least one agent.';
      return;
    }
    flowX = e.clientX || null; flowY = e.clientY || null;
    if (flowX === 0 && flowY === 0) { flowX = null; flowY = null; }
    let agentId = null;
    runFlow(
      btn3, bloom3, 'All set', 1500,
      () => launchApp(btn3, agentId, { agents: picked, package: 'tiny' }),
      'Creating your machine...',
      () => postJson('/api/agents', { name: 'cloud-agents' }).then(dto => {
        agentId = dto.id;
        return postJson('/api/vms/' + encodeURIComponent(dto.id) + '/start', {}).catch(() => null);
      })
    );
  });

  const pkgDd = document.getElementById('pkgDd');
  const pkgTrig = document.getElementById('pkgTrig');
  const pkgLabel = document.getElementById('pkgLabel');
  const pkgMenu = document.getElementById('pkgMenu');
  const PACKAGES = [
    ['1 vCPU \u00b7 512 MB', 'tiny', false, 'tiny'],
    ['1 vCPU \u00b7 2 GiB', 'nano', false, 'nano'],
    ['2 vCPU \u00b7 4 GiB', 'Recommended', true, 'standard'],
    ['4 vCPU \u00b7 8 GiB', 'pro', false, 'pro'],
    ['8 vCPU \u00b7 16 GiB', 'wide', false, 'wide']
  ];
  PACKAGES.forEach((p, i) => {
    const item = document.createElement('button');
    item.type = 'button';
    item.className = 'dd-item';
    item.setAttribute('role', 'option');
    item.setAttribute('aria-selected', 'false');
    item.dataset.size = p[3];
    item.style.setProperty('--j', i);
    item.innerHTML = '<span class="di-name">' + p[0] + '</span><span class="di-meta' + (p[2] ? ' rec' : '') + '">' + p[1] + '</span>';
    pkgMenu.appendChild(item);
  });
  const pkgItems = Array.from(pkgMenu.children);
  let chosenPkg = null;
  let chosenPkgSize = '';
  function openPkg() {
    pkgDd.classList.add('open');
    pkgTrig.setAttribute('aria-expanded', 'true');
    setKbP(-1);
  }
  function closePkg() {
    pkgDd.classList.remove('open');
    pkgTrig.setAttribute('aria-expanded', 'false');
    setKbP(-1);
  }
  let kbP = -1;
  function setKbP(i) {
    kbP = i;
    pkgItems.forEach((it, j) => it.classList.toggle('kb-active', j === i));
  }
  pkgTrig.addEventListener('click', () => {
    if (pkgDd.classList.contains('open')) closePkg();
    else openPkg();
  });
  document.addEventListener('pointerdown', e => {
    if (!pkgDd.contains(e.target) && pkgDd.classList.contains('open')) closePkg();
  });
  pkgDd.addEventListener('keydown', e => {
    if (e.key === 'Escape') { closePkg(); pkgTrig.focus(); return; }
    if (!pkgDd.classList.contains('open')) return;
    if (e.key === 'ArrowDown') { e.preventDefault(); setKbP(Math.min(pkgItems.length - 1, kbP + 1)); }
    else if (e.key === 'ArrowUp') { e.preventDefault(); setKbP(Math.max(0, kbP - 1)); }
    else if (e.key === 'Enter' && kbP >= 0) { e.preventDefault(); pkgItems[kbP].click(); }
  });
  pkgItems.forEach(it => {
    it.addEventListener('mouseenter', () => setKbP(pkgItems.indexOf(it)));
    it.addEventListener('click', () => {
      chosenPkg = it.querySelector('.di-name').textContent.trim();
      chosenPkgSize = it.dataset.size || 'tiny';
      pkgItems.forEach(x => x.setAttribute('aria-selected', x === it ? 'true' : 'false'));
      pkgLabel.textContent = chosenPkg;
      pkgDd.classList.add('has');
      closePkg();
      const [dx, dy] = centerOf(pkgTrig);
      waves.push({ x: dx, y: dy, t: performance.now() });
      statusEl.textContent = `Selected ${chosenPkg}.`;
    });
  });

  btn4.addEventListener('click', e => {
    if (!chosenPkg) {
      pkgTrig.classList.remove('shake');
      void pkgTrig.offsetWidth;
      pkgTrig.classList.add('shake');
      const [gx, gy] = centerOf(pkgTrig);
      waves.push({ x: gx, y: gy, t: performance.now() });
      statusEl.textContent = 'Pick a package first.';
      return;
    }
    flowX = e.clientX || null; flowY = e.clientY || null;
    if (flowX === 0 && flowY === 0) { flowX = null; flowY = null; }
    const size = chosenPkgSize || 'tiny';
    let machineId = null;
    runFlow(
      btn4, bloom4, 'All set', 1500,
      () => launchApp(btn4, machineId, { package: size }),
      'Creating your machine...',
      () => postJson('/api/vms', {
        name: purpose === 'testing' ? 'test-machine' : 'linux-machine',
        size: size
      }).then(dto => {
        machineId = dto.id;
        return postJson('/api/vms/' + encodeURIComponent(dto.id) + '/start', {}).catch(() => null);
      })
    );
  });

  function launchApp(fromBtn, id, extra) {
    try { if (chosen) localStorage.setItem('sparky-last-repo', chosen); } catch (_) {}
    const state = Object.assign({
      onboardingComplete: true,
      purpose: purpose,
      theme: rootEl.classList.contains('dark') ? 'dark' : 'light'
    }, extra || {});
    let sent = false;
    try {
      if (navigator.sendBeacon) {
        sent = navigator.sendBeacon('/api/ui', new Blob([JSON.stringify(state)], { type: 'application/json' }));
      }
    } catch (_) {}
    if (!sent) postJson('/api/ui', state).catch(() => {});
    const b = fromBtn || btn2;
    const [ex, ey] = centerOf(b);
    const fold = document.createElement('div');
    fold.className = 'exit-fold';
    const fr = Math.ceil(Math.hypot(Math.max(ex, W - ex), Math.max(ey, H - ey)) + 14);
    fold.style.setProperty('--ex-x', ex.toFixed(0) + 'px');
    fold.style.setProperty('--ex-y', ey.toFixed(0) + 'px');
    fold.style.setProperty('--ex-r', fr + 'px');
    document.body.appendChild(fold);
    requestAnimationFrame(() => requestAnimationFrame(() => fold.classList.add('go')));
    setTimeout(() => {
      window.location.href = 'app.html' + (id ? '?id=' + encodeURIComponent(id) : '');
    }, 640);
  }

  function tick(now) {
    if (cursor) {
      for (const c of chars) {
        const dx = cursor.clientX - c.x, dy = cursor.clientY - c.y;
        const d = Math.hypot(dx, dy);
        const target = Math.max(0, 1 - d / 110);
        c.v += (target - c.f) * 0.16;
        c.v *= 0.7;
        c.f += c.v;
        const f = clamp(c.f, 0, 1.4);
        c.el.style.transform = f > 0.01 ? `translateY(${(-f * 4).toFixed(2)}px)` : 'none';
        c.el.style.filter = f > 0.02 ? `blur(${(f * 2.6).toFixed(2)}px)` : 'none';
      }
    }

    if (burstT0 > 0) {
      const bt = (now - burstT0) / 700;
      if (bt >= 1) burstT0 = -1;
    }

    cx2.clearRect(0, 0, W, H);
    cx2.drawImage(base, 0, 0, W, H);

    for (let i = waves.length - 1; i >= 0; i--) {
      if (now - waves[i].t > 900) waves.splice(i, 1);
    }

    for (const d of grid) {
      let f = 0, cxw = 0, cyw = 0, waveF = 0;
      if (cursor) {
        const dx = cursor.clientX - d.x, dy = cursor.clientY - d.y;
        const dd2 = Math.hypot(dx, dy);
        f = Math.max(0, 1 - dd2 / 150);
        f *= f;
        cxw = dx; cyw = dy;
      }
      for (const w of waves) {
        const age = (now - w.t) / 1000;
        const wr = age * 620;
        const dd3 = Math.hypot(w.x - d.x, w.y - d.y);
        const g = Math.exp(-Math.pow(dd3 - wr, 2) / 2600) * (1 - age / 0.9);
        if (g > waveF) waveF = g;
      }
      const tot = Math.max(f, waveF);
      if (tot < 0.015) continue;
      let px = d.x, py = d.y;
      if (cursor && f > 0) {
        const dd4 = Math.hypot(cxw, cyw) || 1;
        px += cxw / dd4 * f * 7;
        py += cyw / dd4 * f * 7;
      }
      const mixv = Math.min(1, tot * 0.85);
      const acc = ACC[d.s];
      const rC = lerp(INK[0], acc[0], mixv);
      const gC = lerp(INK[1], acc[1], mixv);
      const bC = lerp(INK[2], acc[2], mixv);
      cx2.fillStyle = `rgba(${rC | 0},${gC | 0},${bC | 0},${(0.055 + tot * 0.34).toFixed(3)})`;
      cx2.beginPath();
      cx2.arc(px, py, 1.15 + tot * 1.6, 0, 7);
      cx2.fill();
    }

    for (let i = parts.length - 1; i >= 0; i--) {
      const p = parts[i];
      p.x += p.vx; p.y += p.vy;
      p.vy += 0.14;
      p.vx *= 0.985; p.vy *= 0.985;
      p.life -= 0.022;
      if (p.life <= 0) { parts.splice(i, 1); continue; }
      const a = ACC[p.c];
      cx2.fillStyle = `rgba(${a[0]},${a[1]},${a[2]},${p.life.toFixed(2)})`;
      cx2.beginPath();
      cx2.arc(p.x, p.y, 2.4 * p.life + 0.6, 0, 7);
      cx2.fill();
    }

    requestAnimationFrame(tick);
  }

  function init() {
    buildGrid();
    bindChars();
    measure();
    setTimeout(() => measure(), 1700);
    if (!reduce) requestAnimationFrame(tick);
  }

  tgl.addEventListener('click', () => {
    if (vtActive) return;
    const goingDark = !rootEl.classList.contains('dark');
    const r = tgl.getBoundingClientRect();
    const x = r.left + r.width / 2, y = r.top + r.height / 2;
    vtVars(x, y);
    rootEl.classList.remove('vt-in', 'vt-out', 'vt-page');
    rootEl.classList.add(goingDark ? 'vt-in' : 'vt-out');
    tgl.setAttribute('aria-pressed', String(goingDark));
    try { localStorage.setItem('sparky-theme', goingDark ? 'dark' : 'light'); } catch (e) {}
    postJson('/api/ui', { theme: goingDark ? 'dark' : 'light' }).catch(() => {});
    const apply = () => {
      rootEl.classList.toggle('dark', goingDark);
      setInk();
    };
    if (document.startViewTransition && !reduce) {
      vtActive = true;
      document.startViewTransition(apply).finished.finally(() => {
        rootEl.classList.remove('vt-in', 'vt-out');
        buildGrid();
        vtActive = false;
      });
    } else {
      apply();
      buildGrid();
    }
  });

  addEventListener('resize', () => { buildGrid(); measure(); });
  init();
