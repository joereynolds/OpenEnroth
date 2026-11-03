# Online co-op

This document describes how online co-op works in this fork, why it's built the way it is, and how to change it.
Read it together with the doxygen comments in `src/Engine/Coop`, which document every class and function.


## Playing

One player hosts, the others join. Click Multiplayer on the main menu, or press P, and pick Host a game or Join a
friend's game. Hosting asks for your name and the port, joining also asks for the host's address and optionally one of
your saves. Enter moves to the next field and Escape goes back one screen. Hosting returns to the main menu, where you
start or load a game as usual.

The same can be done from the command line:

```
OpenEnroth --host 7347 --name Alice
OpenEnroth --connect 192.168.1.10 --name Bob --party save000.mm7
```

* `--host PORT` makes the game listen on `PORT` from startup. Guests are turned away with a "try again later" until the
  host has started or loaded a game.
* `--connect ADDRESS[:PORT]` joins from the main menu. The port defaults to 7347. The join screen keeps retrying for
  two minutes while the host is unreachable or not in a game yet.
* `--name NAME` is shown to the other players in votes and refusals. It defaults to `Player`.
* `--party SAVE` names a save in the guest's own `saves` folder. That save's characters join the host's world. Without
  it, the guest plays a copy of the host's party.

To carry progress between sessions, every player saves their own game as usual. A guest's save holds the guest's own
characters, so rejoining later with `--party` and that save brings their progress along. The host's save holds the
world and the host's party, so the host loads it and hosts again. A guest should save to a slot of their own, joining
overwrites `saves/coop.mm7`. This workflow follows from how joining works, but no test covers it end to end.

The host needs UDP port 7347 open. For two copies on one machine, give the guest its own folder with `--user-path`, so
the two games don't share saves and settings.


## Design decisions

These were the players' calls, and the code is built around them. Changing one of them is a design change, not a fix.

| Topic | Decision |
|-------|----------|
| Parties | Every player has their own party. All players stay on the same map. |
| Pausing | The world doesn't pause for one player, neither in a menu nor when a game's window loses focus. |
| Authority | The host is in charge of the world. |
| Guest party | A guest brings their own party from one of their saves. |
| Map changes | Anyone can propose a trip. Everyone has to agree, and one refusal cancels it. |
| Loot | First come, first served. There is one shared copy of every corpse, chest and ground item. |
| Experience | Only the party that lands the killing blow gets experience. |
| Quests | Quest progress is shared, and every party gets the quest rewards. |
| Rest | Resting needs everyone's agreement. |
| Turn-based mode | Disabled in co-op. |
| Hirelings | A guest's hirelings stay behind in the guest's own world, so no NPC can exist twice. |


## Architecture

The key idea: **every game keeps exactly one local party**. `pParty` is always the local player's own characters, on the
host and on every guest. Other players' parties are never full `Party` objects. They're markers that get drawn
(`Engine::_remoteParties`) and, on the host, stand-in actors that monsters can target (see below). This avoids
touching the roughly 2,500 places in the engine that use `pParty`.

What's shared is the world. Some of it is driven by the host, some of it is relayed between games.

The code is layered like this, lowest first:

| Layer | Where | What it does |
|-------|-------|--------------|
| ENet | `thirdparty/enet` | Reliable UDP. A submodule pinned to `v1.3.18`. |
| `NetHost` | `src/Library/Net` | A thin wrapper around an ENet host. Knows nothing about the game. |
| Protocol | `src/Engine/Coop/CoopProtocol.*` | Message structs and their binary encoding. |
| `CoopServer`, `CoopClient` | `src/Engine/Coop` | Connection handling, the handshake, player ids, votes, and queues of received messages. They don't touch the engine. |
| `CoopSession` | `src/Engine/Coop/CoopSession.*` | Connects the server or client to the running game. This is where engine state is read and written. |
| Engine hooks | `Engine.h`, `CoopEvents.h` | Plain data on `Engine` that game code fills in and the session drains. They keep the engine from depending on the co-op library. |
| Application | `Game.cpp`, `CoopMenuState`, `CoopJoinState`, `OpenEnrothOptions` | Command line options, the co-op and join screens, the vote prompts, and calling the session every frame. |

`coopSession` is a global, like the rest of the engine's world state, and it's null outside of co-op. `Game::gameLoop`
calls `coopSession->update()` once per frame, after the world update and before drawing. In the main menu,
`Game::run` only updates the server, so it can turn early guests away.

### One frame of `CoopSession::update`

1. Poll the network. On the host, keep `gameTimer` running through pauses while any guest is joined
   (`Timer::setKeepsRunning`), because the guests' monsters are the host's. A frame's step is capped meanwhile, so
   blocking work like saving doesn't land on the world in one step. Map loads still pause, `PrepareWorld` switches
   this off while it loads.
2. Send the local party's pose if it changed, and once a second regardless, because poses travel unreliably.
3. On the host: apply guests' hits on monsters, hand out loot, send monster attacks on stand-ins to their guests, move
   the stand-ins, and send monster states.
   On a guest: send its own hits and loot requests, then apply monster states, monster attacks and loot from the host.
