(function () {
  "use strict";
  var dialog = document.getElementById("githubDialog");
  var trigger = document.getElementById("githubBtn");
  var worker = document.getElementById("githubWorkerUrl");
  var message = document.getElementById("githubState");
  var connect = document.getElementById("githubConnect");
  var disconnect = document.getElementById("githubDisconnect");
  var save = document.getElementById("githubSave");
  var timer = null;
  var last = null;
  var busy = false;

  function request(path, method, data) {
    return fetch(path, {
      method: method || "GET",
      cache: "no-store",
      headers: { "Accept": "application/json", "Content-Type": "application/json" },
      body: data === undefined ? undefined : JSON.stringify(data)
    }).then(function (response) {
      return response.text().then(function (body) {
        var result = {};
        try { result = JSON.parse(body || "{}"); } catch (_) {}
        if (!response.ok) throw new Error(result.error || "Request failed (" + response.status + ")");
        return result;
      });
    });
  }

  function note(value, isError) {
    message.textContent = value;
    message.classList.toggle("error", !!isError);
  }

  function stopPolling() {
    if (timer) { clearInterval(timer); timer = null; }
  }

  function render(status) {
    var before = last && last.connected;
    last = status;
    if (document.activeElement !== worker) worker.value = status.workerUrl || "";
    connect.hidden = !!status.connected;
    disconnect.hidden = !status.connected;
    connect.disabled = busy || !!status.connecting;
    save.disabled = busy;
    if (status.connected) {
      note("Connected as " + (status.login || "your GitHub account") + ". Private repositories are available.", false);
      stopPolling();
      if (!before) window.dispatchEvent(new Event("soil:github-connected"));
    } else if (status.error) {
      note(status.error, true);
      if (!status.connecting) stopPolling();
    } else if (status.connecting) {
      note("Finish authorization in your browser. Soil will detect the connection automatically.", false);
      startPolling();
    } else if (!status.configured) {
      note("Enter your Soil GitHub authentication service URL, then connect.", false);
    } else {
      note("Not connected. Select Connect GitHub to sign in or reconnect.", false);
    }
    return status;
  }

  function refresh() {
    return request("/api/github/status").then(render).catch(function (error) {
      note(error.message || "Could not check GitHub status.", true);
    });
  }

  function startPolling() {
    if (timer || !dialog.open) return;
    timer = setInterval(refresh, 1200);
  }

  function setBusy(value) {
    busy = value;
    save.disabled = value;
    connect.disabled = value || !!(last && last.connecting);
    disconnect.disabled = value;
  }

  function saveUrl() {
    var url = worker.value.trim();
    if (!url) return Promise.reject(new Error("Enter your Soil GitHub authentication service URL."));
    if (last && last.connected && url !== last.workerUrl &&
        !window.confirm("Changing the service URL will disconnect the current GitHub account. Continue?")) {
      return Promise.reject(new Error("The service URL was not changed."));
    }
    return request("/api/github/config", "POST", { workerUrl: url }).then(render);
  }

  trigger.addEventListener("click", function () {
    if (!dialog.open) dialog.showModal();
    refresh();
  });
  window.SoilGitHub = { open: function () { trigger.click(); } };
  document.getElementById("githubClose").addEventListener("click", function () { dialog.close(); });
  document.getElementById("githubCloseIcon").addEventListener("click", function () { dialog.close(); });
  dialog.addEventListener("close", stopPolling);

  save.addEventListener("click", function () {
    setBusy(true);
    saveUrl().then(function () { note("Authentication service URL saved. You can now connect GitHub.", false); })
      .catch(function (error) { note(error.message, true); })
      .finally(function () { setBusy(false); });
  });

  connect.addEventListener("click", function () {
    setBusy(true);
    var desired = worker.value.trim();
    var setup = !last || !last.configured || desired !== last.workerUrl
      ? saveUrl() : Promise.resolve(last);
    setup.then(function () {
      return request("/api/github/connect", "POST", {});
    }).then(function (status) {
      render(status);
      if (status.connecting) startPolling();
    }).catch(function (error) {
      note(error.message || "Could not begin GitHub sign-in.", true);
    }).finally(function () { setBusy(false); });
  });

  disconnect.addEventListener("click", function () {
    if (!window.confirm("Disconnect GitHub from Soil?")) return;
    setBusy(true);
    request("/api/github/disconnect", "POST", {}).then(render).catch(function (error) {
      note(error.message || "Could not disconnect.", true);
    }).finally(function () { setBusy(false); });
  });
})();
