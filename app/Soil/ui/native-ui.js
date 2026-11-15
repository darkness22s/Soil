(function () {
  "use strict";
  var openButton = document.getElementById("nativeAppsBtn");
  var dialog = document.getElementById("nativeAppsDialog");
  var list = document.getElementById("nativeAppsList");
  var status = document.getElementById("nativeAppsStatus");
  var importButton = document.getElementById("nativeAppsImport");
  var fileInput = document.getElementById("nativeAppsFile");
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
      launch.disabled = busy || vm.status !== "running";
      launch.title = vm.status === "running" ? "Launch inside this Linux VM" : "Start this VM first";
      launch.addEventListener("click", function () {
        if (busy) return;
        setBusy(true);
        message("Installing and starting " + app.name + " inside the Linux VM...");
        request(url("/" + encodeURIComponent(app.id) + "/launch"), {method:"POST"})
          .then(function () { message("Launched " + app.name + " in the VM. Check its screen."); })
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
        "The VM is running. Imported apps launch inside its Linux desktop." :
        "Import is available while the VM is off. Start it before launching the app.");
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
  fileInput.addEventListener("change", function () {
    var file = fileInput.files && fileInput.files[0];
    fileInput.value = "";
    if (!file || !vm || busy) return;
    if (file.size < 64 || file.size > 256 * 1024 * 1024) {
      message("Choose a Linux ELF or AppImage between 64 bytes and 256 MiB.", true);
      return;
    }
    if (!confirm("Import '" + file.name +
        "' into Soil VM '" + vm.name + "'?\nOnly import executables you trust. They run with normal guest-user permissions.")) return;

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
      refresh().then(function () {
        if (vm.status !== "running") {
          message("Saved " + app.name + ". Start this VM, then choose Launch.");
          return;
        }
        message("Launching " + app.name + " in the running Linux VM...");
        return request(url("/" + encodeURIComponent(app.id) + "/launch"), {method:"POST"})
          .then(function () { message("Launched " + app.name + " in the VM."); });
      }).catch(function (err) { message(err.message, true); })
        .then(function () { setBusy(false); });
    };
    xhr.onerror = function () { message("Could not upload the Linux app.", true); setBusy(false); };
    xhr.send(file);
  });
}());