4. Exchange quest bits, quest rewards, chests, ground item pickups, dropped items and launched spells. The host sends its clock, guests follow it.
5. Act on a finished vote.
6. Rebuild `Engine::_remoteParties` for drawing.
7. Flush the network, so that everything queued this frame goes out this frame instead of on the next poll.

### Joining

The host's welcome carries its current game as a save file, made with `createSaveData`. A guest with `--party` merges
its own save into it with `makeCoopGuestSave` (`CoopGuestSave.cpp`):

* **From the guest:** characters, gold, bank gold, food, party buffs, the item on the cursor, and the party's records.
* **From the host:** position, quest bits, time, shops and everything else.
* **Times:** absolute times in the guest's party are shifted from the guest's clock to the host's, so buffs and
  conditions keep their remaining durations.
* **Hirelings:** a guest's hirelings stay behind, because hiring is also recorded in the guest's own world's NPC table.

The result is written to `saves/coop.mm7` and loaded like any other save.


## What is synced, and how

There are three ways something gets shared:

* **Host-driven:** the host's game decides, and guests follow.
* **Relayed:** a player's game makes a change and reports it. The host applies it and passes it on to everyone else,
  and every receiving game applies it too. Changes are applied directly, not through the code that records them, so
  they aren't sent back again.
* **Voted:** someone proposes, and everybody has to agree.

| What | How | Notes |
|------|-----|-------|
| Party positions | Relayed | `CoopPartyPose`, unreliable, with a heartbeat. Drawn as a chain mail Fighter by `PrepareRemotePartiesDrawList`. |
| Monsters | Host-driven | `CoopActorStates`, with the spell effects each monster is under. Guests skip monster AI and movement (`Engine::_actorsFollowHost`). |
| Guest hits on monsters | Guest to host | `CoopActorDamage`, from party attacks and Armageddon, with stun and knockback. The host applies it and turns the monster toward the guest's stand-in. |
| Guest spell effects on monsters | Guest to host | `CoopActorBuff`. A guest compares its monsters' buffs with the last frame and sends what changed, see `sendActorBuffs`. |
| Monster attacks on guests | Host to guest | `CoopPartyHit`. The guest's game rolls the attack against its own characters. |
| Corpse loot | Host-driven | `CoopLootRequest` and `CoopLoot`. The host rolls the loot and sends it to the first guest who asks. |
| Quest bits | Relayed | `CoopQuestBit`. |
| Quest rewards | Relayed | `CoopQuestRewards`. Rewards from a script run that also changed a quest bit. |
| Chests | Relayed | `CoopChest`, the whole chest, whenever someone changes it. |
| Ground item pickups | Relayed | `CoopGroundItemTaken`. Matched by item kind and nearest position, since indices differ between games. |
| Dropped items | Relayed | `CoopItemDropped`. Every game makes the same throw with `Party::dropItem`. |
| Spells and arrows | Relayed | `CoopSpriteLaunched`, every sprite a party or the host's monsters launch, see below. |
| Map changes | Voted | `COOP_VOTE_TRAVEL`. Every map change ends up in the game loop's `GAME_STATE_CHANGE_LOCATION` branch, and that's where it's turned into a vote. |
| Rest | Voted | `COOP_VOTE_REST`. Once approved, every game opens the rest window and rests normally. |
| Game time | Host-driven | `CoopClock`, every 30 game seconds. A guest more than a game minute off follows it, see `syncClock`. |

### Monsters in detail

The host sends every monster's kind, position, facing, sector, hit points, AI state and current action. It sends only
monsters that changed since the last update, plus all of them once a second. Action time is left out of the change
check, because guests advance it themselves, and otherwise every animating monster would count as changed every frame.

Encounters spawn at random, so a guest's monster list can differ from the host's. A guest makes its list match:

* It grows its list to the host's length.
* It changes a monster's kind when it differs from the host's at that index.
* It marks its own extra monsters as removed. It never shrinks the list, because other objects refer to monsters by
  index.

### Stand-ins

Monster AI can only target the local party or other actors, and `Pid` has no free object type for another party. So
the host keeps a hidden actor for every guest's party on its map, marked with `ACTOR_COOP_STAND_IN`:

* `CoopSession::updateStandIns` creates it, moves it onto the guest's position every frame, and keeps it alive.
* `Actor::_SelectTarget` picks it by the same rules as the local party (`Actor::partyDetectionRange`).
* The AI list builders count distance to the nearest party, local or stand-in, so monsters near a guest get to act.
* `Actor::ActorDamageFromMonster` records an attack on a stand-in in `Engine::_standInHits` instead of dealing it.
* A party's projectiles fly through it and a party's hits don't hurt it.
* A stand-in is never processed by the AI and never drawn. Guests get it as removed, and saves get it as a removed
  actor, so saves stay plain MM7.

### Spells and arrows

`SpriteObject::Create` records every sprite whose caster is one of the local party's characters in
`Engine::_launchedSprites`. That covers bolts, arrows, and the stationary sprites of spells like Immolation. The host
also records the sprites its monsters launch, guests don't run monster AI and never launch any. Ice Blast and Death
Blossom fragments are left out, because every game breaks the blast up on its own.

