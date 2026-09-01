# purple2-ircv3
An attempt to add support for [IRCv3 Updates](https://ircv3.net/irc/index.html) for libpurple2

## Download
Grab the latest version from the [Releases page](https://github.com/EionRobb/purple2-ircv3/releases)

## Building

```bash
sudo apt install -y libglib2.0-dev gettext libsasl2-dev libpurple0t64 libpurple-dev
make
sudo make install
```

## IRCv3 Support
* [`account-notify`](https://ircv3.net/specs/extensions/account-notify) - notifies when users log in or out of services accounts
* [`account-tag`](https://ircv3.net/specs/extensions/account-tag) - attaches sender account name tag to messages
* [`away-notify`](https://ircv3.net/specs/extensions/away-notify) - instantly notifies when users go away or return
* [`batch`](https://ircv3.net/specs/extensions/batch) - groups related server messages together
* [`bot-mode`](https://ircv3.net/specs/extensions/bot-mode) - marks bot users and displays bot emblems on the buddy list
* [`channel-rename`](https://ircv3.net/specs/extensions/channel-rename) - seamlessly handles dynamic channel name changes (`RENAME`) without parting and rejoining (draft)
* [`chathistory`](https://ircv3.net/specs/extensions/chathistory) - requests message history / backscroll from servers and bouncers
* [`chghost`](https://ircv3.net/specs/extensions/chghost) - updates user hostnames and idents without synthetic reconnects
* [`echo-message`](https://ircv3.net/specs/extensions/echo-message) - echoes back sent messages
* [`event-playback`](https://ircv3.net/specs/extensions/event-playback) - historical playback of joins, parts, quits, kicks, modes, and topics in chathistory (draft)
* [`extended-join`](https://ircv3.net/specs/extensions/extended-join) - includes account name and realname in JOIN notifications
* [`extended-monitor`](https://ircv3.net/specs/extensions/extended-monitor) - extends MONITOR to push away, account, and host changes for monitored contacts
* [`invite-notify`](https://ircv3.net/specs/extensions/invite-notify) - notifies when someone has been invited to a channel
* [`labeled-response`](https://ircv3.net/specs/extensions/labeled-response) - correlates sent commands with server responses to avoid repeating sent messages
* [`message-tags`](https://ircv3.net/specs/extensions/message-tags) - typing notifications
* [`metadata-2`](https://ircv3.net/specs/extensions/metadata) - user avatars / buddy icons
* [`monitor`](https://ircv3.net/specs/extensions/monitor) - server-side buddy list presence monitoring
* [`multiline`](https://ircv3.net/specs/extensions/multiline) - supports multiline message batches with embedded newlines (draft)
* [`sasl`](https://ircv3.net/specs/extensions/sasl-3.2) - SASL authentication support
* [`server-time`](https://ircv3.net/specs/extensions/server-time) - shows correct message timestamp of relayed messages
* [`setname`](https://ircv3.net/specs/extensions/setname) - dynamic realname changes without reconnecting
* [`sts`](https://ircv3.net/specs/extensions/sts) - Strict Transport Security policy enforcement
* [`utf8-only`](https://ircv3.net/specs/extensions/utf8-only) - UTF-8 encoding enforcement
* [`whox`](https://ircv3.net/specs/extensions/whox) - extended WHO queries for efficient user status/account retrieval

## Additional Features
* **Modern IRC Protocol & Error Handling**: Comprehensive channel join error reporting for password-protected channels (`475` `+k`), full channels (`471` `+l`), invite-only channels (`473`), bans (`474`), channel limits (`405`), and invalid masks (`476`), invitation confirmation (`341` `RPL_INVITING`), KNOCK support (`/knock` command, `710` `RPL_KNOCK` channel notifications, `711` delivery confirmation, and `712`-`715` error diagnostics), as well as expanded WHOIS metadata reporting (TLS connection ciphers `671`/`275`, real/actual connecting host/IP `338`/`378`, admin and network service info `309`, cert fingerprints `276`, user modes `379`, and bot status `335`), and server registration burst quiet handling (`004` `RPL_MYINFO`).
* **Comprehensive Slash Commands & Aliases**:
  * **Services**: Shortcuts for `/ns` (NickServ), `/cs` (ChanServ), `/ms` (MemoServ), `/os` (OperServ), `/hs` / `/hostserv` (HostServ), `/bs` / `/botserv` (BotServ), `/authserv`.
  * **Channel Moderation**: `/ban` and `/unban` (with automatic nickname-to-hostmask resolution), `/kb` / `/kickban` (ban mask and kick in one step), `/quiet` / `/mute`, `/unquiet` / `/unmute`, `/rename` (rename channel via IRCv3 RENAME), and `/cycle` / `/hop` (part and immediately rejoin active channel).
  * **Server & Discovery**: `/help` (interactive IRC help `704`-`706`), `/who`, `/motd`, `/admin`, `/info`, `/stats`, `/lusers`, `/links`, `/avatar` (set or clear avatar URL), `/setname` / `/realname` (dynamically update realname), and `/raw` (alias for `/quote`).
* **Attention / Nudges**: ASCII `\007` (BEL) integration with Pidgin's native Nudge/Attention API to shake/flash windows and play sound alerts on incoming nudges, as well as sending nudges via `/nudge` or the UI button.
* **URI Handling**: Native `irc://` and `ircs://` protocol handler support for opening IRC links directly in Pidgin.
* **Extended Text Formatting & Colors**: Bidirectional formatting support (aka MIRC colours) including bold, italics (`\x1D`), underline, strikethrough (`\x1E`), monospace (`\x11`), reverse video (`\x16`), 24-bit RGB hex colors (`\x04`), and extended 16-98 mIRC color palettes.
* **Account Ergonomics & Quality-of-Life**:
  * **Auto-Join Channels**: Account option for a comma/space-separated list of channels to enter automatically upon sign-on.
  * **Avatar URL**: Account option to specify your public avatar image URL for IRCv3 metadata broadcast (leave blank to preserve existing server-side avatar).
  * **Custom CTCP VERSION Reply**: Customizable `CTCP VERSION` response string for privacy and client spoofing.
  * **Initial User Modes**: Automatically set (e.g. `+i`, `+R`) or unset user modes upon connecting.
  * **Configurable Part & Quit Messages**: Customizable default messages for leaving channels or disconnecting.
  * **Auto-Rejoin on Kick**: Option to automatically re-enter a channel after being kicked.
* **Smart Notice Routing & Noise Filtering**:
  * **ChanServ & Service Notice Routing**: Directs `[#channel]` entry notices and ChanServ access list notifications into the relevant channel's conversation window as system messages rather than opening disruptive query tabs.
  * **Connection Cruft Suppression**: Filters out noisy server handshake notices (UnrealIRCd hostname/ident checks, `[freenode-info]`, CTCP stats bot probes, and echoed self-invite notices).
* **Avatar & Buddy Icon Support**: Automatic fetching and rendering of user avatars and buddy icons via HTTP/HTTPS metadata (`draft/metadata-2`, `metadata-2`, `draft/metadata`, `metadata`), setting own avatar via account option or `/avatar <url>`, and proactive `METADATA GET` queries for buddy list contacts.
* **TLS Client Certificates & SASL EXTERNAL**: Supports client certificate authentication (CertFP) via a `.pem` file option (relative to the user's `.purple` folder or absolute path), with pre-connection format validation.
* **Event Loop Integration**: Refactored timers and event handlers to run through libpurple's main event loop.

## TLS Client Certificate (CertFP / SASL EXTERNAL) Setup

> [!TIP]
> **Account Action Menu**: From the Buddy List, **Accounts -> <Account> -> TLS Certificate Fingerprint** to view your active certificate fingerprint and ready-to-run NickServ registration command, or automatically generate a new certificate if one does not already exist.

To manually generate a self-signed TLS client certificate and private key in PEM format for SASL `EXTERNAL` authentication (CertFP):

### 1. Generate the Certificate & Key
```bash
# Generate a combined certificate and key file (valid for 3 years)
openssl req -x509 -new -newkey rsa:4096 -sha256 -days 1095 -nodes -out irc_cert.pem -keyout irc_cert.pem -subj "/CN=your_irc_nick"
```

### 2. Get the Certificate Fingerprint
* **Libera.Chat / Atheme networks (SHA-512)**:
  ```bash
  openssl x509 -in irc_cert.pem -outform DER | openssl dgst -sha512 -r | cut -d' ' -f1
  ```
* **Ergo / Anope networks (SHA-256)**:
  ```bash
  openssl x509 -in irc_cert.pem -outform DER | openssl dgst -sha256 -r | cut -d' ' -f1
  ```
* **OFTC networks (SHA-1)**:
  ```bash
  openssl x509 -in irc_cert.pem -outform DER | openssl dgst -sha1 -r | cut -d' ' -f1
  ```

### 3. Register the Fingerprint with NickServ
* **Option A (Easiest)**: Connect to your IRC network with the certificate loaded in Pidgin, identify to your NickServ account, and simply send:
  ```text
  /msg NickServ CERT ADD
  ```
  *(NickServ on most servers will automatically detect the certificate fingerprint from your active TLS connection!)*

* **Option B**: Provide the fingerprint manually:
  ```text
  /msg NickServ CERT ADD <fingerprint>
  ```

### 4. Configure in Pidgin
1. Copy `irc_cert.pem` to your `.purple` directory (e.g. `~/.purple/certs/irc_cert.pem` on Linux or `%APPDATA%\.purple\certs\irc_cert.pem` on Windows).
2. In Pidgin, edit your IRC account:
   * **Advanced Tab**:
     * Check **Use SSL**
     * Check **Authenticate with SASL**
     * In **TLS Client Certificate (.pem)**, enter: `certs/irc_cert.pem` (or the absolute path)






