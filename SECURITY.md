# Security

Switify stores Spotify reusable authentication credentials locally at
`sdmc:/config/Switify/credentials.json`. Never attach that file to an issue or
commit it to a repository. Runtime logs can contain account identifiers and
should be reviewed before sharing.

Report a suspected credential exposure or security issue through GitHub's
private vulnerability-reporting form under this repository's **Security** tab.
Do not open a public issue first. Revoke exposed Spotify sessions from the
Spotify account page and remove the local credentials file.

Switify does not require a Spotify developer client secret and no private key,
token, or credential belongs in the source tree.
