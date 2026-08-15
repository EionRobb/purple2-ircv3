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
* [`away-notify`](https://ircv3.net/specs/extensions/away-notify) - instantly notifies when users go away or return
* [`batch`](https://ircv3.net/specs/extensions/batch) - groups related server messages together
* [`chathistory`](https://ircv3.net/specs/extensions/chathistory) - requests message history / backscroll from servers and bouncers
* [`echo-message`](https://ircv3.net/specs/extensions/echo-message) - echoes back sent messages
* [`extended-join`](https://ircv3.net/specs/extensions/extended-join) - includes account name and realname in JOIN notifications
* [`invite-notify`](https://ircv3.net/specs/extensions/invite-notify) - notifies when someone has been invited to a channel
* [`labeled-response`](https://ircv3.net/specs/extensions/labeled-response) - correlates sent commands with server responses to avoid repeating sent messages
* [`message-tags`](https://ircv3.net/specs/extensions/message-tags) - typing notifications
* [`metadata-2`](https://ircv3.net/specs/extensions/metadata) - user avatars / buddy icons
* [`sasl`](https://ircv3.net/specs/extensions/sasl-3.2) - SASL authentication support
* [`server-time`](https://ircv3.net/specs/extensions/server-time) - shows correct message timestamp of relayed messages
* [`sts`](https://ircv3.net/specs/extensions/sts) - Strict Transport Security policy enforcement
* [`utf8-only`](https://ircv3.net/specs/extensions/utf8-only) - UTF-8 encoding enforcement
* [`whox`](https://ircv3.net/specs/extensions/whox) - extended WHO queries for efficient user status/account retrieval


