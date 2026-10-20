# Ad-hoc race relay

There is no dedicated race server any more. Any player can host a race on
their own headset (race select -> online host), and anyone can join it from
the list of open races (online client).

Headsets sit behind home routers, so they cannot reach each other directly.
This small relay sits in the middle:

- the hosting game registers its race here and keeps one UDP connection open;
- the relay lists the race and gives it its own public UDP port;
- joining players connect to that port, and the relay passes their packets to
  the host and back.

It never looks inside the game traffic, and it uses almost no CPU or memory.

It also keeps the leaderboard (main menu -> Leaderboard). When a hosted race
ends, the host's game sends each player's best lap. The relay keeps the ten
best drivers per track in `/var/lib/sd-adhoc-relay/records.tsv`, and the two
relays pass every new time to each other, so both have the same table.

The game asks the primary relay first and uses the backup when the primary
does not answer. The addresses are in the game's `config/networking.xml`,
section `Ad-hoc Relays`:

    primary  217.154.211.69
    backup   85.215.158.219

## Install (on each server)

    tar xzf sd-adhoc-relay-linux-x86_64.tar.gz
    cd sd-adhoc-relay-linux-x86_64
    sudo ./install.sh

The installer:

- stops the old dedicated race server if it finds it (both use UDP 28500);
- carries over that server's leaderboard;
- works out which of the two relays this is, and points it at the other one;
- installs the relay as a systemd service that starts at boot;
- opens the firewall if ufw or firewalld is running.

Running it again (for example with a newer package) is safe.

If your hosting company has a firewall in its web panel, open these there too:

    UDP 28500           the race list
    UDP 28510-28573     one port per hosted race (64 races at once)
    TCP 28600           the leaderboard

## Check it

    systemctl status sd-adhoc-relay
    journalctl -u sd-adhoc-relay -f        # live log: races opening, players joining

From any other machine (needs `nc`):

    echo -n PING | nc -u -w1 217.154.211.69 28500     # -> PONG
    echo -n LIST | nc -u -w1 217.154.211.69 28500     # -> LIST and one line per race
    curl http://217.154.211.69:28600/records          # the leaderboard
    curl http://217.154.211.69:28600/sessions         # the open races

## Build from source

    ./make-package.sh          # static binary + tests -> dist/sd-adhoc-relay-linux-x86_64.tar.gz
    ./test-relay.py ./sd-adhoc-relay

## Protocol

See the comment at the top of `sd-adhoc-relay.c`. The game side is
`src/modules/csnetworking/csadhoc.cpp` in speed-dreams-code.
