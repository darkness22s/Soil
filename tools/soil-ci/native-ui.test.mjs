import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { runInNewContext } from "node:vm";
import { fileURLToPath } from "node:url";

const root = new URL("../../app/Soil/ui/", import.meta.url);
const html = readFileSync(fileURLToPath(new URL("app.html", root)), "utf8");
const script = readFileSync(fileURLToPath(new URL("native-ui.js", root)), "utf8");
const flush = () => new Promise(resolve => setImmediate(resolve));

class Element {
  constructor() {
    this.handlers = {};
    this.children = [];
    this.dataset = {};
    this.textContent = "";
    this.disabled = false;
    this.open = false;
    this.files = [];
    this.value = "";
    this.classList = { add() {}, remove() {}, toggle() {} };
  }
  addEventListener(name, callback) { this.handlers[name] = callback; }
  click(event = {}) { return this.handlers.click?.(event); }
  appendChild(child) { this.children.push(child); }
  replaceChildren(...children) { this.children = children; }
  setAttribute(name, value) { this[name] = value; }
  showModal() { this.open = true; }
  close() { this.open = false; }
}

function page({ state = "stopped", entries = [] } = {}) {
  const id = "a".repeat(32);
  const vm = { id, name: "Development", kind: "desktop", status: state };
  const selected = new Element();
  selected.dataset.vm = id;
  const ids = ["nativeAppsBtn", "nativeAppsDialog", "nativeAppsList",
    "nativeAppsStatus", "nativeAppsImport", "nativeAppsFile", "nativeAppsDropZone",
    "nativeAppsTitle", "nativeAppsClose", "nativeAppsDone", "projs"];
  const nodes = Object.fromEntries(ids.map(name => [name, new Element()]));
  const calls = [];
  const response = value => ({ ok: true, status: 200, json: async () => value });
  const document = {
    getElementById(name) { return nodes[name]; },
    querySelector(selector) { return selector === "#projs .vm-row.on" ? selected : null; },
    createElement() { return new Element(); }
  };
  class Upload {
    constructor() { this.upload = {}; this.headers = {}; }
    open(method, url) { this.method = method; this.url = url; }
    setRequestHeader(name, value) { this.headers[name] = value; }
    send(file) {
      calls.push([this.method, this.url, file.name]);
      this.status = 200;
      this.responseText = JSON.stringify({
        id: "b".repeat(32), name: file.name, format: "AppImage", bytes: file.size
      });
      queueMicrotask(() => this.onload());
    }
  }
  const fetch = async (url, options = {}) => {
    calls.push([options.method || "GET", url]);
    if (url === "/api/vms") return response([vm]);
    if (url.endsWith("/native-apps") && (!options.method || options.method === "GET"))
      return response(entries);
    if (url.endsWith("/launch")) return response({ id: "b".repeat(32) });
    throw new Error("Unexpected API request: " + url);
  };
  runInNewContext(script, {
    document, fetch, XMLHttpRequest: Upload, MutationObserver: class { observe() {} },
    confirm: () => true, alert: message => { throw Error(message); },
    queueMicrotask, Promise
  });
  return { id, nodes, calls };
}

test("Linux app dialog advertises the single-action file and drop workflows", () => {
  assert.match(html, /id="nativeAppsDropZone"/);
  assert.match(html, /id="nativeAppsImport">Import &amp; Launch/);
  assert.match(html, /id="nativeAppsFile"/);
});

test("installing into a stopped VM uploads and launches without asking user to start it", async () => {
  const { id, nodes, calls } = page();
  nodes.nativeAppsBtn.click();
  await flush();
  assert.equal(nodes.nativeAppsDialog.open, true);
  assert.match(nodes.nativeAppsStatus.textContent, /start this VM/);
  nodes.nativeAppsFile.files = [{ name: "Demo.AppImage", size: 8192 }];
  nodes.nativeAppsFile.handlers.change();
  await flush();
  await flush();
  assert.ok(calls.some(call => call[0] === "POST" &&
    call[1] === "/api/vms/" + id + "/native-apps" && call[2] === "Demo.AppImage"));
  assert.ok(calls.some(call => call[0] === "POST" &&
    call[1] === "/api/vms/" + id + "/native-apps/" + "b".repeat(32) + "/launch"));
  assert.ok(!calls.some(call => call[1].endsWith("/start")));
  assert.match(nodes.nativeAppsStatus.textContent, /Launch requested/);
});

test("existing imported app can be launched even when the VM is stopped", async () => {
  const { id, nodes, calls } = page({ entries: [
    { id: "b".repeat(32), name: "Demo", format: "ELF", bytes: 8192 }
  ] });
  nodes.nativeAppsBtn.click();
  await flush();
  const launch = nodes.nativeAppsList.children[0].children[1];
  assert.equal(launch.disabled, false);
  launch.click();
  await flush();
  assert.ok(calls.some(call => call[0] === "POST" &&
    call[1] === "/api/vms/" + id + "/native-apps/" + "b".repeat(32) + "/launch"));
});
