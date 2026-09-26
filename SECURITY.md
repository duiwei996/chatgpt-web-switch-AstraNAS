# Security Policy

## Supported version

Security fixes are applied to the current `main` branch and the latest published AstraNAS release.

## Reporting a vulnerability

Please do not post credentials, private NAS addresses, access tokens, exploit details, or sensitive logs in a public issue.

Preferred reporting path after the repository is public:

1. Open the repository **Security** tab.
2. Use **Report a vulnerability** / GitHub Security Advisories when that option is enabled.
3. Include the affected version/commit, reproduction steps, expected impact, and the smallest safe diagnostic material needed.

If private vulnerability reporting is not available, open a minimal public issue that only states that you need a private security contact path. Do not include secrets, proof-of-concept payloads, or sensitive logs in that issue.

## Sensitive configuration

AstraNAS may store SMB/WebDAV endpoints and credentials in a local `config.ini`. Real configuration files, logs, private keys, and `.env` files must not be committed to the repository. Use `config.example.ini` for examples.

Before attaching logs, remove or replace usernames, passwords, tokens, private hostnames/IPs, share names, URLs containing credentials, and filesystem paths that reveal private information.

## Scope

Reports are especially useful for:

- path traversal or deletion outside the selected storage/NAS root;
- unintended credential or log disclosure;
- unsafe handling of remote files or malformed package/container input;
- CI/release supply-chain issues;
- vulnerabilities that allow operations outside the behavior explicitly described in the README.

AstraNAS does not provide or bypass platform authorization, title keys, sigpatches, or protected-content credentials.
