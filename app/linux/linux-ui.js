/* Linux-only adapter for Soil's existing app.html.
   Keep the Windows UI untouched while replacing its MJPEG frame with noVNC. */
(function () {
  "use strict";

  // Installer ISOs boot IN QEMU guest VMs; no installer code executes on the host.
  var vmButton = document.getElementById("newVm");
  if (vmButton) {
    var installButton = document.createElement("button");
    installButton.className = "gh-action";
    installButton.id = "installLinuxIso";
    installButton.type = "button";
    installButton.textContent = "Install Linux from ISO";
    installButton.title = "Create a QEMU VM and boot an installer ISO inside it";
    installButton.style.cssText = "width:100%;min-height:36px;margin-top:5px";
    var isoFile = document.createElement("input");
    isoFile.type = "file";
    isoFile.accept = ".iso,application/x-iso9660-image";
    isoFile.hidden = true;
    vmButton.insertAdjacentElement("afterend", installButton);
    installButton.insertAdjacentElement("afterend", isoFile);

    var mountButton = document.createElement("button");
    var rebootIsoButton = document.createElement("button");
    var ejectButton = document.createElement("button");
    [mountButton, rebootIsoButton, ejectButton].forEach(function (button) {
      button.type = "button";
      button.className = "gh-action";
      button.style.cssText = "width:100%;min-height:30px;margin-top:5px";
      button.hidden = true;
      isoFile.insertAdjacentElement("afterend", button);
    });
    mountButton.textContent = "Replace Linux ISO";
    rebootIsoButton.textContent = "Boot ISO on next Start";
    ejectButton.textContent = "Eject installer ISO";

    function selectedLinux() {
      var row = document.querySelector(".vm-row.on");
      if (!row) return Promise.resolve(null);
      return fetch("/api/vms", { cache: "no-store" }).then(function (r) { return r.json(); })
        .then(function (vms) {
          return (vms || []).find(function (vm) { return vm.id === row.dataset.vm && vm.bootMode === "linux-iso"; }) || null;
        });
    }
    function mediaControls() {
      selectedLinux().then(function (vm) {
        var stopped = vm && vm.status === "stopped";
        mountButton.hidden = !stopped;
        rebootIsoButton.hidden = !(stopped && vm.installer && vm.installer.mounted);
        ejectButton.hidden = !(stopped && vm.installer && vm.installer.mounted);
      }).catch(function () {});
    }
    setInterval(mediaControls, 2500);
    mediaControls();

    function jsonRequest(url, method, body) {
      return fetch(url, {
        method: method,
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify(body || {})
      }).then(function (response) {
        return response.json().then(function (data) {
          if (!response.ok) throw new Error(data.error || "Request failed (" + response.status + ").");
          return data;
        });
      });
    }
    function uploadIso(id, file, button) {
      if (!file.name.toLowerCase().endsWith(".iso") || file.size < 32774 ||
          file.size > 12 * 1024 * 1024 * 1024) {
        return Promise.reject(new Error("Choose a Linux .iso file between 32 KiB and 12 GiB."));
      }
      return new Promise(function (resolve, reject) {
        var xhr = new XMLHttpRequest();
        xhr.open("POST", "/api/vms/" + encodeURIComponent(id) + "/installer");
        xhr.setRequestHeader("Content-Type", "application/octet-stream");
        xhr.setRequestHeader("X-Soil-Filename", encodeURIComponent(file.name));
        xhr.upload.onprogress = function (event) {
          if (event.lengthComputable) {
            button.textContent = "Uploading ISO " + Math.round(event.loaded * 100 / event.total) + "%";
          }
        };
        xhr.onload = function () {
          var data = {};
          try { data = JSON.parse(xhr.responseText); } catch (ignore) {}
          if (xhr.status >= 200 && xhr.status < 300) resolve(data);
          else reject(new Error(data.error || "Installer ISO upload failed."));
        };
        xhr.onerror = function () { reject(new Error("Could not upload installer ISO.")); };
        xhr.send(file);
      });
    }

    var replaceId = null;
    installButton.addEventListener("click", function () {
      replaceId = null;
      isoFile.click();
    });
    mountButton.addEventListener("click", function () {
      selectedLinux().then(function (vm) {
        if (!vm || vm.status !== "stopped") return;
        replaceId = vm.id;
        isoFile.click();
      });
    });
    isoFile.addEventListener("change", function () {
      var file = isoFile.files && isoFile.files[0];
      isoFile.value = "";
      if (!file) return;
      var existingId = replaceId;
      replaceId = null;
      if (!file.name.toLowerCase().endsWith(".iso")) {
        alert("Please select a Linux installation .iso file.");
        return;
      }
      var proposedName = file.name.replace(/\.iso$/i, "") || "Linux";
      var vmName = existingId ? null : prompt("Name your new Linux VM (4 GiB RAM, 80 GiB disk):", proposedName);
      if (!existingId && (!vmName || !vmName.trim())) return;
      if (!confirm("Boot this Linux installer INSIDE a QEMU VM?\n\nLinux will be installed onto a new persistent 80 GiB virtual disk. The ISO mounts read-only. The VM starts only after you confirm.")) return;
      installButton.disabled = true;
      mountButton.disabled = true;
      var created = null;
      var vmPromise = existingId
        ? Promise.resolve({ id: existingId })
        : jsonRequest("/api/vms", "POST", { name: vmName.trim(), size: "standard", boot: "linux-iso" })
            .then(function (vm) { created = vm.id; return vm; });
      vmPromise.then(function (vm) {
        return uploadIso(vm.id, file, installButton).then(function () { return vm; });
      }).then(function (vm) {
        installButton.textContent = "Booting Linux...";
        return jsonRequest("/api/vms/" + encodeURIComponent(vm.id) + "/start", "POST", {})
          .then(function () { return vm; });
      }).then(function (vm) {
        try { localStorage.setItem("sparky-selected", vm.id); } catch (ignore) {}
        location.reload();
      }).catch(function (error) {
        alert(error.message + (created ? "\nThe new VM was preserved so you can retry." : ""));
        mediaControls();
      }).then(function () {
        installButton.disabled = false;
        mountButton.disabled = false;
        installButton.textContent = "Install Linux from ISO";
      });
    });

    rebootIsoButton.addEventListener("click", function () {
      selectedLinux().then(function (vm) {
        if (!vm || vm.status !== "stopped") return;
        return jsonRequest("/api/vms/" + encodeURIComponent(vm.id) + "/installer/boot", "POST", {})
          .then(function () { alert("The installer ISO will boot the next time you press Start."); mediaControls(); });
      }).catch(function (error) { alert(error.message); });
    });
    ejectButton.addEventListener("click", function () {
      selectedLinux().then(function (vm) {
        if (!vm || vm.status !== "stopped") return;
        if (!confirm("Eject the installer ISO? The installed Linux disk will remain intact.")) return;
        return fetch("/api/vms/" + encodeURIComponent(vm.id) + "/installer", { method: "DELETE" })
          .then(function (r) {
            return r.json().then(function (data) {
              if (!r.ok) throw new Error(data.error || "Could not eject ISO.");
              mediaControls();
            });
          });
      }).catch(function (error) { alert(error.message); });
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
