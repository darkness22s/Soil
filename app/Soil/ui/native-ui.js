(function () {
  "use strict";
  var openButton = document.getElementById("nativeAppsBtn");
  var dialog = document.getElementById("nativeAppsDialog");
  var list = document.getElementById("nativeAppsList");
  var status = document.getElementById("nativeAppsStatus");
  var importButton = document.getElementById("nativeAppsImport");
  var fileInput = document.getElementById("nativeAppsFile");
  var dropZone = document.getElementById("nativeAppsDropZone");
  var vm = null;
  var busy = false;
  if (!openButton || !dialog || !list || !fileInput) return;

  function currentId() {
    var selected = document.querySelector("#projs .vm-row.on");
    return selected ? selected.dataset.vm : null;
  }
  function url(path) { return "/api/vms/" + encodeURIComponent(vm.id) + "/native-apps" + (path || ""); }
  function message(text, error) {
    status.textContent = text || "";
    status.classList.toggle("error", !!error);
  }
  function request(endpoint, options) {
    return fetch(endpoint, options || {}).then(function (r) {
      if (r.status === 204) return null;
      return r.json().then(function (data) {
        if (!r.ok) throw new Error(data.error || "Request failed (" + r.status + ")");
        return data;
      });
    });
  }
  function setBusy(on) {
    busy = !!on;
    importButton.disabled = busy;
    if (dropZone) dropZone.setAttribute("aria-disabled", String(busy));
    Array.prototype.forEach.call(list.querySelectorAll("button"), function (b) {
      b.disabled = busy;
    });
  }
  function draw(entries) {
    list.replaceChildren();
    if (!entries.length) {
      var empty = document.createElement("p");
      empty.textContent = "No native Linux apps imported for this VM.";
      list.appendChild(empty);
      return;
    }
    entries.forEach(function (app) {
      var row = document.createElement("div");
      row.className = "native-entry";
      var title = document.createElement("strong");
      title.textContent = app.name;
      var detail = document.createElement("small");
      detail.textContent = app.format + " · " + (app.bytes / 1048576).toFixed(1) + " MiB";
      title.appendChild(detail);
      row.appendChild(title);
      var launch = document.createElement("button");
      launch.type = "button";
      launch.textContent = "Launch";
      launch.disabled = busy;
      launch.title = "Start the VM if necessary, then launch this app inside Linux";
      launch.addEventListener("click", function () {
        if (busy) return;
        setBusy(true);
        message("Preparing the Linux VM and launching " + app.name + "...");
        request(url("/" + encodeURIComponent(app.id) + "/launch"), {method:"POST"})
          .then(function () { vm.status = "running"; message("Launch requested for " + app.name + ". Check the VM screen."); return refresh(); })
          .catch(function (err) { message(err.message, true); })
          .then(function () { setBusy(false); });
      });
      row.appendChild(launch);
      var remove = document.createElement("button");
      remove.type = "button";
      remove.textContent = "Remove";
      remove.disabled = busy;
      remove.addEventListener("click", function () {
        if (busy || !confirm("Remove " + app.name + " from this Soil VM?")) return;
        setBusy(true);
        request(url("/" + encodeURIComponent(app.id)), {method:"DELETE"})
          .then(function () { message("Removed " + app.name); return refresh(); })
          .catch(function (err) { message(err.message, true); })
          .then(function () { setBusy(false); });
      });
      row.appendChild(remove);
      list.appendChild(row);
    });
  }
  function refresh() { return request(url(), {cache:"no-store"}).then(draw); }
  function syncButton() {
    openButton.hidden = !currentId();
  }
  var projects = document.getElementById("projs");
  if (projects) new MutationObserver(syncButton).observe(projects, {
    childList:true, attributes:true, subtree:true, attributeFilter:["class"]
  });
  syncButton();

  openButton.addEventListener("click", function () {
    var id = currentId();
    if (!id || busy) return;
    request("/api/vms", {cache:"no-store"}).then(function (vms) {
      var selected = vms.find(function (item) { return item.id === id; });
      if (!selected || selected.kind !== "desktop") {
        throw new Error("Choose a normal Soil desktop VM to install Linux apps.");
      }
      vm = selected;
      document.getElementById("nativeAppsTitle").textContent = "Linux apps · " + vm.name;
      message(vm.status === "running" ?
        "Choose a Linux app file to import and launch in this VM." :
        "Choose a Linux app file. Soil will import it, start this VM, and launch it.");
      dialog.showModal();
      return refresh();
    }).catch(function (err) {
      if (dialog.open) message(err.message, true); else alert(err.message);
    });
  });
  function close() { if (dialog.open) dialog.close(); }
  document.getElementById("nativeAppsClose").addEventListener("click", close);
  document.getElementById("nativeAppsDone").addEventListener("click", close);

  importButton.addEventListener("click", function () {
    if (!busy && vm) fileInput.click();
  });
  if (dropZone) {
    dropZone.addEventListener("click", function () {
      if (!busy && vm) fileInput.click();
    });
    dropZone.addEventListener("keydown", function (event) {
      if (!busy && (event.key === "Enter" || event.key === " ")) {
        event.preventDefault();
        fileInput.click();
      }
    });
    dropZone.addEventListener("dragover", function (event) {
      event.preventDefault();
      if (!busy) dropZone.classList.add("dragging");
    });
    dropZone.addEventListener("dragleave", function () {
      dropZone.classList.remove("dragging");
    });
    dropZone.addEventListener("drop", function (event) {
      event.preventDefault();
      dropZone.classList.remove("dragging");
      if (!busy && event.dataTransfer && event.dataTransfer.files.length === 1)
        installFile(event.dataTransfer.files[0]);
      else if (event.dataTransfer && event.dataTransfer.files.length > 1)
        message("Drop one Linux app at a time.", true);
    });
  }
  fileInput.addEventListener("change", function () {
    var file = fileInput.files && fileInput.files[0];
    fileInput.value = "";
    installFile(file);
  });
  function installFile(file) {
    if (!file || !vm || busy) return;
    if (file.size < 64 || file.size > 256 * 1024 * 1024) {
      message("Choose an x86-64 Linux ELF or Type 2 AppImage between 64 bytes and 256 MiB (.deb and .rpm are not supported).", true);
      return;
    }
    if (!confirm("Import and run '" + file.name +
        "' inside Soil VM '" + vm.name + "'?\nSoil will start this VM if it is off. Only run apps you trust.")) return;

    setBusy(true);
    message("Uploading " + file.name + " into Soil's per-VM storage...");
    var xhr = new XMLHttpRequest();
    xhr.open("POST", url());
    xhr.setRequestHeader("Content-Type", "application/octet-stream");
    xhr.setRequestHeader("X-Soil-Filename", encodeURIComponent(file.name));
    xhr.upload.onprogress = function (e) {
      if (e.lengthComputable) message("Uploading Linux app: " + Math.round(e.loaded / e.total * 100) + "%");
    };
    xhr.onload = function () {
      var app;
      try { app = JSON.parse(xhr.responseText); } catch (ignore) {}
      if (xhr.status < 200 || xhr.status >= 300 || !app || !app.id) {
        message((app && app.error) || "Upload failed (" + xhr.status + ")", true);
        setBusy(false);
        return;
      }
      message("Imported " + app.name + ". Starting the VM if necessary and launching...");
      request(url("/" + encodeURIComponent(app.id) + "/launch"), {method:"POST"})
        .then(function () {
          vm.status = "running";
          message("Launch requested for " + app.name + ". Check the VM screen.");
        })
        .catch(function (err) {
          message("Imported " + app.name + ", but launch failed: " + err.message +
            " You can retry with Launch below.", true);
        })
        .then(function () { return refresh(); })
        .catch(function (err) { message(err.message, true); })
        .then(function () { setBusy(false); });
    };
    xhr.onerror = function () { message("Could not upload the Linux app.", true); setBusy(false); };
    xhr.send(file);
  }
}());