Other games create the sprite with `SpriteObject::isCoopRemote` set. It flies, collides and explodes like any other,
but its impact deals no damage, sets no spell effects and pushes no area attack. What it did reaches the other games
the usual way, as a guest's `CoopActorDamage` and `CoopActorBuff`, in the host's monster states, or as a `CoopPartyHit`
for a monster's hit on a guest. Saves leave these sprites out.

### Votes

`CoopServer` runs one vote at a time. A proposal made during a vote is turned down without a vote. The proposer isn't
asked. Everyone else is, the host included, and a player who disconnects stops counting. A host with nobody connected
gets its proposal approved at once. The host takes part through the same calls a client makes: `requestVote`,
`pendingVote`, `answerVote` and `takeVoteDecision`.

`CoopSession::interceptMapChange` and `CoopSession::interceptRest` turn a local action into a proposal. When the vote
passes, the session replays the action and sets a one-shot "approved" flag, so the interception lets it through.
`Game::showCoopVote` shows the prompt (`GUIWindow_CoopVote`) once the player is back on the main game screen.


## Adding something new to sync

Most features follow the relayed pattern. Quest bits are the smallest complete example, so copy them:

1. **Record the change.** Where the game makes it, push a record into a new `Engine` field, but only while
   `engine->_coopSessionActive` is set. Put the record struct in `CoopEvents.h`. Don't call the co-op library from
   engine code.
2. **Add a message.** Add a struct to `CoopProtocol.h`, with a `playerId` that the host fills in. Add it to the end of
   `CoopMessage`, because the order of the alternatives is the wire format. Write its `serializeBody` and
   `deserializeBody` in `CoopProtocol.cpp`, and add a round trip test to `CoopProtocol_ut.cpp`. Items and chests
   reuse their MM7 save snapshots, see `serializeItem`.
3. **Plumb it through.** `CoopServer` stamps incoming messages with the sender's id, queues them, and gets a send
   function that skips the sender. `CoopClient` gets a send function and a queue.
4. **Sync it in the session.** Add a `syncSomething` step to `CoopSession::update`. It sends local records, applies
   remote ones directly, and on the host relays them. Clear the engine field in `CoopSession`'s destructor.
5. **Test both sides.** Write a game test where the engine hosts and a `CoopClient` in the test plays the guest, and
   one where the engine is a guest and a `CoopServer` in the test plays the host. Use real input for the player's
   action where you can.

For something the host should decide, rather than relay, follow corpse loot instead (`handOutLoot`): the guest sends a
request, and the host checks it's still valid before answering only that guest.

Bump `COOP_PROTOCOL_VERSION` once co-op builds are shared with other people and the messages change.


## Tests

Everything is in the `Coop`, `CoopProtocol` and `CoopSession` suites. The network library's tests are in `NetHost`.

* `src/Library/Net/Tests/NetHost_ut.cpp` covers the network library over loopback.
* `src/Engine/Coop/Tests` covers the protocol and the server and client logic. They link the engine, so they run in
  the game test binary.
* `test/Bin/GameTest/GameTests_Coop.cpp` covers the game side.

Things that will bite you when writing tests:

* **ENet only sends from inside a poll.** A test has to keep updating a client while it waits on the other side,
  including during the handshake.
* **Network waits are loops with a cap,** usually 100 ticks, not fixed tick counts, because delivery isn't tied to
  game frames.
* **Reset the session on scope exit** with `MM_AT_SCOPE_EXIT(coopSession.reset())`. A failed assertion must not leak a
  session into the next test, or into shutdown.
* **`NoActors` also stops outdoor monster movement.** Switch it off after setup if monsters need to move.
* **Space picks sprites from the previous frame's billboards.** After loading, tick once before pressing Space on an
  item.
* **Map spawn points add monsters** once `NoActors` is off, so use the index `spawnMonster` returns, never assume 0.


## Known gaps

* **Dropped items:** every game runs the throw's physics on its own, so where an item lands isn't guaranteed to match
  to the unit. Pickups are matched within a short distance to allow for that.
* **Waiting and taverns** aren't voted on. The host's waits move everyone's clock, a guest's waits are undone by the
  host's clock within a second.
* **Spell effect strength:** guests learn which spell effects the host's monsters are under and how much a shrink
  shrinks them, but not the strength of other effects. None of the others change how a monster is drawn.
* **Chests and ground items** are relayed rather than host-driven, so two players grabbing the same item within one
  round trip can both get it.
* **Quest rewards** are applied to the same character slots, whoever is in them.
* **Disconnects:** a guest who loses the host is told so and carries on as a single player game. Rejoining means
  quitting to the main menu and joining again from the co-op screen.
* **Typing addresses:** text input only knows letters, digits and the period, so host names with a dash can't be
  typed on the co-op screen. Use an IP address, or `--connect`.
* **Map scripts run on leaving** only fire in the proposer's game when the group travels.
* **Fares and travel days** spent before a travel vote starts aren't refunded when the vote fails.
* **Map changes** while players are on different maps: chests, ground items and monsters are only synced for the map
  the receiving game is on.
