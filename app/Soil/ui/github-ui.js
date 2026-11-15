(function () {
  "use strict";
  var dialog = document.getElementById("githubDialog");
  var trigger = document.getElementById("githubBtn");
  var message = document.getElementById("githubState");
  var connect = document.getElementById("githubConnect");
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
    connect.disabled = busy || !!status.connecting || !!status.connected;
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
      note("GitHub sign-in is not configured for this installation.", true);
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
    connect.disabled = value || !!(last && (last.connecting || last.connected));
  }

  trigger.addEventListener("click", function () {
    if (!dialog.open) dialog.showModal();
    refresh();
  });
  window.SoilGitHub = { open: function () { trigger.click(); } };
  document.getElementById("githubClose").addEventListener("click", function () { dialog.close(); });
  document.getElementById("githubCloseIcon").addEventListener("click", function () { dialog.close(); });
  dialog.addEventListener("close", stopPolling);

  connect.addEventListener("click", function () {
    setBusy(true);
    request("/api/github/connect", "POST", {}).then(function (status) {
      render(status);
      if (status.connecting) startPolling();
    }).catch(function (error) {
      note(error.message || "Could not begin GitHub sign-in.", true);
    }).finally(function () { setBusy(false); });
  });

})();
