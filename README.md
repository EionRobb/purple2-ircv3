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
* [`chathistory`](https://ircv3.net/specs/extensions/chathistory) - requests message history / backscroll from servers and bouncers
* [`chghost`](https://ircv3.net/specs/extensions/chghost) - updates user hostnames and idents without synthetic reconnects
* [`echo-message`](https://ircv3.net/specs/extensions/echo-message) - echoes back sent messages
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
* [`sts`](https://ircv3.net/specs/extensions/sts) - Strict Transport Security policy enforcement
* [`utf8-only`](https://ircv3.net/specs/extensions/utf8-only) - UTF-8 encoding enforcement
* [`whox`](https://ircv3.net/specs/extensions/whox) - extended WHO queries for efficient user status/account retrieval

## Additional Features
* **Attention / Nudges**: ASCII `\007` (BEL) integration with Pidgin's native Nudge/Attention API to shake/flash windows and play sound alerts on incoming nudges, as well as sending nudges via `/nudge` or the UI button.
* **URI Handling**: Native `irc://` and `ircs://` protocol handler support for opening IRC links directly in Pidgin.
* **Extended Text Formatting & Colors**: Formatting support including bold, italics (`\x1D`), underline, strikethrough (`\x1E`), monospace (`\x11`), reverse video (`\x16`), 24-bit RGB hex colors (`\x04`), and extended 16-98 mIRC color palettes.
* **Avatar & Buddy Icon Support**: Automatic fetching and rendering of user avatars and buddy icons via HTTP/HTTPS metadata.
* **Event Loop Integration**: Refactored timers and event handlers to run through libpurple's main event loop.



