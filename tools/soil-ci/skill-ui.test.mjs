import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { runInNewContext } from "node:vm";
import { fileURLToPath } from "node:url";

const html = readFileSync(fileURLToPath(new URL("../../app/Soil/ui/app.html", import.meta.url)), "utf8");
const js = readFileSync(fileURLToPath(new URL("../../app/Soil/ui/skill-ui.js", import.meta.url)), "utf8");

class FakeElement {
  constructor() {
    this.textContent = "";
    this.children = [];
    this.handlers = {};
    this.disabled = false;
    this.open = false;
    this.className = "";
  }
  addEventListener(name, handler) { this.handlers[name] = handler; }
  append(child) { this.children.push(child); }
  replaceChildren(...children) { this.children = children; this.textContent = ""; }
  showModal() { this.open = true; }
  close() { this.open = false; }
  click() { assert.ok(this.handlers.click); return this.handlers.click(); }
}
function fakePage(fetch) {
  const ids = ["skillBtn", "skillDialog", "skillResults", "skillContent",
    "skillRetry", "skillClose", "skillCloseIcon"];
  const nodes = Object.fromEntries(ids.map(id => [id, new FakeElement()]));
  const document = {
    getElementById(id) { return nodes[id] || null; },
    createElement() { return new FakeElement(); }
  };
  runInNewContext(js, { document, fetch, Promise, console });
  return nodes;
}
function response(data, ok = true, status = 200) {
  return { ok, status, text: async () => typeof data === "string" ? data : JSON.stringify(data) };
}
test("the default UI uses a working Skill button in place of the MCP badge", () => {
  assert.match(html, /<button[^>]*id="skillBtn"[^>]*aria-haspopup="dialog"/);
  assert.match(html, /<dialog[^>]*id="skillDialog"/);
  assert.ok(html.includes('<script src="skill-ui.js"></script>'));
  assert.doesNotMatch(html, /class="mcp-mark"/);
});
test("click installs and reports all statuses and loads the bundled instructions", async () => {
  const calls = [];
  const page = fakePage((path, opts) => {
    calls.push({ path, method: opts?.method || "GET" });
    if (path === "/api/skill/install") return Promise.resolve(response([
      { agent: "Claude Code", status: "installed", path: "C:\\\\Users\\\\test\\\\.claude\\\\skills\\\\soil-control\\\\SKILL.md" },
      { agent: "Codex", status: "already_installed", path: "codex-path" },
      { agent: "Sparky", status: "not_detected" },
      { agent: "OpenCode", status: "conflict", message: "An existing skill was left unchanged." }
    ]));
    if (path === "/api/skill") return Promise.resolve(response("# Soil commands"));
    throw new Error("Unexpected fetch: " + path);
  });
  await page.skillBtn.click();
  assert.equal(page.skillDialog.open, true);
  assert.deepEqual(calls.map(x => [x.path, x.method]), [["/api/skill/install", "POST"], ["/api/skill", "GET"]]);
  assert.equal(page.skillResults.children.length, 4);
  assert.match(page.skillResults.children[0].children[0].textContent, /Installed/);
  assert.match(page.skillResults.children[1].children[0].textContent, /Already installed/);
  assert.match(page.skillResults.children[2].children[0].textContent, /Not detected/);
  assert.match(page.skillResults.children[3].children[0].textContent, /Existing skill left unchanged/);
  assert.equal(page.skillContent.textContent, "# Soil commands");
  assert.equal(page.skillBtn.disabled, false);
  await page.skillClose.click();
  assert.equal(page.skillDialog.open, false);
  await page.skillRetry.click();
  assert.equal(calls.length, 4);
});
test("API failure is shown without hiding the bundled instructions; retry works", async () => {
  let failing = true;
  const page = fakePage((path) => {
    if (path === "/api/skill/install")
      return Promise.resolve(failing
        ? response({ error: "Installation unavailable" }, false, 500)
        : response([{ agent: "Codex", status: "installed" }]));
    return Promise.resolve(response("# Skill"));
  });
  await page.skillBtn.click();
  assert.match(page.skillResults.textContent, /Installation unavailable/);
  assert.equal(page.skillContent.textContent, "# Skill");
  assert.equal(page.skillRetry.disabled, false);
  failing = false;
  await page.skillRetry.click();
  assert.equal(page.skillResults.children.length, 1);
});
test("rapid repeated clicks cannot start multiple installations", async () => {
  let resolveInstall;
  let count = 0;
  const page = fakePage((path) => {
    if (path === "/api/skill/install") {
      count++;
      return new Promise(resolve => { resolveInstall = resolve; });
    }
    return Promise.resolve(response("# Skill"));
  });
  const pending = page.skillBtn.click();
  await page.skillBtn.click();
  assert.equal(count, 1);
  resolveInstall(response([{ agent: "Codex", status: "installed" }]));
  await pending;
});
