# Debug keystore

`debug.keystore` is a **debug-only** signing key (PKCS12, alias `androiddebugkey`, passwords `android`), committed on purpose so every CI build has the same signature and installs as an update over the previous one.

It protects nothing: anyone can read it. Never use it for a release or a Play upload.
The release key must never be committed. Keep it in a secret store and inject it in CI.
