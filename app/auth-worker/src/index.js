const GITHUB_AUTHORIZE = "https://github.com/login/oauth/authorize";
const GITHUB_TOKEN = "https://github.com/login/oauth/access_token";
const GITHUB_API = "https://api.github.com";
const SESSION_TTL = 60 * 60 * 24 * 30;
const STATE_TTL = 60 * 10;
const USER_AGENT = "soil-auth-worker";

export default {
  async fetch(request, env) {
    try {
      const url = new URL(request.url);
      if (request.method === "GET" && url.pathname === "/health") {
        return json({
          ok: true,
          configured: configured(env),
          kv: Boolean(env.SOIL_AUTH)
        });
      }

      if (request.method === "GET" && url.pathname === "/auth/github/start") {
        return startOAuth(request, env);
      }

      if (request.method === "GET" && url.pathname === "/auth/github/callback") {
        return finishOAuth(request, env);
      }

      if (request.method === "GET" && url.pathname === "/api/me") {
        const session = await readSession(request, env);
        if (!session) return json({ error: "Not connected." }, 401);
        return json({ login: session.login, avatarUrl: session.avatarUrl });
      }

      if (request.method === "GET" && url.pathname === "/api/repos") {
        return listRepos(request, env);
      }

      if (request.method === "GET" && url.pathname === "/api/archive") {
        return downloadArchive(request, env);
      }

      if (request.method === "POST" && url.pathname === "/auth/logout") {
        const session = await readSession(request, env);
        if (session) await env.SOIL_AUTH.delete(session.key);
        return json({ ok: true });
      }

      return json({ error: "Not found." }, 404);
    } catch (error) {
      console.error("soil-auth failure", error && error.stack ? error.stack : error);
      return json({ error: "The authentication service had a problem." }, 500);
    }
  }
};

async function startOAuth(request, env) {
  if (!configured(env)) return json({ error: "The service is not configured." }, 500);

  const url = new URL(request.url);
  const redirectUri = url.searchParams.get("redirect_uri") || "";
  const state = url.searchParams.get("state") || "";

  if (!validLoopbackRedirect(redirectUri)) {
    return json({ error: "Only a local Soil callback can be used." }, 400);
  }
  if (!/^[A-Za-z0-9_-]{16,128}$/.test(state)) {
    return json({ error: "Invalid state." }, 400);
  }

  await env.SOIL_AUTH.put(
    "state:" + state,
    JSON.stringify({ redirectUri, createdUtc: new Date().toISOString() }),
    { expirationTtl: STATE_TTL }
  );

  const authorize = new URL(GITHUB_AUTHORIZE);
  authorize.searchParams.set("client_id", env.GITHUB_CLIENT_ID);
  authorize.searchParams.set("redirect_uri", callbackUrl(request));
  authorize.searchParams.set("scope", env.GITHUB_SCOPE || "read:user repo");
  authorize.searchParams.set("state", state);
  authorize.searchParams.set("allow_signup", "false");
  return Response.redirect(authorize.toString(), 302);
}

async function finishOAuth(request, env) {
  if (!configured(env)) return html("Soil authentication is not configured.", 500);

  const url = new URL(request.url);
  const state = url.searchParams.get("state") || "";
  const code = url.searchParams.get("code") || "";
  const oauthError = url.searchParams.get("error");

  if (oauthError) return html("GitHub did not approve the connection.", 400);
  if (!state || !code) return html("The GitHub callback was incomplete.", 400);

  const stateKey = "state:" + state;
  const pending = await env.SOIL_AUTH.get(stateKey, "json");
  if (!pending || !pending.redirectUri || !validLoopbackRedirect(pending.redirectUri)) {
    return html("This sign-in request expired. Return to Soil and try again.", 400);
  }

  const tokenResponse = await fetch(GITHUB_TOKEN, {
    method: "POST",
    headers: {
      accept: "application/json",
      "content-type": "application/x-www-form-urlencoded",
      "user-agent": USER_AGENT
    },
    body: new URLSearchParams({
      client_id: env.GITHUB_CLIENT_ID,
      client_secret: env.GITHUB_CLIENT_SECRET,
      code,
      redirect_uri: callbackUrl(request)
    })
  });

  const tokenData = await tokenResponse.json().catch(() => null);
  if (!tokenResponse.ok || !tokenData || !tokenData.access_token) {
    console.error("GitHub token exchange failed", tokenResponse.status, tokenData && tokenData.error);
    return html("GitHub did not return an access token.", 400);
  }

  const user = await githubUser(tokenData.access_token);
  if (!user) return html("GitHub did not return a profile.", 400);

  const sessionToken = randomToken(32);
  const encrypted = await encrypt(env.SESSION_SECRET, {
    accessToken: tokenData.access_token,
    login: user.login,
    avatarUrl: user.avatar_url || "",
    scopes: tokenData.scope || ""
  });

  const sessionKey = "session:" + sessionToken;
  await env.SOIL_AUTH.put(
    sessionKey,
    JSON.stringify({
      token: encrypted,
      login: user.login,
      avatarUrl: user.avatar_url || "",
      createdUtc: new Date().toISOString()
    }),
    { expirationTtl: SESSION_TTL }
  );
  await env.SOIL_AUTH.delete(stateKey);

  const target = new URL(pending.redirectUri);
  target.searchParams.set("session", sessionToken);
  target.searchParams.set("state", state);
  target.searchParams.set("login", user.login);
  return Response.redirect(target.toString(), 302);
}

