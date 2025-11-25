# TEK Steam Sharing Server
[![Discord](https://img.shields.io/discord/937821572285206659?style=flat-square&label=Discord&logo=discord&logoColor=white&color=7289DA)](https://discord.gg/JBUgcwvpfc)

tek-s3 is an HTTP/WebSocket server that can provide access to downloading files for Steam applications owned on given accounts, without exposing any account information to the users. It is written in C and C++ and can be built both for Windows and Linux.

A reference tek-s3 server for public use is running at https://api.teknology-hub.com/s3

## Installing

### Windows

There is a statically linked tek-s3.exe in [releases](https://github.com/teknology-hub/tek-s3/releases), which doesn't have any external dependencies other than Windows system DLLs. It's built in MSYS2 CLANG64 environment with some of the packages rebuilt with customized build options to enable missing features and building static libraries where they weren't being built. The exe is signed by Nuclearist's code signing certificate, which in turn is signed by [TEK CA](https://teknology-hub.com/public-keys/ca.crt), so it will be trusted by OS if TEK CA certificate is.

### Linux

There is `tek-s3-x86_64.AppImage` in [releases](https://github.com/teknology-hub/tek-s3/releases), which is built in a Fedora 43 container and signed by Nuclearist's [GPG key](https://teknology-hub.com/public-keys/nuclearist.asc). The .full.AppImage also includes libstdc++.so and libc.so, so it can run on systems that have too old versions of those, or don't use libstdc++. It can be run with `TEK_S3_USE_SYSTEM_LIBS=1` environment variable to prefer system libraries over bundled ones.

Alternatively, you may install a package for your distro listed below, or [build from source](https://github.com/teknology-hub/tek-s3/blob/main/BUILD.md) if it's not there.

|Distro|Package|
|-|-|
|Gentoo|`games-util/tek-s3` from [tek-overlay](https://github.com/teknology-hub/tek-overlay)|

## Using

### Windows

tek-s3.exe can be executed directly to run under current user, in that case the settings file should be located at `%appdata%\tek-s3\settings.json`, and the state file should be located at `%localappdata%\tek-s3\state.json`

You can also run tek-s3.exe with `--register-svc` command-line argument to register it as a system service, which you can start/stop in Services Manager, and also enable to start automatically at boot. This is recommended approach for production use. Keep in mind that it registers the exe at its current path, so if you want to move it to another folder, do that *before* registering. For the service, the settings file should be located at `%windir%\ServiceProfiles\tek-s3\AppData\Roaming\tek-s3\settings.json`, and the state file should be located at `%windir%\ServiceProfiles\tek-s3\AppData\Local\tek-s3\state.json`

### Linux

tek-s3 can be run directly under current user, in that case the settings file should be located at `${XDG_CONFIG_HOME}/tek-s3/settings.json` (`XDG_CONFIG_HOME` falls back to `~/.config`), and the state file should be located at `${XDG_STATE_HOME}/tek-s3/state.json` (`XDG_STATE_HOME` falls back to `~/.local/state`). For the root user (which also includes running tek-s3 as a service), the settings file should be located at `/etc/tek-s3/settings.json`, and the state file should be located at `/var/lib/tek-s3/state.json`.

On systems with systemd, you can (and should) run tek-s3 as a service via `systemctl start tek-s3.service`, and enable it to start automatically at boot via `systemctl enable tek-s3.service`.

### Details

A settings file is a JSON file; currently the only available setting is `listen_endpoint`, which specifies the IP address and port to listen on. Its default value is `127.0.0.1:8080`, which means it'll only accept local connections. To listen on all IPv4 network interfaces at port 80, your settings file should look like this:
```json
{
  "listen_endpoint": "0.0.0.0:80"
}
```
On Linux, when running under root user, you may also choose to listen on a Unix socket instead, by specifying `listen_endpoint` as `unix:{user}:{group}`, where `{user}` is name of the user and `{group}` is name of the group that will own the socket. The socket will be located at `/run/tek-s3.sock` and have `660`/`rw-rw----` access permissions. The state file stores current server state, which includes account authentication tokens, last available apps/depots, and known depot decryption keys. This is the file that you should move as well when moving a server to another system, to preserve its data.

tek-s3 doesn't provide any security features on its own, so it's highly recommended to hide it behind a reverse proxy like Nginx or Apache when exposing it for public use. Here's a snippet of Nginx configuration used for https://api.teknology-hub.com/s3:
```nginx
location /s3 {
  proxy_pass http://unix:/run/tek-s3.sock:/;
  proxy_http_version 1.1;
  proxy_set_header Host $host;
  proxy_set_header Connection $http_connection;
  proxy_set_header Upgrade $http_upgrade;
}
```

Users usually communicate with the server via [tek-steamclient](https://github.com/teknology-hub/tek-steamclient)'s tek-s3 client API / s3c command module in tek-sc-cli. For other means, technical details are given below

The server has the following HTTP GET endpoints:
- `/manifest` - A JSON file listing available applications and their depots that the server can provide manifest request codes for, and known depot decryption keys. The manifest is updated automatically when an account is added/removed/gains a new license. Here's a simplified example of such manifest:
```json
{
  "apps": {
    "{App ID}": {
      "name": "{Application name}",
      "pics_at": "{PICS access token}",
      "depots": [
        1,
        2,
        3
      ]
    }
  },
  "depot_keys": {
    "{Depot ID}": "{Base64-encoded AES-256 key}"
  }
}
```
- `/manifest-bin` - Same as `/manifest` but in binary format, which you may see in `src/manifest.cpp`. tek-steamclient supports and prefers it starting with version 2.1.0
- `/mrc` - Takes 3 URL parameters, all mandatory: `app_id`, `depot_id` and `manifest_id`. On success, returns current manifest request code for given manifest. `401` status code is returned when none of available accounts have a license for specified app/depot, and `500` is returned when a tek-steamclient error occurs while requesting the manifest request code, usually due to invalid manifest ID being specified.

There is a WebSocket endpoint `/signin` for submitting Steam accounts to the server. The communication is done entirely in text frames with JSON content in the following sequence:
1. Client sends the "init" message containing the following fields:
  - `type` - must be `credentials` or `qr`. The other 2 fields are only necessary when `credentials` type is used.
  - `account_name` - Steam account name (aka login).
  - `password` - Steam account password in clear text.
2. Server sends back one of the following messages; confirmation related messages may be sent multiple times, others result in connection being closed:
  - Confirmation message (for credentials-based authentication):
    + `confirmations` - an array of strings specifying available confirmation methods, which can be:
      - "device" - Confirming via Steam mobile app popup.
      - "guard_code" - Entering TOTP Steam Guard code from Steam mobile app.
      - "email" - Entering a code sent via email.
  - QR code URL message (for QR code-based authentication):
    + `url` - URL string that should be converted into a QR code and scanned in Steam mobile app.
  - Success message:
    + `renewable` - boolean indicating whether auth token can be renewed by the server.
    + `expires` - present only when `renewable` is `false`, is a Unix timestamp (seconds since Epoch) indicating when the auth token expires.
  - Error message (sent only for tek-steamclient errors, other kinds of errors simply close the connection):
    + `type` - Numeric value of `type` field in the `tek_sc_err` structure.
    + `primary` - Numeric value of `primary` field in the `tek_sc_err` structure.
    + `secondary` - Present only when `type` is not zero, numeric value of `secondary` field in the `tek_sc_err` structure.
3. During credentials-based authentication, if the client chooses to use `guard_code` or `email_code` confirmation method, it can send the following message to submit the confirmation code to Steam:
  - `type` - either "guard_code" or "email", according to selected confirmation method.
  - `code` - confirmation code value in clear text.

## Project structure

- `pkgfiles` - Files or file templates for package managers to use.
- `res` - Resource files for Windows binary
- `src` - Source code
- `subprojects` - Meson subproject directory. The repository includes wrap files and package files for dependencies that do not have their own packages in major distros. Currently the only such is [ValveFileVDF](https://github.com/TinyTinni/ValveFileVDF)
