import { soil } from "./soil.js";

const note = document.querySelector("#note");
const output = document.querySelector("#output");
const status = document.querySelector("#status");

async function boot() {
  const info = await soil.app.info();
  document.title = info.name;
  status.textContent = `${info.name} ${info.version}`;
  const saved = await soil.store.get("note");
  if (saved.value) note.value = saved.value;
  output.textContent = `data: ${info.dataDir}`;
}

document.querySelector("#save").addEventListener("click", async () => {
  await soil.store.set("note", note.value);
  output.textContent = "saved to soil.store";
});

document.querySelector("#notify").addEventListener("click", async () => {
  await soil.notify(document.title, "The Soil bridge is working.");
  output.textContent = "notification sent";
});

document.querySelector("#info").addEventListener("click", async () => {
  output.textContent = JSON.stringify(await soil.app.info(), null, 2);
});

boot().catch((error) => {
  status.textContent = "runtime unavailable";
  output.textContent = String(error);
});
