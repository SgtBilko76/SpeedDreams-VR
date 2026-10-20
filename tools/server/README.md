# Dedicated server (retired)

Online races are now ad-hoc: players host on their own headsets and the relay
in `adhoc/` lists the races and carries the traffic. See `adhoc/README.md`.
What follows is the old dedicated server, kept for reference; it uses the same
UDP port as the relay, so do not run both on one machine.


    ./setup-linux-server.sh /path/to/speed-dreams-code   # build + prepare
    ./configure-race.py --track jarama --laps 3 --bots 8 # pick the race
    ./run-server.sh                                      # host it

`speed-dreams-server.service` supervises it. Open UDP 28500 inbound.

## Testing it without a headset

`sd-join-probe.c` connects as a player and sends a real PLAYERINFO packet,
which is enough to exercise the whole lobby path - acceptance, the roster XML
push, and the bot-replacement logic.

    gcc -o sd-join-probe sd-join-probe.c -lenet
    ./sd-join-probe 127.0.0.1 28500 TestPlayer 1 8

With an eight-bot grid, one probe joining should turn

    simplix, simplix, shadow, shadow, axiom

into

    networkhuman, simplix, simplix, shadow, shadow

in `~/.speed-dreams-2/config/raceman/netserver.tmp` - the field stays the size
it was configured at, and the player has taken a robot's place. The server logs
`Grid: 1 player(s), 4 robot(s)` when it does.

Run several probes with different names and indices to fill the grid.
