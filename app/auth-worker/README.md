# Soil GitHub authentication Worker

This Worker keeps the GitHub OAuth client secret and GitHub access tokens off the
desktop. The desktop app only receives an opaque session token for repository
listing and archive downloads.

## 1. Create the GitHub OAuth app

Open GitHub Settings > Developer settings > OAuth Apps > New OAuth App.

- Application name: `Soil`
- Homepage URL: `https://github.com/darkness22s/Soil`
- Authorization callback URL: `https://<your-worker-domain>/auth/github/callback`

Copy the Client ID. Generate a Client Secret and keep it private.

The callback URL must exactly match the deployed Worker domain, including a
custom domain if you use one.

## 2. Create the Cloudflare resources

```powershell
cd app\auth-worker
npm install
npx wrangler login
npx wrangler kv namespace create SOIL_AUTH
```

Put the returned namespace id into `wrangler.jsonc` at
`kv_namespaces[0].id`, and put the GitHub Client ID into
`vars.GITHUB_CLIENT_ID`.

Store both secrets with Wrangler so they never enter the repository or the MSI:

```powershell
npx wrangler secret put GITHUB_CLIENT_SECRET
npx wrangler secret put SESSION_SECRET
```

For `SESSION_SECRET`, use at least 32 random bytes. For example:

```powershell
$rng = [System.Security.Cryptography.RandomNumberGenerator]::Create()
$bytes = New-Object byte[] 32
$rng.GetBytes($bytes)
[Convert]::ToBase64String($bytes)
$rng.Dispose()
```

Or on Linux/macOS:

```bash
openssl rand -base64 32
```

## 3. Deploy

```powershell
npx wrangler deploy
```

Wrangler prints a URL such as
`https://soil-auth.<your-subdomain>.workers.dev`.

Check it:

```powershell
curl https://soil-auth.<your-subdomain>.workers.dev/health
```

## 4. Connect Soil

Open Soil > Settings > GitHub service URL, paste the Worker URL without a
trailing slash, and save. Then open New app > Connect GitHub.

## Security notes

- `GITHUB_CLIENT_SECRET` and `SESSION_SECRET` exist only as Worker secrets.
- GitHub access tokens are encrypted with AES-GCM before being stored in KV.
- The desktop receives an opaque session token, never the GitHub token.
- Only `http://127.0.0.1:<port>/oauth/callback` loopback callbacks are accepted.
- OAuth state expires after ten minutes and is single-use.
- Sessions expire after 30 days; reconnect to refresh.
- The requested OAuth scope is `read:user repo` so private repositories can be
  listed. Use a dedicated GitHub account or a fine-grained authorization if that
  scope is too broad for your needs.
- The Worker never logs tokens or secrets.

## API

- `GET /health`
- `GET /auth/github/start?redirect_uri=...&state=...`
- `GET /auth/github/callback?code=...&state=...`
- `GET /api/me` with `Authorization: Bearer <session>`
- `GET /api/repos` with `Authorization: Bearer <session>`
- `GET /api/archive?owner=...&repo=...&ref=...` with `Authorization: Bearer <session>`
- `POST /auth/logout` with `Authorization: Bearer <session>`