async function listRepos(request, env) {
  const session = await readSession(request, env);
  if (!session) return json({ error: "Connect GitHub first." }, 401);

  const response = await fetch(
    GITHUB_API + "/user/repos?per_page=100&sort=updated&affiliation=owner,collaborator,organization_member",
    { headers: githubHeaders(session.token) }
  );
  if (response.status === 401) {
    await env.SOIL_AUTH.delete(session.key);
    return json({ error: "The GitHub session expired." }, 401);
  }
  if (!response.ok) {
    return json({ error: "GitHub could not list repositories." }, 502);
  }

  const repos = await response.json().catch(() => []);
  if (!Array.isArray(repos)) return json({ repos: [] });
  return json({
    repos: repos.map((repo) => ({
      fullName: repo.full_name,
      owner: repo.owner && repo.owner.login,
      name: repo.name,
      private: Boolean(repo.private),
      defaultBranch: repo.default_branch,
      htmlUrl: repo.html_url,
      description: repo.description || "",
      language: repo.language || "",
      updatedAt: repo.updated_at
    }))
  });
}

async function downloadArchive(request, env) {
  const session = await readSession(request, env);
  if (!session) return json({ error: "Connect GitHub first." }, 401);

  const url = new URL(request.url);
  const owner = url.searchParams.get("owner") || "";
  const repo = url.searchParams.get("repo") || "";
  const ref = url.searchParams.get("ref") || "";
  if (!/^[A-Za-z0-9_.-]{1,100}$/.test(owner) || !/^[A-Za-z0-9_.-]{1,100}$/.test(repo)) {
    return json({ error: "Invalid repository." }, 400);
  }
  if (ref && !/^[A-Za-z0-9._/-]{1,200}$/.test(ref)) {
    return json({ error: "Invalid branch." }, 400);
  }

  const archive = GITHUB_API + "/repos/" + encodeURIComponent(owner) + "/" +
    encodeURIComponent(repo) + "/zipball";
  const target = ref
    ? archive + "/" + ref.split("/").map((part) => encodeURIComponent(part)).join("/")
    : archive;
  const response = await fetch(target, { headers: githubHeaders(session.token) });
  if (response.status === 401) {
    await env.SOIL_AUTH.delete(session.key);
    return json({ error: "The GitHub session expired." }, 401);
  }
  if (response.status === 404) return json({ error: "Repository or branch not found." }, 404);
  if (!response.ok) return json({ error: "GitHub could not create the archive." }, 502);

  const headers = new Headers();
  headers.set("content-type", "application/zip");
  headers.set("cache-control", "no-store");
  headers.set("x-content-type-options", "nosniff");
  headers.set(
    "content-disposition",
    'attachment; filename="' + repo.replace(/["\\]/g, "") + '.zip"'
  );
  return new Response(response.body, { status: 200, headers });
}

async function readSession(request, env) {
  const auth = request.headers.get("authorization") || "";
  if (!auth.startsWith("Bearer ")) return null;
  const token = auth.slice(7).trim();
  if (!/^[A-Za-z0-9_-]{32,128}$/.test(token)) return null;

  const key = "session:" + token;
  const stored = await env.SOIL_AUTH.get(key, "json");
  if (!stored || !stored.token) return null;

  try {
    const decrypted = await decrypt(env.SESSION_SECRET, stored.token);
    if (!decrypted || !decrypted.accessToken) return null;
    return {
      key,
      token: decrypted.accessToken,
      login: stored.login || decrypted.login || "",
      avatarUrl: stored.avatarUrl || decrypted.avatarUrl || "",
      scopes: decrypted.scopes || ""
    };
  } catch {
    await env.SOIL_AUTH.delete(key);
    return null;
  }
}

function configured(env) {
  const clientId = env.GITHUB_CLIENT_ID || "";
  return Boolean(
    clientId &&
    !clientId.startsWith("REPLACE_") &&
    env.GITHUB_CLIENT_SECRET &&
    env.SESSION_SECRET &&
    env.SOIL_AUTH
  );
}

function callbackUrl(request) {
  return new URL(request.url).origin + "/auth/github/callback";
}

function validLoopbackRedirect(value) {
  try {
    const url = new URL(value);
    if (url.protocol !== "http:") return false;
    if (url.username || url.password) return false;
    if (!["127.0.0.1", "localhost", "[::1]"].includes(url.hostname)) return false;
    if (!url.port) return false;
    if (url.pathname !== "/oauth/callback") return false;
    if (url.search || url.hash) return false;
    return true;
  } catch {
    return false;
  }
}

async function githubUser(token) {
  const response = await fetch(GITHUB_API + "/user", { headers: githubHeaders(token) });
  if (!response.ok) return null;
  return response.json().catch(() => null);
}

function githubHeaders(token) {
  return {
    accept: "application/vnd.github+json",
    authorization: "Bearer " + token,
    "user-agent": USER_AGENT,
    "x-github-api-version": "2022-11-28"
  };
}

function json(value, status = 200) {
  return new Response(JSON.stringify(value), {
    status,
    headers: {
      "content-type": "application/json; charset=utf-8",
      "cache-control": "no-store",
      "x-content-type-options": "nosniff",
      "referrer-policy": "no-referrer"
    }
  });
}

function html(message, status = 200) {
  const safe = String(message).replace(/[&<>"']/g, (c) => ({
    "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;"
  }[c]));
  return new Response(
    "<!doctype html><meta charset=\"utf-8\"><title>Soil</title>" +
    "<body style=\"font:16px system-ui;padding:48px;max-width:560px;margin:auto\">" +
    "<h1 style=\"font-size:22px\">Soil</h1><p>" + safe + "</p></body>",
    {
      status,
      headers: {
        "content-type": "text/html; charset=utf-8",
        "cache-control": "no-store",
        "x-content-type-options": "nosniff",
        "referrer-policy": "no-referrer"
      }
    }
  );
}

function randomToken(bytes) {
  const data = new Uint8Array(bytes);
  crypto.getRandomValues(data);
  return base64url(data);
}

function base64url(bytes) {
  let value = "";
  for (let i = 0; i < bytes.length; i++) value += String.fromCharCode(bytes[i]);
  return btoa(value).replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/g, "");
}

