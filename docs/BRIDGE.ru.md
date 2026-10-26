# In-game bridge (coop-anything RPC)

The plugin embeds `vendor/coopbridge/server.h` (from `coop-anything/tools/bridge`) and
speaks its protocol, so the repo's own client drives it.

Enable in `scripts\nfsu2coop.ini`:

```ini
[Debug]
BridgePort=27971
```

Then:

```powershell
python -I tools\bridge\bridge_client.py --endpoint tcp://127.0.0.1:27971 ping
python -I tools\bridge\bridge_client.py --endpoint tcp://127.0.0.1:27971 state
python -I tools\bridge\bridge_client.py --endpoint tcp://127.0.0.1:27971 call car.slots
python -I tools\bridge\bridge_client.py --endpoint tcp://127.0.0.1:27971 call car.name '{"slot":0}'
python -I tools\bridge\bridge_client.py --endpoint tcp://127.0.0.1:27971 call mem.read '{"addr":197366848,"size":64}'
python -I tools\bridge\bridge_client.py --endpoint tcp://127.0.0.1:27971 call mem.write '{"addr":...,"hex":"..."}'
```

Methods: the standard `ping`, `version`, `methods`, `events`, `state`, plus the NFSU2
ones `mem.read`, `mem.write`, `car.slots`, `car.name`. `state` returns `pid`, `cars`
(the four `[0x89CCF8]` slots), `localPos`, `localModel`.

Rules from the playbook hold: the server listens on loopback only, every request runs
inside `Pump()` on the game thread (called from the EndScene hook), and `ping` answers
even while the game is loading or paused. The game pauses when it loses focus, so
`mainLoopAgeMs` in `ping` tells "paused" from "dead".
