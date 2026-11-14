/* Linux-only adapter for Soil's existing app.html.
   Keep the Windows UI untouched while replacing its MJPEG frame with noVNC. */
(function () {
  "use strict";
  // Linux native apps are imported from local ELF/AppImage files. They execute
  // as the host user, not inside a VM; never silently start untrusted code.
  var newVmButton = document.getElementById("newVm");
  if (newVmButton) {
    var nativeButton = document.createElement("button");
    nativeButton.id = "importLinuxNative";
    nativeButton.type = "button";
    nativeButton.className = "gh-action";
    nativeButton.textContent = "Import Linux app";
    nativeButton.title = "Import a trusted Linux x86_64 ELF or AppImage";
    nativeButton.style.cssText = "width:100%;margin-top:6px;min-height:34px";
    var nativeInput = document.createElement("input");
    nativeInput.type = "file";
    nativeInput.accept = ".AppImage,.elf,.bin,application/x-executable";
    nativeInput.hidden = true;
    newVmButton.insertAdjacentElement("afterend", nativeButton);
    nativeButton.insertAdjacentElement("afterend", nativeInput);
    nativeButton.addEventListener("click", function () { nativeInput.click(); });
    nativeInput.addEventListener("change", function () {
      var file = nativeInput.files && nativeInput.files[0];
      nativeInput.value = "";
      if (!file) return;
      if (!file.size || file.size > 256 * 1024 * 1024) {
        alert("Choose a Linux x86_64 ELF or AppImage no larger than 256 MiB.");
        return;
      }
      if (!confirm("Import and run '" + file.name + "'?\n\nThis native app runs as YOUR Linux user on the host, not inside a VM or sandbox. Only run software you trust.")) return;
      nativeButton.disabled = true;
      nativeButton.textContent = "Importing...";
      fetch("/api/apps/native", {
        method: "POST",
        headers: { "Content-Type": "application/octet-stream", "X-Soil-Filename": encodeURIComponent(file.name) },
        body: file
      }).then(function (response) {
        return response.json().then(function (body) {
          if (!response.ok) throw new Error(body.error || "Linux native app import failed.");
          return body;
        });
      }).then(function (app) {
        nativeButton.textContent = "Launching...";
        return fetch("/api/vms/" + encodeURIComponent(app.id) + "/start", {
          method: "POST", headers: { "Content-Type": "application/json" }, body: "{}"
        }).then(function (response) {
          return response.json().then(function (body) {
            if (!response.ok) throw new Error((body.error || "Launch failed.") + " App was imported; use Start to retry.");
          });
        });
      }).then(function () {
        window.location.reload();
      }).catch(function (error) {
        alert(error.message || "Unable to import the Linux app.");
        nativeButton.disabled = false;
        nativeButton.textContent = "Import Linux app";
        // Refresh on failed launch too: the imported app may now be listed.
        window.dispatchEvent(new Event("focus"));
      });
    });
  }
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
