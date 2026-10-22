import { useEffect, useState } from "react";
import { soil, type SoilInfo } from "./soil";

export default function App() {
  const [info, setInfo] = useState<SoilInfo | null>(null);
  const [note, setNote] = useState("");
  const [output, setOutput] = useState("starting…");
  const [error, setError] = useState("");

  useEffect(() => {
    let active = true;
    (async () => {
      try {
        const appInfo = await soil.app.info();
        if (!active) return;
        setInfo(appInfo);
        document.title = appInfo.name;
        const saved = await soil.store.get("note");
        if (!active) return;
        if (saved.value) setNote(saved.value);
        setOutput(`data: ${appInfo.dataDir}`);
      } catch (err) {
        if (active) setError(String(err));
      }
    })();
    return () => {
      active = false;
    };
  }, []);

  async function saveNote() {
    try {
      await soil.store.set("note", note);
      setOutput("saved to soil.store");
    } catch (err) {
      setError(String(err));
    }
  }

  async function notify() {
    try {
      await soil.notify(info?.name ?? "Soil app", "The Soil bridge is working.");
      setOutput("notification sent");
    } catch (err) {
      setError(String(err));
    }
  }

  async function showInfo() {
    try {
      const appInfo = await soil.app.info();
      setInfo(appInfo);
      setOutput(JSON.stringify(appInfo, null, 2));
    } catch (err) {
      setError(String(err));
    }
  }

  return (
    <main className="app">
      <p className="eyebrow">SOIL APP</p>
      <h1>{info?.name ?? "Loading…"}</h1>
      <p className="lede">React + TypeScript, packaged as one signed .sol file.</p>

      <section className="card">
        <label htmlFor="note">Session note</label>
        <textarea
          id="note"
          rows={4}
          value={note}
          onChange={(event) => setNote(event.target.value)}
          placeholder="Type something…"
        />
        <div className="actions">
          <button type="button" onClick={saveNote}>Save note</button>
          <button type="button" className="secondary" onClick={notify}>Notify</button>
          <button type="button" className="secondary" onClick={showInfo}>App info</button>
        </div>
        {error ? <pre className="error">{error}</pre> : null}
        <pre aria-live="polite">{output}</pre>
      </section>

      <footer>
        <span>{info ? `${info.name} ${info.version}` : "connecting…"}</span>
        <span>Built with Vite + the Soil SDK</span>
      </footer>
    </main>
  );
}
