/* Linux-only adapter for Soil's existing app.html.
   Keep the Windows UI untouched while replacing its MJPEG frame with noVNC. */
(function () {
  "use strict";
  var typeList = document.getElementById("mkTypeList");
  var lab = typeList && typeList.querySelector('[data-type="lab"]');
  if (lab) {
    lab.click();
    typeList.querySelectorAll('[data-type="apps"],[data-type="agents"]').forEach(function (option) {
      option.remove();
    });
    var label = document.getElementById("mkTypeDd");
    if (label) label.parentElement.hidden = true;
  }
  var term = document.getElementById("term");
  if (!term) return;
  function mount() {
    var img = term.querySelector("img");
    if (!img || img.dataset.soilLinuxConsole === "1") return;
    var source = img.getAttribute("src") || "";
    var match = source.match(/\/api\/vms\/([^/]+)\/stream\.mjpg/);
    if (!match) return;
    img.dataset.soilLinuxConsole = "1";
    // The Windows shell's MJPEG retry handler must not fight the Linux console.
    img.onerror = null;
    img.removeAttribute("src");
    img.style.display = "none";
    var id = decodeURIComponent(match[1]);
    fetch("/api/vms/" + encodeURIComponent(id) + "/console", { cache: "no-store" })
      .then(function (response) {
        if (!response.ok) throw new Error("The VM console is not available yet.");
        return response.json();
      })
      .then(function (result) {
        if (!img.isConnected || !result.url) return;
        var consoleUrl = new URL(result.url);
        if (consoleUrl.protocol !== "http:" || consoleUrl.hostname !== "127.0.0.1") {
          throw new Error("The console endpoint is not local.");
        }
        var frame = document.createElement("iframe");
        frame.src = consoleUrl.href;
        frame.title = "Soil Linux VM console";
        frame.setAttribute("allow", "clipboard-read; clipboard-write");
        frame.style.cssText = "display:block;width:100%;height:100%;border:0;background:#080908";
        term.appendChild(frame);
      })
      .catch(function (error) {
        if (!img.isConnected) return;
        var hint = document.createElement("div");
        hint.style.cssText = "padding:24px;color:#ddd;font:14px system-ui,sans-serif";
        hint.textContent = error.message + " Try stopping and starting the VM.";
        term.appendChild(hint);
      });
  }
  new MutationObserver(mount).observe(term, { childList: true });
  mount();
})();