function fromBase64url(value) {
  const normal = value.replace(/-/g, "+").replace(/_/g, "/");
  const padded = normal + "=".repeat((4 - (normal.length % 4)) % 4);
  const binary = atob(padded);
  const bytes = new Uint8Array(binary.length);
  for (let i = 0; i < binary.length; i++) bytes[i] = binary.charCodeAt(i);
  return bytes;
}

async function encryptionKey(secret) {
  const digest = await crypto.subtle.digest("SHA-256", new TextEncoder().encode(secret));
  return crypto.subtle.importKey("raw", digest, { name: "AES-GCM" }, false, ["encrypt", "decrypt"]);
}

async function encrypt(secret, value) {
  const key = await encryptionKey(secret);
  const iv = crypto.getRandomValues(new Uint8Array(12));
  const encrypted = await crypto.subtle.encrypt(
    { name: "AES-GCM", iv },
    key,
    new TextEncoder().encode(JSON.stringify(value))
  );
  return base64url(iv) + "." + base64url(new Uint8Array(encrypted));
}

async function decrypt(secret, value) {
  const parts = String(value).split(".");
  if (parts.length !== 2) throw new Error("bad ciphertext");
  const key = await encryptionKey(secret);
  const plain = await crypto.subtle.decrypt(
    { name: "AES-GCM", iv: fromBase64url(parts[0]) },
    key,
    fromBase64url(parts[1])
  );
  return JSON.parse(new TextDecoder().decode(plain));
}
