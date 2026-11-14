(function () {
  "use strict";

  var button = document.getElementById("skillBtn");
  var dialog = document.getElementById("skillDialog");
  var results = document.getElementById("skillResults");
  var instructions = document.getElementById("skillContent");
  var retry = document.getElementById("skillRetry");
  var close = document.getElementById("skillClose");
  var closeIcon = document.getElementById("skillCloseIcon");
  if (!button || !dialog || !results || !instructions || !retry || !close || !closeIcon)
    return;

  var installing = false;
  var labels = {
    installed: "Installed",
    already_installed: "Already installed",
    conflict: "Existing skill left unchanged",
    not_detected: "Not detected",
    error: "Installation failed"
  };

  function readJson(response) {
    return response.text().then(function (bodyText) {
      var body;
      try {
        body = JSON.parse(bodyText);
      } catch (ignore) {
        body = null;
      }
      if (!response.ok)
        throw new Error(body && body.error ? body.error : "The skill installation request failed (" + response.status + ").");
      if (!Array.isArray(body))
        throw new Error("Unexpected skill installation response.");
      return body;
    });
  }

  function readSkill(response) {
    if (!response.ok)
      throw new Error("Could not load the bundled Soil skill (" + response.status + ").");
    return response.text();
  }

  function addResult(item) {
    var row = document.createElement("div");
    row.className = "skill-result " + (item.status || "");
    var title = document.createElement("strong");
    title.textContent = (item.agent || "Assistant") + ": " + (labels[item.status] || item.status || "Unknown");
    row.append(title);
    if (item.path) {
      var path = document.createElement("code");
      path.textContent = item.path;
      row.append(path);
    }
    if (item.message) {
      var message = document.createElement("span");
      message.textContent = item.message;
      row.append(message);
    }
    results.append(row);
  }

  function installSkill() {
    if (installing) return Promise.resolve();
    if (!dialog.open) dialog.showModal();
    installing = true;
    button.disabled = true;
    retry.disabled = true;
    retry.textContent = "Installing…";
    results.textContent = "Checking for supported AI assistants…";
    instructions.textContent = "Loading bundled Soil commands…";

    return Promise.allSettled([
      fetch("/api/skill/install", { method: "POST", headers: { "Accept": "application/json" } }).then(readJson),
      fetch("/api/skill", { cache: "no-store" }).then(readSkill)
    ]).then(function (outcome) {
      if (outcome[0].status === "fulfilled") {
        results.replaceChildren();
        var installs = outcome[0].value;
        if (!installs.length)
          results.textContent = "No supported AI assistants were detected.";
        installs.forEach(addResult);
      } else {
        results.textContent = "Could not install the Soil skill: " + outcome[0].reason.message;
      }
      instructions.textContent = outcome[1].status === "fulfilled"
        ? outcome[1].value
        : "Could not load the Soil skill: " + outcome[1].reason.message;
    }).catch(function (error) {
      results.textContent = "Could not install the Soil skill: " + error.message;
    }).finally(function () {
      installing = false;
      button.disabled = false;
      retry.disabled = false;
      retry.textContent = "Install again";
    });
  }

  button.addEventListener("click", installSkill);
  retry.addEventListener("click", installSkill);
  close.addEventListener("click", function () { dialog.close(); });
  closeIcon.addEventListener("click", function () { dialog.close(); });
})();
