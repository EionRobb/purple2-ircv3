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
* invite-notify (notifies when someone has been invited to a channel)
* server-time (shows correct message timestamp of relayed messages)
* echo-message (echoes back messages)
* labeled-response (help not repeat own-sent messages)
* message-tags (typing notifications)
* utf8-only (utf-8 only)